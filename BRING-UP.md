# Bring-up guide: status and reproducibility assessment

ROADMAP.md's v1 gate (§2) requires a bring-up guide "reproducible by a third
party on a clean board." This file is an honest assessment of where that
stands today, written by walking the actual tooling in this tree (not by
guessing at hardware steps this repo doesn't encode). It supersedes nothing —
`README.md` remains the practical quick-start; this file is the gap analysis
plus the consolidated start-to-finish path for the parts that ARE covered.

**Bottom line: not yet reproducible by an independent third party starting
from a truly blank board, using only what's in this repo.** The steps from
"board has never had anything on it" to "U-Boot prompt reachable" are not
part of this tree, and two required boot artifacts (the FreeBSD kernel ELF
and the board DTB) live outside this repo and aren't built or fetched by
anything here. See "What's missing" below for the precise list.

## What IS covered end-to-end (confirmed from the tooling)

1. **Toolchain** — see `TOOLCHAIN.md`: exact verified `aarch64-linux-gnu-gcc`
   / binutils versions.
2. **Build** — `make dbg` (see `README.md` "Building"; `Makefile` has the
   authoritative flag/target list). Produces `microkernel-dbg.elf/.bin`.
3. **Loading onto a board that already has U-Boot on it** —
   `python3 reliable_load.py --expect-vbk --cycles 3` (one-shot, verified
   load) or `python3 -u chimpd.py` (persistent supervisor; see
   `SESSION-RULES.md` R3 for the operational rule: always run chimpd without
   `--once`, never hand-time `wdt_reset`). Both are deterministic state
   machines, not "usually works" scripts — see their module docstrings
   (`reliable_load.py` lines 1-20, `chimpd.py` lines 1-38) for the exact
   catch → TFTP → `loady`/`bootelf` → verify sequence and recovery paths.
4. **Establishing the EMAC debug link** — automatic once the image is
   running: `hvdbg.py`'s `HV()` opens an `AF_PACKET` raw socket on ethertype
   `0x88B5` on interface `br0`. No separate "establish the link" step exists
   beyond having the host's `br0` bridge configured and the board on the
   same L2 segment (see `docs/security-notes.md` for what this implies).
5. **Getting FreeBSD to boot from the loaded image** — automatic: the `dbg`
   build's `main_dbg.c` loads the FreeBSD kernel directly (no U-Boot stage
   for the guest) and enters it; `chimpd.py`'s monitor loop watches for the
   `login:` console marker as the success signal.
6. **Recovering a wedged board without physical access** — `supervise.py`
   (auto-reload on power-up / break-glass over the USB-ACM console) and the
   watchdog-based anti-brick design documented in `SESSION-RULES.md` R2
   ("anti-brick SPL@8KiB always yields U-Boot").

## What's missing or unverified (do not guess past this point)

These are gaps confirmed by grepping the tree for the relevant step and
finding nothing — not steps this report fills in with assumed hardware
knowledge:

1. **Getting U-Boot onto a genuinely blank board in the first place.**
   Every loader in this repo (`reliable_load.py`, `chimpd.py`,
   `loady_over_acm.py`) *assumes U-Boot is already on the board* and catches
   its `=>` prompt over USB — none of them flash SPL/U-Boot via FEL mode.
   `PROJECT.md` line 12 explicitly notes the board "Грузит и исполняет
   U-Boot (**не FEL**)" — i.e. this project deliberately started from a
   board that already had a working U-Boot (and SPL, per `SESSION-RULES.md`
   R2's "anti-brick SPL@8KiB") on it. How that U-Boot/SPL got there
   originally (stock Banana Pi M64 image? `sunxi-fel` flashing? factory
   eMMC image?) is not recorded anywhere in this repo. A third party
   starting from an unflashed board needs to source and flash a working
   U-Boot themselves — this repo neither documents nor automates that step.
2. **The FreeBSD guest kernel + DTB artifacts are not in this repo.**
   `chimpd.py`'s own header lists them as prerequisites "already in the
   tree" — but they are not: `KERNEL = "/opt/bzdos/tftpboot/kernel"` and
   `DTB = "/opt/bzdos/tftpboot/bananapi-min.dtb"` (see `chimpd.py` lines
   47-48) point at a **sibling directory outside this repository**. Per
   `docs/virtio-blk-dtb.md` (lines 12-17), the checked-in `.dts` for that
   DTB is even stale relative to the `.dtb` actually in use, and nothing in
   this tree recompiles it. A third party cloning only this repo has
   neither the FreeBSD kernel ELF nor a working DTB, nor a documented
   recipe to (re)build either from scratch.
3. **Host-side network prerequisites are assumed, not set up by anything
   here.** `chimpd.py` hardcodes `IFACE = "br0"`, a TFTP server on the host
   (auto-started via dnsmasq per its own header), and fixed board/host IPs
   (`BOARD_IP`/`SRV_IP`, `chimpd.py` lines 52-53). Setting up that bridge
   interface and TFTP service on a fresh host is not covered by any script
   in this tree.
4. **The USB port path is host-machine-specific.** `reliable_load.py` hard-
   codes `USB_NODE = "/sys/bus/usb/devices/1-4"` (line 26) as "board's OTG
   port on this host" — a third party on different hardware/wiring must
   find and substitute their own sysfs path; there's no autodetection.
5. **Pine64+ port (ROADMAP T5 mentions it as a stretch bring-up target).**
   Not attempted anywhere in this tree; all addresses/pinmux are A64/BPi-M64
   specific (see `README.md`'s own "Known gaps" section and `PROJECT.md`).

## Recommendation

Items 1-4 above are exactly what would need to be written up (and, for #1,
actually exercised on a spare/blank board) before the v1 gate's "reproducible
by a third party on a clean board" line can be honestly checked off. None of
them are something this documentation pass should guess at — they involve
either hardware procedures not exercised in this session (FEL flashing) or
artifacts/infrastructure that live deliberately outside this repo
(`/opt/bzdos/tftpboot`, `/opt/bzdos/build/u-boot`, host network config) and
whose inclusion is a scoping decision for the project owner, not something
to fold in unilaterally here.
