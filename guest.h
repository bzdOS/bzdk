/* guest.h — EL1 guest-execution infrastructure for bzdOS becoming a Type-1
 * hypervisor on the Allwinner A64 (Cortex-A53).
 *
 * We run at EL2 (see exceptions.h / exceptions.S: our EL2 vector table is
 * installed via el2_install() and every exception, including ones taken FROM
 * a lower EL, lands in el2_trap() with a full saved context). This file adds
 * the other half: dropping to EL1 to run guest code, and doing it in a way
 * that our EL2 CNTP timer tick (already delivered as an EL2 IRQ because
 * gic_timer.c sets HCR_EL2.IMO=1 — see gic_timer.c's big comment) transparently
 * PREEMPTS the guest and returns control to EL2 on every tick. That preemption
 * is the core Type-1-hypervisor proof this file exists to demonstrate.
 *
 * FIRST MILESTONE ONLY: a small self-contained EL1 test payload
 * (guest_demo_el1) that spins forever incrementing a breadcrumb counter, to
 * prove (a) EL1 code executes and (b) the EL2 tick keeps regaining control.
 * This is NOT FreeBSD — no stage-2 translation, no vGIC, no device model, no
 * DTB. See the roadmap comment at the bottom of guest.c for what a real
 * guest kernel additionally needs; the hooks for that are named and
 * documented but intentionally inert here (VM=0 in HCR_EL2, VTCR_EL2/
 * VTTBR_EL2 untouched).
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only, flat physical
 * addressing (host EL2 MMU state is untouched by this file; the guest's own
 * SCTLR_EL1 has its MMU off, so guest virtual == physical, sharing DRAM with
 * the host — fine for a test payload, NOT fine for a real guest OS image
 * that expects its own address space, which is why stage-2 is future work).
 */
#ifndef BZDOS_GUEST_H
#define BZDOS_GUEST_H

#include <stdint.h>

/* Program EL2/EL1 system registers for a minimal, non-virtualized-memory,
 * AArch64 EL1 guest:
 *   - HCR_EL2: RW=1 (EL1 is AArch64, not AArch32), VM=0 (stage-2 OFF — flat
 *     first cut; see VTCR_EL2/VTTBR_EL2 hook note in guest.c), IMO left
 *     exactly as gic_timer.c set it (=1, so physical IRQs, i.e. our tick,
 *     keep being routed to EL2 no matter what runs at EL1 — THIS is the
 *     preemption mechanism and must never be cleared here).
 *   - SCTLR_EL1: MMU off (M=0), caches off, only architectural RES1 bits
 *     set — flat physical execution for the guest, matching the "no
 *     stage-2 yet" cut.
 *   - VBAR_EL1: 0 for now (documented hook — a real guest installs its own
 *     table; our milestone payload never traps to EL1 by design, it either
 *     spins or takes an HVC which is routed to EL2, not EL1).
 * Safe to call multiple times (idempotent register writes). Does NOT touch
 * HCR_EL2.IMO, does NOT mask EL2 IRQs, does NOT touch the host's own EL2
 * MMU/VBAR_EL2 — none of this can block the EL2 tick.
 */
void guest_config(void);

/* Drop to EL1 and never return (guests don't return to their caller — the
 * only way back to EL2 is via a trap, e.g. our timer tick or an HVC).
 * Sets SP_EL1 = sp_el1, ELR_EL2 = entry, SPSR_EL2 = EL1h with IRQ/FIQ
 * unmasked (harmless: HCR_EL2.IMO routes physical IRQs to EL2 regardless of
 * the EL1 mask bits — see guest.c), then `eret`. Execution continues at
 * `entry`, running at EL1, with SP_EL1 = sp_el1.
 */
void guest_enter(uint64_t entry, uint64_t sp_el1) __attribute__((noreturn));

/* Small self-contained EL1 test payload. Runs at EL1 (entered via
 * guest_enter). Repeatedly increments the guest breadcrumb loop counter
 * (word[1] at GUEST_BC_BASE) so EL2/the host can observe EL1 making
 * progress, and spins forever — it is deliberately interrupted by the EL2
 * timer tick (preemption), which is the point. Never returns. Periodically
 * issues `hvc #0` (see guest.c) as an optional guest->EL2 hypercall
 * demonstration; el2_trap's existing lower-EL SYNC path (group 2, ESR.EC ==
 * 0x16) already handles this generically (records + skips the instruction),
 * no changes needed there for that alone.
 */
void guest_demo_el1(void) __attribute__((noreturn));

/* One-call entry point for the REPL: guest_config(); pick a private stack;
 * guest_enter((uint64_t)guest_demo_el1, stack_top). After this call returns
 * control has already left EL2 for EL1 — by construction this function
 * itself never returns to its caller either (guest_enter doesn't return),
 * except via the timer-tick trap path, which resumes at the `eret` inside
 * el2_common, NOT inside guest_start_demo(). Callers (e.g. the REPL command
 * handler) should treat invoking this as a one-way trip for the CPU: after
 * it, the machine alternates between "running the EL1 guest" and "servicing
 * the EL2 tick", forever, by design.
 */
void guest_start_demo(void) __attribute__((noreturn));

/* Call from el2_trap (in el2_exc.c, owned by the integration lane) whenever
 * a lower-EL AArch64 exception group is seen for an IRQ/FIQ, i.e.
 * (kind >> 2) == 2 — that is precisely "the EL2 timer tick just preempted
 * the EL1 guest". Increments the guest breadcrumb preemption counter
 * (word[2]) so a live `bc 0x50000b00` shows it climbing. Cheap (one
 * cache-coherent store), safe to call from IRQ context, does not touch the
 * frame or ELR/SPSR — purely observational.
 */
void guest_note_preempt(void);

#endif /* BZDOS_GUEST_H */
