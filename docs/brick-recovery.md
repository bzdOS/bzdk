# The 2026-08-12 "brick" that was not one — and what it actually taught

> **Current loader (2026-10-02):** the eMMC FIT at LBA 0x50 is
> `tftpboot/u-boot-nc.itb` (netconsole on, waits at most 3 s for USB). The
> SPL at LBA 16 is unchanged. The previous FIT is `tftpboot/u-boot-retry.itb`.
> Roll back with
> `uboot_flash_fit.py --fit u-boot-retry.itb --current u-boot-nc.itb --expect-version "Sep 26 2026 - 13:46"`
> (that needs the USB cable for the prompt). Test any new loader with
> `tools/chainload/uimg_chainload_test.sh` first; see `autoboot-no-cable.md`.

## CORRECTION FIRST: the board was never bricked, and this file said it was

Everything below the next section was written while I believed the board had lost
its SPL and dropped into the BROM's FEL recovery mode. **That was wrong.** The
correction, established afterwards by measurement:

- `1f3a:efe8` is **U-Boot's own USB gadget in this project**, not only FEL.
  `SESSION-RULES.md` R2 says so in as many words: *"Гаджет U-Boot
  (`1f3a:efe8`, `ttyACM0`/`ttyCHIMP`) появляется ЦИКЛАМИ"*. `lsusb`'s string
  *"sunxi SoC OTG connector in FEL/flashing mode"* is just `usb.ids`' label for
  that VID:PID, and I read the label instead of the evidence.
- The evidence was in `dmesg` the whole time:
  `Product: USB download gadget` / **`Manufacturer: bzdOS`** — the BROM does not
  announce itself as bzdOS. And `cdc_acm 2-4:1.0: ttyACM0: USB ACM device`: the
  console attached successfully.
- **The watchdog did exactly what R2 says it does.** The board reset and came
  back to a U-Boot prompt on its own. Confirmed afterwards by the project's own
  tooling: `board_ctl.board_state()` → `AT_UBOOT`.
- `sunxi-fel version` timed out **because it is not FEL**. Worse, running it
  detached `cdc_acm` from the interface (`usbfs: process (sunxi-fel) did not
  claim interface 1 before use`), so my own diagnosis broke the working console I
  was trying to reach.
- Recovery was one line, no card, no FEL:
  `echo 2-4:1.0 > /sys/bus/usb/drivers/cdc_acm/bind`, then a normal reload.

**So: no brick, no lost SPL, and the anti-brick path worked.** What follows is
kept because the observations are accurate and the FEL procedure is still the
right thing to have written down — but read it as "what I thought at the time",
not as an incident record.

### The actual lesson, which is not the one I first drew

Two independent channels degrading within minutes was real and worth stopping
for. But the failure was **a board that reset itself and waited in U-Boot** —
the designed behaviour — and I turned it into an emergency by trusting a
human-readable USB label over `dmesg`. Check `Manufacturer:` before concluding
anything about which firmware is answering, and run `board_ctl.board_state()`,
which exists precisely to answer this question, before reaching for `sunxi-fel`.

---

# Original account, written under the mistaken FEL diagnosis

Written while the board was still down, so the state is recorded rather than
remembered. Nothing here has been executed — the recovery procedure below is
prepared, not proven, and that distinction is the whole point of writing it now.

## What happened, in the order it was observed

1. A guest-side build (Mesa deps + a 387 MB source extraction) ran for ~40 min,
   writing heavily to the eMMC through `vblk_emmc`.
2. `ssh` to the guest stopped answering. At that moment the board was **fine**:
   `bzdctl.py status` showed HV uptime ~62 000 s, `exceptions none recorded`,
   CPU0 and CPU1 both advancing, 43.6 °C. So it read as load, and that reading
   was correct at the time.
3. Then `ssh` became `No route to host` — the guest's network went.
4. Then `guest_sh.py` failed with `OSError: [Errno 5]` writing to the tty — the
   HV's USB-ACM gadget had left the bus.
5. Then EMAC stopped answering too: `bmc ver` returned an empty string, raw
   memory reads over EMAC returned nothing, `bzdctl.py status` →
   `UNREACHABLE over EMAC`.
6. `lsusb` showed the board back on the bus as
   `1f3a:efe8 sunxi SoC OTG connector in FEL/flashing mode`.

**FEL mode means the BROM found nothing bootable.** Not a hung HV, not a wedged
guest — the boot ROM ran, looked for an SPL, and fell through to USB recovery.

`sunxi-fel version` then timed out three times in a row
(`usb_bulk_recv() ERROR -7`), so even FEL was not answering.

## Why the board could reach that state at all

`vblk_emmc` exposes the entire eMMC to the guest 1:1 — virtio sector N is eMMC
LBA N, no offset (its own geometry comment says so), capacity 15269888, the real
device size. That is deliberate: the guest has to read the GPT to find its root
on p3.

The unguarded consequence was that the guest could **write** anywhere on the
device, including LBA 16 — the fixed byte offset (8 KiB) the A64 BROM reads the
SPL from, and the only reason this board boots. `SESSION-RULES.md` R2 asserted
the board could not brick because of "anti-brick SPL@8KiB"; nothing enforced
that. It was an assumption about guest behaviour, in a driver with a documented
history of LBA-level bugs (lost kicks at unexpected LBAs, partial-sector
stitching, a CPU0/CPU2 lock race).

**This is not a proof that the guest destroyed the SPL.** It is a statement that
nothing prevented it.

