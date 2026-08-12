# Read-only guest root on the eMMC, writable volume on the microSD

`guest-ro-root.sh` references this file; this is it. The script is half of the
design — the half that can be done today, with no new hardware and no new
hypervisor code. The other half needs a second virtio-blk device and a card.

## Why read-only at all

Not for wear, and not for security. For **a boot that always completes.**

The chain this removes has cost this project more time than any single bug:

    unclean stop  ->  root fs dirty  ->  boot-time `fsck -p` fails
                  ->  /etc/rc reaches stop_boot
                  ->  hostname / netif / defaultrouter / sshd never run
                  ->  "the guest lost its network again"  ->  manual recovery

`docs/guest-rootfs-persistence.md` has the measured detail: the network config
was correct and persistent the whole time, and `/etc/rc` simply never reached it.
`fsck_y_enable="YES"` (applied 2026-08-12) breaks the symptom; restoring a
working `/sbin/shutdown` breaks the cause. **A read-only root removes the
premise:** nothing is written, so nothing is ever dirty, so `fsck -p` has nothing
to fail on.

That matters here specifically because unclean stops are not accidents on this
rig — they are the *designed* recovery path. `SESSION-RULES.md` R2: the hardware
watchdog resetting the board back into U-Boot is intentional, and it is what
rescues a wedged hypervisor without a human touching the board. One gate run on
2026-08-11 took 20 break-glass resets. Each one currently leaves a dirty fs. With
a read-only root, a hard reset becomes a non-event.

Secondary benefit, worth naming because it is easy to overclaim: it shrinks the
brick surface. The hypervisor now refuses guest writes below
`VBLK_BOOT_GUARD_LBA` (`vblk_emmc.c`) — that is enforcement against a guest that
*tries*. A read-only root is the guest not trying. Both are worth having and
neither replaces the other. Note that the measured evidence says the guest was
never writing there anyway: 227 guest writes, zero guard refusals
(`docs/brick-recovery.md`).

## What the script does

Three edits, all reversible, all with timestamped backups, idempotent:

1. **`/etc/fstab`** — the root line's options field `rw` → `ro`. Matched on the
   mount point (`$2 == "/"`), not the device name, so it stays correct if the
   device is ever renamed.
2. **`/etc/fstab`** — the swap line commented out. Swap on p4 is eMMC writes, and
   heavy ones; leaving it on defeats the point. Commented rather than deleted so
   the line and the partition are still there when swap moves to the card.
3. **`/etc/rc.conf`** — `root_rw_mount="NO"`, `varsize="64m"`, `tmpsize="128m"`.

`varmfs`/`tmpmfs` are deliberately **left at their default `AUTO`**: FreeBSD's own
`rc` already creates memory-backed `/var` and `/tmp` when the underlying fs is
read-only, so forcing `YES` adds nothing. Only the *sizes* need changing — the
defaults (32m/20m) are too small for `/var/log` plus `/var/run`.

The sizes are modest on purpose. This is memory-backed and it comes out of the
guest's **1 GiB** (`STAGE2_DRAM_SIZE`, `stage2.h`). 64+128 MiB leaves ~830 MiB for
actual work.

Nothing takes effect until the next boot. To change something afterwards:
`mount -u -o rw /`. To undo entirely: `guest-ro-root.sh --revert`.

## What the script deliberately does NOT do

**It does not create anywhere to write.** With root read-only and swap off:

- `pkg install` fails — no writable `/usr/local`, no writable pkg db.
- Any real build fails, and not only for lack of disk: **no swap plus 1 GiB of
  RAM will not link Mesa.** That is not a theoretical limit; it is the reason the
  writable volume has to exist before the read-only root is the steady state.
- `/var` and `/tmp` are memory-backed, so their contents — including `/var/log`
  — do not survive a reset. Anything worth keeping across a reset has to be
  written somewhere else, which is again the card.

