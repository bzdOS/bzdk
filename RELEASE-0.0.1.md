# bzdk 0.0.1-prealpha

**A bare-metal EL2 hypervisor for the Banana Pi M64 (Allwinner A64, 4× Cortex-A53,
1 GiB DRAM), running FreeBSD 15.1 arm64 as its guest.** Written from scratch: no
KVM, no Xen, no Jailhouse.

This is an **intermediate snapshot, tagged so the work can be put on hold at a
known-good point.** Pre-alpha means what it says: it runs, it is measured, and it
is one board. Nothing here has been run on a second unit, packaged, or installed
by anyone who did not write it.

Tag: `v0.0.1-prealpha`. Default build: `make dbg`.

## What actually runs, with the numbers

Every figure below was taken on the board, not estimated.

| | |
|---|---|
| Guest | FreeBSD 15.1-RC3 arm64 boots unattended to userland with networking, root on `/dev/vtbd0p3` (6.0 GB), no console interaction required |
| Boot | ~17 s from reset to hypervisor liveness; no USB cable needed — the board TFTPs and boots on its own |
| Storage | virtio-blk over a from-scratch eMMC driver. 512 MiB of raw reads with zero I/O errors; 65 MiB write/read-back byte-identical under deliberate fault injection |
| Network | virtio-net over a from-scratch EMAC driver; guest reachable over ssh |
| Display | 1920×1080 HDMI brought up from cold (CCU→DE2→TCON1→DW-HDMI→PHY), with a live hypervisor HUD |
| GPU | Mali-400 renders under the guest through an out-of-tree lima port + Mesa 26.2: textures, depth, blending, 2420 draw calls/frame |
| Presentation | zero-copy — the guest renders into a `gbm_surface` and the hypervisor repoints the DE2 layer at it. 1030 fps, 0 rejected |
| KMS | a real DRM/KMS device in the guest whose page flip *is* the hypervisor doorbell: 3601 flips in 60 s at 60.0 fps, 0 refused, each waiting for FLIP_COMPLETE |
| vblank | the real panel vblank, observed by EL2 from TCON's latching status bit: 60.04 Hz |
| SMP guest | **opt-in**: `make dbg VCPU2=1` gives CPU2 to the guest as a second vCPU. Verified — the guest enumerates `CPU 1 ... affinity: 2`, sets up IPIs, completes `Release APs` |
| Second guest | **opt-in**: `make dual` runs Zephyr on CPU3 concurrently with FreeBSD, under its own disjoint stage-2 tables |
| Isolation | stage-2 partitioning with a hardware self-check (`AT S12E1W`) and dynamic W^X on guest DRAM (27 of 64 L3 tables used, 0 exhausted) |
| Recovery | a debug plane on CPU1 that outlives the guest: it owns the watchdog, so a total EL2 wedge reboots to U-Boot and reloads automatically, with no human and no power cycle |

Core allocation in the default build: **CPU0** guest, **CPU1** debug/watchdog
plane, **CPU2** async eMMC I/O, **CPU3** idle.

## What this is for

It is a debugging substrate as much as a hypervisor. The guest runs under an
observer that survives it: breadcrumb windows readable post-mortem, a captured
guest console, an event ring and a PC-sample profiler, a GDB stub, snapshot and
restore, coredumps over the network, and a framebuffer that can be screenshotted
from outside the guest while the guest is wedged. That is why the bug list below
is specific rather than vague — the instrumentation is the product.

## Known broken or unproven

Named because a release that lists none of this is not honest.

- **The RSP channel goes dark in long GDB sessions.** Unfixed. Breakpoints and
  register reads work; a long-running session eventually stops responding and
  only a board reset recovers it.
- **No microSD support in practice.** The controller path exists and programs its
  own pinmux and clock, but no card has ever been present — `sd init` reports
  "no card". The second virtio-blk device that would back is not built.
- **The guest's root filesystem is writable.** Making it read-only is the single
  change that would retire a whole class of corruption; not done.
- **`hvfb` and the scanout-import route are dead ends** kept for the record. The
  import path they describe was measured to write an imported dma-buf exactly
  once, at ~400 fps of nothing changing.
- **`drm_gem_shmem_purge()` has never executed** in the lima port. Nothing calls
  it; it becomes reachable the moment a shrinker is added.
- **The "100 clean boots in a row" gate figure is pre-fix.** It was closed on
  2026-08-04 by supervisors we now know wrote false failures. The owner's
  decision is not to re-run it, with the reasoning recorded in `ROADMAP.md`: over
  the last 14 days it is 59 boots, 56 clean, and of the three failures two are
  instrument error and one a loader retry — 1 board-attributable failure in 59.
- **One board.** No second unit, no second SoC revision, no thermal or
  long-duration soak in the release criteria (the 72-hour soak was deliberately
  removed from the gate; the harness remains as a tool).

## Ten upstream fixes, none submitted

Bringing this up required fixing ten defects in **other people's trees** —
FreeBSD base, drm-kmod, and the ports tree. Every one has a patch and a
write-up; none has been submitted, which needs accounts rather than engineering.
`../bsdOS/hal/lima/patches/UPSTREAM-INDEX.md` lists them with apply order and
destinations.

One is not a porting fix and matters to people who have never heard of this
project: drm-kmod puts the shared `hw.dri` sysctl node in a per-device context,
so an **unprivileged `sysctl -a` panics the kernel** on any FreeBSD machine
running a DRM driver where debugfs is unavailable.

## Related projects

- **bzdOS** — the operating system: <https://github.com/bzdOS/bsdos>. A separate
  project; this hypervisor is not it.
- **lima-freebsd** — the Mali-400 DRM driver extracted for anyone with Utgard
  silicon: <https://github.com/bzdOS/lima-freebsd>. Public, with the ten patches.
- **hubd** — <https://github.com/bzdOS/hubd> — the tracker this was run through.
  It mattered here for a specific reason: the build machine and the board were
  never the same machine, and several agents worked the same board in parallel.

## Where to start reading

`CLAUDE.md`, then `SESSION-RULES.md` (the operating rules, R0–R6 — mandatory
before touching the board), then `ROADMAP.md`. Run `python3 orient.py` first in
any session: it reports build identity, whether each core is actually moving, and
guest state, from the live board.
