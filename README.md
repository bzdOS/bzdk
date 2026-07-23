# bzdOS microkernel — a from-scratch ARMv8 EL2 hypervisor for the Banana Pi M64

A bare-metal, hand-written type-1 hypervisor (no reference codebase, no
KVM/Xen ancestry) that boots FreeBSD/arm64 as an EL1 guest on a Banana Pi M64
(Allwinner A64, 4x Cortex-A53). There is no physical UART on this board — the
entire bring-up and live debugging happens over a custom debug protocol on
raw Ethernet frames (EMAC) plus a USB-OTG serial channel, both implemented
from scratch in this tree.

**Current milestone:** the HV boots the FreeBSD kernel, mounts root from an
eMMC-backed virtio-blk block device (read **and** write confirmed on
hardware), and reaches an interactive root shell over the emulated UART0
console — with clean, un-corrupted EL0 (userland) execution.

## What's actually here

- **`main_dbg.c`** — the live debug build's entry point: brings up EMAC,
  USB-ACM console, SMP (a dedicated CPU1 "debug core"), loads the FreeBSD
  kernel directly (no U-Boot bootloader stage for the guest), sets up
  stage-2, and enters the guest.
- **`stage2.c`** — EL2 stage-2 translation setup (guest physical == host
  physical; see `stage2.h` for the explicit isolation caveat).
- **`vblk_emmc.c`** — the guest's disk: a trap-and-emulated virtio-mmio
  block device backed by the HV's own PIO eMMC driver (`emmc_bio.c`).
- **`emac.c` / `dbgmon.c`** — the from-scratch Ethernet debug monitor: read/
  write physical memory, call arbitrary HV functions, inject breadcrumbs,
  all framed as raw Ethernet frames (no IP stack) at ethertype `0x88B5`.
- **`musb.c` / `usbacm.c`** — a USB-OTG CDC-ACM gadget giving a second,
  independent debug console over `/dev/ttyACM0` — critical fallback when
  EMAC is unreachable (see Recovery below).
- **`el2_ncmap.c`** — a designed-but-not-yet-enabled EL2 stage-1 remap that
  makes the HV a non-cacheable observer of guest DRAM (see
  `docs/el2-nc-guest-dram.md`); gated off by `DBG_NCMAP_ENABLE` in
  `main_dbg.c` pending a live hardware test.
- **`docs/`** — design + integration notes for each subsystem, written as
  the features were built. `docs/aw-mmc-dma-coherency.md` in particular is
  the definitive writeup of the DMA-coherency investigation that consumed
  most of one session.

## Building

```sh
make dbg      # -> microkernel-dbg.elf / .bin — the live-debug build used below
```

(`make stage0` / `make net` / `make repl` / `make fbsd` / `make hdmi` build
earlier, narrower milestones from the same tree; `dbg` is the current one.)

## Loading onto the board — do NOT do this by hand

The board has no physical UART and its USB gadget identity is ambiguous
(`/dev/ttyCHIMP` can be either U-Boot's download gadget, VID:PID `1f3a:efe8`,
or the HV's own USB console, `1d6b:0010` — see `reliable_load.py`'s header for
the full story). Use the tooling, not raw `dd`/`tftp`/serial commands:

```sh
python3 reliable_load.py --expect-vbk --cycles 3
```

This is a deterministic state machine (gate on USB VID:PID -> catch U-Boot ->
TFTP + `loady`/`bootelf` -> verify EMAC + a VBK1 breadcrumb), not a "usually
works" retry loop. See its module docstring for the exact recovery paths it
encodes.

For unattended operation (auto-reload on power-up, self-heal a wedged board
without ever needing a physical power-cycle), run:

```sh
python3 supervise.py
```

It polls USB VID:PID + EMAC liveness and: loads the build when it sees
U-Boot; leaves a healthy HV alone; if EMAC goes dark but the HV's own USB
gadget is still up, sends a break-glass reset sequence over `/dev/ttyACM0`
(see `usbacm.c`'s `bg_seq`) to force a watchdog reboot back to U-Boot, then
reloads. The only case it can't recover from is the board falling off the
USB bus entirely (in which case it says so — that needs a physical
power-cycle).

## Debugging a live board

`hvdbg.py` is the host-side Python client for the EMAC debug protocol
(`HV().read_words(pa, n)`, `.write_word(pa, val)`, `.call(fn_pa, ...)`).
Physical addresses of interest (breadcrumb windows, ring buffers) are
documented at the top of each subsystem's `.c` file — grep for
`BC_BASE`/`_BC_BASE` to find them all.

## Known gaps toward a "release"

This is a debug-instrumented milestone build, not a hardened release.
Notably: stage-2 currently maps ALL of guest DRAM (including the HV's own
image and scratch windows) as one flat, unrestricted region — see
`stage2.h`'s own header comment — and `HCR_EL2.IMO=0` gives the guest
unmediated access to the real GICv2. Neither is "isolation" in the
type-1-hypervisor sense yet; both are explicit, documented trade-offs made
to get FreeBSD booting, not oversights. `el2_ncmap.c` is a step toward
fixing the DRAM side of this but is not yet enabled by default.

The EMAC debug protocol described above (ethertype `0x88B5`) has **no
authentication** — see `docs/security-notes.md` for exactly what that means
and the two options ROADMAP.md names as future work (HMAC on the protocol,
or compiling it out for a "prod" build).

See `BRING-UP.md` for a from-scratch-board reproducibility assessment (what's
actually covered by the tooling in this repo vs. what a third party would
still need to source/figure out themselves), and `TOOLCHAIN.md` for the
exact compiler/binutils versions this has been verified to build with.

## License

BSD 2-Clause, see `LICENSE`. Individual source files do not yet carry a
per-file copyright/license header — that's a deliberate follow-up decision
for the project owner (whether/how to stamp ~90 existing `.c`/`.h` files),
not something applied unilaterally here. New files are encouraged to start
with a one-line `SPDX-License-Identifier: BSD-2-Clause` comment once that
convention is adopted project-wide.
