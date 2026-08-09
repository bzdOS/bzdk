/* SPDX-License-Identifier: BSD-2-Clause */

/* main_dual_qemu.c — bzdOS microkernel, QEMU `virt` CI target proving the
 * EXISTING, UNMODIFIED PSCI CPU_ON multi-core bring-up (smp.c's smp_init() /
 * smp_secondary_main(), start.S's _start_secondary) actually works with no
 * physical board attached. This is Step 0 of a larger dual-guest effort
 * being built on top of it elsewhere — this target proves ONLY the
 * foundation: 4 vCPUs, PSCI CPU_ON'd, all landing in smp_secondary_main().
 * It does not enter any guest, arm any timer, or unmask any interrupt.
 *
 * ============================================================================
 * THE HOLE THIS TARGET EXISTS TO FILL
 * ============================================================================
 * Every existing QEMU CI target in this tree (qemu-ci.sh, vgic-qemu-ci.sh,
 * zephyr-qemu-ci.sh, snapshot-qemu-ci.sh, linux-qemu-ci.sh) runs with exactly
 * ONE vCPU. None of them link smp.o at all (grep the Makefile: smp.o appears
 * in DBG_OBJS/GDB_OBJS/FBSD_OBJS/REPL_OBJS/ZEPHYR_OBJS/HDMI_OBJS/STAGE0/NET,
 * i.e. every REAL-HARDWARE target, and not one *_QEMU_OBJS list). So nothing
 * board-free has ever proven that smp.c's PSCI CPU_ON bring-up — the exact
 * mechanism a planned dual-guest feature needs to start a second guest's
 * vCPU(s) on — works at all outside the one physical Banana Pi M64.
 *
 * ============================================================================
 * WHY start.S's _start_secondary NEEDED NO CHANGES (empirically confirmed
 * below, not just argued)
 * ============================================================================
 * start.S's own header explains why the secondary path enables the MMU: CPU0
 * runs with U-Boot's MMU already on, so every shared global lives in
 * cacheable Normal memory from CPU0's point of view, and a secondary coming
 * up MMU-off would see an incoherent view of it — hence _start_secondary
 * reloads CPU0's captured MAIR/TCR/TTBR0/VBAR/HCR/SCTLR (smp_boot_config[])
 * and enables its own MMU to match, before touching any shared state.
 *
 * Under QEMU there is no U-Boot and start_qemu.S's own header says so
 * explicitly: "MMU is left OFF for the entire run... every access is Device
 * memory... this isn't a shortcut, it's simply everything is uncached". So
 * THIS file (CPU0's entry) never turns its own EL2 stage-1 MMU on either —
 * it does exactly what every other QEMU target's main_*_qemu.c does
 * (el2_install() for the vector table; nothing else EL2-MMU-related).
 * smp_init() therefore captures SCTLR_EL2 with M=0 into smp_boot_config[],
 * and when a secondary (started by PSCI CPU_ON — architecturally always
 * MMU-off, VBAR-at-reset, on an undefined SP, REGARDLESS of any other core's
 * state; this is true on real hardware AND under QEMU identically) reloads
 * that captured SCTLR_EL2 value and writes it "last", the write is a
 * faithful reload that happens to leave M=0 — the MMU stays off, matching
 * CPU0. No page tables, no TTBR0 validity requirement, nothing to get wrong.
 * The board's whole cache-coherency concern (SMPEN, cacheable Normal
 * mappings) simply does not arise when nothing is cached in the first place.
 * See start_secondary_qemu.S's header for the full point-by-point argument.
 *
 * RESULT: start.S's _start_secondary routine required ZERO logic changes.
 * The only reason it isn't linked verbatim from start.S is that start.S ALSO
 * defines `_start` (the real-board U-Boot entry, a different boot contract
 * start_qemu.S already owns for this family of targets) — a straight link
 * would be a duplicate `_start` symbol. start_secondary_qemu.S is therefore a
 * byte-for-byte copy of start.S's _start_secondary in its own translation
 * unit, purely to resolve that name collision; see that file's header for
 * why a copy (not a shared #include, not a rewrite) was the right call here.
 *
 * ============================================================================
 * WHAT THIS FILE ACTUALLY DOES
 * ============================================================================
 *   1. pl011_init() + el2_install() — the same minimal EL2 setup every other
 *      QEMU target does (see main_vgic_qemu.c / main_snapshot_qemu.c). No
 *      stage-2, no guest, no GIC/timer: this proof doesn't need any of them,
 *      and skipping them keeps the object list (and the set of things that
 *      could go wrong) as small as the claim being tested.
 *   2. Sets smp.c's OWN existing `dbg_core_enable` diagnostic flag to 0
 *      BEFORE calling smp_init() — see smp.c's comment on that flag: "Default
 *      0 = ...; Set to 1 ... the debug core runs EMAC/dbgmon normally (the
 *      production path)" [sic — the default in smp.c is 1; this target
 *      deliberately overrides it]. This is NOT a change to smp.c's dispatch
 *      logic (smp.c is byte-for-byte unmodified) — it is using a runtime
 *      switch smp.c's own author built for exactly this purpose ("ISOLATION
 *      TEST... skip EMAC/dbgmon on CPU1") from a new caller, the same way
 *      bmc.c already does (`extern volatile uint32_t dbg_core_enable;`) for
 *      its own unrelated purpose. Without this, CPU1's smp_secondary_main()
 *      dispatch would poke Allwinner-A64-only MMIO (the WDOG, a PIO pinmux
 *      register) that QEMU virt has nothing at that address for — this
 *      target's proof is about PSCI CPU_ON reachability, not about surviving
 *      unbacked MMIO writes, so it turns that class of confounding failure
 *      off the same way wdt_qemu_stub.c/smp_qemu_stub.c turn off the others.
 *   3. Calls smp_init() — the REAL, unmodified function (smp.h) — exactly as
 *      main_dbg.c/main_gdb.c/main_zephyr.c already do on real hardware: after
 *      el2_install(), nothing else needed here since there is no primary
 *      timer to sequence against.
 *   4. Reads back smp_num_online() (the existing accessor smp.h already
 *      exposes) plus the raw SMP1 breadcrumb (SMP_BC_BASE, smp.h's own
 *      documented layout: word[1] online bitmap, words[2..4] PSCI CPU_ON
 *      return codes for cores 1..3, words[14..17] each core's
 *      "reached _start_secondary asm" stage marker) for a diagnosable
 *      PASS/FAIL rather than a bare boolean.
 *   5. Prints one greppable verdict line and cleanly exits QEMU via PSCI
 *      SYSTEM_OFF (same convention as every sibling target).
 *
 * NOT wired into ci.sh / Makefile's default targets yet — this is the
 * standalone Step-0 proof; the later dual-guest step wires its own gate in
 * once it exists on top of this foundation.
 */
