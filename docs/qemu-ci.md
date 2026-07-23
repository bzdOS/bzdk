# QEMU `virt`-machine CI target (ROADMAP.md T3)

## Why this exists

Until now the only place this hypervisor could run was the real Banana Pi
M64 (Allwinner A64) board — a single point of hardware failure and a hard
requirement for any regression check. This target is a **second, portable
place to run the core hypervisor logic** — stage-2 identity mapping, guest
EL1 entry, GICv2 timer/IRQ handling, a serial console — under
`qemu-system-aarch64 -M virt`, fully scriptable in CI, no board needed.

It is **not** a FreeBSD-under-QEMU port, and it does not need any of the
board-specific drivers (eMMC/EMAC/USB-OTG are Allwinner silicon and don't
exist on `virt`). The guest is a tiny bare-metal "hello from EL1" payload
(`guest_qemu_payload.c`) that proves the properties CI actually cares about:
the guest runs at EL1, it does so under an enabled stage-2 translation
(`HCR_EL2.VM=1`), and the EL2 GICv2 timer tick preempts it repeatedly.

## Build

```sh
make qemu          # -> microkernel-qemu.elf
make clean-qemu    # removes just this target's objects + ELF
```

This is a **new, separate** object list/linker script/target — see the
Makefile's "QEMU `virt`-machine CI target" section. It does not touch
`link.ld`, `DBG_OBJS`/`GDB_OBJS`/`REPL_OBJS`/`FBSD_OBJS`, or any of their
targets. `stage2.c`/`stage2.h` and `sd_bio.c`/`sd_bio.h` are untouched
entirely (not even read-only-included differently) — see "What's reused
unmodified" below for why that's possible.

## Run

```sh
qemu-system-aarch64 \
    -machine virt,gic-version=2,virtualization=on \
    -cpu cortex-a53 \
    -m 1024 \
    -nographic \
    -kernel microkernel-qemu.elf
```

Flags that matter, and why:

- **`virtualization=on`** — REQUIRED. This hypervisor runs at EL2; without
  this the vCPU doesn't implement EL2 at all, direct-kernel boot drops the
  image in at EL1 instead, and `el2_install()`'s `msr vbar_el2` (EL2-only)
  traps as an undefined instruction with no vector table installed to catch
  it — the image hangs after printing exactly two boot lines (verified: see
  "Self-verification" below). This is a genuinely load-bearing flag, not
  cargo-culted.
- **`gic-version=2`** — REQUIRED. `gic_timer_qemu.c` is a classic-GICv2
  register driver (`GICD_ISENABLER`/`GICC_IAR`/`GICC_EOIR`, no
  redistributors) at `virt`'s fixed GICv2 addresses (GICD `0x08000000`,
  GICC `0x08010000`); a GICv3-configured machine has different MMIO at
  those offsets and this driver would not work against it.
- **`-cpu cortex-a53`** — matches the real board's CPU (closest apples-to-
  apples comparison), and its PARange is what `stage2.c` reads at runtime
  for `VTCR_EL2.PS` (not hardcoded — see `stage2_init()`), so this isn't
  load-bearing the way the two flags above are; `-cpu max`/`-cpu cortex-a57`
  etc. should also work, just untested here.
- **`-m 1024`** — matches `STAGE2_DRAM_SIZE` (1 GiB, `stage2.h`), so the
  stage-2 DRAM identity map has real backing all the way through. Less RAM
  would probably still boot fine (nothing this milestone's guest touches
  goes near the top of that range), but this keeps the map and the backing
  store honestly consistent.
- **No `-dtb`/`-bios`**: this target hardcodes every MMIO address it needs
  (GIC, PL011, RAM base) instead of parsing a device tree, matching the rest
  of this codebase's "cited, not guessed" style — just for `virt`'s
  addresses instead of the A64's. QEMU still auto-generates and hands the
  guest a DTB in `x0` (the direct-kernel-boot Linux ABI), which
  `start_qemu.S` explicitly ignores.

## What a passing run looks like

```
=== bzdOS microkernel -- QEMU virt CI target ===
HV: entered at EL2, MMU off, physical addressing (see start_qemu.S)
HV: EL2 vector table installed (exceptions.S, unchanged)
HV: stage-2 identity map programmed + enabled (HCR_EL2.VM=1)
HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)
HV: GICv2 timer armed (INTID 30 / CNTP, 100 ms period)
HV: IRQ/FIQ unmasked -- dropping to EL1 guest now
GUEST EL1: hello from under stage-2 translation, CurrentEL=1
GUEST EL1: still alive, loop=1
...
HV: first tick — EL2 preempted the EL1 guest
...
HV: tick 20 (preempted guest)
QEMU-CI: PASS (stage-2 + EL1 guest + GICv2 timer preemption confirmed)
```

The interleaving of `GUEST EL1: ...` and `HV: tick ...` lines is itself part
of the proof — it's what "the EL2 timer preempts the running EL1 guest"
looks like on a shared serial console (both write to the *same* real PL011
UART, the guest reaching it directly through the stage-2 identity map — see
`pl011_qemu.h`'s banner for why no trap-and-emulate is needed here, unlike
`vconsole.c` on the real board).

