/* SPDX-License-Identifier: BSD-2-Clause */

/* main_dbg.c — LIVE hypervisor debugger. Brings up the EMAC network console,
 * boots the real FreeBSD kernel as an EL1 guest under stage-2, and arms the
 * CNTP timer tick so the guest is PREEMPTIBLE. On every tick el2_trap calls
 * dbgmon_service(guest_frame) which services network debug commands against
 * the live guest — so you can read the guest's registers/PC/memory/sysregs
 * while it runs, no reset. (fbsd-boot.py TFTPs the kernel ELF to 0x44000000
 * and DTB to 0x4a000000 before this image is loaded.)
 *
 * DEAD-MAN'S SWITCH (2026-07-16): a ~16s hardware watchdog IS armed here
 * (wdt_arm), petted from el2_trap() on every EL2 exception. While EL2 services
 * anything (tick / vconsole / any guest fault / host dbgmon cmd) the WDT is fed
 * and the debugger stays resident. Only a TOTAL wedge — EL2 takes no more
 * exceptions, EMAC dead, board unreachable (the state that used to need a
 * physical power-cycle) — lets it fire, rebooting to U-Boot where a persistent
 * chimpd (python3 -u chimpd.py) catches it and auto-reloads. Self-healing,
 * bounded ~16s, no human, no manual reset. See SESSION-RULES.md. */
#include <stdint.h>
#include "emac.h"
#include "exceptions.h"
#include "kload.h"
#include "stage2.h"
#include "guest.h"
#include "vconsole.h"
#include "usbacm.h"
#include "gtrace.h"
#include "gic_timer.h"
#include "wdt.h"
#include "smp.h"
#include "dbgmon.h"
#include "reboot.h"
#include "hwbp.h"
#ifdef HV_HDMI
#include "hdmi.h"
#include "hud.h"
#endif
#include "onebp.h"
#include "vgic.h"
#include "vblk_emmc.h"
#include "vblk_async.h"
#include "vnet_emac.h"   /* ROADMAP C1: virtio-net multiplexed onto this EMAC */
#include "el2_ncmap.h"
#include "dbgtools.h"    /* CPU1 heartbeat / build-id / entry-hold (2026-07-26) */

/* bmc.c — software-BMC management plane. Extern decl only (bmc.h pulls in
 * exceptions.h + its own struct; main_dbg.c only needs the init entry). */
extern void bmc_init(void);

/* GDB-stub RSP byte transport (ROADMAP B2). gdbstub.c reaches for these three
 * to move Remote-Serial-Protocol bytes; in the standalone `make gdb` build
 * main_gdb.c provides them, so the dbg build must too. They ride the SAME
 * EMAC 0x88B5 console as dbgmon/REPL — the CPU1 debug loop only ever drives
 * one of {dbgmon, gdbstub} per iteration (gated on gdb_channel, see smp.c), so
 * they never double-drain the RX ring. gdb_getc() pumps emac_poll() itself
 * because the stub spins on it while the guest is stopped and EL2 IRQs masked. */
#include "emac.h"
int  gdb_getc(void)  { emac_poll(); return emac_getc(); }
void gdb_putc(int c) { emac_putc(c); }
void gdb_flush(void) { emac_flush(); }

/* zstage.c — dual-guest bulk loader. Weak no-op so this file, which is shared
 * by the `dbg`, `gdb` and `dual` targets, links unchanged for the first two:
 * only `dual` links zstage.o, whose strong definition wins. Same weak/strong
 * linkage pattern smp.c already uses for zephyr_cpu3_run() (default: park in
 * WFI) and vblk_async_cpu2_run() — see zstage.h and the call site below for
 * why the copy has to happen exactly where it does. */
__attribute__((weak)) void zguest_stage_copyin(void) { }

/* DIAGNOSTIC (temporary): one-shot breakpoint inside pmap_bootstrap_dmap,
 * right after "ldr x0,[x21,#320]" (VA 0xffff000000939940) — x0 there is the
 * address about to be memset(0)'d by memset_early, which faults at FAR=0x1000.
 * See PROGRESS.md's pmap_bootstrap_dmap investigation (new blocker after the
 * DTBP/HOWTO fix). Breadcrumb ONEBP @ 0x50007000. Remove once resolved. */
