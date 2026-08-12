# Board unbootable: the 2026-08-12 FEL incident, and how to get back

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
nothing prevented it, that heavy guest writes immediately preceded the failure,
and that "it could not have been us" is not available as a defence. The
alternative explanation — the watchdog reset the board and the boot medium
failed for its own reasons — is equally unproven.

Fixed since, in `vblk_emmc.c` (`cb8186c`): guest **writes** below
`VBLK_BOOT_GUARD_LBA` (18432 = 9 MiB, derived from `bpi-image.sh`'s
`UBOOT_RESERVE_MIB=8` plus the GPT plus 1 MiB slack) are refused by the
hypervisor. Reads are not restricted. Breadcrumb `[59]` counts refusals — a
non-zero value there means the guest tried to write the boot area and was
stopped. R2 has been corrected to withdraw the guarantee.

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
