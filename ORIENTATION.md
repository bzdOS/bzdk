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

## Read next, in this order

| Doc | What it is |
|---|---|
| `SESSION-RULES.md` | **Operating rules (R0–R7). Mandatory.** How to fix live over the network, how to reload, why the watchdog resetting the board is *by design*. |
| `DEBUG_RULES.md` | Rules for live-hardware debugging specifically. |
| `PROGRESS.md` / `ROADMAP.md` | Where the project is and what v1 requires. |
| `WOW_FEATURES.md` §0 | Hard prohibitions — read rule 7 before "just patching" anything on disk. |

## The four rules that bite hardest

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

## Core layout

- **CPU0** — runs the guest, takes its traps.
- **CPU1** — EMAC/debug core; survives guest wedges, hosts `dbgmon` and the
  GDB stub, owns the hardware watchdog.
- **CPU2** — async eMMC I/O offload.
- **CPU3** — idle (WFI).

## Talking to the guest

```sh
python3 guest_sh.py 'uname -a'          # console, works even with no network
ssh -i /root/.ssh/chimp_ed25519 root@192.168.88.82   # once /etc/rc has run
```

The guest is **also a build host**: it has `/usr/bin/cc` and ~4.7 GB free, so a
misbehaving base tool is usually faster to rebuild there than to debug. To copy
files *in*, the **guest listens and the host connects** (`nc -l` on the guest) —
inbound TCP to this host is firewalled, so fetching from a host HTTP server
fails with "Connection refused". Helper: `push.py` pattern in the scratchpad.

Never change the guest's MAC: it must stay `02:bd:05:00:00:01` or networking
dies instantly (the guest relies on `VIRTIO_NET_F_MAC`).

## Commit convention

Author **and** committer `Andrey Bodrov <ap.bodrov@gmail.com>`. No attribution trailers.
Never `git add -A` / `git add .` in this tree — commit named files.