#define ONEBP_PMAP_DMAP_PA  (K_PABASE + 0x934788UL)  /* initarm: "mov w0,wzr" right after "mov x21,x0" (x21 = lastaddr = parse_boot_param's return value) */
#define ONEBP_IMM             0x99u

#define K_ELF     0x44000000UL
#define K_PABASE  0x46000000UL
#define DTB_SRC   0x4a000000UL   /* DTB as TFTP'd by U-Boot (outside kernel map) */
#define DTB_DST   0x47200000UL   /* DTB copied here, just above _end, in-window  */
#define MODINFO   0x47400000UL   /* modinfo scratch, above the relocated DTB     */
#define SP_EL1    0x4c000000UL
#define TICK_US   10000u        /* 10 ms tick -> ~100 debug services/sec */

/* Debug console rides the EMAC network channel (raw 0x88B5), same as the REPL.
 * dbgmon.c calls these extern hooks. */
int  console_getc(void)   { return emac_getc(); }
void console_putc(int c)  { emac_putc(c); }
void console_poll(void)   { emac_poll(); }
void console_flush(void)  { emac_flush(); }

#define DBG_BC(i, v) do { \
	volatile uint32_t *p = (volatile uint32_t *)(0x50000e00UL + (uint32_t)(i) * 4u); \
	*p = (uint32_t)(v); \
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory"); \
} while (0)

