/* SPDX-License-Identifier: BSD-2-Clause */

/* main_dual2_qemu.c — bzdOS microkernel, QEMU `virt` CI target: dual-guest
 * Step 1, the board-free proof that CPU0 and CPU3 run two INDEPENDENT
 * payloads GENUINELY CONCURRENTLY, under two different stage-2 regimes,
 * before ever attempting real Zephyr or real hardware. Builds directly on
 * top of two already-committed, already-verified pieces of work:
 *
 *   - c78280d (smp-qemu-ci.sh / main_dual_qemu.c, "Step 0"): proved the
 *     REAL, unmodified smp.c/start.S PSCI CPU_ON secondary-core bring-up
 *     works under QEMU, with no guest ever entered. This file reuses that
 *     exact bring-up unchanged (smp_init(), dbg_core_enable=0,
 *     start_secondary_qemu.o) and goes one step further: it actually enters
 *     a guest on TWO of those cores at once.
 *   - 7e5303c (dual-guest): the real Zephyr-on-CPU3 mechanism -- smp.c's
 *     weak/strong zephyr_cpu3_run() hook, zguest_cpu3.c's wfe-poll-for-
 *     `zboot` + one-way boot sequence (zload2_parse_and_place() ->
 *     stage2_zephyr_init/enable() -> isolation selfcheck ->
 *     vconsole_init_chan1() -> guest_config() -> kload_enter()), all
 *     LINKED AND CALLED HERE UNMODIFIED. This file is the first thing in
 *     the tree that ever calls zguest_cpu3_start_set() outside of a live
 *     `zboot` EMAC command on real hardware.
 *
 * ============================================================================
 * WHAT "GENUINELY CONCURRENT" MEANS HERE, AND HOW THIS PROVES IT
 * ============================================================================
 * "Boots, then the other boots" (sequential) is trivial to fake by just
 * printing two banners one after another. The actual claim is that BOTH
 * cores make independent forward progress DURING THE SAME WALL-CLOCK
 * INTERVAL, under two DIFFERENT physical-memory views (CPU0: stage-2
 * disabled entirely, flat physical, guest.c's already-proven
 * guest_demo_el1(); CPU3: stage2_zephyr's own disjoint table, restricting
 * it to its private 32 MiB slice, enforced by an AT S12E1W hardware
 * self-check that HALTS rather than proceeding on any violation -- see
 * stage2_zephyr.c). el2_exc_dual2_qemu.c's tick handler is where that
 * interval is actually measured: it samples BOTH payloads' own breadcrumb
 * counters at tick 2 (~200 ms in) and again at tick 12 (~1.2 s in) and
 * requires BOTH to have strictly increased across that interval before
 * printing PASS. Since CPU0 is the only core that ever takes this trap
 * (CPU3 never touches MMIO/HVC, and its own CNTP is never armed -- see
 * zg3_trivial_payload.S), CPU0's own IRQ-driven sampling can only observe
 * CPU3 having made progress "on its own", off in DRAM, with nothing on
 * CPU0's side ever having driven it there -- which is exactly what
 * concurrent, independent execution looks like from the outside.
 *
 * ============================================================================
 * CPU0's PAYLOAD: guest_demo_el1(), UNCHANGED, ALREADY PROVEN
 * ============================================================================
 * guest.c's own header says it plainly: guest_demo_el1() is "a small
 * self-contained EL1 test payload... spins forever incrementing a
 * breadcrumb counter", already exercised on the real board (main_dbg.c) and
 * on QEMU by main_qemu.c's sibling guest_qemu_payload.c (a thin wrapper
 * around the SAME guest_config()/guest_enter() calls this file makes
 * directly via guest_start_demo()). This target reuses guest.c/guest.h
 * completely unmodified -- no stage-2 either (see guest.h's own header:
 * "NOT FreeBSD -- no stage-2 translation... HCR_EL2.VM=0"), since
 * guest_demo_el1() only ever touches its own DRAM breadcrumb word
 * (GUEST_BC_BASE+4, "GST1" word[1]), never MMIO, so it needs none.
 *
 * ============================================================================
 * CPU3's PAYLOAD: the REAL zguest_cpu3.c code path, against a hand-built
 * trivial ELF (see zg3_trivial_payload.S for why this, not a QEMU-only
 * bypass, and why not real Zephyr yet)
 * ============================================================================
 * zephyr_cpu3_run() (zguest_cpu3.c, linked here as the STRONG override of
 * smp.c's weak default -- see smp.c's own comment on that hook) wfe-parks
 * until zguest_cpu3_start_set() is called, exactly the same as it would
 * wait for dbgmon.c's `zboot` command on real hardware. This file calls
 * that function directly, right after smp_init() returns (which itself
 * bounded-waits for CPU3 to already be parked there), standing in for the
 * EMAC operator command this milestone's real deployment uses -- there is
 * no EMAC/dbgmon under QEMU, and this proof is about the boot MECHANISM,
 * not about the remote-trigger transport.
 *
 * The staged image at ZG3_ELF_STAGE_PA (0xBF000000, computed the same way
 * zguest_cpu3.c computes it: ZSTAGE2_DRAM_BASE + ZSTAGE2_DRAM_SIZE/2) is
 * dual-qemu-ci.sh's own hand-built zg3_trivial_payload.elf, staged via
 * QEMU's generic loader with force-raw=on -- the SAME staging technique
 * zephyr-qemu-ci.sh/snapshot-qemu-ci.sh already use for their own guest
 * images. zload2_parse_and_place() therefore genuinely parses a real
 * ELF64/AArch64 header and program headers out of DRAM, exactly as it would
 * for a real Zephyr image; nothing about that code path is bypassed or
 * special-cased for this test.
 *
 * TWO STAGING ROUTES, ONE BINARY (added with the bulk loader, zstage.c)
 * --------------------------------------------------------------------
 * Staging by QEMU's generic loader stands in for an operator writing the image
 * into DRAM, which on real hardware originally meant dbgmon's single-word `w`
 * command -- fine for this trivial payload's 144 meaningful bytes, hopeless for
 * a real image. The real mechanism is now a third TFTP from U-Boot into a
 * low-DRAM landing window (ZSTAGE_LOW_PA) plus a CPU0-side copy into
 * ZG3_ELF_STAGE_PA before any guest runs -- see zstage.h.
 *
 * This target calls that copy-in unconditionally, in the same position
 * main_dbg.c does, so ONE binary exercises both routes and dual-qemu-ci.sh
 * asserts each in its own pass with identical criteria: load the payload at
 * ZSTAGE_LOW_PA and the copy-in is what puts it where zload2 will find it;
 * load it at ZG3_ELF_STAGE_PA and the copy-in finds garbage at the landing
 * window, copies nothing, and the directly-staged image is untouched. The
 * second pass is therefore a genuine regression guard on the original Step 1
 * route, not a duplicate of the first.
 *
 * ============================================================================
 * -m 2048 IS REQUIRED (see dual-qemu-ci.sh)
 * ============================================================================
 * Zephyr's stage-2 slice (ZSTAGE2_DRAM_BASE=0xBE000000) sits in the HIGH
 * GiB (0x80000000-0xC0000000). QEMU virt's RAM starts at 0x40000000, so
 * that range only physically exists with at least -m 2048 (2 GiB) --
 * snapshot-qemu-ci.sh already established this exact requirement for the
 * same high-GiB window (see that script's own comment).
 */