#include <stdint.h>
#include "exceptions.h"
#include "smp.h"
#include "pl011_qemu.h"

/* smp.c's own "ISOLATION TEST flag" (see smp.c's header comment on it and
 * this file's own banner above). Declared extern here exactly the way
 * bmc.c already declares the same and sibling flags for its own purpose —
 * smp.h deliberately does not expose these as a public API (they are
 * described as diagnostic/opt-in knobs, not part of smp_init()'s contract),
 * so an extern declaration at the point of use is this tree's existing
 * convention, not a new one. */
extern volatile uint32_t dbg_core_enable;

#define SMP_BC(i) (*(volatile uint32_t *)((uintptr_t)SMP_BC_BASE + (uint32_t)(i) * 4u))

static void
dual_qemu_poweroff(void) __attribute__((noreturn));

static void
dual_qemu_poweroff(void)
{
	register uint64_t x0 __asm__("x0") = 0x84000008ull; /* PSCI SYSTEM_OFF */

	__asm__ volatile("smc #0" :: "r"(x0) : "memory");
	for (;;)
		__asm__ volatile("wfi");
}

long
main(void)
{
	uint32_t online, bc_online, rc1, rc2, rc3, stage0, stage1, stage2, stage3;
	uint32_t cpu;

	pl011_init();
	pl011_puts("\n=== bzdOS microkernel -- QEMU virt SMP/PSCI CPU_ON proof (dual-guest Step 0) ===\n");
	pl011_puts("HV: entered at EL2, MMU off (see start_qemu.S)\n");

	el2_install();
	pl011_puts("HV: EL2 vector table installed (exceptions.S, unchanged)\n");

	/* See file banner point 2: skip CPU1's board-only EMAC/dbgmon/WDOG/USB
	 * MMIO under QEMU. smp.c's own existing runtime switch, unmodified. */
	dbg_core_enable = 0;
	pl011_puts("HV: dbg_core_enable=0 (CPU1 debug-core MMIO skipped under QEMU -- smp.c unmodified)\n");

	pl011_puts("HV: calling the REAL smp_init() (smp.c, unmodified) -- PSCI CPU_ON for cores 1..3\n");
	smp_init();

	online    = smp_num_online();       /* existing accessor, smp.h */
	bc_online = SMP_BC(1);              /* online bitmap breadcrumb  */
	rc1 = SMP_BC(2);                    /* PSCI CPU_ON rc, core 1    */
	rc2 = SMP_BC(3);                    /* PSCI CPU_ON rc, core 2    */
	rc3 = SMP_BC(4);                    /* PSCI CPU_ON rc, core 3    */
	stage0 = SMP_BC(14);                /* _start_secondary stage marker, cpu0 (never written -- CPU0 doesn't run it) */
	stage1 = SMP_BC(15);                /* cpu1 */
	stage2 = SMP_BC(16);                /* cpu2 */
	stage3 = SMP_BC(17);                /* cpu3 */

	pl011_puts("HV: smp_num_online()=");
	pl011_put_udec(online);
	pl011_puts(" online-bitmap=0x");
	pl011_put_hex32(bc_online);
	pl011_puts("\n");

	pl011_puts("HV: PSCI CPU_ON rc: cpu1=");
	pl011_put_hex32(rc1);
	pl011_puts(" cpu2=");
	pl011_put_hex32(rc2);
	pl011_puts(" cpu3=");
	pl011_put_hex32(rc3);
	pl011_puts("\n");

	pl011_puts("HV: _start_secondary stage markers (0xa5a5_<stage>_<cpuid>, stage 0x04=reached smp_secondary_main): ");
	pl011_puts("cpu1=0x");
	pl011_put_hex32(stage1);
	pl011_puts(" cpu2=0x");
	pl011_put_hex32(stage2);
	pl011_puts(" cpu3=0x");
	pl011_put_hex32(stage3);
	pl011_puts("\n");
	(void)stage0;

	for (cpu = 1; cpu < SMP_MAX_CPUS; cpu++) {
		pl011_puts("HV: cpu");
		pl011_put_udec(cpu);
		pl011_puts(" online=");
		pl011_puts((bc_online & (1u << cpu)) ? "yes" : "NO");
		pl011_puts("\n");
	}

	if (online == SMP_MAX_CPUS && bc_online == 0xFu) {
		pl011_puts("DUAL-QEMU-CI: PASS (PSCI CPU_ON brought up all 3 secondaries; "
		           "all 4 cores incl. CPU0 reported online via smp_secondary_main())\n");
	} else {
		pl011_puts("DUAL-QEMU-CI: FAIL (expected online=4 bitmap=0xf; see cpuN online=NO / "
		           "PSCI rc / stage markers above for which core and where it stopped)\n");
	}

	dual_qemu_poweroff();
}
