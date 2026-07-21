/* main_fbsd.c — auto-boot the real FreeBSD kernel as an EL1 guest under our
 * EL2 hypervisor, with NO network/REPL/command dependency (the lossy link made
 * a network trigger unreliable). U-Boot (via fbsd-boot.py) TFTPs the kernel ELF
 * to 0x44000000 and the DTB to 0x4a000000, then loads+runs this image at
 * 0x42000000. main() then, immediately and deterministically:
 *   install EL2 vectors -> parse/place the kernel -> build FreeBSD modinfo ->
 *   guest EL1 config + stage-2 identity map -> arm a ~6s WDT -> eret into the
 *   kernel's physical entry at EL1.
 * When the kernel faults (first unmapped MMIO / DTB issue), el2_trap records
 * ESR/ELR/FAR/HPFAR to 0x50000400; the WDT returns us to U-Boot to read it.
 * Own progress breadcrumb at 0x50000e00 ("FBS1") so we can see exactly which
 * step reached (or which one hung) even before the guest runs. */
#include <stdint.h>
#include "wdt.h"
#include "exceptions.h"
#include "kload.h"
#include "stage2.h"
#include "guest.h"
#include "vconsole.h"
#include "gtrace.h"
#include "reboot.h"

#define K_ELF     0x44000000UL   /* raw kernel ELF (TFTP'd here)          */
#define K_PABASE  0x46000000UL   /* contiguous physical load base        */
#define DTB_SRC   0x4a000000UL   /* DTB as TFTP'd by U-Boot (outside map) */
#define DTB_DST   0x47200000UL   /* DTB copied here, just above _end,     */
                                 /* inside the kernel's early map window  */
#define MODINFO   0x47400000UL   /* modinfo scratch, above the reloc'd DTB*/
#define SP_EL1    0x4c000000UL   /* guest EL1 stack top                   */

volatile int dbgmon_call_active = 0;

#define FBS_BC(i, v) do { \
	volatile uint32_t *p = (volatile uint32_t *)(0x50000e00UL + (uint32_t)(i) * 4u); \
	*p = (uint32_t)(v); \
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory"); \
} while (0)

int main(void)
{
	uint64_t mi, entry;

	FBS_BC(0, 0x46425331);   /* "FBS1" */
	FBS_BC(1, 1);            /* stage 1: entered */

	/* Clean-disconnect U-Boot's now-unserviced USB gadget so a later WDT
	 * reset re-enumerates cleanly (no zombie). See reboot.h. */
	usb_gadget_disconnect();

	/* Own the EL2 exception vectors so a guest fault is caught + recorded. */
	el2_install();
	vconsole_init();         /* lay down the UART capture ring @0x50000f00 */
	gtrace_init();           /* HCR_EL2.TVM=1 + VBAR trampoline + trace ring   */
	FBS_BC(1, 2);

	if (!kload_parse_elf(K_ELF)) {
		FBS_BC(1, 0xBAD1);   /* bad kernel ELF at K_ELF */
		for (;;) { }         /* WDT (bring-up 16s) will reset us */
	}
	FBS_BC(1, 3);
	FBS_BC(4, (uint32_t)kload_entry_pa());   /* KVA entry (pre-place) */

	kload_place_segments(K_ELF, K_PABASE);
	FBS_BC(1, 4);
	FBS_BC(5, (uint32_t)kload_kernel_end_pa());

	mi = kload_build_modinfo(DTB_SRC, DTB_DST, MODINFO);   /* returns modulep KVA */
	entry = kload_entry_pa();
	FBS_BC(1, 5);
	FBS_BC(6, (uint32_t)entry);   /* physical eret target */
	FBS_BC(7, (uint32_t)mi);      /* modulep KVA (low word; high word 0xffff0000) */

	guest_config();          /* HCR_EL2.RW=1, SCTLR_EL1 MMU off, VBAR_EL1=0 */
	/* Override the guest's initial VBAR_EL1 with our HVC trampoline so an
	 * early EL1 fault (before the kernel installs its own vectors) traps to
	 * EL2 and gtrace records ELR/ESR/FAR instead of looping invisibly. */
	{
		uint64_t vb = gtrace_vbar_el1();
		__asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(vb));
	}
	
	/* Zero CNTVOFF_EL2 so the guest's virtual counter CNTVCT_EL0 matches CNTPCT_EL0
	 * and DELAY() doesn't hang. */
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	stage2_init();
	stage2_enable();         /* HCR_EL2.VM=1, identity IPA=PA */
	FBS_BC(1, 6);

	/* Arm a ~6s watchdog: when the kernel faults/hangs we return to U-Boot to
	 * read the fault breadcrumb (0x50000400) and this one (0x50000e00). */
	*(volatile uint32_t *)0x01c20cb4UL = 1u;
	*(volatile uint32_t *)0x01c20cb8UL = 0x61u;
	*(volatile uint32_t *)0x01c20cb0UL = 0x14AFu;
	FBS_BC(1, 7);            /* stage 7: about to eret into the kernel */

	kload_enter(entry, mi, SP_EL1);   /* noreturn: -> FreeBSD locore at EL1 */
	for (;;) { }
}