#include <stdint.h>
#include "exceptions.h"
#include "smp.h"
#include "pl011_qemu.h"
#include "gic_timer_qemu.h"
#include "guest.h"
#include "zguest_cpu3.h"
#include "zstage.h"

/* smp.c's own "ISOLATION TEST flag" -- see main_dual_qemu.c (Step 0) for the
 * full rationale; reused verbatim here for the same reason: CPU1's
 * board-only EMAC/dbgmon/WDOG/USB MMIO does not exist under QEMU. */
extern volatile uint32_t dbg_core_enable;

#define SMP_BC(i) (*(volatile uint32_t *)((uintptr_t)SMP_BC_BASE + (uint32_t)(i) * 4u))

static void
dual2_qemu_poweroff(void) __attribute__((noreturn));

static void
dual2_qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull; /* PSCI SYSTEM_OFF */

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

long
main(void)
{
	uint32_t online, bc_online;

	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- QEMU virt dual-guest Step 1 proof ===\n");
	pl011_puts("HV: entered at EL2, MMU off (see start_qemu.S)\n");

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	/* See file banner: skip CPU1's board-only EMAC/dbgmon/WDOG/USB MMIO
	 * under QEMU. smp.c's own existing runtime switch, unmodified. */
	dbg_core_enable = 0;
	pl011_puts("HV: dbg_core_enable=0 (CPU1 debug-core MMIO skipped under QEMU -- smp.c unmodified)\n");

	/* Dual-guest bulk loader (zstage.h), the REAL one main_dbg.c calls, in
	 * the REAL position: before smp_init() brings CPU3 up and before any
	 * guest runs. It moves a raw guest ELF from the low-DRAM landing window
	 * that U-Boot TFTPs into (ZSTAGE_LOW_PA) to the staging address
	 * zload2_parse_and_place() reads (ZG3_ELF_STAGE_PA).
	 *
	 * This target now exercises BOTH staging routes from one binary, which
	 * is what lets dual-qemu-ci.sh assert them in two passes with identical
	 * criteria:
	 *   - payload loaded at ZSTAGE_LOW_PA  -> this call finds it and copies
	 *     it, proving the whole new bulk-loader chain;
	 *   - payload loaded at ZG3_ELF_STAGE_PA (the original Step 1 route) ->
	 *     this call finds DRAM garbage at the landing window, copies
	 *     nothing, and the directly-staged image is still there untouched.
	 * Copying nothing is deliberately not an error -- see zstage.h. */
	pl011_puts("HV: zguest_stage_copyin() (zstage.c) -- move a staged guest ELF "
	           "from the low-DRAM TFTP landing window into CPU3's slice\n");
	zguest_stage_copyin();

	pl011_puts("HV: calling the REAL smp_init() (smp.c, unmodified) -- PSCI CPU_ON for cores 1..3\n");
	smp_init();

	online    = smp_num_online();
	bc_online = SMP_BC(1);
	pl011_puts("HV: smp_num_online()=");
	pl011_put_udec(online);
	pl011_puts(" online-bitmap=0x");
	pl011_put_hex32(bc_online);
	pl011_puts("\n");

	if (online != SMP_MAX_CPUS || bc_online != 0xFu) {
		/* Step 0's own smp-qemu-ci.sh is the isolated diagnostic for
		 * exactly this failure mode; this target does not re-derive
		 * it, just refuses to proceed into a guest on a core that
		 * never came up. */
		pl011_puts("DUAL-QEMU-CI2: FAIL (PSCI CPU_ON bring-up did not complete -- "
		           "see smp-qemu-ci.sh for isolated diagnosis)\n");
		dual2_qemu_poweroff();
	}

	pl011_puts("HV: CPU3 is parked in zguest_cpu3.c's wfe-poll loop (real, strong zephyr_cpu3_run())\n");
	pl011_puts("HV: kicking CPU3's REAL one-way boot sequence: zload2_parse_and_place() -> "
	           "stage2_zephyr_init/enable() -> isolation selfcheck -> vconsole_init_chan1() -> "
	           "guest_config() -> kload_enter()\n");
	zguest_cpu3_start_set();

	gic_timer_qemu_init(100000u);   /* 100 ms tick, same period as qemu-ci.sh */
	pl011_puts("HV: GICv2 timer armed (INTID 30 / CNTP, 100 ms period) on CPU0 only\n");

	__asm__ volatile("msr daifclr, #3" ::: "memory");   /* unmask IRQ + FIQ on CPU0 */
	pl011_puts("HV: IRQ/FIQ unmasked on CPU0 -- dropping to EL1's guest_demo_el1() "
	           "(guest.c, already proven by qemu-ci.sh/the real board) now\n");

	/* guest.c's guest_config()/guest_enter(): reused completely unmodified.
	 * noreturn: eret to EL1. From here CPU0 alternates between "running
	 * guest_demo_el1()" and "servicing the EL2 tick" (el2_exc_dual2_qemu.c),
	 * which is where the actual before/after sampling and PASS/FAIL verdict
	 * happen. */
	guest_start_demo();

	for (;;)
		__asm__ volatile("wfi");
}
