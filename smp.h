/* smp.h — SMP bring-up for the bzdOS EL2 microkernel (AArch64, Allwinner A64 /
 * BPI-M64 = 4x Cortex-A53). RTOS primitive: light up the other 3 A53 cores via
 * PSCI CPU_ON, give each its own EL2 environment (stack, VBAR, MMU/caches, CNTP
 * tick) and let the (now SMP) scheduler in sched.c run tasks on all of them.
 *
 * -----------------------------------------------------------------------------
 * WHY THE SECONDARY PATH ENABLES THE MMU (read before touching start.S)
 * -----------------------------------------------------------------------------
 * The PRIMARY (CPU0) runs with the MMU + D-cache ON — U-Boot set them up and
 * `go 0x42000000` handed control to us in that state (see start.S / PROJECT.md).
 * All of our shared state — the scheduler run-queue, the SCHD breadcrumb, every
 * .bss global — therefore lives in *cacheable* Normal memory as far as CPU0 is
 * concerned. A core brought up by PSCI CPU_ON, by contrast, starts with the MMU
 * OFF, and with the stage-1 MMU disabled EVERY data access is Device /
 * non-cacheable regardless of SCTLR.C. A secondary running MMU-off would thus
 * see an INCOHERENT view of the run-queue CPU0 built in its cache — the whole
 * SMP scheduler would be reading/writing past each other. So the secondary MUST
 * come up with the SAME translation regime as the primary (identical
 * MAIR/TCR/TTBR0/SCTLR) so its accesses are cacheable Normal too, and the A53
 * SCU keeps the D-caches coherent in hardware — but ONLY once CPUECTLR.SMPEN=1
 * (below). We do not build our own page tables: we CAPTURE the primary's
 * MMU/EL config in smp_boot_config[] (smp_init(), on CPU0) and each secondary
 * reloads it verbatim in _start_secondary before turning its own MMU on.
 *
 * -----------------------------------------------------------------------------
 * SMPEN / cache coherency (the A53 gotcha the task called out)
 * -----------------------------------------------------------------------------
 * On Cortex-A53 the data cache does NOT participate in the inner-shareable
 * coherency domain until CPUECTLR_EL1.SMPEN (bit 6) is set. If a secondary
 * enables its D-cache (SCTLR.C via the MMU) WITHOUT SMPEN=1 first, its cached
 * lines are private and never snooped — silent corruption of every shared
 * structure. _start_secondary therefore sets SMPEN=1 (with a dsb+isb) BEFORE it
 * touches SCTLR. CPU0 already had SMPEN set by U-Boot/BL31; secondaries set
 * their own (it is a per-core register).
 */
#ifndef BZDOS_SMP_H
#define BZDOS_SMP_H

/* --- constants shared between C and start.S (plain macros only) ------------ */
#define SMP_MAX_CPUS     4            /* A64 = 4x Cortex-A53, affinity aff0 0..3 */
#define SMP_DEBUG_CPU    1            /* CPU1 dedicated to EMAC/dbgmon (survives guest wedge) */
#define SMP_STACK_SIZE   0x4000       /* 16 KiB per-core EL2/idle/exception stack */
#define SMP_TICK_US      1000u        /* per-core CNTP preemption tick period    */

/* smp_boot_config[] indices — the EL2/MMU sysregs a secondary must restore to
 * match the primary. Kept as a flat uint64_t array so start.S can index it by
 * a plain byte offset (INDEX*8) without knowing a C struct layout. */
#define SMP_CFG_MAIR     0            /* MAIR_EL2   */
#define SMP_CFG_TCR      1            /* TCR_EL2    */
#define SMP_CFG_TTBR0    2            /* TTBR0_EL2  */
#define SMP_CFG_VBAR     3            /* VBAR_EL2   */
#define SMP_CFG_HCR      4            /* HCR_EL2    */
#define SMP_CFG_SCTLR    5            /* SCTLR_EL2 (enables MMU+caches; set LAST) */
#define SMP_CFG_N        6

/* Heartbeat / status breadcrumb — DRAM window 0x50000900, magic "SMP1".
 * Chosen clear of every other window in this tree: MUSB 0x50000000, EMAC
 * 0x50000100, REPL 0x50000300, EXC 0x50000400, TIMR 0x50000500, RING 0x50000600,
 * ALLOC 0x50000700, GICT 0x50000800, SCHED 0x50000a00, GUEST 0x50000b00,
 * STAGE2 0x50000c00, KLOAD 0x50000d00, dbg/fbsd 0x50000e00, vconsole 0x50000f00,
 * GTRACE 0x50001000/0x50002000, FFL1 0x50002400, SST1 0x50002800, HDMI 0x50003000.
 *
 *   [0]      magic 0x534D5031 ("SMP1")
 *   [1]      online bitmap: bit i set once CPU i entered smp_secondary_main()
 *   [2..4]   PSCI CPU_ON return code for cores 1,2,3 (0 = success, <0 = error)
 *   [5]      reserved
 *   [6..9]   per-CPU heartbeat counter, cpu0..3 (bumped every sched tick)  <-- WATCH THESE
 *   [10..13] per-CPU current task id, cpu0..3 (-1 = idle)
 *   [14..17] per-CPU "reached _start_secondary asm" marker, cpu0..3
 *            (0xA5A50000|cpuid, written MMU-off before the MMU is enabled;
 *             lets you tell "PSCI accepted but died in MMU bring-up" apart
 *             from "never started")
 * To verify all 4 cores are alive from the network REPL, `bc 0x50000900` and
 * watch words [6..9] (byte offsets 0x18..0x24) all incrementing. */
/* NB: no integer-suffix on SMP_BC_BASE — start.S uses it in assembler
 * expressions (movz/movk halves), where a C 'UL' suffix would not parse. */