int main(void)
{
	uint64_t mi, entry, i;

	DBG_BC(0, 0x44424731);   /* "DBG1" */
	DBG_BC(1, 1);
	usb_gadget_disconnect();   /* clean-disconnect U-Boot gadget on takeover */

	/* aw_mmc IDMAC DMA-coherency: a blanket dc-civac sweep of all guest DRAM
	 * was tried here and reliably wedged EMAC (see docs/aw-mmc-dma-coherency.md
	 * for the failure analysis). Superseded by el2_ncmap.c, which makes EL2
	 * itself a non-cacheable observer of guest DRAM instead of flushing it —
	 * see el2_ncmap_apply() below. */

	/* Network debug console up first. */
	emac_init();
	for (i = 0; i < 200000000UL && !emac_link_up(); i++)
		__asm__ volatile("nop");
	DBG_BC(1, emac_link_up() ? 2 : 0x1177);   /* 0x1177 = link never came up */

	el2_install();
	vconsole_init();

	/* CPU1 heartbeat / build-id / entry-hold breadcrumb lane (dbgtools.h).
	 * Must run before smp_init() (below) brings CPU1 up, so the cold-vs-
	 * warm-reset detection + build-id stamp are in place before anything
	 * could observe them, and before the dbg_hold_before_entry-style gate
	 * near kload_enter() reads HOLD/RELEASE. */
	dbgtools_init();

	/* eMMC-backed virtio-blk: bring the eMMC up and register the modern
	 * virtio-mmio device at 0x0A000000 BEFORE stage2_init()/stage2_enable()
	 * below, so the trapped MMIO window is emulatable and the eMMC is ready the
	 * moment the guest can fault on it. vblk_init() calls emmc_bio_init()
	 * itself (which forces PC5->func3), zeroes the eMMC-controller mutex (a
	 * fixed DRAM word that must not carry a stale "locked" value across a warm
	 * WDT reset), and lays the "VBK1" breadcrumb at 0x50020000 (lock word at
	 * 0x50020100 — both inside the DTB-reserved hv-scratch window). Returns <0
	 * if the eMMC didn't come up; the device still registers and completes
	 * every request as VIRTIO_BLK_S_IOERR until it does. Injects its completion
	 * IRQ via the real GICD (INTID 137/SPI 105) under the existing IMO=0 policy
	 * — no HCR change needed. See docs/virtio-blk-design.md / -integration.md.
	 *
	 * RE-ENABLED for the virtio-blk RETEST (guest aw_mmc disabled in DTB,
	 * root=vtbd0p3): the original "hard error on every read" ran BEFORE the
	 * DTB /reserved-memory carve-out existed, so the guest allocator could
	 * hand out the very pages the HV's lock/breadcrumb writes hit — plus two
	 * real vblk bugs (chain-truncation status write, GET_ID) are now fixed.
	 * NOTE: vblk_init() must stay DISABLED for direct-aw_mmc tests — its
	 * emmc_bio_init() leaves the controller in a state the guest's aw_mmc
	 * can't re-enumerate from (no mmcsd0). */
	vblk_init();

	/* virtio-net multiplexed onto this same EMAC (ROADMAP C1 — see
	 * vnet_emac.h). Registers the modern virtio-mmio device at 0x0A001000,
	 * inside vblk's already-trapped 2 MiB stage-2 block (no stage2.c change
	 * needed). Must run before stage2_init()/stage2_enable() below, same as
	 * vblk_init() above. Always "succeeds" (rides on emac_init(), already up
	 * a few lines above) — see vnet_init()'s own doc comment. TX (guest ->
	 * wire) drops ethertype 0x88B5 (our own debug protocol) so the guest can
	 * never spoof the HV's control channel; RX (wire -> guest) is fed by
	 * emac.c's RX demux via vnet_emac_rx_frame() (weak no-op there when this
	 * object isn't linked in). Cross-core TX safety vs the CPU1 debug core's
	 * own EMAC console traffic is provided by emac.c's EMAC_TX_LOCK_PA
	 * (see the "TX CROSS-CORE MUTUAL EXCLUSION" block in emac.c). */
	vnet_init();

	/* USB-OTG CDC-ACM interactive console bridge (usbacm.c): brings up the
	 * MUSB gadget (musb_init()) so the CPU1 debug core's usbacm_poll()
	 * (see smp.c) can pump host<->guest bytes through vconsole's RX/TX-tee
	 * rings once smp_init() below starts that core. MUST run before
	 * smp_init() -- musb_init() lays down musb.c's static gadget state
	 * that CPU1 immediately starts reading/mutating in its poll loop, and
	 * (like every other CPU0-inits/CPU1-consumes structure in this tree)
	 * relies on SMPEN cache coherency rather than an explicit hand-off. See
	 * usbacm.h for the full design + the documented MUSB-hardware-
	 * ownership risk if the guest's DTB also exposes this device. */
	usbacm_init();
	DBG_BC(1, 0x05B0AC30);   /* 'USB-ACM3' truncated: usbacm_init() done   */

	/* EXPERIMENT (FreeBSD bad-SP): gtrace_init() sets HCR_EL2.TVM=1, so the
	 * guest's SCTLR/TTBR/TCR writes trap to EL2 and we emulate them. That
	 * emulation is the prime suspect for corrupting a guest GPR — the guest's
	 * SP landed at _etext (0xa90000), NOT the real bootstack (0x1136040),
	 * which is the classic "restore-wrong-register-on-trap-return" signature.
	 * With TVM off the kernel runs its own sysreg writes natively. If SP_EL1
	 * is now sane / the guest progresses, our TVM emulation was the bug.
	 * Toggle via DBG_NO_TVM (Makefile -DDBG_NO_TVM) so we can A/B on hardware. */
#ifndef DBG_NO_TVM
	gtrace_init();
#endif
	dbgmon_init();
	bmc_init();                /* lay down BMC1 breadcrumb + first health record */
	DBG_BC(1, 3);

	/* Arm the dead-man's-switch watchdog EARLY — before any of the risky
	 * setup (kload, stage2, guest entry). If ANYTHING here crashes/hangs
	 * before the guest starts producing console, the HW watchdog fires in
	 * ~16s and reboots to U-Boot (persistent chimpd reloads) — NO hung board
	 * that needs a physical power-cycle. Once the guest boots and emits
	 * console, wdt_note_progress() (vconsole) feeds the 3-min window. */
	wdt_arm();

	/* aw_mmc IDMAC DMA-coherency fix, part 1 (2026-07-20 rewrite): make EL2
	 * itself a NON-CACHEABLE observer of guest-owned DRAM, instead of the
	 * blanket `dc civac` sweep above (dead code, `if (0)`, WEDGED EMAC even
	 * at top-of-main and is deliberately left disabled/unremoved as a
	 * documented failed attempt). See el2_ncmap.h/.c +
	 * docs/el2-nc-guest-dram.md for the full design.
	 *
	 * WHY HERE, EXACTLY: this must run
	 *   - AFTER el2_install() (line ~107) and wdt_arm() (just above), so a
	 *     stray fault during the table build/switch lands in OUR OWN vector
	 *     table (not U-Boot's) and the HW watchdog is already armed +
	 *     petted through the sweep below (el2_ncmap.c calls wdt_pet()
	 *     periodically) -- both were NOT true of the old dead sweep, which
	 *     ran before either;
	 *   - BEFORE kload_parse_elf/kload_place_segments/kload_build_modinfo
	 *     (right below), so the ~30 MB kernel + DTB + modinfo copy goes
	 *     through the NEW Non-cacheable mapping directly (kload.c's own
	 *     kload_cache_clean_inval() calls become redundant, harmless
	 *     no-ops -- see the doc);
	 *   - BEFORE smp_init() (well below), so CPU1 -- which does not exist
	 *     yet at this point -- captures (in smp_boot_config[], via
	 *     smp_init()'s own unmodified mrs/clean_boot_config() sequence) the
	 *     POST-switch MAIR_EL2/TCR_EL2/TTBR0_EL2 and thus runs under the
	 *     identical NC mapping automatically, with zero smp.c changes.
	 * el2_ncmap_apply() is fail-safe: if any of its runtime assumptions
	 * don't hold it leaves a breadcrumb at 0x50011000 ("NCM1") and returns
	 * WITHOUT switching TTBR0_EL2, so the HV simply keeps running on
	 * U-Boot's original tables exactly as it does today.
	 *
	 * STAGED ROLLOUT: compiled out (DBG_NCMAP_ENABLE=0) for the virtio-blk
	 * retest boot — that test must run with a minimal delta vs the last known
	 * state. Flip to 1 for the direct-aw_mmc coherency test (test #2), once
	 * EMAC/remote-reset is confirmed working so a bad first switch can be
	 * recovered without a physical power-cycle. */
#define DBG_NCMAP_ENABLE 0
#if DBG_NCMAP_ENABLE
	el2_ncmap_apply();
#endif

	if (!kload_parse_elf(K_ELF)) { DBG_BC(1, 0xBAD1); for (;;) { } }
	kload_place_segments(K_ELF, K_PABASE);
	mi = kload_build_modinfo(DTB_SRC, DTB_DST, MODINFO);   /* returns modulep KVA */
	entry = kload_entry_pa();
	DBG_BC(1, 4);
	DBG_BC(6, (uint32_t)entry);

	guest_config();

	/* EL2 SELF-WATCH (opt-in, off unless built with -DEL2_SELFWATCH_ADDR=0x...):
	 * arm a write watchpoint that matches OUR OWN (EL2) stores to one address,
	 * so the hardware names the instruction instead of us guessing which
	 * subsystem wrote a breadcrumb window. Armed HERE — after guest_config(),
	 * before the guest can run — because the writes worth catching happen during
	 * guest bring-up, leaving no window for the host to arm it over EMAC.
	 *
	 * A hit is a current-EL sync exception: el2_trap() records it in flightrec
	 * (FLTR_K_FAULT, a0=ESR a1=ELR), steps ELR past the store, and carries on.
	 * So this costs one flightrec entry per hit and cannot wedge the boot.
	 * Read it back with triage.py and resolve the ELR:
	 *   aarch64-linux-gnu-addr2line -f -e microkernel-dbg.elf <elr> */
#if defined(EL2_SELFWATCH_ADDR) && (EL2_SELFWATCH_ADDR)
	hwbp_set_wp_el2(0, (uint64_t)(EL2_SELFWATCH_ADDR));
	DBG_BC(2, 0x5e1f0000u | 0u);   /* selfwatch armed on WP slot 0 */
#endif

	/* GUEST-KERNEL BREAKPOINT (opt-in: -DGUEST_BP_ADDR=0x...).
	 *
	 * This is how EL2 observes a USERLAND crash, which it cannot do directly:
	 * a null-deref faults in the guest's own stage-1 translation and never
	 * reaches stage-2, and gtrace's VBAR_EL1 trampoline is gone as soon as the
	 * guest installs its own vectors (proved on hardware — see FLTR_K_GFAULT).
	 * So instead: break on the guest KERNEL's signal-delivery path, which does
	 * run at EL1 where a hardware breakpoint can match. Resolve the address
	 * from kernel.debug, e.g.
	 *   aarch64-linux-gnu-nm kernel.debug | grep ' trapsignal$'
	 * -> 0xffff000000529924 for the kernel currently deployed.
	 *
	 * Armed HERE, on CPU0, deliberately: DBGBVR/DBGBCR are banked per PE and
	 * CPU0 is the core that runs the guest. Project memory records hardware
	 * breakpoints as "never fire", armed instead from CPU1 — the same per-PE
	 * banking that made this week's EL2 watchpoint report a confident nothing.
	 *
	 * Safe against the "breakpoint kills the board" history: hwbp_handle() is
	 * one-shot — it records the hit and disarms the slot, so the guest steps
	 * past on eret instead of re-faulting forever. */
#if defined(GUEST_BP_ADDR) && (GUEST_BP_ADDR)
	hwbp_set(0, (uint64_t)(GUEST_BP_ADDR), 0);
	DBG_BC(3, 0x6b700000u);   /* guest breakpoint armed on BP slot 0 */
#endif

#ifndef DBG_NO_TVM
	{ uint64_t vb = gtrace_vbar_el1();
	  __asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(vb)); }
