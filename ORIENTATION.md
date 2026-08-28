# bzdOS Chimp — read this before touching anything

Bare-metal **EL2 hypervisor** on a Banana Pi M64 (Allwinner A64, 4× Cortex-A53)
running a **FreeBSD 15.1 arm64 guest** at EL1. This file is the entry point; it
is deliberately short and points at the real docs.

## First command of every session

```sh
cd /opt/bzdos/microkernel && python3 orient.py
```

It reports, from the live board: build identity (on-disk vs running), whether
each core is actually moving, guest disk/mount/network state, and whether the
serial port is already busy. **Do not skip it** — several long debugging
sessions in this project were spent chasing a "wedged board" that was a stale
image, a busy tty, or a stuck line editor.

## One tool for the board lifecycle

```sh
python3 bzdctl.py status          # state in one screen (EMAC only, safe anytime)
python3 bzdctl.py serve           # read-only web dashboard on :8088
python3 bzdctl.py power reset|hold|release|uboot
python3 bzdctl.py console [--follow] [--inject TEXT]
python3 bzdctl.py boot-watch      # wait for liveness, record in the ledger
python3 bzdctl.py crash           # bundle status + triage (+ optional coredump)
python3 bzdctl.py ledger          # the v1 "100 clean boots" gate counter
```

`status` and `serve` never open the tty, so they are safe while a reload runs.
`power uboot` is the one subcommand that needs it.

## Read next, in this order

| Doc | What it is |
|---|---|
| `SESSION-RULES.md` | **Operating rules (R0–R7). Mandatory.** How to fix live over the network, how to reload, why the watchdog resetting the board is *by design*. |
| `DEBUG_RULES.md` | Rules for live-hardware debugging specifically. |
| `PROGRESS.md` / `ROADMAP.md` | Where the project is and what v1 requires. |
| `WOW_FEATURES.md` §0 | Hard prohibitions — read rule 7 before "just patching" anything on disk. |

## The five rules that bite hardest

1. **Never ask the user to press reset or re-plug power** (R0). Fix it live over
   EMAC, or let the agent do a network reload itself. Silently.
2. **`triage.py` first, never `gr`** — `gr` shows a *stale saved frame*, not the
   live PC. "Moving vs frozen" needs two samples.
3. **One process on `/dev/ttyACM0` at a time.** Two readers steal each other's
   bytes and a healthy channel looks dead. `fuser -v /dev/ttyACM0` before
   blaming the board.
4. **Per-PE registers are banked.** `HCR_EL2`, `MDSCR_EL1`, `DBGB*`, GIC PPI
   state — a read over the debug channel is serviced by **CPU1** and tells you
   nothing about the guest's core. This has produced at least four confidently
   wrong diagnoses in this project.
5. **A guest-written DRAM word read from another core is not a liveness
   probe on real hardware.** A guest running with its stage-1 MMU off makes
   its own stores Device-typed; a Device write does not invalidate another
   core's cached copy, so the reader can return a stale value forever. QEMU
   models no caches and will not catch this — it made exactly this probe lie
   about a working retry path on 2026-08-10 (see `docs/dual-guest.md`'s
   RETRACTED section). Trust progress observed *through a stage-2 fault*
   (console bytes, an EL2 breadcrumb with explicit `dc civac`) instead.

## Core layout

> **CHANGED 2026-08-26, again 2026-08-27 (v0.0.2-prealpha).** CPU1 AND CPU2 are GUEST vCPUs in the default
> build now, not the debug plane. The description below is the pre-0.0.2 layout
> and is kept only because most of the tree's comments still assume it. Read
> `RELEASE-0.0.2.md` and `vcpu1.h` before reasoning about core roles.

- **CPU0** — runs the guest, takes its traps.
- **CPU1** — **now the guest's second vCPU** (`vcpu1.c`, armed by default via
  `board-config.xml`): FreeBSD reports `hw.ncpu=2` and gets real parallel
  throughput. It STILL owns the EMAC/dbgmon channel, the GDB stub and the
  hardware-watchdog kick — but from an unmaskable 10 ms EL2 tick instead of a
  tight loop, so a *software* hang of the guest on it cannot starve the
  watchdog, while a *hardware* wedge of that core no longer self-recovers.
  That tradeoff is now live by default; `vcpu1.h` states it in full, and
  flipping one attribute in `board-config.xml` (plus `gen_config.py`) restores
  the old dedicated-debug-core behaviour.
- **CPU2** — **now the guest's third vCPU** (`vcpu2.c`, armed by default via
  `board-config.xml`), hardware-proven 2026-08-27: `hw.ncpu=3`,
  `cpu2:rendezvous` nonzero, three parallel spinners completing identical work.
  It no longer runs `vblk_async.c`'s async eMMC I/O offload — `smp.c`'s `cpu==2`
  dispatch tries `vcpu2_run()` first and falls back to the offload only while
  the feature is off. That trade is deliberate: the offload is a performance
  path, not a correctness one (`vblk_async_post()` gates on
  `g_vblk_async_ready`, and the `gdb` target has always run the synchronous
  fallback on hardware).
- **CPU3** — **FOURTH guest vCPU since 2026-08-27** (`vcpu3` armed in
  `board-config.xml`, hardware-proven: `hw.ncpu=4`, `cpu3:rendezvous`
  nonzero, guest network end-to-end green). The root cause of the original
  4-vCPU freeze (CPU3 data aborts routed into `dual`'s Zephyr handler set)
  is fixed in `el2_exc.c` (gate on `!dbg_vcpu3`). In the `dual` build CPU3
  instead runs a genuine second guest
  (Zephyr) concurrently with FreeBSD on CPU0, under its own disjoint stage-2
  table — see `docs/dual-guest.md`. `vcpu3` and that Zephyr guest are mutually
  exclusive at LINK TIME (a duplicate `bzdos_cpu3_owner` symbol), not merely by
  convention.

## Talking to the guest

```sh
python3 guest_sh.py 'uname -a'          # console, works even with no network
ssh -i /root/.ssh/chimp_ed25519 root@192.168.88.82   # once /etc/rc has run
```

The guest is **also a build host** — but note the 2026-08-25 layout: `/` is
mounted read-only *by design* (fstab `ro`), the build area is **`/opt`
(vtbd1p2, ~44 GB free, rw)** with `/var` (8 GB) beside it; `/tmp` is a ~2.4 GB
tmpfs and fits a kernel-source unpack + build. `/usr/src` does NOT exist in
the guest; the FreeBSD trees live on the host (`/opt/bzdos/freebsd-src`,
`/opt/bzdos/freebsd-src-earlyboot-wt` — the latter is the dirty lineage the
running guest kernel was built from). `nc -l` pattern for pushing files in.

Never change the guest's MAC: it must stay `02:bd:05:00:00:01` or networking
dies instantly (the guest relies on `VIRTIO_NET_F_MAC`).

## Commit convention

Author **and** committer `Andrey Bodrov <ap.bodrov@gmail.com>`. No attribution trailers.
Never `git add -A` / `git add .` in this tree — commit named files.
