# bzdOS microkernel — progress checkpoint (internal task, Chimp / BPI-M64 / A64)

**MILESTONE 2026-07-30 — the FreeBSD guest boots to an interactive root shell on
real hardware, and is reachable over ssh** (`root@192.168.88.82`). Four
hypervisor bugs fixed to get there: the sync fallback racing CPU2 for the eMMC
lock (`3b56041`), `emmc_bio` not settling the controller on failure paths
(`e6f1e4e`), a self-latching CNTV mask that killed the guest's timebase on one
lost injection (`dfc6eda`), and virtio-net TX broadcasting every frame instead of
using the guest's destination MAC (`f91aeff`). A soak then showed that boot was
about 1-in-5 reproducible and drove four more fixes; see
`docs/sessions/SESSION-STATUS-2026-07-30.md` §7-§10 for the numbers, and note
that the last two (partial-sector stitch, retry budget 8) are **not yet
hardware-verified**.

**Earlier the same day**: vtimer
"permanently lost" theory retracted (banked-register misread; watchdog fired
0 times), `mountroot>` cracked (guest already has BZDOS-RACE retry
instrumentation, a bare CR unsticks it — matches `auto_mount_root()`), and a
NEW real bug found past it: single-user `/bin/sh` dies on a genuine GEOM EIO
(`g_vfs_done():vtbd0p3[RAD(offset=801767424,length=163840)]error=5`) —
partition bounds and physical media both ruled out live, points to the same
vblk-lost-kick bug class recurring at a new LBA.

Snapshot of a long multi-track session. Board = Banana Pi M64 (Allwinner A64, 4× Cortex-A53).
Hypervisor runs at non-secure EL2; boots FreeBSD arm64 as an EL1 guest under stage-2.

## 🚫 RULE: no resets that involve the user — agent does network reloads itself
Never make the user power-cycle/reset by hand, never propose a reset as their decision. Fix the
live HV over EMAC (`sw`/`patch`/`call`/`w`); if a binary change must land, the AGENT does the
network reload (`wdt_reset` + chimpd) autonomously. See memory `never-suggest-reset`.

## ✅ FreeBSD track — EHCI INTID 106 storm re-killed LIVE + UART1 vconsole fix (2026-07-16)

*   **Regression found:** the on-board image had `HCR_EL2.IMO=1` again (`gic_timer_init`/`vgic_init`
    were back in `main_dbg.c`), re-creating the interrupt storm. Diagnosed live over EMAC:
    guest wedged in `ehci_reset()`'s DELAY at guest PC 0x91becc; `preempt_cnt` climbing ~145k/s
    while EL2 timer only ~100/s → a non-timer IRQ storming. GICD_ISPENDR + GICC_HPPIR = **INTID 106**;
    DTB: INTID 106 = SPI 74 = `usb@1c1b000` EHCI, **level-high**. GICH list regs empty (vgic forwarded
    nothing) → EL2 EOI'd but never cleared the level source → re-fired forever.
*   **Fixed LIVE (no reset):** `hv.cmd('sw hcr 0x84000003')` cleared IMO+FMO → storm 145k/s → 0,
    console 18519 → 20343 bytes. Baked into `main_dbg.c` permanently (removed `vgic_init`/`gic_timer_init`,
    explicit IMO/FMO clear). GICV redirect in `stage2_l3_uart[130/131]` verified CORRECT (not the cause).
*   **Next blocker found + fixed:** after the storm, guest spun in `pci_cfgregopen`/`generic_bs_r`
    polling **UART1** (`serial@1c28400`) IIR/MSR — three snps-dw UARTs share the one 4 KiB page at
    offsets 0x000/0x400/0x800. vconsole only matched UART0 (offset 0x000); UART1's IIR (page-offset
    0x408) missed `off==UART_REG_IIR`, returned 0 (= "int pending") → ns8250 ISR spun forever.
    Fix in `vconsole.c`: `reg = off & 0x3FF` folds UART0/1/2 onto one 16550 layout for IIR/LSR/USR
    emulation (THR capture stays UART0-only). Builds clean; landing via autonomous network reload.
*   **Breadcrumb map note:** current build uses SRAM 0x00018200 (GICT), 0x00018100 (EXC1),
    0x00018300 (irq_counter); GST1 preempt counter at 0x50000b00[2]. Old handoff map (GICT@0x50000800) is stale.

## ✅✅✅✅✅ FreeBSD track — Watchdog & Interrupt Storm/Delay hangs fixed, guest boots past EHCI (2026-07-15 late night)

*   **Watchdog Disable in DTB**: Traced guest hang/reset issues to FreeBSD's own watchdog driver `aw_wdog.c` attaching to the hardware watchdog node and disabling the WDT we configured in EL2 as a safety mechanism. Patched `/opt/bzdos/tftpboot/bananapi-min.dts` to add `status = "disabled";` to `watchdog@1c20ca0`, preventing the guest from disabling our safety net.
*   **Physical Interrupt Routing (`IMO = 0`)**: Removed `gic_timer_init()` and `daifclr` from `main_dbg.c` to leave `HCR_EL2.IMO = 0` (routing all physical interrupts directly to the guest). This lets the guest handle device interrupts (like EHCI/MMC) natively, completely resolving the interrupt storm at EL2 and the resulting hangs during EHCI initialization.
*   **Vconsole-based Debugger Polling**: Modified `el2_exc.c` to call `dbgmon_service()` inside the Stage-2 virtual UART `vconsole` trap handler. Because the guest console driver constantly polls/writes to the virtual UART, this provides high-frequency polling opportunities for EMAC debugging commands without requiring timer ticks or EL2 interrupt preemption.
*   **Verified on hardware**: In cycle 9, the guest successfully booted past the EHCI/USB probes and attached the `uart0` console driver! The EMAC network debugger remains fully active, responsive, and able to read/write guest state live during boot.

## ✅✅✅✅ FreeBSD track — ROOT CAUSE FOUND: missing /memory node in DTB (2026-07-15 evening)

**The "Cannot get physical memory regions" panic that blocked ALL boots since the 03:13 build was caused by a MISSING `/memory` node in `bananapi-min.dtb`.** The node was manually added in the earlier session (see below — 18KB console captured), but a DTB rebuild at 01:30 (to disable EHCI) regenerated the .dtb from the .dts source, and the .dts did NOT have `/memory`. Every subsequent boot panicked at `initarm+0x664` because `OF_finddevice("/memory")` returned -1 (FDT_ERR_NOTFOUND). The panic's stack overflow (SP into rodata, unwind_frame NULL deref) was a secondary symptom.