#define SMP_BC_BASE      0x50000900
#define SMP_BC_MAGIC     0x534D5031u  /* "SMP1" */

#ifndef __ASSEMBLER__

#include <stdint.h>
#include "exceptions.h"

/* Captured primary EL2/MMU config; secondaries reload it in _start_secondary.
 * Non-static so the assembler entry can reference the symbol. */
extern uint64_t smp_boot_config[SMP_CFG_N];

/* Per-core EL2/idle/exception stacks. _start_secondary (start.S) only ever
 * indexes rows 1..SMP_MAX_CPUS-1 (stack_top(cpu) = &smp_stacks[cpu]
 * [SMP_STACK_SIZE], cpu = 1..3) -- row 0 is CPU0's OWN stack: _start (start.S,
 * H2(a) stack hardening, 2026-07-24) switches to smp_stacks[0]'s top for the
 * entire resident phase, immediately after saving U-Boot's inherited SP and
 * before any C code (including el2_install()) runs, and switches back only
 * right before the final `ret` to U-Boot. This is what closes the
 * "unprotected EL2 stack" gap: smp_stacks[] is a plain .bss array inside the
 * image, already covered by stage2.c's HVIMG_L2_IDX exclusion, whereas
 * U-Boot's inherited stack lives in plain identity-mapped guest DRAM with no
 * carve-out. See start.S's header comment for the full rationale. */
extern uint8_t smp_stacks[SMP_MAX_CPUS][SMP_STACK_SIZE];

/* Assembly secondary entry point (start.S); passed to PSCI CPU_ON as the
 * physical entry address (VA==PA identity under U-Boot's tables). */
extern void _start_secondary(void);

/* This core's id = MPIDR_EL1 affinity 0 (0..3 on the single-cluster A64). */
static inline uint32_t smp_cpu_id(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
	return (uint32_t)(v & 0xffu);
}

/* --- IRQ mask save/restore (for task-context critical sections) ------------ */
static inline uint64_t smp_irq_save(void)
{
	uint64_t d;
	__asm__ volatile("mrs %0, daif" : "=r"(d));
	__asm__ volatile("msr daifset, #2" ::: "memory"); /* mask IRQ (I) */
	return d;
}

static inline void smp_irq_restore(uint64_t d)
{
	__asm__ volatile("msr daif, %0" :: "r"(d) : "memory");
}

/* --- Ticket-free test-and-set spinlock (ldaxr/stlxr, WFE/SEV) --------------- *
 * Acquire has ldaxr (load-acquire) semantics, release has stlr (store-release):
 * everything written inside the critical section is published before the lock
 * word is cleared, and nothing from after the acquire is speculated before it.
 * WFE in the spin lets a waiting core sleep until the SEV in spin_unlock (or any
 * event) nudges it, so contention doesn't burn the bus. */
typedef struct { volatile uint32_t lock; } spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l)
{
	uint32_t tmp;
	__asm__ volatile(
		"	sevl\n"              /* prime the local event so 1st wfe falls through */
		"1:	wfe\n"
		"2:	ldaxr	%w0, [%1]\n"     /* load-acquire current lock value */
		"	cbnz	%w0, 1b\n"       /* held? go back to sleep on wfe */
		"	stxr	%w0, %w2, [%1]\n"/* try to store 1 */
		"	cbnz	%w0, 2b\n"       /* store failed (lost race)? retry */
		: "=&r"(tmp)
		: "r"(&l->lock), "r"(1u)
		: "memory");
}

static inline void spin_unlock(spinlock_t *l)
{
	__asm__ volatile(
		"	stlr	wzr, [%0]\n"     /* store-release 0: publish the critical section */
		"	sev\n"                   /* wake any core spinning in wfe */
		:
		: "r"(&l->lock)
		: "memory");
}

/* -----------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Bring up the other cores. Runs on CPU0: captures the primary EL2/MMU config,
 * cleans it to the PoC (so secondaries can read it MMU-off), then PSCI CPU_ON
 * for affinities 1..3 pointed at _start_secondary. Bounded-waits for them to
 * report online. Safe no-op fields for cores that fail to start (their online
 * bit just never sets; their CPU_ON return code is in the breadcrumb). Call
 * AFTER el2_install() and the primary gic_timer_init(), and BEFORE the primary
 * unmasks IRQs / calls sched_start(). */
void smp_init(void);

/* Number of cores that have reported online (including CPU0). */
uint32_t smp_num_online(void);

/* C entry for a secondary, called from _start_secondary with the MMU already
 * ON and coherent. Sets up this core's CNTP tick, unmasks IRQ, then idles in
 * WFI — the timer tick drives sched_tick(), which pulls a ready task and erets
 * into it. Never returns. */
void smp_secondary_main(uint64_t cpuid) __attribute__((noreturn));

/* Per-core CNTP tick handler for SECONDARY cores. gic_timer.c's gic_timer_irq()
 * keeps single-core module state (one shared deadline) and is used by CPU0 only;
 * secondaries call this instead so each has an INDEPENDENT deadline with no
 * cross-core races. Call it from el2_trap()'s IRQ arm for cpu != 0 (see the
 * integration snippet in the delivery notes). */
void smp_timer_irq(struct el2_frame *frame);

/* Bump this core's heartbeat + publish (current task) to the SMP breadcrumb.
 * Called once per tick from sched_tick() for EVERY core (so CPU0, which uses
 * gic_timer_irq() not smp_timer_irq(), still shows a heartbeat). */
void smp_heartbeat(uint32_t cpu, int current_task);

#endif /* __ASSEMBLER__ */
#endif /* BZDOS_SMP_H */