#endif
	stage2_init();
	stage2_enable();
	stage2_unmap_guest_vector();   /* arm first-fault trap: unmap guest EL1
	                                * vector page so the guest's first fault
	                                * takes a stage-2 abort to EL2, exposing
	                                * the ORIGINAL ELR_EL1/ESR_EL1/FAR_EL1. */
	/* A1 isolation self-check: hardware-prove (AT S12E1W) that the guest can't
	 * reach the hv-image / hv-scratch DRAM windows. Result → STG2 breadcrumb
	 * [14..18]; see stage2_isolation_selfcheck(). Runs now that all stage-2
	 * tables (incl. the vector-page split) are final and stage-2 is enabled. */
	stage2_isolation_selfcheck();
	DBG_BC(1, 5);

	/* Zero CNTVOFF_EL2 so the guest's virtual counter CNTVCT_EL0 matches CNTPCT_EL0
	 * and DELAY() doesn't hang. */
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	/* IRQ POLICY (interrupt-virtualization milestone, supersedes the
	 * internal task IMO=0 workaround): EL2 now owns EVERY physical IRQ/FIQ
	 * (HCR_EL2.IMO=1, FMO=1) and forwards each one to the guest through the
	 * vGIC (GICH List Registers, HW mode — vgic.c) instead of routing them
	 * straight to EL1.
	 *
	 * WHY THIS SUPERSEDES, NOT JUST REVERTS, the old policy: the old
	 * IMO=0/FMO=0 policy was itself a documented WORKAROUND for a real bug —
	 * an earlier attempt set IMO=1 to run a preemptive EL2 debug tick and
	 * expected vgic_init() to forward device IRQs into GICH_LR, but that
	 * forwarding was never completed (LRs left empty), so a level-triggered
	 * IRQ taken to EL2 was EOI'd-but-never-cleared and re-fired at ~145 kHz
	 * (EHCI/INTID 106), starving the guest. Reverting to IMO=0 fixed the
	 * storm by letting FreeBSD's own drivers service+deactivate their
	 * interrupts directly — but at the cost of EL2 never seeing (and never
	 * being able to virtualize/inject/multiplex) a single guest interrupt.
	 * The fix here is not "flip IMO back and hope" — it's COMPLETING the
	 * forwarding that was missing before: gic_timer_irq() (gic_timer.c) now
	 * calls vgic_inject_hw() for every physical INTID, tying it (HW=1) to a
	 * guest List Register, so the guest's own virtual EOI is what
	 * deactivates the physical source in hardware — the guest's driver
	 * genuinely services and clears the level condition, same as under
	 * IMO=0, just routed through EL2 instead of bypassing it. Nothing is
	 * silently dropped: vgic_inject_hw()/vgic_maintenance() queue on List-
	 * Register exhaustion instead of discarding (see vgic.c).
	 *
	 * ORDERING (must hold, or this regresses exactly like the two prior
	 * reverts): stage2_init() above already re-added the GICC(0x1c82000)->
	 * GICV(0x1c86000) redirect (stage2.c's stage2_build_mmio_tables()) —
	 * REQUIRED in lockstep with IMO=1, because under IMO=1 the real GICC is
	 * EL2's alone; the guest must ack/EOI through GICV instead. vgic_init()
	 * runs BEFORE the HCR_EL2 write below, and EL2 IRQ/FIQ stays MASKED
	 * until AFTER both vgic_init() and this HCR_EL2 write have completed —
	 * so EL2 never takes a physical IRQ before it has somewhere (a List
	 * Register) and a policy (HW-mode injection) ready to put it.
	 *
	 * dbgmon still does NOT need an EL2 periodic tick: el2_exc.c polls
	 * dbgmon_service() inside vconsole_handle_fault (guest UART0 access),
	 * and wdt_pet() now fires on every EL2 exception — which happens on
	 * every guest device IRQ too, so the watchdog stays fed at least as
	 * well as before. gic_timer_cpuif_init() below only opens the shared
	 * CPU-interface-wide registers (GICD_CTLR/GICC_PMR/GICC_CTLR) vgic
	 * needs; it deliberately does NOT arm this module's own CNTP debug tick
	 * (gic_timer_init()'s other half) — not needed here, and INTID 30 would
	 * otherwise just be one more physical IRQ this policy forwards to the
	 * guest like any other (harmless, but pointless when nothing enables it
	 * at the distributor in this build). */
	gic_timer_cpuif_init();
	vgic_init();
	/* Arm EL2's own CNTP tick after all. The comment above says dbgmon does not
	 * need a periodic tick, and that is still true -- but vtimer_mask_watchdog()
	 * does. gic_timer.c masks the guest's CNTV (VGIC_CNTV_HW=0) before injecting
	 * the virtual tick, and that mask is SELF-LATCHING: masked means no further
	 * CNTV PPI, which means no further chance to inject, so a single injection
	 * that vgic_inject_cntv() gates or drops (all List Registers busy, or a live
	 * vINTID 27 still un-EOIed) costs the guest its timebase for the rest of the
	 * boot. Measured live 2026-07-30: 8047 ticks forwarded fine, then one was
	 * lost mid-ldconfig and the guest idled in WFI forever with a perfectly
	 * healthy kernel. The watchdog is called only from the tick handler, so
	 * without this the recovery it implements is unreachable -- FLTR_K_VTRESCUE
	 * stayed 0 not because the bug never happened but because nothing could run.
	 * Preserving-CNTVOFF variant: see its header comment. */
	gic_timer_arm_preserving_cntvoff(10000u);   /* 10 ms: recovery within ~20 ms */
	{
		uint64_t hcr;
		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr |= (1ull << 4) | (1ull << 3);   /* set IMO(4)=1 and FMO(3)=1 */
		/* TSC (trap guest SMC) UNCHANGED from the prior policy — see the
		 * original 2026-07-19 rationale: FreeBSD reboots via PSCI
		 * SYSTEM_RESET (SMC); trapping it lets el2_exc.c log the fnid
		 * (0x50000200) and, with dbg_block_reset=1, BLOCK the reset (return
		 * PSCI SUCCESS without resetting) so the guest keeps running /
		 * spins, the console survives, and we can see what triggered the
		 * reboot. Non-reset SMCs are still forwarded to real EL3 (x0-x3
		 * preserved via psci_guest_filter()'s whitelist) so PSCI still
		 * behaves. */
		hcr |= (1ull << 19);
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
	}
	/* Unmask EL2 IRQ + FIQ now that vgic_init() and stage2's GICC->GICV
	 * redirect are BOTH in place (see the ordering note above) — from this
	 * instruction on, EL2 takes every physical interrupt and
	 * gic_timer_irq() forwards it via the vGIC. */
	__asm__ volatile("msr daifclr, #3" ::: "memory");
	DBG_BC(1, 6);

	/* NOTE: HW breakpoints/watchpoints/single-step CANNOT catch the guest's
	 * early-boot fault — FreeBSD locore runs with PSTATE.D=1 (debug masked)
	 * until cninit, so debug exceptions never fire. The technique that works
	 * is stage-2 unmapping the guest's exception-vector page so the guest's
	 * fault-handler execution takes a stage-2 instruction abort to EL2,
	 * exposing the ORIGINAL ELR_EL1/ESR_EL1/FAR_EL1. */

	/* onebp disarmed (2026-07-15): the pmap_bootstrap_dmap / FAR=0x1000
	 * blocker was resolved by the kload.h MODINFOMD tag off-by-one fix.
	 * The breakpoint address is on the working boot path now. Leave the
	 * module linked (el2_exc.c's HVC dispatch still calls onebp_handle_hvc). */
	/* onebp_arm(ONEBP_PMAP_DMAP_PA, ONEBP_IMM); */

	/* DEBUGGING SMP bring-up (B): re-enabled under the EARLY watchdog so a crash
	 * here auto-recovers to U-Boot, where SMP breadcrumbs at 0x50000900 (DRAM,
	 * survives the reset) can be read via `md` to localise the fault. The
	 * bracket breadcrumbs below (0x50000e04) tell us whether smp_init returned:
	 *   0x5359_1417 = about to call smp_init
	 *   0x5359_0417 = smp_init returned cleanly
	 * If we see the "about to" value but not the "returned" value post-reset,
	 * smp_init faulted; 0x50000900 shows how far. */
	/* SMP debug core: bring up CPU1 to run dbgmon over EMAC independently of the
	 * guest (the real tooling fix). Bracket breadcrumbs at DBG1[1] (0x50000e04)
	 * + SMP breadcrumbs at 0x50000900 localise any bring-up fault post-reset.
	 *
	 * ROADMAP C2: this SAME call also brings up CPU2, which smp.c's
	 * smp_secondary_main() now dispatches (for cpu==2) into
	 * vblk_async_cpu2_run() (vblk_async.c) instead of parking it in WFI —
	 * the dedicated async eMMC PIO-offload core for vblk_emmc.c's virtio-blk
	 * device (see vblk_async.h for the full mailbox design). It sets
	 * g_vblk_async_ready=1 once its loop is actually running, which is what
	 * lets vblk_kick()'s QueueNotify handler start handing T_IN/T_OUT
	 * requests to CPU2 instead of blocking CPU0 on the eMMC PIO inline.
	 * Nothing else needs to change here — vblk_init() (above) already
	 * brought the eMMC and the virtio-mmio device up before this point;
	 * this just gives CPU2 something useful to do instead of idling. */