So the honest status is: **read-only root is correct as the end state and
premature as the only state.** Until the card is in, run it and remount rw for
the occasional change (that is what the script's closing message tells you), or
hold off — both are defensible. Do not flip it on and then start a multi-hour
build on a remounted-rw root: that is the worst of both, because it is exactly
the long window in which a watchdog reset produces corruption.

## The other half: writable volume on the microSD

### Why the card and not a second eMMC partition

The eMMC is the boot medium. Every write to it is a write to the device the board
needs in order to start at all, which is the risk the boot guard exists to bound.
The card is physically separate, trivially re-imaged from the build host, and —
per `docs/brick-recovery.md` — the **A64 BROM tries microSD before eMMC**, so a
card is independently the recovery path. Putting the writable volume there means
the one medium that must stay intact is also the one nothing writes to.

### What is already in place

`sd_bio.c` (450 lines) implements the MMC2 controller path for the card:
`sd_bio_init()` (bring-up + card identification, CCS detection), `sd_bio_read()`
(CMD17 single block), `sd_bio_write()` (CMD24). It is linked into the `dbg` and
`gdb` targets and has a hosted test (`test_sd_bio_addr`, in `make test`) covering
the byte-vs-block addressing arithmetic.

**It has never executed on hardware.** The `sd` command added to `dbgmon.c`
(`sd init` / `sd read <lba>` / `sd write <lba> CONFIRM`) exists to change that,
and `sd_show_bc()` decodes its seven failure stages so a failure says which stage.
The `CONFIRM` token on `write` is not decoration — this is the one code path in
the tree that can write to a medium the BROM boots from.

Card sizing is settled and not a constraint: 64 GB SDXC is fine. The BROM reads
the SPL from a fixed 8 KiB offset and ignores the partition table, so boot is
capacity-agnostic; `sd_bio.c` uses CCS-bit block addressing, and a `uint32_t` LBA
reaches 2 TB.

### What is missing

A **second virtio-blk device** so the guest sees the card as its own disk. The
MMIO layout has room and needs no stage-2 change, because the whole containing
2 MiB block is already invalid at stage 2 and every access in it already faults
to EL2:

    0x0A000000 .. 0x0A000200   vblk_emmc   (existing)
    0x0A001000 .. 0x0A001200   vnet_emac   (existing)
    0x0A002000 .. 0x0A002100   scanout     (existing)
    0x0A003000 .. 0x0A003200   vblk_sd     <- the free slot to use
    0x0A003200 .. 0x0A200000   still spare

`vblk_emmc.c`'s device state is a single `g_blk`, and its cross-core locking
assumes one eMMC consumer, so a second device is not a matter of instantiating
the struct twice — that is the actual work, and it is the reason this is a
separate task rather than a paragraph here.

### Target layout, once both halves are in

    eMMC p3  ->  vtbd0p3   /            ufs  ro     (never written)
    eMMC p4  ->  (unused)  swap                     (disabled, kept for later)
    card p1  ->  vtbd1p1   /usr/local   ufs  rw     packages
    card p2  ->  vtbd1p2   /home        ufs  rw     builds, sources
    card p3  ->  vtbd1p3   swap                     makes linking Mesa possible
                           /var, /tmp   mfs  rw     memory-backed, per boot

`/var` and `/tmp` can stay memory-backed even then — they are small, they benefit
from not being on flash, and losing them on reset is correct behaviour for both.

## Order of operations

1. Card arrives; image it from the build host (it is also the recovery card, so
   write U-Boot's SPL to it at the 8 KiB offset first — `docs/brick-recovery.md`).
2. `sd init` over EMAC. Do not proceed past a failure; `sd_show_bc()` names the
   stage.
3. `sd read` a known sector and check it against the host's view of the same
   card. This is the first hardware execution of `sd_bio.c`; treat it as
   bring-up, not as a check-box.
4. Second virtio-blk device, guest sees `vtbd1`.
5. Move `/usr/local`, `/home` and swap onto it. Confirm the guest still boots
   with the card **absent** — a rig that will not boot without its scratch disk
   has traded one fragility for another.
6. Only then `guest-ro-root.sh`, and reboot.
7. Then the soak (it is skewed by fs corruption, so it wants this done first).
