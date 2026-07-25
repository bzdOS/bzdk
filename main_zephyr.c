/* main_zephyr.c — auto-boot a standalone Zephyr RTOS image (board: bpi_m64_hv,
 * see zephyr-guest/boards/bzdos/bpi_m64_hv/) as an EL1 guest under our EL2
 * hypervisor, replacing FreeBSD for this boot (see main_fbsd.c, whose flow
 * this file deliberately mirrors almost step for step). U-Boot TFTPs the
 * Zephyr ELF to 0x44000000 (the same staging address main_fbsd.c already
 * uses for the FreeBSD kernel ELF — chosen for exactly the same reason: well
 * clear of both stage-2 exclusion windows, see stage2.h) and loads+runs this
 * image at 0x42000000, same as every other main_*.c in this tree.
 *
 * WHAT'S DIFFERENT FROM main_fbsd.c, AND WHY (this is the concrete answer to
 * "is guest.c/kload.c really guest-kernel-agnostic?" the roadmap asked for):
 *
 *   1. NO DTB, NO MODINFO. FreeBSD's boot protocol needs a relocated DTB
 *      plus a loader-built "modinfo" metadata blob, with x0 = a KVA pointing
 *      at it (see kload_build_modinfo()'s big comment in kload.c/kload.h).
 *      Zephyr has no runtime devicetree at all — its board.dts is compiled
 *      into the image at build time (see zephyr-guest/boards/bzdos/
 *      bpi_m64_hv/bpi_m64_hv.dts's file header) — so none of that exists
 *      here. kload_build_modinfo() is simply never called.
 *
 *   2. x0 AT ENTRY IS A DON'T-CARE. Zephyr's own reset code
 *      (arch/arm64/core/reset.S, __reset/__reset_prep_c) never reads x0.
 *      So unlike main_fbsd.c's kload_enter(entry, mi, SP_EL1) (mi = modinfo
 *      KVA in x0, load-bearing), we pass 0.
 *
 *   3. THE LOADER-SUPPLIED INITIAL SP_EL1 IS ALSO A DON'T-CARE. FreeBSD's
 *      locore.S expects the loader to hand it a usable stack. Zephyr's own
 *      __reset computes ITS OWN initial SP internally
 *      (`ldr x24, =(z_interrupt_stacks + __z_interrupt_stack_SIZEOF)`) and
 *      overwrites SP_EL1 with it before any C code runs — whatever
 *      kload_enter() wrote there first is clobbered immediately. Passed as
 *      SP_EL1_PLACEHOLDER below purely so the call site reads sensibly; its
 *      actual value has been confirmed (by reading reset.S) to not matter.
 *
 *   4. kload_parse_elf()/kload_place_segments() NEEDED NO CHANGES. Zephyr's
 *      ELF is a plain EXEC (not position-independent) with a single PT_LOAD
 *      whose p_vaddr IS the real physical load address (0x51000000 — see
 *      the board's memory@51000000 node) — i.e. kernbase == pa_base here,
 *      so the "KVA-relocate to pa_base" placement kload.c was written for
 *      FreeBSD degenerates to a plain identity copy for Zephyr. Same
 *      function, same call sequence, zero code changes.
 *
 *   5. THE GUEST CPU-STATE CONTRACT (guest_config(): HCR_EL2.RW=1, flat
 *      SCTLR_EL1, EL1h/D/A-masked SPSR_EL2) and STAGE-2 IDENTITY MAP
 *      (stage2_init()/stage2_enable()) ARE COMPLETELY UNCHANGED — reused
 *      verbatim, no Zephyr-specific branches anywhere in either. This is
 *      the actual proof that guest.c/stage2.c are guest-OS-agnostic, not
 *      FreeBSD-specific: the only guest-specific code in this entire file
 *      is "don't build a modinfo blob" and "use a different ELF load/place
 *      address than main_fbsd.c does".
 *
 * KNOWN GAP, LEFT DELIBERATELY UNSOLVED HERE (see the board's
 * Kconfig.defconfig for the guest-side half of this): the Zephyr build for
 * bpi_m64_hv runs CONFIG_SYS_CLOCK_EXISTS=n / CONFIG_ARM_ARCH_TIMER=n — it
 * does not depend on any interrupt reaching EL1 at all. Wiring up the
 * virtual-timer tick (vgic.c already knows how to inject it — see
 * VGIC_VTIMER_INTID — but that needs a stage-2 GICC-IPA -> GICV-physical
 * redirect, and stage2.c is explicitly frozen for a parallel milestone in
 * this pass) is NOT done by this file either: vgic_init() is deliberately
 * never called here, exactly like main_fbsd.c doesn't call it today.
 */