#ifdef HV_HDMI
	/* HDMI/HUD integration (ROADMAP B-milestone observability): bring up the
	 * DE2 -> TCON1 -> DWC-HDMI -> PHY -> scanout pipeline and draw the initial
	 * HUD frame on the physical monitor BEFORE the guest starts, so the screen
	 * is live from boot. The framebuffer (HDMI_FB_BASE = 0x4D000000, 8 MiB) is
	 * carved out of the guest's stage-2 map (stage2.c, HV_HDMI) and reserved
	 * no-map in the guest DTB (hv-fb@4d000000), so the guest can neither
	 * allocate over it nor corrupt it. hdmi_init() is bounded (never hangs —
	 * see hdmi.h). Per-frame LIVE refresh runs on the CPU1 debug core
	 * (smp_secondary_main), which owns the display just like it owns the debug
	 * console — CPU0 enters the guest via kload_enter() and never returns. */
	if (hdmi_init() == 0) {
		extern struct el2_frame g_last_guest_frame;  /* el2_exc.c, CPU0-authored */
		hud_init();
		hud_update(&g_last_guest_frame);   /* first frame (guest not yet running) */
	}
#endif

	/* Dual-guest bulk loader (zstage.h): move a second guest's raw ELF from
	 * the low-DRAM TFTP landing window (0x4E000000) into its own private
	 * slice. Weak no-op below, strongly overridden by zstage.c, which is
	 * linked ONLY into the `dual` target -- same weak/strong pattern smp.c
	 * uses for zephyr_cpu3_run()/vblk_async_cpu2_run(), so this line is
	 * provably inert for dbg/gdb/fbsd/zephyr.
	 *
	 * THE POSITION OF THIS CALL IS LOAD-BEARING, not stylistic. The landing
	 * window is inside the FreeBSD guest's own gigabyte, memory FreeBSD may
	 * allocate over the instant it runs. Copying here -- BEFORE smp_init()
	 * brings CPU3 up and BEFORE kload_enter() hands control to FreeBSD --
	 * means no guest on any core has executed a single instruction yet.
	 * Moving it after smp_init(), or doing it on CPU3 itself once it parks,
	 * reintroduces a real race: CPU0 would be free to enter FreeBSD while
	 * CPU3 is still copying. See zstage.h's header comment. */
	zguest_stage_copyin();

	DBG_BC(1, 0x53591417);   /* 'SY..' about to call smp_init */
	smp_init();
	DBG_BC(1, 0x53590417);   /* smp_init returned cleanly */
	DBG_BC(1, 7);

	/* eMMC pinmux fix: mux PC5 (SMHC2/MMC2 CLK) to function 3 (mmc2). FreeBSD's
	 * A64 pinctrl muxes every OTHER mmc2 pin (PC1/PC6/PC8-16 read func 3) but
	 * LEAVES PC5 at gpio_in (0) — so the eMMC clock pad is dead and the eMMC
	 * @0x1c11000 never gets a clock: CMD1 returns OCR=0 -> "No compatible cards".
	 * Confirmed LIVE (2026-07-19): with PC5 forced to func 3, CMD0 -> CMD_DONE
	 * and CMD1 returns OCR=0xc0ff8080 (ready) — the eMMC IS present and answers.
	 * We set it here before the guest boots; if FreeBSD later clobbers PC5 the
	 * CPU1 debug core re-enforces it (see smp.c). PC_CFG0 @ 0x01C20848, PC5 =
	 * nibble 5 (bits[23:20]). */
	{
		volatile uint32_t *pc_cfg0 = (volatile uint32_t *)0x01C20848UL;
		uint32_t v = *pc_cfg0;
		v = (v & ~(0xFu << 20)) | (3u << 20);
		*pc_cfg0 = v;
		__asm__ volatile("dsb sy" ::: "memory");
	}

	/* DIAGNOSTIC (dbg_no_guest): skip entering the guest so ONLY the hypervisor
	 * + CPU1 debug core run. If the board then stays resident and EMAC answers
	 * reliably, the debug-core mechanism is sound and the guest is what disrupts
	 * it; if it still dies, the debug core itself is broken. CPU0 just pets +
	 * wfi so CPU1 owns the board. Restore kload_enter once diagnosed. */
	{
		extern volatile uint32_t dbg_no_guest;
		if (dbg_no_guest) {
			DBG_BC(1, 0x60007);
			for (;;) {
				wdt_pet();
				__asm__ volatile("wfe" ::: "memory");
			}
		}
	}

	/* PAUSE-BEFORE-ENTRY GATE (dbgtools.h HOLD/RELEASE, 2026-07-26): unlike
	 * dbg_no_guest above (a PERMANENT "never enter the guest" diagnostic),
	 * this is a TEMPORARY hold a host tool can RELEASE, meant to let it
	 * plant breakpoints/watch the very first guest instructions instead of
	 * racing the clock. Same wdt_pet()+wfe idiom as dbg_no_guest so the
	 * dead-man's-switch watchdog stays fed while held.
	 *
	 * Default OFF (both words start 0 on a cold boot — see dbgtools_init())
	 * so this is a no-op, byte-for-byte identical to today's boot-straight-
	 * through behavior, on every ordinary chimpd cold-TFTP cycle: nothing
	 * runs early enough in a cold load to arm HOLD before this point, so it
	 * reads 0 and falls straight through to kload_enter() below exactly as
	 * before this change. It ONLY takes effect across a WARM
	 * hv.wdt_reset()-style reload, where a host tool wrote HOLD=1 (dbgmon's
	 * `hold` verb) before triggering the reset and DRAM survives it — see
	 * dbgtools.h/hv_addrmap.h for exactly why. Release with dbgmon's
	 * `release` verb (sets RELEASE nonzero); the host may also just watch
	 * for DBG_BC(1, 0x60008) below over `bc 0x50000e00` to confirm it's
	 * actually held before proceeding. */
	if (dbgtools_hold_get()) {
		DBG_BC(1, 0x60008);
		for (;;) {
			wdt_pet();
			if (dbgtools_release_get())
				break;
			__asm__ volatile("wfe" ::: "memory");
		}
		DBG_BC(1, 0x60009);   /* released -- falling through to kload_enter() */
	}

	kload_enter(entry, mi, SP_EL1);   /* noreturn -> FreeBSD at EL1, we debug it live */
	for (;;) { }
}
