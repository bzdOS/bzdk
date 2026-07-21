# Session status dump — 2026-07-22

Written on request ("сбрось текущий статус и контекст в файл") to checkpoint a
very long session before continuing. Read this first if picking the work back
up; `SESSION-HANDOFF.md`/`SESSION-RULES.md` are from 2026-07-15/16 and are
**stale** relative to this — most of their "always ask the human to reset"
rules are now handled autonomously by `supervise.py` (see below).

## 1. Big picture — what's actually working right now

The bzdOS EL2 hypervisor boots FreeBSD/arm64 as an EL1 guest on the Banana Pi
M64, with a working eMMC-backed virtio-blk disk (read **and** write, confirmed
on hardware), and has reached **multi-user boot to a `login:` prompt** at
least once. This is the headline milestone of the whole session. Root login
itself was blocked by a golden-image defect (see §4), just fixed.

The long-running "userland SIGSEGV" mystery from earlier sessions is **fully
resolved**: it was aw_mmc IDMAC DMA corruption, which virtio-blk (backed by
the HV's PIO `emmc_bio` driver) structurally avoids. EL0 execution is clean —
confirmed via a static `/rescue/sh` exiting 0, and a full interactive shell
session running `ls`/`cat`/`uname`/`fsck`/etc without incident.

## 2. Board recovery — now autonomous, no human power-cycles needed

`/opt/bzdos/microkernel/supervise.py`, run as a persistent process, watches
USB VID:PID + EMAC liveness and self-heals:
- **UBOOT** (`1f3a:efe8`) → auto-loads the current build via `reliable_load.py`.
- **HV_OK** (`1d6b:0010` + EMAC answers) → left alone.
- **WEDGED** (HV gadget up, EMAC dead >60s) → sends a break-glass byte
  sequence over `/dev/ttyACM0` (`usbacm.c`'s `bg_seq`) → forces
  `wdt_debug_hold=1` → HW watchdog reboots to U-Boot within ~16s → reloads.
- **GONE** (no USB gadget at all) → the only case needing a physical
  power-cycle; it says so instead of silently doing nothing.

**Bug found and fixed THIS session** (was causing a self-inflicted infinite
reset loop): the state machine gated `do_load()` on the *debounced* `state`
variable, which lingers at `"UBOOT"` for one extra poll after a successful
load (debounce requires 2 confirmations to leave a state). That extra poll
re-triggered `do_load()` on an already-healthy HV; `reliable_load` saw a
non-U-Boot gadget and "helpfully" reset the perfectly fine board, over and
over. Fixed by latching the load-trigger on the **raw**, undebounced vidpid
reading (resets the latch only when the raw reading leaves `"UBOOT"`), while
keeping debounce for the WEDGED/GONE *announcements* only. **Verified fixed
live**: multiple load cycles since with zero spurious reloads.

Currently running as a persistent Monitor task in this session
(`supervise.py`); if you're picking this up fresh, just run
`python3 supervise.py` in `/opt/bzdos/microkernel` and leave it running.

## 3. FreeBSD source is now checked out locally — use it, not WebFetch

`/opt/bzdos/microkernel/freebsd-src/` (separate git repo, not part of the HV
repo): sparse + partial clone (`--filter=blob:none`, cone sparse-checkout) of
`github.com/freebsd/freebsd-src`, branch `releng/15.1`
(commit `0f691888dc56a068f74c213bc87b32939b6b354e`). Checked-out paths:
`sys/ufs`, `lib/libutil`, `lib/libc/gen`, `contrib/openpam/lib/libpam`,
`libexec/rc`, `sbin/fsck_ffs`, `sbin/newfs`, `sbin/init`.

**Use this for anything UFS2/PAM/rc.d-related** — grep it directly instead of
guessing filenames via WebFetch (which 404'd repeatedly this session on wrong
guessed paths before this checkout existed). To add more subtrees:
`env -u http_proxy -u https_proxy -u ALL_PROXY git sparse-checkout add <path>`
then the objects fetch on next checkout automatically (partial clone).
**Note:** git operations against github.com must bypass the shell's default
proxy (`env -u http_proxy -u https_proxy -u ALL_PROXY git ...`) or they hang —
same proxy caveat as the host's global LAN-target note, just for a
different reason (the proxy itself seems to stall on this, not a LAN
unreachability issue).

## 4. Fixes applied directly to the live guest disk (via HV `emmc_bio`)

All done with the same discipline: read current state first, print it,
require an explicit confirmation flag, write, then mandatory read-after-write
verify. Tools (in `/opt/bzdos/microkernel/`, all reuse
`scratchpad/ufs2walk.py` — a hand-verified UFS2 path/inode walker whose struct
offsets were checked by *compiling* C mirrors against the real FreeBSD headers,
not hand-counted):

- **`fstab_patch.py`** — rewrote `/etc/fstab` from `/dev/mmcsd0pX` (aw_mmc
  naming, doesn't exist under virtio) to `/dev/vtbd0p3`, and dropped the fsck
  pass number to 0. Fixed the root cause of `swapon` failing and an early
  boot hang. **Durable** — doesn't get undone by anything.

- **`ufs_clean.py`** — flips the UFS2 superblock's `fs_clean` byte (offset 209
  in `struct fs`) from 0→1, matching what a real clean unmount/fsck-completion
  writes (verified against `sbin/fsck_ffs/fsutil.c:ckfini()` and
  `sys/ufs/ffs/ffs_vfsops.c` in the local checkout — exact fields: `fs_clean`,
  clear `FS_UNCLEAN`/`FS_NEEDSFSCK` in `fs_flags`, zero
  `fs_pendingblocks`/`fs_pendinginodes`; checksum recompute (`fs_ckhash`) was
  researched but turned out **not enforced** on this filesystem
  — `fs_metackhash`'s `CK_SUPERBLOCK` bit is clear — so it's skipped, safely).
  **⚠️ NOT DURABLE — this is the fragile part, see §5.**

- **`fix_ownership.py`** — `/etc/login.conf` and `/etc/pam.d/login` were
  owned by `uid=1001` instead of `0` (an image-packaging defect — files
  created by a non-root build user and never chowned before the image was
  baked). This makes PAM (`openpam_check_owner_perms()`) and libutil's
  `_secure_path()` refuse to use them (`pam_start(): System error` /
  `_secure_path: ... is not owned by root`), which is what was blocking root
  login even after boot reached `login:`. Fixed: `di_uid=0` on both inodes
  (mode bits were already fine, no group/other-write). **Durable** — this is
  real on-disk content, not a runtime flag; won't recur.
  **NOT YET RE-TESTED end-to-end after the most recent fs_clean re-fix** —
  next step is exactly this: reboot, get to `login:`, actually try `root`
  login and confirm PAM succeeds now.

## 5. ⚠️ Open issue: `fs_clean` re-dirties itself — expected, not a new bug

Confirmed via the local FreeBSD checkout, `sys/ufs/ffs/ffs_vfsops.c:1145`:
```c
if (ronly == 0) {
    ...
    fs->fs_clean = 0;
    (void) ffs_sbupdate(ump, MNT_WAIT, 0);
}
```
A read-write mount (which a normal multi-user boot performs once, remounting
root from the kernel's initial read-only mount) **unconditionally re-dirties
the on-disk flag**, and it's only cleared again by a clean unmount/shutdown.
This is completely normal, correct UFS behavior — the same thing would happen
on real hardware if you pulled the power after reaching multi-user without
`shutdown -r`.

**The problem is entirely our own testing methodology**: every reset in this
session's tooling (`reboot_clean_via_emac()`, the break-glass USB path, WDOG
resets) is an **external hard reset** — the guest never gets to run a clean
shutdown first. So: fix `fs_clean` → reboot → reaches multi-user → rw-remounts
→ re-dirties itself on-disk → **next** external reset → back to square one
(rc hangs on fsck again, or per the last observed run, `/bin/sh on /etc/rc
terminated abnormally` — not yet root-caused whether that's the same
fsck-on-mounted-device deadlock from earlier in the session, manifesting
differently, or something new; **was mid-investigation of `libexec/rc/rc.d/fsck`
when this status dump was requested** — the script itself was just read
(see the file, straightforward: runs `fsck -F -p` or `fsck ${fsck_flags}` in
foreground, `case $?` handles 0/2/3/4/8/16/12/130, `stop_boot`/`reboot`/retry
per exit code — nothing exotic, the interesting part is what `fsck` itself
does against an already-ro-mounted device with real corruction-check work to
do, which is where the earlier interactive hang was diagnosed. **Not yet
determined whether boot-time fsck (via rc, before any rw-remount) hits the
same "avail_idx==used_idx frozen, zero virtio I/O" deadlock, or something
else — this needs the same live-instrumentation technique used earlier
(sample the virtqueue avail/used indices while it's stuck) applied to a
FRESH dirty-boot, not re-guessed.**

**Options for a durable fix (not yet chosen/applied):**
1. **Reapply `ufs_clean.py --write` after every external hard reset that
   followed a successful multi-user boot.** Cheap (one command, seconds), but
   manual/fragile exactly as flagged — must remember to do it, and doesn't
   scale to unattended operation.
2. **Make reset tooling attempt a graceful guest shutdown first** (inject
   `shutdown -r now\n` — or send SIGTERM equivalent — over the console before
   any HV-triggered hard reset), so `fs_clean` naturally stays 1 across
   *intentional* resets. Does **not** help the WEDGED/break-glass path by
   definition (the guest is unreachable in that case) — that's an honest
   analog of an unclean power loss and would need real fsck to recover from,
   same as on physical hardware.
3. **Actually fix boot-time fsck to complete instead of hanging on a dirty
   fs** — the "real" fix, but needs the virtqueue-deadlock root cause found
   first (see above), and is the most work.
4. **Ship a pre-fsck'd/known-clean golden image** so this never comes up in
   normal operation — sidesteps the whole question, doesn't fix the
   underlying deadlock but may not need to for a release.

No decision made yet on which to pursue — flagged for the user.

## 6. HV/microkernel source changes this session (all in `/opt/bzdos/microkernel`, git-tracked)

- **`emmc_bio.c`** — root-caused and fixed the actual virtio-blk "hard error
  despite HV S_OK" bug (it was never a virtio or cache-coherency bug): the
  PIO eMMC read returned right after draining 128 FIFO words without waiting
  for `RINT_DATA_OVER`; back-to-back reads (virtio's tight per-sector loop)
  raced the controller's own CRC/retire phase. Fixed by polling
  `RINT_DATA_OVER` + card-idle after the drain, matching what the write path
  already did. This one fix unblocked everything downstream (GPT taste,
  root mount, clean userland). Also: HS-clock (25MHz/4-bit) support added by
  a sub-agent with a verified hardware fallback to 400kHz/1-bit on any
  failure (breadcrumb-gated, see `emmc_bio.c`'s `EBIO_BC_*` window).
- **`emac.c` / `wdt.c` / `smp.c`** — `emac_init()` no longer aborts
  ring/DMA/MAC setup on a slow-training PHY (was the root cause of "dbgmon
  never comes up" on some boots); added `emac_link_watchdog()` self-heal on
  CPU1; `wdt_pet()` now honors `wdt_debug_hold` (previously ignored it,
  meaning a chatty guest could never be forced to reboot remotely — this is
  *why* every dead-EMAC boot used to cost a physical power-cycle, before
  `supervise.py` existed).
- **`vconsole.c`** — IIR was previously always "no interrupt pending", which
  silently broke FreeBSD's ns8250 TX-ready interrupt path the first time
  userland (not just the kernel's polled printf path) tried to write to the
  console — this is what caused rc's output to go completely silent after
  one byte in earlier attempts. Fixed with a proper edge-latched
  IIR (TXRDY set on THR-write/IER-enable, cleared on IIR read) + RXRDY +
  MSR=0xB0 (DCD|DSR|CTS asserted, else `open()` blocks forever waiting for
  carrier). This fix is what let the full rc output actually appear.
- **`usbacm.c`** — added the break-glass reset sequence (§2).
- **`vblk_emmc.c`** — fixed a real (if not-yet-hit) chain-truncation bug that
  could silently corrupt data on long descriptor chains; implemented
  `VIRTIO_BLK_T_GET_ID`; added `gmem_cmo()` (explicit dc-civac cache
  maintenance around every guest-memory access) as defense-in-depth — turned
  out not to be the actual bug (that was `emmc_bio.c`'s DATA_OVER issue) but
  kept as cheap insurance.
- **`el2_ncmap.c`/`.h`** (new) — designed, hardware-syntax-verified but
  **not yet enabled** (`DBG_NCMAP_ENABLE 0` in `main_dbg.c`) EL2 stage-1 remap
  that makes the HV a non-cacheable observer of guest DRAM, instead of the
  earlier blanket cache-flush experiment that reliably wedged EMAC (now
  removed, see git history). Fixed by me post-agent-delivery: added the
  HV-private high-GiB mapping (U-Boot's stack lives there on this 2GB board;
  without it the whole feature would either corrupt the live stack or
  self-abort via its own fail-safe gate) and an SRAM Device mapping.
- **`kload.c`** — `boot_single=YES` and `init_path=/rescue/sh` were both
  *tried* to force single-user for interactive fstab repair and **both
  reverted** — neither is honored by this FreeBSD/arm64 kernel build the way
  expected (`boot_single` ignored entirely; `init_path=/rescue/sh` works but
  panics on shell exit since nothing else can be PID 1). The disk-level
  fstab/superblock/ownership patches above made single-user unnecessary.
- **`supervise.py`** — the uboot-latch bug fix (§2).

## 7. Release-readiness — audit done, Stage 0 done, Stage 1 (this) mostly done

Full audit findings from a background agent are in this conversation's
history (not repeated here in full — ask to see them again if needed, or
just re-run the same audit prompt). Summary of the plan and progress:

- **Stage 0 (housekeeping) — DONE.** `git init`, baseline commit tagged
  `v0-baseline-live-shell`, removed the orphaned RAM-disk virtio stack
  (`virtio_blk.c`/`virtio.c`/`virtio_console.c`/`virtio.h` — confirmed
  unreferenced by any Makefile target), removed the dead `if(0)` cache-flush
  block from `main_dbg.c`, added `README.md`.
- **Stage 1 (clean FS → login) — mostly done, see §5's open issue** for what's
  left before this is *durably* true rather than "true once."
- **Stage 2 (real stage-2 isolation)** — not started. Concrete target:
  `stage2.h` currently maps ALL of guest DRAM (`0x40000000-0x80000000`,
  including the HV's own image and every scratch/breadcrumb window) as one
  flat RWX region — the header says outright "this is intentionally NOT
  isolation." `el2_ncmap.c` (§6) is a *stage-1* (EL2's own view) fix for the
  DMA-coherency problem, **not** a stage-2 fix — it does not restrict what
  the *guest* can do to HV memory. A real fix needs the stage-2 tables
  themselves to exclude the HV regions (fault on guest access), which is a
  separate, not-yet-designed piece of work.
- **Stage 3 (GDB-stub over EMAC)** — not started; per its own design doc
  (`docs/gdbstub-integration.md`) the smallest integration gap of the
  unbuilt "wow" features (Makefile line + ~2 `dbgmon.c` hooks + ~3 lines in
  `main_dbg.c`).
- **Stage 4 (release/debug build split)** — not started.

## 8. Immediate next step when resuming

1. Reboot (fs_clean is currently freshly re-applied as of this dump) and
   confirm: (a) reaches `login:` again, (b) `root` login now succeeds (PAM
   fix from §4 not yet verified end-to-end post-re-fix).
2. Decide + implement a durable answer to §5 (recommend: option 2 — graceful
   shutdown attempt before intentional resets — as the best effort/value
   trade-off; option 3 needs the virtqueue-deadlock root cause first).
3. Continue the release plan at Stage 2 (stage-2 isolation) once §5 is
   settled, per user's stated priority ("Этап 1, чистая ФС" is this stage).

## 9. Tool inventory (all in `/opt/bzdos/microkernel/` unless noted)

| File | Purpose |
|---|---|
| `reliable_load.py` | deterministic loader (VID:PID gate → TFTP/loady/bootelf → verify) |
| `supervise.py` | autonomous board supervisor (§2) — **run this and leave it running** |
| `hvdbg.py` | host-side EMAC debug-protocol client (`HV` class) |
| `scratchpad/ufs2walk.py` | UFS2 path→inode→block walker (read/write via caller callbacks) |
| `fstab_patch.py` | patches `/etc/fstab` on the live disk |
| `ufs_clean.py` | inspects/patches the UFS2 superblock's `fs_clean` flag |
| `fix_ownership.py` | inspects/patches inode uid/mode for PAM-required files |
| `freebsd-src/` | local sparse+partial FreeBSD source checkout (§3) |