**Fix**: `fdtput -c bananapi-min.dtb /memory; fdtput -t s ... device_type memory; fdtput -t x ... reg 40000000 40000000` — one command on the host, no rebuild. Also added `/memory` to `bananapi-min.dts` source so future recompilations don't lose it.

**Verified on hardware**: vconsole = 18,519 bytes (0x4857), exact match to the earlier session. Console shows EHCI/cpufreq/simplebus device probes. ffv = "no vector first-fault" — the panic is gone. Guest reaches the known DELAY() hang blocker (ELR=0xffff00000091bec4).

**Diagnosis method**: used the live EMAC debugger throughout — read breadcrumbs, guest registers, walked stage-2 page tables, compared ELF source vs DRAM (found rodata corruption was a symptom), decoded ADRP+ADD to identify the panic string, checked FDT header/structure, and finally ran `fdtget` on the DTB to discover the missing node. No rebuild needed for the fix.

### Hot-debug infrastructure added (2026-07-15)

New dbgmon commands (in the 47768-byte microkernel-dbg.bin):
- `sr2` — dump EL2 system registers (CNTHCTL, HCR, VTCR, VTTBR, CPTR, SCTLR_EL2)
- `sw <name> <val>` — write a system register LIVE (cnthctl, hcr, sctlr1/2, vtcr, vttbr, ...)
- `call <pa> [x0..x3]` — call a hypervisor function by address (RPC), with address validation (must be in .text) + crash recovery (returns 0xDEAD if callee faults)
- `patch <pa> <word>` — write instruction + I-cache flush (hot-patch code without reset)

`hvdbg.py` — reusable Python library wrapping the EMAC protocol: cmd/read_words/read_bytes/dump/write_word/patch/call/wdt_reset/gict/vconsole/ffv/gr/sr2/walk_stage2/reenter_guest. Import and debug live.

`chimpd.py` — autonomous board supervisor: catches U-Boot, TFTP kernel+DTB, loady+bootelf ELF, monitors breadcrumbs over EMAC, auto-reset on hang.

### Key findings about CNTHCTL_EL2

`sr2` revealed `CNTHCTL_EL2 = 0x3` — set by ATF/BL31, NOT by our code. The guest already has native EL1 access to CNTPCT and CNTP_CTL/CVAL. The CNTHCTL fix from the earlier session (attempting `msr cnthctl_el2, #0x3` in gic_timer_init) was a no-op (the value was already 0x3). It has been removed; gic_timer.c now has a comment documenting that ATF sets it and we don't touch it.

### Current blocker: DELAY() hang (unchanged from earlier session)

Guest produces 18KB console, then ELR freezes in DELAY() body (0xffff00000091bec4 / 0xffff00000091b698). Timer tick (GST1 ring) keeps advancing — the hypervisor is alive, the guest's PC is genuinely stuck. Next step: read CNTFRQ_EL0 and the DELAY calibration path. The live debugger (`sr`, `gr`, `dump`) can do this without a board reset.

## ✅✅✅ FreeBSD track — MODINIOMD tag off-by-one fixed + vconsole UART bugs fixed (2026-07-15) + vgic timer integration reverted (2026-07-15) — 18 KB of REAL console output, ready for hardware test

**vgic timer integration attempted then reverted (2026-07-15, late session)**: hypothesis that FreeBSD picks CNTV (not CNTP) and needs it injected from the hypervisor was correct in theory, but the implementation (creating vgic.c/vgic.h to intercept GICC register access and redirect to GICV) caused a regression (NULL panic in unwind_frame before console output, worse than the baseline). Fully reverted all vgic code + integration points (removed from Makefile, main_dbg.c, gic_timer.c, stage2.c). Conclusion: the earlier three bugs (MODINIOMD tag, FAR_EL2 IPA, IIR loop) were the real culprits, not timer routing.

**Preventive fix: CNTHCTL_EL2 counter access (2026-07-15, final)**: added early setup of `CNTHCTL_EL2 = 0x3` (bits 0-1 = EL1PCEN|EL1VCEN) in `gic_timer_init()` to ensure the guest at EL1 can read both physical and virtual timer counters. This is necessary for FreeBSD's `DELAY()` busy-wait to terminate. Previously CNTHCTL_EL2 was left at reset default, which might have had EL1 counter access disabled. Clean rebuild passes. Build is ready for hardware test.