After printing the `QEMU-CI: PASS` line, the HV issues a PSCI `SYSTEM_OFF`
SMC (`el2_exc_qemu.c`'s `qemu_poweroff()`), which QEMU's `virt` machine
answers itself (no real firmware needed) and **terminates the process with
exit code 0** — a genuine, scriptable pass/fail signal, not just a log grep.

## Scripting it for CI

```sh
#!/bin/sh
set -e
make qemu
timeout 15 qemu-system-aarch64 \
    -machine virt,gic-version=2,virtualization=on \
    -cpu cortex-a53 -m 1024 -nographic \
    -kernel microkernel-qemu.elf > qemu-ci.log 2>&1
rc=$?
grep -q "QEMU-CI: PASS" qemu-ci.log || { echo "FAIL: no PASS marker"; cat qemu-ci.log; exit 1; }
[ "$rc" -eq 0 ] || { echo "FAIL: qemu exited $rc (expected clean PSCI shutdown)"; exit 1; }
echo "PASS"
```

Both checks matter independently: `timeout` catches an outright hang (exit
124, the exact failure mode reproduced below when `virtualization=on` is
dropped); the `grep` catches a run that exits some other way (e.g. a crash
during the fault path in `el2_exc_qemu.c`, which prints `QEMU-CI: FAULT ...`
and halts instead of the PASS marker, and never issues the PSCI call, so it
would only ever be caught by `timeout`, not by exit code).

## What's reused unmodified vs. what's new

| File | Status | Notes |
|---|---|---|
| `stage2.c` / `stage2.h` | **Unmodified** (as instructed) | QEMU `virt`'s RAM base (`0x40000000`) and its low MMIO cluster both land inside the exact ranges `stage2.c` already identity-maps (0..1 GiB device, 1 GiB DRAM from `0x40000000`) — no board-specific constant in it needed to change for this to work. `VTCR_EL2.PS` is read from `ID_AA64MMFR0_EL1` at runtime, not hardcoded, so it self-adjusts to whatever `-cpu` is used. The A64-specific `UART0_BASE` (`0x01C28000`) carve-out is still built into the map; on `virt` nothing lives at that address so it's simply an extra unmapped device page nobody touches. |
| `guest.c` / `guest.h` | **Unmodified** | Fully generic AArch64 EL1 drop — no board addresses in it at all. |
| `exceptions.S` / `exceptions.h` | **Unmodified** | Pure vector-table + register save/restore, EL-generic. |
| `timer.c` / `timer.h` | **Unmodified** | Pure ARM Generic Timer system-register access (`CNTFRQ_EL0`/`CNTPCT_EL0`), no MMIO. |
| `libmin.c` | **Unmodified** | Freestanding `mem*`/`__popcount*` helpers, board-agnostic. |
| `start_qemu.S` | **New** | Different boot contract from `start.S`: QEMU's direct-kernel boot hands us an *undefined* SP and there's no firmware to `ret` back into (see its header comment) — U-Boot's `start.S` assumptions don't hold. |
| `link_qemu.ld` | **New** | Loads at `0x40080000` instead of the board's `0x42000000`; QEMU's `-kernel` loader honors the ELF's own load address either way. |
| `gic_timer_qemu.c` / `.h` | **New** | Same GICv2 register-level programming as `gic_timer.c`, different MMIO bases (`virt`'s `0x08000000`/`0x08010000` vs. the A64's GIC-400 at `0x01c8x000`). Kept as a separate file specifically so this work never touches `gic_timer.c`. |
| `pl011_qemu.c` / `.h` | **New** | A real PL011 driver, not an emulation trick — see its banner for why `vconsole.c`'s 16550 trap-and-emulate approach isn't needed (or reused) here. |
| `el2_exc_qemu.c` | **New** | A deliberately minimal `el2_trap()` — the real `el2_exc.c` is the full board debugger's dispatch table and pulls in a dozen Allwinner-specific modules (wdt, smp/PSCI-on-real-silicon, vblk_emmc, dbgmon, hwbp, ...) that don't apply here or aren't needed for this milestone's proof. |
| `guest_qemu_payload.c` / `.h` | **New** | The "hello from EL1" guest payload; calls `guest.c`'s unmodified `guest_config()`/`guest_enter()`. |
| `main_qemu.c` | **New** | Wires the above together; see its own header comment for the exact bring-up order. |

## Self-verification performed

Both run in this environment (`qemu-system-aarch64 10.1.5`, Fedora host):

1. **Happy path**: `make qemu` then the documented invocation above —
   boots, prints the full expected sequence, guest and HV tick lines
   interleave (proving real preemption, not just "the guest ran once"),
   prints `QEMU-CI: PASS`, and QEMU exits with status **0**.
2. **Negative control** (confirms the CI signal actually means something):
   dropping `virtualization=on` from the invocation reproduces the "EL2 not
   available" failure mode exactly as predicted — the image prints its
   first two boot lines and then hangs (no vector table catches the
   EL2-register access from what is actually EL1), `timeout` kills it,
   exit code **124**. No `QEMU-CI: PASS` in the log, no clean exit — both
   of the CI script's checks correctly call this a failure.

## Follow-up / not done here

- No SMP (`smp.c`'s PSCI `CPU_ON` bring-up is real-board-flavored /
  untested against QEMU's PSCI implementation) — this target is
  deliberately single-CPU; T3 only asks for the core single-core proof.
- No virtio-net/EMAC-equivalent side channel — per the task brief, this is
  explicitly out of scope for the first cut (option (a): skip the debug
  protocol entirely for the QEMU target). `virtio-net` under QEMU is a
  plausible stretch goal if a live network debug channel is ever wanted
  here too, but nothing about the CI use case needs it — serial console
  output is sufficient and simpler.
- No FreeBSD-under-QEMU: out of scope per the task brief; this target
  proves the hypervisor core, not a second FreeBSD port.