**And it has since been measured: the guest does NOT write there.** With the
guard deployed on real hardware and its counter on a slot that is actually free
(see below), 227 guest write requests — including a deliberate 2 MiB `dd` — produced
**zero** refusals. So the guard does not false-positive on normal guest I/O, and
there is no evidence the guest ever wrote the boot area. Combined with the
correction at the top of this file (the SPL was never lost), the
"guest bricked the board" hypothesis has nothing left supporting it.

The guard is still right to exist: it turns an unenforced assumption into an
enforced boundary, and it costs one comparison per request.

Fixed since, in `vblk_emmc.c` (`cb8186c`): guest **writes** below
`VBLK_BOOT_GUARD_LBA` (18432 = 9 MiB, derived from `bpi-image.sh`'s
`UBOOT_RESERVE_MIB=8` plus the GPT plus 1 MiB slack) are refused by the
hypervisor. Reads are not restricted. Breadcrumb **`[62]`** counts refusals — a
non-zero value there means the guest tried to write the boot area and was
stopped. (It was `[59]` for one build; that slot was already taken. See the last
section of this file.) R2 has been corrected to say the guarantee is *now*
enforced rather than assumed — not, as it first said, that it had been violated.

## Recovery route 1 (preferred): bootable microSD

**The A64 BROM tries microSD before eMMC.** So a card with a valid SPL boots the
board regardless of what happened to the eMMC, with no FEL involved and nothing
to plug or press beyond inserting the card.

Artifacts are present and dated 2026-07-08:

```
/opt/bzdos/build/u-boot/u-boot-sunxi-with-spl.bin   907213 bytes  <- write this
/opt/bzdos/build/u-boot/spl/sunxi-spl.bin            32768 bytes
/opt/bzdos/build/u-boot/u-boot.bin                  835272 bytes
```

`u-boot-sunxi-with-spl.bin` is the combined image the sunxi BROM boot flow
expects at the 8 KiB offset:

```sh
# On the build host, with the card at /dev/sdX (CHECK IT -- lsblk first)
dd if=/opt/bzdos/build/u-boot/u-boot-sunxi-with-spl.bin of=/dev/sdX \
   bs=1024 seek=8 conv=fsync
```

`seek=8` with `bs=1024` puts it at exactly 8 KiB = LBA 16, the BROM-mandated
offset (`bpi-image.sh`'s `SPL_OFFSET_KIB=8`, marked FIXED). No partition table is
needed on the card for this — the BROM reads the raw offset and ignores the GPT.

Once U-Boot is up from the card, the eMMC is reachable from its prompt
(`mmc dev 1`, `mmc write ...`) and the boot area can be restored the same way,
now under the LBA guard.

**Card choice matters less than it looks.** Endurance ratings address write *wear*;
what corrupts a card on this rig is unclean power-off during a write, which is a
different failure. A boot-only card is written once, so a cheap A1 card is fine.
Endurance is worth paying for only if the card also becomes the writable scratch
volume.

## Recovery route 2: FEL

Only if route 1 is unavailable. `sunxi-fel` is installed (`/usr/bin/sunxi-fel`)
and `sunxi-tools` is checked out at `/opt/bzdos/build/sunxi-tools/`.

**Blocked as of writing:** `sunxi-fel version` times out. A FEL device that
enumerates but will not answer `version` is not in a state to be written to, and
guessing at it is how a recoverable board becomes an unrecoverable one. Do not
`sunxi-fel write` anything until `version` returns.

The shape of it, for when it does:

```sh
sunxi-fel version                                    # must answer FIRST
sunxi-fel spl /opt/bzdos/build/u-boot/u-boot-sunxi-with-spl.bin
sunxi-fel write 0x4a000000 <u-boot.bin> ; sunxi-fel exe 0x4a000000
```

This project has **never** exercised FEL. `BRING-UP.md` §"What's missing" says so
explicitly and adds that how the current SPL/U-Boot originally got onto the board
is not recorded anywhere in the repo. So route 2 is being written from the
outside, not from experience, and should be treated accordingly.

## What was NOT lost

Everything from the session is committed and board-independent: the storage
guard and its tests, W^X enforcement, the zero-copy scanout HV side,
`dma_buf_mmap`, the L2 cache implementation, the PMU fix, the clk audit, the
upstream reports, and this document. The Mesa 26.2.0 tarball and its verified
sha256 are on the build host. Tier 0 already passed on hardware and is recorded.

## The lesson worth keeping

Three symptoms arrived in sequence — guest network, then USB gadget, then EMAC —
and each one individually had an innocent explanation that was true when checked.
The board was genuinely healthy at step 2. What that sequence actually was, in
hindsight, is a progressive loss of every channel, and the useful signal was the
*pattern*, not any single reading.

Concretely: **if two independent channels to the board degrade within minutes of
each other, stop and take a full snapshot before continuing.** Reading each one
as "probably load" is locally correct and globally wrong.


## One more mistake, recorded because it nearly became a published fact

The boot-guard counter was first put at breadcrumb index `[59]`. That slot
already belonged to `g_lock_retries` — eMMC lock-acquire retries, a normal and
frequent event during guest I/O. So the very first reading, `[59] = 2` and then
`[59] = 4` after a `dd`, looked exactly like "the guest tried to write the boot
area and was stopped four times", and I said so before checking.

It was four lock retries. `hv_addrmap.h`'s `_Static_assert` chain proves that
breadcrumb *windows* do not overlap; nothing proves that *indices inside* a
window do not. Moved to `[62]` (0..61 were all taken; 63 is the last one free),
zeroed at init so "never happened" is distinguishable from "this build has no
such counter", and the index map is now documented in `vblk_emmc.c` with this
story attached.

Two wrong readings in one afternoon, both from trusting a number without
checking who else writes it. Grep for the slot before believing the value.
