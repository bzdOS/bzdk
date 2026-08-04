/* SPDX-License-Identifier: BSD-2-Clause */

/* main_zephyr_qemu.c — boot a real Zephyr RTOS image as an EL1 guest under
 * this hypervisor ON QEMU `virt`, with no physical board involved.
 *
 * WHY THIS FILE EXISTS. Two targets in this tree were each half of the
 * answer and neither was the whole one:
 *
 *   - `make zephyr` (main_zephyr.c) boots a real Zephyr image, but only on
 *     the real Banana Pi M64: it TFTPs the guest ELF, arms the A64 hardware
 *     watchdog by poking 0x01c20cb0, and reports progress into breadcrumb
 *     words you need the EMAC debug channel to read. Nothing about it can
 *     be checked without the board.
 *   - `make qemu` (main_qemu.c) needs no board, but its guest is
 *     guest_qemu_payload.c: 40 lines of hand-written EL1 code that prints
 *     to QEMU's PL011. It proves stage-2 + EL1 entry + a timer tick; it
 *     proves nothing about loading a real guest OS image.
 *
 * This file is the intersection: main_zephyr.c's actual guest-loading
 * sequence (kload_parse_elf -> kload_place_segments -> guest_config ->
 * stage2 -> kload_enter), run on a target a CI script can execute. What it
 * therefore proves, board-free, is exactly the part main_qemu.c could not:
 * that kload.c can parse and place a genuine third-party OS image, and that
 * that image runs at EL1 under our stage-2 map and reaches its own main().
 *
 * THE GUEST IMAGE IS THE SAME FILE THAT GETS FLASHED. Not a QEMU-shaped
 * stand-in, not a second board port: zephyr-qemu-ci.sh runs the very
 * `zephyr-guest/build.sh bpi_m64_hv` artifact that U-Boot TFTPs to the real
 * board, byte for byte. That is worth spelling out because it was not
 * obvious it would work — the Allwinner MMIO addresses baked into that image
 * are not QEMU virt addresses. Two separate reasons it does work:
 *
 *   - UART0 (0x01c28000) is never mapped through to the memory system at
 *     all. bzdOS's stage-2 tables leave exactly that 4 KiB page INVALID (see
 *     stage2.h's UART0_BASE and its table-topology comment), so Zephyr's
 *     ns16550 driver accesses take a stage-2 data abort into EL2 and are
 *     emulated by vconsole.c. Whether QEMU has a device there is irrelevant.
 *     This is the real console mechanism, not a stand-in, and it is the
 *     single most valuable thing this target exercises.
 *   - The GIC (0x01c81000) IS identity-mapped, so guest accesses do go out
 *     to the memory system, where QEMU virt has nothing. Measured on QEMU
 *     10.1.5: those accesses are silently dropped (reads return 0) rather
 *     than raising an external abort, so Zephyr's arm_gic_init() completes
 *     and boot continues. Nothing in this build ever ENABLES a GIC
 *     interrupt (tickless board, polled UART — see the guest board's
 *     Kconfig.defconfig), so a black-hole GIC changes no observable
 *     behaviour here. BE HONEST ABOUT THE LIMIT THIS IMPLIES: a passing run
 *     says nothing about GIC programming, and the day the guest gets a real
 *     tick (vgic + the GICC-IPA -> GICV redirect) this target will need a
 *     GIC that answers.
 *
 * ALSO IDENTICAL TO THE BOARD PATH:
 *
 *   1. The guest ELF is staged in DRAM at physical 0x44000000 and parsed
 *      from there — the same address, and the same "the loader put a file in
 *      RAM, now go find it" contract, that U-Boot's `tftpboot 0x44000000
 *      zephyr.elf` sets up on hardware (see zephyr-guest/LOADING.md §2).
 *      Under QEMU that staging is done by
 *      `-device loader,file=zephyr.elf,addr=0x44000000,force-raw=on`, which
 *      is a pure memory-image drop with no ELF interpretation — QEMU does
 *      NOT parse it, kload.c does, exactly as on the board.
 *   2. kload.c, stage2.c, guest.c, vconsole.c and exceptions.S are used
 *      COMPLETELY UNMODIFIED, and are the same objects the board build
 *      links.
 *
 * WHAT DIFFERS FROM main_zephyr.c, AND WHY:
 *
 *   a. No hardware watchdog arming (the A64 WDOG at 0x01c20cb0 does not
 *      exist under QEMU). The wrapping `timeout` in zephyr-qemu-ci.sh plays
 *      that role.
 *   b. No gtrace/VBAR_EL1 preload, no usb_gadget_disconnect(), no
 *      ZEP_BC breadcrumbs as the primary progress channel: this target has
 *      a real console (PL011), so progress is printed where a CI script can
 *      read it. The breadcrumb writes are kept anyway (same window, same
 *      stage numbering as main_zephyr.c) so the two boot paths stay
 *      diffable and a memory dump reads identically.
 *
 * Paired with el2_exc_zephyr_qemu.c, which is where the guest's emulated
 * console bytes are echoed to the PL011 and where the PASS marker is
 * decided.
 */
#include <stdint.h>
#include "exceptions.h"
#include "kload.h"
#include "stage2.h"
#include "guest.h"
#include "vconsole.h"
#include "pl011_qemu.h"