#include <stdint.h>
#include "wdt.h"
#include "exceptions.h"
#include "kload.h"
#include "stage2.h"
#include "guest.h"
#include "vconsole.h"
#include "gtrace.h"
#include "reboot.h"

#define Z_ELF        0x44000000UL   /* raw Zephyr ELF (TFTP'd here) — same
                                      * staging address main_fbsd.c uses for
                                      * the FreeBSD kernel ELF; nothing else
                                      * in this file is resident there at
                                      * boot time so the reuse is safe. */
#define Z_PABASE     0x51000000UL   /* == the ELF's own p_vaddr (== board's
                                      * memory@51000000 node) -- identity
                                      * placement, see file header point 4. */
#define SP_EL1_PLACEHOLDER 0x51000000UL   /* see file header point 3: never
                                            * actually used by Zephyr. */

/* Stand-in for dbgmon.c's global of the same name -- identical to
 * main_fbsd.c's own copy right next to it (see main_gdb.c's file header for
 * the full rationale). el2_exc.c's cmd_call() fault-recovery path
 * references this `extern` unconditionally from every build; this build
 * never links dbgmon.o, so it's simply always false here -- a no-op. */
volatile int dbgmon_call_active = 0;

/* Breadcrumb window: 0x50008000 ("ZEP1"). Distinct from every other window
 * already in this tree (see guest.c's header comment for the existing list
 * up to 0x50000f10 / vconsole.c's ring above that) — chosen well clear of
 * all of them, still inside the hv-scratch window (0x50000000..0x50200000)
 * so it survives the same way every other breadcrumb in this tree does. */
#define ZEP_BC(i, v) do { \
	volatile uint32_t *p = (volatile uint32_t *)(0x50008000UL + (uint32_t)(i) * 4u); \
	*p = (uint32_t)(v); \
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory"); \
} while (0)

int main(void)
{
	uint64_t entry;

	ZEP_BC(0, 0x5a455031);   /* "ZEP1" */
	ZEP_BC(1, 1);            /* stage 1: entered */

	usb_gadget_disconnect();

	el2_install();
	vconsole_init();
	gtrace_init();
	ZEP_BC(1, 2);

	if (!kload_parse_elf(Z_ELF)) {
		ZEP_BC(1, 0xBAD1);   /* bad Zephyr ELF at Z_ELF */
		for (;;) { }
	}
	ZEP_BC(1, 3);

	if (!kload_place_segments(Z_ELF, Z_PABASE)) {
		ZEP_BC(1, 0xBAD2);
		for (;;) { }
	}
	ZEP_BC(1, 4);
	ZEP_BC(5, (uint32_t)kload_kernel_end_pa());

	entry = kload_entry_pa();
	ZEP_BC(1, 5);
	ZEP_BC(6, (uint32_t)entry);   /* physical eret target, expect 0x5100100c */

	guest_config();
	{
		uint64_t vb = gtrace_vbar_el1();
		__asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(vb));
	}
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	stage2_init();
	stage2_enable();
	ZEP_BC(1, 6);

	/* Same ~6s bring-up watchdog as main_fbsd.c: on a hang/fault, U-Boot
	 * gets control back to read this breadcrumb + gtrace's fault record. */
	*(volatile uint32_t *)0x01c20cb4UL = 1u;
	*(volatile uint32_t *)0x01c20cb8UL = 0x61u;
	*(volatile uint32_t *)0x01c20cb0UL = 0x14AFu;
	ZEP_BC(1, 7);

	kload_enter(entry, 0 /* x0: don't-care, see file header point 2 */,
	            SP_EL1_PLACEHOLDER /* don't-care, see file header point 3 */);
	for (;;) { }
}
