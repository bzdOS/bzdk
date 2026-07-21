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
#include "onebp.h"
#include "vgic.h"
#include "vblk_emmc.h"
#include "el2_ncmap.h"

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
	DBG_BC(1, 5);

	/* Zero CNTVOFF_EL2 so the guest's virtual counter CNTVCT_EL0 matches CNTPCT_EL0
	 * and DELAY() doesn't hang. */
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	/* IRQ POLICY (fix internal task, EHCI INTID 106 storm): this is a THIN, single-
	 * guest debug hypervisor — EL2 has no reason to own physical interrupts.
	 * Route every physical IRQ straight to the guest's EL1 (HCR_EL2.IMO=0,
	 * FMO=0) so FreeBSD's native drivers service AND deactivate their level-
	 * triggered device IRQs (e.g. EHCI@0x01c1b000 = SPI 74 = INTID 106).
	 *
	 * Previously gic_timer_init() set IMO=1 to run a preemptive EL2 debug tick,
	 * and vgic_init() was supposed to forward device IRQs into GICH_LR — but the
	 * forwarding was never completed, so a level-triggered IRQ taken to EL2 was
	 * EOI'd-but-never-cleared and re-fired at ~145 kHz, starving the guest and
	 * wedging it in ehci_reset()'s DELAY(). See memory ehci-intid106-storm.
	 *
	 * dbgmon stays live WITHOUT an EL2 tick: el2_exc.c polls dbgmon_service()
	 * inside vconsole_handle_fault (guest UART0 access), as in the 21:21 build.
	 *
	 * NB: no vgic_init(), no gic_timer_init(), and IRQ stays MASKED at EL2
	 * (no daifclr) — belt-and-suspenders so EL2 never intercepts a guest IRQ. */
	{
		uint64_t hcr;
		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr &= ~((1ull << 4) | (1ull << 5));   /* clear IMO(4) and FMO(5) */
		/* TSC (trap guest SMC) RE-ENABLED 2026-07-19: the guest DOES reboot from
		 * userland (root mounts, rc runs, then a full SoC reset wipes the vconsole
		 * ring — can't see the cause). FreeBSD reboots via PSCI SYSTEM_RESET (SMC);
		 * trap it so el2_exc.c logs the fnid (0x50000200) and, with dbg_block_reset=1,
		 * BLOCKS the reset (returns PSCI SUCCESS w/o resetting) — the guest keeps
		 * running / spins, the console survives, and we finally SEE what triggered the
		 * reboot. The old cpufreq-breakage concern is moot: DVFS is stripped from the
		 * DTB now, boot already passes cpufreq, and non-reset SMCs are forwarded to
		 * real EL3 (x0-x3 preserved) so PSCI still behaves. */
		hcr |= (1ull << 19);
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
	}
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
	 * + SMP breadcrumbs at 0x50000900 localise any bring-up fault post-reset. */
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

	kload_enter(entry, mi, SP_EL1);   /* noreturn -> FreeBSD at EL1, we debug it live */
	for (;;) { }
}