**Root cause of the earlier "HOWTO returned as lastaddr" saga (see the 2026-07-14 entry below), corrected**: `kload.h`'s `MODINFOMD_ENVP/HOWTO/KERNEND` were `0x0007/0x0008/0x0009`, off-by-one against the real `sys/sys/linker.h` (verified against the exact matching commit, `freebsd-src` `releng/15.1` @ `9263fb9`, which has `ENVP=0x0006/HOWTO=0x0007/KERNEND=0x0008`). `MD_FETCH()` matches by tag, so our ENVP record was silently misread as HOWTO and our HOWTO record as KERNEND. Fixed in `kload.h`; `kload.c`'s `howto_val` reverted from the `= kernend_val` workaround back to a plain `0x800` (RB_VERBOSE). Verified on hardware via `onebp`: `x21` (parse_boot_param's return value) now reads a correct, large KERNEND-based KVA instead of the leaked HOWTO value.

**Two real bugs found and fixed in `vconsole.c` (the trap-and-emulate virtual UART0), both via reading exact FreeBSD driver source, not guessing:**
1. **VA-vs-IPA address bug.** `vconsole_handle_fault()` compared `frame->far` (which is `FAR_EL2` — the guest's faulting **virtual** address per `exceptions.S`) directly against the physical `UART0_BASE`. This accidentally worked during early boot (guest MMU off, VA==IPA==PA) but broke the instant `pmap_bootstrap_dmap()` ran and FreeBSD started addressing UART0 via its DMAP kernel VA (e.g. `0xffff007ffffff008`) — the comparison then always failed, `vconsole_handle_fault` returned "not mine", and since a guest-group fault's ELR is never advanced on the generic unhandled path (`el2_exc.c`), the guest re-faulted on the exact same instruction forever (looked identical whether `-DDBG_NO_TVM` was set or not — that A/B was a red herring; the real bug was this address check). **Fix**: reconstruct the true IPA from `HPFAR_EL2` (`FIPA[47:12]`) OR'd with `far`'s low-12-bit page offset, same pattern already used by `virtio.c` in this tree.
2. **IIR always read as 0 → false "interrupt pending" spin.** Once the address bug was fixed, the guest still spun at ~4×10⁶ faults/sec cycling IIR (offset 0x08) → MSR (0x18) reads. Root-caused via the exact FreeBSD source (`sys/dev/uart/uart_dev_ns8250.c` `ns8250_clrint()`, checked out from `releng/15.1`@`9263fb9`, sparse-checkout extended with `git sparse-checkout add sys/dev/uart sys/dev/ic`): `while ((iir & IIR_NOPEND) == 0)` never exits if IIR always reads 0 (masks to `IIR_MLSC`, reads MSR, loops forever). Fixed: vconsole now returns `IIR_NOPEND` (`0x01`, per `sys/dev/ic/ns16550.h`) on IIR reads instead of a synthesized 0.

**Verified on hardware after both fixes**: `vconsole`'s capture ring (`0x50000f00`) shows `total_bytes=0x4857` (18,519 real captured bytes) and `fault_count=0x90b6` (37,046 — a sane, non-spinning count), both **static** across repeated samples (guest moved past this UART activity, not spinning on it). Dumped the ring (`d 0x50000f10 512` + wrap-seam math) — genuine FreeBSD boot console text: EHCI/usbus attach, per-CPU cpufreq nodes, ofwbus/pmu/simplebus/pcm device probes. This is the first time real FreeBSD device-autoconfiguration output has ever been captured on this board.

**NEW next blocker (separate from UART, not yet root-caused): guest wedges inside `DELAY()`.** After the last captured console line (`usbus3: EHCI version 1.0`), the guest's `elr` freezes inside/around `DELAY()`'s internal busy-wait body (`ffff00000091bec4` / `ffff00000091b698`, both within the `DELAY` symbol per the kernel disassembly) for 20+ seconds straight (4 samples, 5s apart, byte-identical `elr` each time) while an independent liveness counter (`guest.c`'s `GST1` ring, `0x50000b00`, word[2] = tick-preemption count, driven by our own EL2 timer and unrelated to guest UART/MMU state) climbs normally throughout — i.e. the debugger's own tick mechanism is provably alive, but the guest's PC genuinely never leaves this narrow region. Ruled out (with hardware evidence, not guessing):
  - **Not a stale-ESR illusion**: `ESR_EL2`/`FAR_EL2` are architecturally only updated on synchronous exceptions, not IRQ/FIQ, so a `gr` frame captured via the periodic tick's `dbgmon_service(frame)` call can show stale `esr`/`far` from a much earlier sync fault — but `elr`/GPRs ARE live, correctly-saved-per-trap values, so the frozen `elr` alone is trustworthy evidence of a real hang (a lesson worth keeping: don't decode `esr`/`far` from a tick-driven capture as if it described the current instant).
  - **Not the TVM/gtrace sysreg-trap mechanism**: `gtrace`'s own counter (`0x50002000`, word[1]) is frozen at `8` (the handful of early-boot MMU-setup traps) — zero new TVM traps are occurring, and TVM only ever covers SCTLR/TTBR0/TTBR1/TCR/ESR/FAR/AFSR0/AFSR1/MAIR/AMAIR/CONTEXTIDR (the EL1 MMU-control registers), never `CNTPCT_EL0`/`CNTVCT_EL0` — so a CNTHCTL_EL2-trapped-counter-read theory doesn't route through this path at all.
  - **Leading hypothesis, not yet confirmed**: `DELAY()`'s internal deadline (computed from a timer frequency / calibration value) may never be satisfied — either the underlying counter genuinely isn't advancing from the guest's exact vantage point for some other reason, or (more likely, given TVM/gtrace show nothing wrong) some caller requested/computed an absurdly large delay target. `sr` shows the guest's own `CNTP_CTL_EL0=1` (ENABLE=1) already configured (separately from our own EL2 tick, which owns the physical timer IRQ via `HCR_EL2.IMO=1` — worth checking whether the guest's own timer setup is starved of an interrupt it's implicitly waiting on, though `DELAY()` itself is a pure busy-poll with no interrupt dependency per the disassembly, so this may be an unrelated red herring). **Next step**: read `CNTFRQ_EL0` and the exact `DELAY()`/timecounter calibration path in `sys/arm64/arm64/machdep.c` or the relevant timecounter driver from the exact-matching source (same sparse-checkout technique used above) before touching hardware again — per the DEBUG_RULES.md R2/R4 discipline, no more board cycles until a concrete, falsifiable hypothesis is in hand.
- Files touched: `kload.h` (tag fix, from before this session), `vconsole.c` (both fixes, this session). `/opt/bzdos/build/freebsd-src` sparse-checkout extended with `sys/dev/uart` + `sys/dev/ic` this session (in addition to the existing `sys/arm64/arm64`, `sys/arm64/include`, `sys/contrib/libfdt`, `sys/dev/fdt`, `sys/dev/ofw`, `sys/kern`, `sys/sys`).
- New file this session: `/opt/bzdos/microkernel/DEBUG_RULES.md` — operating rules (R1–R7) for this exact class of live-hardware hypervisor debugging, derived from the specific ways this task has previously produced misdiagnoses (also mirrored into the project notes as `chimp-debug-rules`).

## ✅ BOARD RECOVERED (2026-07-14 ~20:48) — was a real power/USB-cable event, not a software wedge
Board was fully gone from USB (no `1-4` device object at all, confirmed via `/sys/bus/usb/devices/`) for ~21h. `uhubctl` investigation confirmed the board's port sits directly on the host's XHCI root hub (`1d6b:0002`, `xhci_hcd/10p` — the mobo's 4 physical USB ports, not a switchable external hub), which does **not** support per-port power switching (`ppps`) — so no software-level power-cycle was possible from the host side. Since USB also supplies the board's power, this looked like a genuine VBUS/power-latch event, not just a gadget-driver race. **Fix: user physically reseated the USB cable** ("рестартанул") → board re-enumerated cleanly (`usb 1-4: new high-speed USB device ... Product: USB download gadget, Manufacturer: bzdOS`), `/dev/ttyACM0` back, U-Boot `=>` prompt responds normally. Root cause of the original power loss itself still unconfirmed (could be board-side brownout/latch, could be host-side port fault) — no recurrence yet.
- Durable-fix ideas discussed (not yet implemented): independent UART-TTL console on the board's debug header (removes dependence on the single USB-OTG channel for recovery visibility), and/or an external powered/switchable USB hub with `ppps` between host and board (root-hub ports on this mobo don't support it) so future power-cycles can be done in software via `uhubctl -a cycle`.
- TFTP server: none was running after the session restart (no tftpd/tftpy previously installed as a persistent service — it had been running ad-hoc in a now-gone session shell). Fixed by starting `dnsmasq --port=0 --interface=br0 --bind-interfaces --enable-tftp --tftp-root=/opt/bzdos/tftpboot --no-daemon --log-facility=- &` (log at /tmp/dnsmasq-tftp.log). Re-run this if TFTP fails again after another restart and `ss -lun | grep :69` shows nothing.
- Local FreeBSD source now available at `/opt/bzdos/build/freebsd-src` (see the FreeBSD section below) — DTBP question fully resolved using it, no more curling GitHub needed for that investigation.
- **Harness fix applied (2026-07-14) to prevent a repeat of the hang**: `loady_over_acm.py` gained (1) `acquire_port_lock()`/`release_port_lock()` — a cross-process `flock` on `/tmp/chimp-acm.lock`, now called automatically inside `open_tty()`, so two script invocations can no longer race-open the ACM port (root suspect for the wedge: a diagnostic probe + two back-to-back `dbg-boot.py` runs, one straddling a slow TFTP failure, opened/interrupt-spammed the port within seconds of each other); it also adds a 0.5s settle delay after acquiring, and is idempotent per-process (won't self-deadlock on the sb-handoff open/close/reopen pattern). (2) `tftp_preflight(host,port,tftp_root,autostart=True)` — checks `:69` is listening before any `tftpboot`, auto-starting `dnsmasq --enable-tftp` if not, instead of letting U-Boot retry into an unknown-duration internal timeout. Wired into `dbg-boot.py`/`dbg-boot-notvm.py`/`fbsd-boot.py` (each now calls `L.tftp_preflight()` right after `wait_port()`, before opening the tty). `repl-client.py`/`repl-cmd.py` don't TFTP so they only benefit from the port lock (automatic, no script change needed there).

## ✅✅ FreeBSD track — REAL ROOT CAUSE FOUND AND FIXED (2026-07-14 ~22:50) — "Cannot get physical memory regions" is GONE
Everything in the "FreeBSD track" section below (DTB `/memory` node, libfdt version skew, DTBP virtual-vs-physical) was a chase down the WRONG branch. The DTB was always fine. The real bug: **`MODINFOMD_HOWTO` was emitted as `0`** (`kload.c`'s `kload_build_modinfo()`, `howto_val = 0 /* normal boot */`). This specific kernel build's `parse_boot_param()`/`freebsd_parse_boot_param()` (inlined, doesn't match current FreeBSD "main" source 1:1 — some vendor/version skew) treats a HOWTO value of exactly 0 as a signal that "no real preloaded modinfo was found", and silently falls through to its own internal `fake_preload_metadata(NULL, 0)` — a hardcoded stub with `MODINFO_NAME="kernel"`/`MODINFO_TYPE="elf kernel"`/`ADDR`/`SIZE` but **no DTBP tag at all**. That's why `OF_init` never ran (`fdtp`/`ofw_obj->ops` stayed NULL) and `OF_finddevice("/memory")` always returned -1 regardless of how correct our real DTB was.
- **Found via**: a new one-shot HVC software breakpoint tool, `onebp.c`/`onebp.h` (now a permanent part of the debugger image, wired into `el2_exc.c`'s HVC dispatch ahead of `gtrace_handle_hvc`, breadcrumb `ONEBP @ 0x50007000`). Needed because HW breakpoints/watchpoints/single-step are masked (`PSTATE.D=1`) this early — see memory `chimp-guest-debug-masked.md`. Mechanism: patch one guest instruction with `hvc #imm` (HVC isn't gated by PSTATE.D), dump GPRs to a breadcrumb on the trap, restore the original instruction, rewind ELR by 4, resume — fires exactly once. `onebp_arm(pa, imm)` / `onebp_handle_hvc(frame)`.
  - Walked the bug live, one checkpoint at a time: confirmed `x0` at `_start`'s "backup the module pointer" (`mov x1,x0`, VA `_start+0x85c`) was **already correct** (our modinfo blob's KVA, `0xffff000001400000`) — ruling out a loader/eret bug. Then confirmed `abp->modulep` was still correct at `parse_boot_param`'s entry. Then confirmed `fdt_check_header(modulep)` correctly REJECTED it (not a DTB, as expected — it's our modinfo blob). Then confirmed `preload_kmdp` was STILL correctly pointing at our real blob right after the first `preload_initkmdp()` call (i.e. the "elf kernel" search succeeded). The value only went bad after the HOWTO fetch — traced the disassembly there and found the `cbnz` on the HOWTO value gating a silent re-run of `fake_preload_metadata`+`preload_initkmdp` that clobbers `preload_kmdp`.
  - **Caution learned the hard way**: a first version of `onebp_handle_hvc` also did a raw `*(volatile uint64_t*)frame->x[0]` to capture "whatever x0 points at" in the same shot. That's an EL2-context dereference of a GUEST KVA with no stage-1/2 translation — wrong address space, caused an EL2-own data abort (recorded in the generic `EXC1` breadcrumb, harmless/self-recovering since `kind>>2 != 2` advances ELR, but noisy and misleading — looked like a new guest fault until traced to the diagnostic itself). Removed; onebp now only ever reads `frame->x[]`/`frame->elr` (already-captured register values, no address translation risk).
- **The fix** (`kload.c`, `kload_build_modinfo()`): `howto_val = 0x800` (`RB_VERBOSE` from `sys/sys/reboot.h` — a real, harmless boothowto bit; just means more boot console chatter) instead of `0`. Applied to `kload.c` (shared by `dbg`/`fbsd`/`repl` builds) — rebuilt `microkernel-dbg.bin`/`microkernel-fbsd.bin`/`microkernel-net.bin` all pick it up. Makefile also updated: `onebp.o` added to `DBG_OBJS`, `FBSD_OBJS`, `REPL_OBJS` (anything linking `el2_exc.o` now needs it, since the HVC dispatch calls `onebp_handle_hvc` unconditionally).
- **Verified on hardware, post-fix**: `fdtp` (static var in `ofw_fdt.c`, KVA `0xffff000000eccd58`) now correctly reads `0xffff000001200000` (our real DTB) instead of `0`. `preload_kmdp`/`preload_metadata` (KVA `0xffff0000010d5b88`/`...b90`) now correctly read `0xffff000001400000` (our real modinfo blob) instead of the fake buffer's `0xffff000001134ce9`. The OLD panic ("Cannot get physical memory regions", `initarm+0x664`) is GONE. The OLD stack-corruption symptom (`SP_EL1` ending up in rodata, `_etext+0x70`) is ALSO gone — `SP_EL1` at the new fault is a proper stack address (`0xffff000000e878d0`).
- **NEW (next) blocker, much further along**: guest now faults at `ELR_EL1 = 0xffff000000935be4` (well past the old panic point `0x934da4`, deeper into/after `initarm`) with `ESR_EL1 = 0x96000045` (EC=0x25, data abort, DFSC=0x05, translation fault level 1) and `FAR_EL1 = 0x1000`. Latched via the existing `firstfault.c`/FF1V mechanism (`0x50005800`, full GPR dump). Root-caused the call site from the latched `x30`/LR (`0xffff000000939950`) and `x2`=`0xfc0` (memset length): this is **`pmap_bootstrap_dmap()`** calling `memset_early(dest=0x1000, 0, 0x1000)` (caller passes `w2=0x1000`; memset_std's head-alignment shaves 0x40 off before the `dc zva` bulk loop, hence the `0xfc0` seen at the fault) — i.e. FreeBSD is zeroing a page at VA `0x1000` while building the kernel's direct map, and that VA isn't mapped yet.
  - **Checked and RULED OUT**: the DTB `/memory` node's `reg` property. `fdtget -t x bananapi-min.dtb /memory reg` → `40000000 40000000` — base `0x40000000`, size `0x40000000` (1 GiB), which is the correct/sane DRAM base for this SoC. So `fdt_foreach_mem_region`'s region data is fine; the bug is NOT in the DTB this time.
  - **Traced one more layer**: `pmap_bootstrap_dmap` is called from `initarm` (call site `0xffff0000009348ec`) with `x0 = x21 - KERNBASE` (compiled as `add x0,x21,#0x1000000000000` — adding the two's-complement of `KERNBASE` is cheaper than a 64-bit `sub`, since `KERNBASE`'s low 48 bits are 0; `-KERNBASE mod 2^64 == 0x1000000000000`). `x21 = parse_boot_param()`'s return value (`lastaddr` = our `kernend_val`, a KVA) — so `x0 = kernend_val - KERNBASE = max_delta`, i.e. a **physical-relative SIZE/offset** (≈`0x1600000`, the aligned kernel+DTB+modinfo extent our loader computed), not a bug by itself. The `0x1000` fault address must come from somewhere DEEPER in `pmap_bootstrap_dmap`'s own loop (it iterates `physmem_avail()` regions, storing running `[x21/x28/x9 +320/328/336/1360/1368/...]` offsets into a static array around `bootstack+0x160` — see disassembly from `0xffff0000009392c4` onward) — not yet fully traced to the exact wrong value. **Next step**: keep walking this loop (VA `0xffff000000939344` `bl physmem_avail` onward) with the same `r`/`gva` tools, or drop another `onebp` breakpoint right at the `dc zva` fault site (`0xffff000000935be4`, though note this is inside `memset_std`, so a breakpoint at the *caller* — `0xffff000000939940`, the `ldr x0,[x21,#320]` right before the `memset_early` call — would show the exact bad address landing in `[x21+320]` before it's ever passed down).
  - **Confirmed via `onebp`**: right before `memset_early` (VA `0x939944`, just after `ldr x0,[x21,#320]` at `0x939940`), `x0 = 0x1000` exactly — `pmap_bootstrap_dmap`'s own prologue-computed value really is that small, well before the actual fault. Went one level further back to the call site itself (`0xffff0000009348e8`, `add x0,x21,x8`) to read `x21`/`x8` directly, but that shot came back internally inconsistent (`x21=0x1000000000000`, `x8=0xffffffffffffffff` — doesn't square with the `x0=0x1000` result already confirmed one step later). Most likely a manually-miscounted breakpoint PA on that last shot, not a new finding — didn't chase it further. **Disarmed `onebp` again** (removed the `onebp_arm()` call from `main_dbg.c`; module + breadcrumb stay in the tree, harmless when unarmed). Trustworthy bottom line: the argument to `pmap_bootstrap_dmap` is genuinely in `(0, 0x1000]` instead of the multi-megabyte value expected — whether that's `parse_boot_param`'s return value itself being near-zero, or a callee-saved register (`x19`-`x28` per AAPCS) getting reused somewhere between the `mov x21,x0` right after `parse_boot_param` and this call site, is still open. Re-verify the exact `add x0,x21,x8` instruction's PA from a fresh disassembly line before trusting a register dump there again.

## Board / build state (as of before the hang — last known-good)
- Board currently running **microkernel-dbg.bin** (FreeBSD guest frozen in the early panic-storm).
- All images build green: microkernel{,-stage0,-net,-repl,-fbsd,-dbg,-hdmi}.bin. Cross: aarch64-linux-gnu-.
- Reload cycle (button-free): `repl-cmd.py "reset"` (REPL) or WDOG poke (`wb 0x01c20cb4 1; wb 0x01c20cb8 1; w 0x01c20cb0 0x14af`) from dbgmon → U-Boot (bootdelay=-1) → `repl-client.py` (REPL) / `dbg-boot.py` (FreeBSD dbg, TFTPs kernel+DTB).
- Command channels: EMAC raw Ethernet 0x88B5 (repl-cmd.py / dbgmon) + reliable netcon 0x88B6. Loady over ttyACM (U-Boot). The running image drops U-Boot's ACM gadget (zombie-fix), so /dev/ttyACM0 disappears while our image runs and returns after a WDOG reset to U-Boot.

## ============ FreeBSD track — MAJOR breakthrough, one blocker left ============

### The full root-cause chain (traced symptom → root this session)
The infamous "bogus SP in rodata" (SP_EL1=0xffff000000a90070=_etext+0x70) was only a SYMPTOM.
Real chain, innermost last:
```
SP=0xa90070 in rodata            ← boot-stack overflow from a recursive exception storm
  ← unwind_frame(NULL)            ← data abort on VA 0 (PC 0xffff000000952d04, ESR 0x96000005 = xlat fault L1)
    ← stack_save                  ← reads pcpu->pc_curthread = NULL this early (x0=0)
      ← kdb_backtrace
        ← vpanic / panic()
          ← initarm+0x664 (0xffff000000934da4): panic("Cannot get physical memory regions")
            ← fdt_foreach_mem_region (0x19ffa0) returns nonzero
              ← OF_finddevice("/memory") (called at 0x19ffd4) returns -1   ◀── CURRENT BLOCKER
```
- Confirmed: fdt_foreach_mem_region returns **0 on success, errno on failure** (NOT a count). The exclusion/KERNEND path (physmem_exclude_region @0x9348a8) runs AFTER this branch, so **MODINFOMD_KERNEND is NOT the cause** of this panic.
- MODINFOMD_KERNEND is a VIRTUAL (KVA) end address in FreeBSD arm64 — our 0xffff000001600000 is the right kind (only matters post-panic).

### Layer-1 fix already applied and verified (modinfo/DTB reachability)
- The kernel's early identity map (TTBR0) only covered ~[0x46000000, 0x47000000). Our old modinfo/DTB at 0x4a000000/0x4a100000 were unmapped → parse_boot_param's `ldr [modulep]` faulted.
- FIX (in kload.c / main_dbg.c / main_fbsd.c / repl.c): hand a **virtual modulep** (create_pagetables sizes TTBR1 from x0 when x0≥KERNBASE → maps [pa_base, phys(modulep)+~6MB]); relocate DTB→phys 0x47200000 (KVA 0xffff000001200000), modinfo→phys 0x47400000 (KVA 0xffff000001400000). Verified on HW: `gva` now maps both (DTB reads d00dfeed, modinfo reads MODINFO_NAME).

### Current blocker: OF_finddevice("/memory") == -1 despite a valid DTB
- DTB `/opt/bzdos/tftpboot/bananapi-min.dtb` is a MINIMAL, non-standard DTB (root #address/#size-cells = **1/1**, NOT the arm64-stock 2/2). The FreeBSD FDT parser reads cells from root and decodes 1/1 correctly; **do NOT switch to 2/2** — /soc uses `ranges;` with 1/1 and 2/2 would misparse GIC/UART/timer regs → new early fault.
- Added a `/memory` node; iterated 3 formats (page-9 `memory@40000000`; page-0 exact-name `memory` first child + `/chosen` 2nd). Latest blob: size 42940, totalsize 0xa7bc, md5 b000eb1a58b3a1ec8b361f78f8ea6423. Backups: bananapi-min.dtb.bak (no mem node), .page9-memnode.bak.
- `fdtget -l /` on our blob lists children: **memory, chosen, cpus, display-engine, ...** → `/memory` IS structurally findable at root.
- Loaded blob verified on HW at guest phys 0x47200000 (d00dfeed, totalsize 0xa7bc). DTBP (KVA) points at it. OF_init SUCCEEDED (the "OF_init failed"/"Cannot install FDT" panics did NOT fire).
- YET the guest still panics identically (live sr: SP_EL1=0xa90070; guest-stack vpanic caller byte-identical = initarm+0x664). So **the kernel's OF layer isn't resolving /memory in the fdt it installed.**

### Deeper trace done manually (this checkpoint) — traced initarm's DTBP handoff by hand
Disassembled `initarm` around VA 0xffff0000009347d4-0x934818 (the DTBP tag lookup + OF_install/OF_init call) and `OF_finddevice`/`preload_search_info`:
```
mov w1, #0x9002          ; MODINFO_METADATA(0x8000) | MODINFOMD_DTBP(0x1002)
ldr x0, [x23, #2952]     ; x0 = kmdp
bl preload_search_info    ; find the DTBP metadata record
cbz x0, +skip             ; tag not found -> skip OF_install/OF_init entirely
ldr x22, [x0]             ; x22 = *record = the DTBP VALUE itself
cbz x22, +skip             ; value==0 -> also skip
bl OF_install              ; OF_install("FDT", 0)
tbz w0,#0, <panic>         ; OF_install failure -> panic (NOT hit — confirms tag found, x22 nonzero)
mov x0, x22; bl OF_init     ; OF_init(x22 = our DTBP value)
cbnz w0, <panic>            ; OF_init failure -> panic (NOT hit either)
... (converges here whether or not the tag was found; unconditionally calls fdt_foreach_mem_region later)
```
So OF_install/OF_init do NOT explicitly fail — meaning our DTBP tag IS found with a nonzero value, and OF_init "succeeds" per its own check. But `fdt_foreach_mem_region`'s internal `OF_finddevice("/memory")` still returns -1.

**Prime suspect found: a documentation/implementation mismatch in OUR OWN loader.** `kload.h` line 173:
```c
#define MODINFOMD_DTBP        0x1002u  /* vm_paddr_t PHYSICAL pointer to the DTB */
```
— our own header comment says DTBP should be **PHYSICAL**. But `kload.c` (~line 487) computes:
```c
dtbp_val = kls.kernbase + (dtb_dst_pa - kls.pa_base);  /* KVA, NOT physical! */
```
— we emit a **VIRTUAL (KVA)** value. If the real kernel's `ofw_fdt_init`/`ofw_fdt_finddevice` (no exported symbol found in the stripped binary — likely static/inlined, could not pin down directly) internally treats the value as physical and re-translates it (e.g. DMAP-style conversion), handing an already-virtual KVA would double-translate into garbage memory — plausibly still "readable" (no crash) but not our actual DTB, which would exactly explain OF_init "succeeding" while `/memory` still resolves to ENXIO.

**RESOLVED — DTBP physical-vs-virtual question is CLOSED, our KVA was always correct.** Confirmed by reading real FreeBSD source (see checkout below, no curl needed):
- `sys/arm64/arm64/machdep.c` `try_load_dtb()`: `vm_offset_t dtbp = MD_FETCH(preload_kmdp, MODINFOMD_DTBP, vm_offset_t);` then `OF_init((void *)dtbp)`. **`vm_offset_t` is a VIRTUAL type** — confirms our KVA is the right kind, not physical. `kload.h`'s "PHYSICAL pointer" comment was simply wrong (stale/incorrect note from earlier research) — safe to ignore/fix later, not a functional bug.
- `sys/dev/ofw/ofw_fdt.c` `ofw_fdt_init(ofw_t, void *data)`: does `fdt_check_header(data)` then **`fdtp = data;`** — stores the pointer VERBATIM, zero translation. Since OF_init doesn't panic, `fdt_check_header` on our DTB succeeded.
- `ofw_fdt_finddevice(ofw_t, const char *device)`: `offset = fdt_path_offset(fdtp, device); if (offset < 0) return (-1);` — this is a **bog-standard libfdt call**, nothing FreeBSD-specific. So `OF_finddevice("/memory")` returning -1 means plain **`fdt_path_offset(fdtp, "/memory")` fails inside libfdt itself** on our blob, despite host-side `fdtget`/`dtc` parsing the same file fine.

**libfdt version skew — ALSO RULED OUT.** Built a tiny host-side test program linking the EXACT libfdt source bundled in the FreeBSD tree (`sys/contrib/libfdt/{fdt.c,fdt_ro.c,fdt_wip.c,fdt_strerror.c,fdt_addresses.c}`, compiled with plain gcc, see `/tmp/fdt_test.c`) and ran it directly against our actual file:
```
$ /tmp/fdt_test /opt/bzdos/tftpboot/bananapi-min.dtb
fdt_check_header: 0 (OK)
fdt_path_offset("/memory"): 140
device_type: memory
reg len=8
```
**The exact kernel-bundled libfdt code parses our file perfectly** — `/memory` resolves at offset 140, `device_type`/`reg` read back correctly. So: not the DTB content, not libfdt, not a version quirk, not DTBP phys-vs-virtual. Confirmed the file tested is byte-identical (md5 b000eb1a58b3a1ec8b361f78f8ea6423) to what was loaded onto the board.

**This narrows the remaining mystery to ONE place: something about the GUEST's actual runtime state differs from "host file + correct libfdt", despite every static check passing.** Was mid-way through re-verifying the guest's live in-memory copy via dbgmon when the board went unresponsive (see the hang note at the top of this file) — untested.

### FreeBSD — NEXT STEPS (in priority order, DTBP + libfdt threads are both CLOSED — do not re-open either)
1. **FIRST: get the board responding again** (see hang note at top of file — this blocks everything below).
2. **Verify the GUEST's in-memory copy of the DTB byte-for-byte**, not just header+totalsize. We only checked magic (d00dfeed) + totalsize match at guest phys 0x47200000 — read the actual struct-block bytes at the `/memory` node's known host-file offset (0x8c onward, node found at libfdt offset 140 decimal = 0x8c) and diff against the host file via dbgmon `d 0x4720008c <len>`. A partial/corrupted `memcpy` in `kload_build_modinfo`, a cache-coherency gap (D-cache clean happens on our side, but confirm the GUEST reads via a mapping that actually sees the cleaned data and not a stale cache line), or an off-by-one in `kload_dtb_totalsize` are the remaining live suspects — this is now the single most likely place the bug lives.
3. Fallback: instrument w0 at initarm 0x934884 (return of fdt_foreach_mem_region): 6=ENXIO (node/reg not found), 34=ERANGE (cells/len) — narrows it further if needed. Or: use the `ffv`/stage-2-vector-unmap machinery to catch this SPECIFIC panic path once more precisely (it currently only proves "some early panic happened", not which one — could be extended to latch at the panic() call site directly instead of the vector, e.g. an additional stage-2 trap on the panic string's code page).

### Local FreeBSD source checkout (use this, do NOT curl raw.githubusercontent.com)
`/opt/bzdos/build/freebsd-src` — sparse, shallow (depth 1, blob:none filter) clone of `github.com/freebsd/freebsd-src` (main branch), ~32 MB. Currently checked-out dirs: `sys/dev/ofw`, `sys/dev/fdt`, `sys/arm64/arm64`, `sys/kern`, `sys/sys`. To pull in more of the tree without re-cloning: `cd /opt/bzdos/build/freebsd-src && git sparse-checkout add <path>` (e.g. `sys/contrib/libfdt` for the actual FDT parser). Cloned with proxy env vars unset (they were empty in this shell already — no explicit unset needed, but if a future shell has http_proxy/https_proxy set, unset them for the clone per the global proxy note).

### KEY DEBUG TOOL built this session: `ffv` (first-fault catcher)
- `main_dbg.c` calls `stage2_unmap_guest_vector()` → unmaps the guest EL1 vector page (IPA 0x46927000) in stage-2. The guest's first fault → tries to execute its vector → stage-2 instruction abort to EL2 → `firstfault_handle()` (firstfault.c) latches the guest's ORIGINAL ELR_EL1/ESR_EL1/FAR_EL1 + GPRs to FF1V @ 0x50005800, remaps, resumes. `ffv` decodes it.
- CRUCIAL: HW breakpoints / watchpoints / single-step DO NOT work in FreeBSD early boot — the guest runs PSTATE.D=1 (debug masked) until cninit (verified: 0 bp hits). Stage-2 vector-unmap is the only way to catch early guest faults. (Saved to memory: chimp-guest-debug-masked.md.)
- CAVEAT: FF1V/ffv shows unwind_frame(NULL) for ANY early panic (all route through kdb→unwind_frame) — it does NOT distinguish the panic REASON. To get the reason, walk the guest stack above SP (FP chain {fp,lr} pairs) and resolve LRs with addr2line vs /opt/bzdos/tftpboot/kernel.

## ============ USB gadget track — advanced from broken to "SET_ADDRESS sticks" ============
musb.c (sunxi A64 MUSB gadget). Layers solved this session (each verified on HW via breadcrumbs):
1. **Framing**: 18-byte device descriptor was sent as 20 (32-bit-only FIFO writes padded). Fixed → word-head + byte-tail (no 16-bit halfword — A64 FIFO word+byte only).
2. **Hang**: musb_poll AHB-stalled after bus reset. Root = a reset-time `CSR0=FLUSHFIFO` write. Removed. word34=0x2f (musb_init complete), word35=0x3f (poll returns) confirm no hang.
3. **DATA toggle**: first EP0 IN packet after SETUP must be DATA1. Was DATA0 (SVDRXPKTRDY folded with TXPKTRDY). Fixed → SVDRXPKTRDY alone, spin for RXPKTRDY clear, then TXPKTRDY alone.
4. **DATAEND / SETUPEND**: separated DATAEND to status stage; added COUNT0-priority SETUP decode for coalesced SETUP+SETUPEND.
5. **SET_ADDRESS now STICKS**: word16=FADDR readback=9, word32 high=1 (device receives a SETUP at the assigned address 9). First traffic at the assigned address.

### USB — current blocker + NEXT STEP
- After SET_ADDRESS sticks, the host's addressed GET_DESCRIPTOR SETUP is RECEIVED (word32 high=1) but not decoded/answered → word46=0, device count stuck at 1. Host dmesg cycles "device descriptor read/all -110", "not accepting address -62".
- The USB agent's latest build adds **word47 (0x500000bc) = (bmRequestType<<24)|(bRequest<<16)|wLength** and **word48 (0x500000c0) = (COUNT0<<16)|wValue** — raw capture of the addressed SETUP. NOT YET TESTED (needs REPL rebuild + disconnect→mi→3×mp, then `bc 0x500000bc 2`).
- Breadcrumb map (0x50000000 base, MUSB): counters 0x60 (device/reset/config/setaddr), word16/17 @0x40 (FADDR readback / timeout), word32/33 @0x80 (SETUPs@FADDR≠0 / reset-suspend), word34 @0x88 (init phase, 0x2f=done), word35/36 @0x8c (poll phase / INTRUSB), word40-43 @0xa0 (EP0-TX), word44 @0xb0 (0xA armed/0xB stuck/0xC host-read), word45 @0xb4 (SETUPEND), word46 @0xb8 (full-descriptor count), word47/48 @0xbc/0xc0 (addressed SETUP raw).
- USB test recipe: on REPL image — `c 0x42008e00` (usb_gadget_disconnect) then `mi` then `mp 5000000` ×3 back-to-back (tight timing needed — host gives up in ~2s; too much network latency between mi and mp misses the window). Then read counters/words + host `dmesg | grep "1-4"` / `lsusb | grep 1d6b:0010`.

## ============ RTOS primitives — DONE, 3/3 self-tests PASS on HW ============
- ktimer.c (soft timers: one-shot/periodic/cancel, ksleep), ksync.c (mutex w/ priority inheritance, counting sem, mailbox), wcet.c (period/budget/deadline monitor). Integrated into REPL.
- sched.c gained `sched_block_if(token)` + `sched_set_priority()` (appended). REPL commands: `ktt`/`kst`/`wct`.
- Results: KTIMER (0x50005600) pass, periodic=5, cancel ok. KSYNC (0x50005500) pass, mbox=4, sem=1. WCET (0x50005400) pass (overruns=1,budget=1,deadline=0).

## ============ Delivered modules awaiting integration (new files + snippets ready) ============
All compile clean; NOT yet wired into images (integrate incrementally, build+test each):
- **HW breakpoints + backtrace** (hwbp.c/.h, backtrace.c/.h) — INTEGRATED in dbg/repl/fbsd (`bp`/`wp`/`bpc`/`bpl`/`bt`). (Note: bp/wp don't fire in debug-masked early guest boot.)
- **SMP** (smp.c/.h + start.S/sched.c) — linked into all images (smp.o), but `smp_init()` call site NOT yet added, so secondaries stay parked. Heartbeat breadcrumb 0x50000900. Needs the el2_exc per-core timer snippet + smp_init() in a main to activate the 4 cores.
- **firstfault** (firstfault.c/.h) — INTEGRATED in dbg (the `ffv` tool above).
- **event-trace + profiler** (trace.c/.h, profiler.c/.h) — trace ring 0x50004000 "TRC1" (16-byte entries), PROF histogram 0x50006800 "PROF". Snippets: trace_ctx_switch in sched, trace_tick/trace_trap/profiler_sample in el2_exc IRQ/trap. NOT integrated. NOTE: HUD Gantt/flame track assumed PROF@0x50004800 but profiler put it at 0x50006800 — reconcile when integrating.
- **HUD Gantt/flamegraph** (hud.c/.h edited) — new SCHED GANTT + PROFILE panels reading the trace ring + PROF histogram. Owns hud.c (in REPL). Only shows data once trace/profiler are integrated AND the scheduler/guest runs.
- **panic-log + coredump** (panic.c/.h, coredump.c/.h) — persistent panic log 0x50005000 "PANC" (survives reset, auto-dump on next boot via panic_dump_if_present); coredump over Ethernet 0x88B7 (ELF core for gdb). Snippets for el2_exc fatal path + mains. NOT integrated.
- **virtio-console + virtio-blk** (virtio.c/.h, virtio_console.c, virtio_blk.c) — virtio-mmio @0x0A000000 (console)/0x0A000200 (blk), RAM disk @0x7C000000, guest INTIDs 48/49. Breadcrumb 0x50005000... (collides with PANC — reassign). Needs stage2 unmap of the virtio window + el2_exc fault dispatch + guest DTB nodes. NOT integrated. (Only useful once FreeBSD boots.)
- **vGIC + virtual timer** (vgic.c/.h) — GICv2 virt: GICH@0x01c84000, GICV@0x01c86000, maintenance INTID25, vtimer INTID27. Self-test EL1 guest; breadcrumbs 0x50001c00/0x50001d00. Needs stage2 GICC→GICV remap + el2_exc inject. NOT integrated.

## Breadcrumb address map (avoid collisions)
0x50000000 musb · 0x50000100 emac · 0x50000300 repl · 0x50000400 exc · 0x50000600 hwbp ·
0x50000700 btr · 0x50000900 smp · 0x50000e00 dbg · 0x50000f00 vconsole · 0x50001c00/0x50001d00 vgic ·
0x50002000 gtrace · 0x50002400 ffl1 · 0x50002800 sst1 · 0x50003000 hdmi · 0x50004000 trace ·
0x50004800/0x50006800 prof (mismatch — HUD vs profiler) · 0x50005000 panc/virtio(collision!) ·
0x50005400 wcet · 0x50005500 ksync · 0x50005600 ktimer · 0x50005800 ff1v(ffv)

## libmin additions
Added `__popcountdi2`/`__popcountsi2` (smp.c uses __builtin_popcountll; freestanding = no libgcc).

## Also verified working earlier this project
EL2 traps, CNTP 1ms tick + jitter, lock-free ring, slab allocator, fixed-priority scheduler, EL1 guest + preemption, netcon reliable transport, hot-reload, network REPL + remote/fast reset, HDMI 1080p + HUD dashboard, live debugger (gr/sr/gva/r/rb/d/w/wb/t/pt/ff/ss/bp/wp/bt/ffv), trap-and-emulate vconsole/gtrace.