/* Staging address of the raw Zephyr ELF — identical to main_zephyr.c's
 * Z_ELF, see file header point 1. */
#define Z_ELF        0x44000000UL

/* Physical load base == the ELF's own p_vaddr (the guest board's
 * memory@51000000 node), so kload_place_segments()'s KVA relocation
 * degenerates to an identity copy — see main_zephyr.c's header point 4. */
#define Z_PABASE     0x51000000UL

/* Zephyr's __reset computes and installs its own SP_EL1 before any C code
 * runs, so whatever we hand it is overwritten immediately — see
 * main_zephyr.c's header point 3. */
#define SP_EL1_PLACEHOLDER 0x51000000UL

/* Same breadcrumb window and stage numbering as main_zephyr.c ("ZEP1" at
 * 0x50008000), kept purely so the board and QEMU boot paths produce an
 * identical memory picture — see file header point b. Under QEMU this is
 * plain RAM (virt's DRAM starts at 0x40000000). */
#define ZEP_BC(i, v) do { \
	volatile uint32_t *p = (volatile uint32_t *)(0x50008000UL + (uint32_t)(i) * 4u); \
	*p = (uint32_t)(v); \
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory"); \
} while (0)

/* stage2_at_check() records PAR_EL1 here — same addresses main_qemu.c reads
 * back for its own self-check line (see stage2.h). */
#define S2_AT_PAR_LO (*(volatile uint32_t *)0x50000c1cUL)

/* PSCI SYSTEM_OFF, so a failure exits QEMU instead of hanging until the
 * script's timeout. Duplicated from el2_exc_zephyr_qemu.c rather than shared
 * through a header: three lines, and neither file should have to include the
 * other's interface just to die. */
static void qemu_poweroff(void) __attribute__((noreturn));

static void
qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull;

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

static void die(const char *why) __attribute__((noreturn));

static void
die(const char *why)
{
	pl011_puts("ZEPHYR-QEMU-CI: FAIL — ");
	pl011_puts(why);
	pl011_puts("\n");
	qemu_poweroff();
}

long
main(void)
{
	uint64_t entry;

	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- Zephyr EL1 guest on QEMU virt ===\n");

	ZEP_BC(0, 0x5a455031);   /* "ZEP1" */
	ZEP_BC(1, 1);

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	/* Must precede stage2_enable(): once the UART0 page is unmapped the
	 * guest can fault on it at any time, and vconsole_handle_fault()
	 * needs the capture ring's header laid down first (vconsole.h). */
	vconsole_init();
	pl011_puts("HV: vconsole 16550 trap-emulator armed (guest console = 0x01c28000)\n");
	ZEP_BC(1, 2);

	/* --- Load the guest, using kload.c completely unmodified --------- */
	if (!kload_parse_elf(Z_ELF)) {
		ZEP_BC(1, 0xBAD1);
		die("no valid guest ELF staged at 0x44000000 (did QEMU get -device loader?)");
	}
	ZEP_BC(1, 3);
	pl011_puts("HV: guest ELF at 0x44000000 parsed (kload_parse_elf)\n");

	if (!kload_place_segments(Z_ELF, Z_PABASE)) {
		ZEP_BC(1, 0xBAD2);
		die("kload_place_segments() rejected the guest ELF");
	}
	ZEP_BC(1, 4);
	ZEP_BC(5, (uint32_t)kload_kernel_end_pa());

	entry = kload_entry_pa();
	ZEP_BC(1, 5);
	ZEP_BC(6, (uint32_t)entry);
	pl011_puts("HV: segments placed at 0x51000000, entry_pa=0x");
	pl011_put_hex64(entry);
	pl011_puts("\n");

	/* --- Guest CPU state + stage-2, both reused verbatim ------------- */
	guest_config();

	/* Zephyr reads CNTVCT even with its architected-timer driver disabled
	 * (k_busy_wait's calibration path and arch_timing_* helpers touch it);
	 * a nonzero CNTVOFF_EL2 left over from firmware would make the guest's
	 * view of the virtual counter jump. Same one-liner main_zephyr.c does,
	 * same reason. */
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	stage2_init();
	stage2_enable();
	pl011_puts("HV: stage-2 identity map programmed + enabled (HCR_EL2.VM=1)\n");

	stage2_at_check(STAGE2_SELFTEST_IPA);
	if ((S2_AT_PAR_LO & 1u) != 0u)
		die("stage-2 AT S12E1R self-check failed (PAR_EL1.F=1)");
	pl011_puts("HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)\n");
	ZEP_BC(1, 6);

	pl011_puts("HV: eret to EL1 -- everything after this, prefixed [guest], is Zephyr\n");
	ZEP_BC(1, 7);

	/* IRQ/FIQ stay MASKED at EL2 on purpose: this build arms no timer and
	 * the guest enables no interrupt source (tickless board, polled UART
	 * — see the guest board's Kconfig.defconfig), so there is nothing to
	 * take and unmasking would only widen the failure surface. */
	kload_enter(entry, 0 /* x0: don't-care for Zephyr, see main_zephyr.c */,
	            SP_EL1_PLACEHOLDER /* likewise */);
	for (;;)
		__asm__ volatile("wfi");
}
