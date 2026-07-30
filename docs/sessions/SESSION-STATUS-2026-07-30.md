# Session status — 2026-07-30: mountroot cracked, vtimer theory retracted, new EIO bug found

Point-in-time snapshot (see `docs/sessions/README.md` for how these age).
Board: `08688afa045f+` running, matches on-disk tree. All findings below are
live, hardware-confirmed unless marked otherwise.

## 1. RETRACTED: "permanently lost vtimer" root cause

Earlier same-session claim — EL2 masks the guest's CNTV then re-injects
through a disabled vGIC (`GICH_HCR=0`), so the timer dies forever and the
guest sleeps in WFI — is **wrong**. It was built on banked-register reads
(GICH/HCR_EL2 read over the debug channel describe **CPU1's bank**, not the
guest core's).

Disproof: added a mask-rescue watchdog (`gic_timer.c:vtimer_mask_watchdog()`,
`FLTR_K_VTRESCUE` in `flightrec.h`) that would fire if the guest's ISR ever
failed to clear `CNTV_CTL.IMASK` after EL2 sets it. Live result: TIMER PPIs
arrive ~1100/s, **zero VTRESCUE records** — the guest's own ISR clears the
mask every single time. The vtimer handshake works; it was never lost.

The watchdog itself is kept in the tree as harmless defence-in-depth. Memory
file `vtimer-masked-vgic-off-deadlock.md` has been corrected in place (not
deleted, so the retraction and the reasoning both stay visible). Task #15
("gate CNTV mask+reinject on GICH_HCR.En") is now moot — needs re-scoping or
closing.

## 2. What the guest was ACTUALLY doing: `mountroot>` fast-retry loop, not a hang

Bulk-dumped the vconsole capture ring (`0x50000f10`, 64 KiB) instead of the
slow `hv.dump()`. The "frozen" guest was cycling through `---<<BOOT>>---` /
`mountroot>` / `Invalid file system specification` repeatedly — a fast
software retry loop, not a wedge. All previously-"frozen" vblk counters
(`g_reads`, `g_irqs`, etc.) were frozen because there was nothing to do at an
idle prompt, not because anything was stuck.

**Discovery: this build's FreeBSD tree is already instrumented for exactly
this race.** `grep -rn BZDOS-RACE /opt/bzdos/freebsd-src-earlyboot-wt` finds
debug `printf`s already inserted at:
- `sys/kern/vfs_mountroot.c:988` — `vfs_mountroot_wait enter`
- `sys/kern/vfs_mountroot.c:1026` — `vfs_mountroot_wait_if_neccessary fs=%s dev=%s`
- `sys/geom/part/g_part.c:1977` — `g_part_taste mp=%s pp=%s`
- `sys/geom/geom_subr.c:579` — `g_new_provider_event pp=%s`

This is a prior (undocumented-in-memory) attempt at instrumenting the exact
GEOM-taste-vs-mountroot race described in `mountroot-geom-stall-investigation.md`
and `rootmount-gpt-healthy-blocker-guestside.md`. Someone built retry logic
into `vfs_mountroot_wait_if_neccessary` for this.

**Confirmed live: a single bare CR typed at `mountroot>` was enough to get
the mount to succeed and the guest to proceed past it**, all the way into
single-user init. This matches `reliable_load.py`'s existing
`auto_mount_root()` docstring almost exactly ("typing ANYTHING at the
interactive mountroot> prompt... makes the mount succeed right after") —
this was already known and automated; this session just directly observed
it happening rather than relying on the automation.

## 3. Self-inflicted regression, fixed: single-user init loop

While probing, a `?` sent right after the CR landed on init's **next**
prompt — "Enter full pathname of shell or RETURN for /bin/sh:" — instead of
mountroot, because the mount had already succeeded and boot had moved on.
Result: `init: can't exec ? for single user: No such file or directory` →
"single user shell terminated, restarting" → infinite loop reprinting the
same prompt.

Fixed by sending one more bare CR (defaults to `/bin/sh`). Lesson for next
time: after nudging `mountroot>`, wait for the NEXT prompt to actually appear
before sending the next canned keystroke — don't fire a fixed sequence
blind, the boot moves faster than expected once unstuck.

## 4. NEW real bug found: single-user shell dies on a genuine disk read error

After the single-user loop was fixed, a new, different, reproducible failure
appeared — `/bin/sh` itself faults:

```
g_vfs_done():vtbd0p3[RAD(offset=801767424, length=163840)]error = 5
... shell terminated, restarting  (signal 10 — SIGBUS, no core dump)
```

This is a **real EIO from GEOM**, not console noise, and it repeats
identically every retry (same offset, same length).

### Ruled out (both checked read-only, live):

- **Partition too small.** Read the real GPT off the eMMC directly
  (`emmc_bio_read`, bypassing virtio entirely): `p3` ("rootfs") spans LBA
  278562–2858721, i.e. 2,580,160 sectors / 1321 MB. The failing
  partition-relative sector (`801767424 / 512 = 1565952`) is well inside
  that range. Not an out-of-bounds read.
- **Bad physical media at that LBA.** Absolute LBA = `278562 + 1565952 =
  1844514`. Directly read the first, middle, and last sector of the failing
  320-sector (163840-byte) span via `emmc_bio_read` — completely bypassing
  vblk/virtio. All three came back clean, stable, and plausible (looks like
  termcap/terminfo text: `vt100-nav`, `term 1.0 UCB ascii...`). The eMMC
  itself is healthy at this location.

### What this points to

The bug is somewhere in the **virtio-blk front end or the guest driver's
interaction with it**, not the disk. Per `vblk_emmc.c`'s own comments, this
guest negotiates no `VIRTIO_BLK_F_SEG_MAX`, so FreeBSD's `vtblk` caps every
descriptor chain to a single page-aligned data segment (`n==3` always:
header + one ≤4096-byte data desc + status; `VBLK_MAX_CHAIN=32`,
`VBLK_SECTOR_BYTES=512`). A 163840-byte GEOM-level read is therefore NOT one
virtio request — it's ~40 separate 8-sector (4096-byte) requests under the
hood, and only checking 3 of the underlying ~320 sectors does not rule out
one specific sub-request among those ~40 silently failing to complete.

**This looks like the same bug class as `vblk-lost-kick-mountroot-blocker.md`
(one specific guest read never reaching `vblk_kick()`), now shown to recur
at a DIFFERENT LBA** (previously LBA 278882, seen during early GEOM taste;
now deep inside `p3`'s data area, hit while paging in `/bin/sh` at
single-user shell exec). That memory file scoped the bug to one fixed
address — this session's finding contradicts that scoping: **it is a
general race/bug in the vblk request-completion path, not a one-off tied to
a specific hardcoded LBA.**

### Not yet done

A full 320-sector re-read via `emmc_raw.py` (to find exactly which
sub-request-sized chunk fails) was started but the run timed out (stdout
buffering hid all output until the 180s timeout killed it — `python3 -u`
needed, and 320 sequential `hv.call()`s at ~0.4s retry granularity is simply
slow). Not re-run yet this session. This is the concrete next step: rerun
unbuffered, sector-by-sector (or in the ~8-sector sub-request groups vtblk
actually issues) across the whole 320-sector span to localize the exact
failing sub-request, then correlate against `vblk_emmc.c` breadcrumbs
(`g_kicks_seen`, `g_heads_popped`, `g_dabt_seen`) captured at the same
moment to see whether that specific request even reached `vblk_kick()`.

## Tooling note

`hvsh.py -f <path>` requires a real file path — `-f /dev/stdin` fed via a
bash heredoc does not work (the client does a plain `open()`, not a stdin
read). Write the script to a temp file first.

## 5. LATER THE SAME DAY: the HV is exonerated, and the probe was the problem

Three commits landed after the sections above were written, and they change
the conclusion of §4 substantially.

### 5a. Sticky S_IOERR forensics (commit `18a1e50`) — decisive negative

§4 ended by planning to catch which sub-request fails. It was built on a
misreading, now corrected: a short used-ring `len` (the 1 and 24577 seen live)
does **not** imply S_IOERR. FreeBSD's `vtblk_request_error()` inspects only the
ack byte and ignores `len` entirely.

`vblk_emmc.c` now records, at the instant either completion path decides on
S_IOERR, per-cause totals plus a snapshot of the first and last failure —
exact failing LBA, rc, bytes served before dying, head, direction, and which
core served it (bc[42..56]; `triage.py` decodes them and now reads all 64
breadcrumb words instead of stopping at 48). Everything else describing a
completion is overwritten by the next request, which is precisely why the
evidence kept vanishing.

Live result on a fresh boot: **`g_ioerrs = 0`, 317/317 requests completed
S_OK.** The HV's virtio-blk/eMMC path is not failing I/O and not losing the
eMMC lock. Combined with 5b below, treat "the HV corrupts or fails guest
reads" as refuted unless new evidence appears.

### 5b. `emmc_raw.py` never held the eMMC lock (commit `842a502`) — ROOT CAUSE of the "bad media" ghost

`vblk_emmc.h`'s `VBLK_EMMC_LOCK_PA` comment always stated the contract:
virtio-blk takes the controller lock internally, but the CPU1 debug core
"cannot be hooked at compile time (it enters emmc_bio via a runtime `call`)",
so a CPU1 caller must bracket its access with
`vblk_emmc_trylock`/`vblk_emmc_unlock` or the two collide and "corrupt the
in-flight transfer". `emmc_raw.py` never did.

Measured with the guest reading the disk in a loop — twelve reads of ONE
unchanging sector (LBA 287342):

| | result |
|---|---|
| before | 9 OK but **two different contents** (aarch64 code bytes 9×; a UFS inode 2× — `0x41ed` = mode 040755, a different sector entirely) plus 3 hard `rc=-1`. Neighbours 287340/287341 failed outright. |
| after | 12/12 OK, one identical content, both neighbours fine. |

A failing sector never returns two different plausible contents. Two
consequences, the second worse than the first:

1. Every `emmc_raw` measurement taken while the guest was doing virtio-blk I/O
   is suspect — **including §4's "bad physical media ruled out" claim**. That
   conclusion happens to be safe (it was taken with the guest idle at a
   prompt, doing no I/O, and returned stable plausible data) but it was not
   sound at the time it was made.
2. The probing itself was corrupting the guest's transfers, i.e. manufacturing
   the exact class of symptom (EIO / SIGBUS `pager read error` / SIGSEGV on a
   paged-in binary) it was being used to investigate.

### 5c. `check_build_id()` compared against git, not the ELF (commit `4a4ab42`)

It recomputed the Makefile's BUILD_ID from git state, so any commit that did
not rebuild reported a bogus skew — including a host-side-only Python commit,
which cannot change the firmware. Committing 5b made `emmc_raw.py` refuse to
run against an untouched board. It now reads `g_build_id_str` out of the ELF
whose symbols it is resolving, with git as fallback for a checkout with no
build in it.

### 5d. Current guest state

Boots, mounts root, reaches `start_init: trying /sbin/init`, then init's child
SIGSEGVs every 30s in a loop. In the local image `/bin/sh` is byte-exact valid
(11 program headers, section table ending exactly at EOF = 167384), as are
`ld-elf.so.1`, `libc.so.7` and `/sbin/init`; `/bin/init` does not exist at all
(the console's "bin/init" is `/sbin/init` with dropped bytes). `/bin/sh`,
`/lib/libedit.so.8` and `/etc/rc` carry the documented uid=1001 defect. A
byte-for-byte board-vs-image comparison of `/bin/sh` is the open item — the
board's p3 was hot-patched live earlier by `fix_ownership.py`/`ufs_clean.py`,
which still carry a stale `SCRATCH_PA` (0x50030000).

### 5e. Snapshots: written, linked, unreachable

`snapshot.c` + `snapshot_net.c` (37 KB) are compiled and linked into the
running firmware but have **zero callers** — `snapshot_save`/`restore` are
reached only from `snapshot_net_send`/`recv`, which nothing calls. `snapshot.h`
names the intended wiring ("a new `snap`/`rest` command" off the dbgmon tick
path); it was never written. So there is no live checkpoint/rollback today,
which is what makes every experiment cost a full reload.

Note for whoever wires it: `snapshot.h`'s own "NOT CAPTURED" list excludes MMIO
device state (**including MMC/eMMC**), GIC state and in-flight DMA — so a
snapshot is the *wrong* tool for block-I/O bugs specifically, however useful it
would be for skipping boot.

Hot-patching, by contrast, already works: `hv.patch(pa, insn)` (write +
I-cache flush), `hv.call(fn_pa, ...)` and `hv.write_word_verified()`. Single
instructions and flags can be changed with no rebuild and no reload; reach for
that before a reload cycle.

## 6. ROOT CAUSE FOUND AND FIXED: the sync fallback raced CPU2 for the eMMC lock

`vblk_request()`'s synchronous fallback went straight at the controller. But the
commonest reason that path runs is **"async mailbox busy"** — i.e. CPU2 is
mid-transfer and holding the eMMC lock. `emmc_lock_acquire_bounded()` then timed
out and the request completed **S_IOERR**: a hard I/O error handed to the guest
for a healthy disk. The path was structurally guaranteed to lose, because the
condition that selects it is the condition that makes the lock unavailable.

The sticky counters from §5a named it on the first try:

```
g_ioerrs=2, g_ioerr_busy=2 (rc=-200 = VBLK_RC_BUSY)
FIRST: LBA 513458, served 8192 bytes before failing, head=10, READ, CPU0 (sync)
LAST:  LBA 279771, served 0 bytes,                   head=3,  READ, CPU0 (sync)
```

and the guest had printed `hard error cmd=read 513442-513505` /
`279770-279777` — both HV-recorded LBAs fall **inside** the guest-reported
ranges. This is what §5a's instrumentation existed for, and it is why the bug
survived earlier passes: every other completion breadcrumb is overwritten by the
next request, so one later success erased the evidence.

Fix (`3b56041`): drain the async mailbox via the already-present
`vblk_async_drain_bounded()` before the sync loop.

### Hardware A/B, same board and image

| | before | after |
|---|---|---|
| `g_reads` | 1019 | 3585 |
| `g_async_fallbacks` | 8 | **88** |
| `g_ioerrs` / `g_ioerr_busy` | **2 / 2** | **0 / 0** |

The racy path runs 11× more often and fails zero times.

### What it unblocked

Furthest boot ever reached on this board: past `mountroot`, a **complete fsck**
(SALVAGED — blocks missing in bitmaps, free-block count wrong, summary info bad;
18389 files, 0.0% fragmentation), `lo0` up, local filesystems mounted, `ldconfig`
run, into `/etc/rc`.

### New blocker (task #17)

Stops dead right after `/etc/rc: WARNING: $hostname is not set`, console ring
frozen at 31149 bytes. CPU0's flightrec is **frozen** (no EL2 entries at all)
while CPU1/CPU2 stay healthy — so the guest is idle in WFI or wedged at EL1, not
looping. Last FLTR events: repeated guest **stage-2 translation faults**
(`DFSC=0x06`, level 2) on 4-byte accesses at guest PCs `0xffff00000090c8fc`
(READ) and `0x90ca5c` (WRITE), interleaved with virtio-blk IRQ 137.

TIMER events also stop, and `triage.py` helpfully points at the vtimer theory —
**ignore that**, it is the twice-reverted trap (`vtimer-masked-vgic-off-deadlock`).
The aborts are the lead. The missing datum is the **IPA**: FLTR records ESR and
ELR but not FAR/HPFAR, so the faulting address is unknown. Surfacing it is the
next concrete step; `kernel.debug` DWARF already exists to symbolize those PCs.

Cosmetic, not blockers: `swapon: /dev/mmcsd0p4: No such file or directory` (the
image's fstab uses the bare-metal `mmcsd0pN` names while the guest sees `vtbd0`)
and the `$hostname` warning.

## 7. MILESTONE: the guest boots to an interactive root shell

```
FreeBSD/arm64 (Amnesiac) (ttyu0)

login: root
...
root@:~ # echo BZDOS-SHELL-OK; uname -m; id
BZDOS-SHELL-OK
arm64
uid=0(root) gid=0(wheel) groups=0(wheel),5(operator)
```

### The last blocker: a self-latching CNTV mask

With `VGIC_CNTV_HW=0`, `gic_timer.c` masks the guest's CNTV *before* injecting
the virtual tick. That mask is **self-latching**: masked means no further CNTV
PPI reaches EL2, which means no further chance to inject, so recovery depends
entirely on the guest's ISR clearing IMASK. `vgic_inject_cntv()` returns 0 when
it *gates* a tick (a live vINTID 27 still un-EOIed) or *drops* it (all List
Registers busy) — and either one then kills the timebase permanently.

Measured on `3b56041`: 8047 ticks forwarded correctly, then one lost during the
heavy `ldconfig` burst (peak LR pressure) → `gt_cntv_el2_masked=1` forever and
the guest idled in WFI with a completely healthy kernel. Proven healthy by
injecting INTID 137 (`GICD_ISPENDR` 0x01C81210 bit 9): the guest woke, ran a
textbook vtblk ISR, and went straight back to sleep — 40 injections, exactly 3.0
events each, zero console progress. So it was waiting on *time*, nothing else.

`vtimer_mask_watchdog()` already implemented the recovery, but it is called only
from a CNTP tick handler `main_dbg.c` never armed. `FLTR_K_VTRESCUE = 0` meant
**unreachable**, not "never needed".

### Fix (`dfc6eda`) — two parts, and the second is the non-obvious one

1. `main_dbg.c` arms the tick via a new `gic_timer_arm_preserving_cntvoff(10ms)`.
   It save/restores `CNTVOFF_EL2` around the existing `gic_timer_init()`, which
   zeroes it as an old pre-vGIC `DELAY()` workaround — the guest ticks fine on
   ATF's value, so changing that as well would alter a working virtual timebase
   and confound a single-boot verification.
2. `gic_timer_irq()`: `if (vgic_active() && intid != TIMER_INTID)`. **Arming the
   tick alone does nothing** — with vgic active, INTID 30 was swallowed by the
   generic "Device SPI" arm into `vgic_inject_hw()`, so the tick handler, and
   `vtimer_mask_watchdog()` with it, never ran. Safe: `irq_counter[30]` measured
   0 across a full boot, i.e. this guest uses CNTV and never programs CNTP.

Verified on hardware in **one** reload, first attempt: `gt_ticks` 1646→3219
(~105/s, matches the period), `irq30` climbing in lockstep (handled locally),
`gt_cntv_el2_masked` oscillating 0/1 (mask set *and* cleared), console advancing
past the old stall point, boot completing through entropy/network to `login:`.

### Also worth keeping: live recovery with no reboot

The same wedge was first cleared **without any reload**. Both vgic calls in
`gic_timer_irq` are tail calls, and `vgic_inject_cntv()` is global and
argument-less, so redirecting one instruction is enough:

```
hv.patch(0x4200307c, 0x140030f3)   # b vgic_inject_hw -> b vgic_inject_cntv
<inject INTID 137 so CPU0 runs it on its OWN GICH bank>
hv.patch(0x4200307c, 0x14003242)   # restore immediately
```

Encoding: `0x14000000 | ((target-pc)/4)`; validate the formula against the
existing branch at `0x420030ec`, which must reproduce `0x140030d7`. Calling
`vgic_inject_cntv()` via `hv.call` would be useless — that runs on CPU1 and
writes the wrong (banked) GICH.

### Cosmetic leftovers, not blockers

`/etc/rc.conf` is **missing** on the board's p3 although present in the
reference image (hence `$hostname is not set` and the "Amnesiac" hostname), and
`swapon: /dev/mmcsd0p4: No such file or directory` because the image's fstab
uses bare-metal `mmcsd0pN` names while the guest sees `vtbd0`. Both belong to
task #14. A stuck eMMC controller lock (`0x50020100` = 1) was also found and
cleared — left held it would fail every guest read with S_IOERR, i.e. reproduce
§6's bug.

## Carry-forward for next session

The EIO saga (§6) and the timebase wedge (§7) are both **closed**, and the guest
now reaches an interactive root shell. What remains:

1. **Task #14 — rebuild a clean rootfs.** Now the top item, and the evidence for
   it is concrete: `/etc/rc.conf` is missing on the board, `/bin/sh`
   `/lib/libedit.so.8` `/etc/rc` carry the uid=1001 defect, fstab uses
   bare-metal `mmcsd0pN` names, and this boot's `fsck` had to SALVAGE three
   different classes of metadata damage.
2. Consider whether `VGIC_CNTV_HW=1` is the better long-term answer than §7's
   watchdog-based recovery — it removes the software mask, and with it the
   failure mode, entirely. Also still uninvestigated: whether `vgic.c`'s
   pending-injection queue held the lost tick.
3. `/bin/sh` and `/libexec/ld-elf.so.1` on the board were **verified
   byte-identical** to the local image (sha256 over the full files), so the
   medium is not the problem and that question is settled.
4. Task #14 detail — rebuild a clean rootfs on the dev VM
   (`/root/.claude/plans/jolly-honking-snowflake.md`). The uid=1001 defect is
   confirmed present on the very files in the failing path, the pipeline fix
   already exists (`bsdos-build.sh`'s `sudo tar`, 2026-07-22) and was simply
   never re-applied to a fresh image. `WOW_FEATURES.md` §0 rule 7 forbids
   continuing to hot-patch the live image instead.
3. Do NOT add more HV-side virtio-blk instrumentation. That avenue produced its
   answer (`g_ioerrs=0`, 317/317 S_OK) and is now exhausted twice over.
4. Audit any other host tool that reaches `emmc_bio` via `hv.call` for the same
   missing-lock bug as §5b — `ufs_clean.py` / `fix_ownership.py` /
   `fix_pam_ownership.py` are the obvious candidates, and they additionally
   still carry the stale `SCRATCH_PA = 0x50030000`.
5. Optional but high-leverage: wire `snap`/`rest` (§5e) to stop paying a full
   reload per experiment — while remembering it cannot model block-I/O state.
