/* SPDX-License-Identifier: BSD-2-Clause */

/* gtrace.h — guest early-locore tracing for the bzdOS EL2 hypervisor.
 *
 * The real FreeBSD/arm64 kernel we load as an EL1 guest runs ~6s and then a
 * WDT resets us, having made ZERO UART accesses and ZERO EL2 faults — i.e.
 * it is stuck somewhere in its own early locore (most likely bringing up
 * its EL1 MMU, or looping in its own vector table on an early exception
 * that never reaches EL2). This file provides two independent hypervisor
 * instruments to make that invisible hang visible:
 *
 *   1. HCR_EL2.TVM trap-and-emulate of the guest's SCTLR_EL1/TTBRn_EL1/
 *      TCR_EL1/ESR_EL1/FAR_EL1/AFSR{0,1}_EL1/MAIR_EL1/AMAIR_EL1/
 *      CONTEXTIDR_EL1 accesses (ESR_EL2.EC==0x18, "trapped MSR/MRS/system").
 *      This is exactly the register sequence locore writes while enabling
 *      its own MMU — trapping it tells us how far locore got.
 *
 *   2. A guest VBAR_EL1 trampoline: before FreeBSD installs its own vector
 *      table, any EL1 exception it takes goes to VBAR_EL1 (0 by default —
 *      a silent bogus-address loop). We install a 2KB-aligned 16-entry
 *      table of bare `hvc #<vector index>` instructions instead, so an
 *      early guest fault becomes a visible EL2 trap (ESR_EL2.EC==0x16)
 *      instead of an invisible spin.
 *
 * Both instruments record into a single cache-coherent trace ring in DRAM
 * (see layout below) so a post-WDT-reset `md.l` can read exactly what the
 * guest was doing right before it got stuck.
 *
 * Freestanding: no libc, <stdint.h> only, no FP/SIMD (matches
 * -mgeneral-regs-only). All DRAM stores use the same cache-coherent
 * pattern as the rest of this tree: *p = v; dc civac, p; dsb sy.
 */
#ifndef BZDOS_GTRACE_H
#define BZDOS_GTRACE_H

#include <stdint.h>
#include "exceptions.h"

/* ---------------------------------------------------------------------
 * Trace ring — fixed DRAM window at 0x50001000, magic "GTRC".
 *
 * *** INTEGRATION NOTE (read before wiring) ***
 * This base address was specified as 0x50001000. That range OVERLAPS the
 * existing vconsole UART-capture buffer: VCONSOLE_BUF_BASE = 0x50000f10,
 * VCONSOLE_BUF_SIZE = 3072, i.e. vconsole owns 0x50000f10..0x50001b10.
 * The gtrace ring below (0x50001000 .. 0x50001210, 132 words) sits
 * squarely inside that window. Since this file only owns gtrace.c/.h, the
 * collision is flagged here rather than silently worked around: whoever
 * wires main_fbsd.c should either relocate one of the two windows (e.g.
 * move GTRACE_BASE past 0x50001b10, or shrink/relocate vconsole's buffer)
 * or accept that heavy guest UART traffic and gtrace events will stomp on
 * each other in the same run. Given the guest makes ZERO UART accesses in
 * the failing case this is meant to diagnose, the two are unlikely to
 * collide in practice for *this* bug — but it's a landmine for later.
 *
 * Distinct from every other breadcrumb window already in this tree: MUSB
 * 0x50000000, EMAC 0x50000100, REPL 0x50000300, EL2 exceptions 0x50000400,
 * jitter/TIMR 0x50000500, ring 0x50000600, alloc 0x50000700, GIC timer
 * 0x50000800, sched 0x50000a00, guest 0x50000b00, stage2 0x50000c00,
 * kload 0x50000d00, main_fbsd 0x50000e00, vconsole 0x50000f00.
 *
 * Word layout (uint32_t each, offset in words from GTRACE_BASE):
 *   [0]  magic        GTRACE_MAGIC ("GTRC" = 0x47545243)
 *   [1]  event_count  total events recorded. Once the log fills
 *                      (GTRACE_MAX_EVENTS), further events still bump this
 *                      counter but are NOT stored — so a post-mortem dump
 *                      always shows the FIRST GTRACE_MAX_EVENTS events,
 *                      i.e. the earliest (most diagnostic) activity.
 *   [2]  last_sctlr   low 32 bits of the last value the guest wrote to
 *                      SCTLR_EL1 via the TVM trap (bit0 = M = the guest's
 *                      own MMU-enable bit) — watch this to see whether,
 *                      and with what value, locore reached MMU-on.
 *   [3]  fault_count  number of guest EL1 vector events recorded via the
 *                      VBAR_EL1 trampoline (instrument 2) — a nonzero
 *                      value means the guest took its OWN exception before
 *                      it ever got as far as installing its real VBAR_EL1.
 *   [4..] event log: GTRACE_MAX_EVENTS entries x 2 words (tag, value).
 *
 *     SYSREG event (TVM trap, ESR_EL2.EC==0x18) — one event, 2 words:
 *       tag  bit31    = 0                    (SYSREG event)
 *            bit30    = direction: 0 = write (MSR emulated),
 *                                  1 = read  (MRS emulated)
 *            bits[7:0]= GTRACE_REG_* id below, or GTRACE_REG_UNKNOWN (0xff)
 *                       for an encoding not in the known TVM-trappable set
 *       value = low 32 bits of the value written (write) or read (read)
 *
 *     HVC vector event (guest EL1 exception -> our VBAR trampoline,
 *     ESR_EL2.EC==0x16) — ALWAYS 2 consecutive event slots (4 words),
 *     recorded back-to-back so they always pair up in the log:
 *       slot A tag: bit31=1, bit29=0            ("info" half)
 *                   bits[27:24] = EL1 vector index (0..15, the HVC imm)
 *                   bits[13:8]  = ESR_EL1.EC (6 bits) of the guest's fault
 *              value = ELR_EL1 low 32 bits (where the guest was executing
 *                       when it took the exception)
 *       slot B tag: bit31=1, bit29=1            ("far" half)
 *                   bits[27:24] = same vector index (pairs with slot A)
 *              value = FAR_EL1 low 32 bits (fault address, meaningful for
 *                       abort vectors; ignore for e.g. SError/undef)
 * --------------------------------------------------------------------- */
#define GTRACE_BASE        0x50002000UL
#define GTRACE_MAGIC       0x47545243u   /* "GTRC" */
#define GTRACE_MAX_EVENTS  64u

/* ---------------------------------------------------------------------
 * FIRST-FAULT LATCH (instrument 2b) — a ONE-SHOT freeze of the guest's
 * complete state on the VERY FIRST EL1 synchronous exception that reaches
 * our VBAR_EL1 trampoline (gtrace_handle_hvc). The gtrace event ring above
 * keeps a rolling log, but under the recursive-exception storm that motivates
 * this file the FIRST fault's PC (ELR_EL1) and FAR_EL1 are what matter, and
 * they are quickly masked by thousands of nested re-faults. This latch grabs
 * them once and never re-arms, so a post-mortem read always shows the
 * ORIGINAL fault, not the storm.
 *
 * Fixed DRAM window at 0x50002400, magic "FFL1". 76 words (0x130 bytes),
 * i.e. 0x50002400..0x50002530 — clear of the gtrace ring below it
 * (0x50002000..0x50002210) and the single-step ring above it (0x50002800).
 *
 * Word layout (uint32_t each, offset in words from GTRACE_FF_BASE):
 *   [0]      magic  GTRACE_FF_MAGIC ("FFL1")
 *   [1]      valid  0 while being written / not yet triggered, 1 once frozen
 *   [2]      vec    trampoline vector index (HVC imm, 0..15) that fired
 *   [3]      reserved (0)
 *   [4],[5]  ESR_EL1  (lo, hi)  — the first fault's syndrome (EC/ISS)
 *   [6],[7]  ELR_EL1  (lo, hi)  — guest PC at the first fault (KEY DATUM)
 *   [8],[9]  FAR_EL1  (lo, hi)  — faulting VA (KEY DATUM for aborts)
 *   [10],[11] SP_EL1  (lo, hi)  — guest SP at the fault
 *   [12],[13] SPSR_EL1 (lo, hi) — guest PSTATE at the fault
 *   [14 + i*2, +1] x[i] (lo, hi) for i = 0..30  (GPRs at the first fault)
 * --------------------------------------------------------------------- */
#define GTRACE_FF_BASE   0x50002400UL
#define GTRACE_FF_MAGIC  0x46464C31u   /* "FFL1" */

/* GTRACE_REG_* — ids for the ~11 registers HCR_EL2.TVM traps to EL2 as
 * EC==0x18 writes/reads (see gtrace_handle_sysreg() for the exact
 * Op0/Op1/CRn/CRm/Op2 encodings matched against each). */
enum {
	GTRACE_REG_SCTLR_EL1      = 0,
	GTRACE_REG_TTBR0_EL1      = 1,
	GTRACE_REG_TTBR1_EL1      = 2,
	GTRACE_REG_TCR_EL1        = 3,
	GTRACE_REG_ESR_EL1        = 4,
	GTRACE_REG_FAR_EL1        = 5,
	GTRACE_REG_AFSR0_EL1      = 6,
	GTRACE_REG_AFSR1_EL1      = 7,
	GTRACE_REG_MAIR_EL1       = 8,
	GTRACE_REG_AMAIR_EL1      = 9,
	GTRACE_REG_CONTEXTIDR_EL1 = 10,
	GTRACE_REG_UNKNOWN        = 0xFF,
};

/* Set HCR_EL2.TVM=1 (read-modify-write, preserving VM/RW/IMO/etc — must
 * NOT clobber stage2_enable()'s HCR_EL2.VM or guest_config()'s HCR_EL2.RW),
 * isb; zero + re-magic the trace ring; build the VBAR_EL1 trampoline (see
 * gtrace_vbar_el1()). Call this ONCE, before the guest ever runs — HCR.TVM
 * must be set before entry or the guest's early SCTLR_EL1/TTBRn_EL1/...
 * writes go through untrapped and this instrument sees nothing. */
void gtrace_init(void);

/* Handle an ESR_EL2.EC==0x18 trap (trapped MSR/MRS to a TVM-covered EL1
 * system register) taken from the EL1 guest. Decodes ISS Op0/Op1/CRn/CRm/
 * Op2/Direction/Rt, performs the REAL access on the guest's behalf (the
 * trap means the access did not actually happen), records it into the
 * trace ring, advances frame->elr by 4 (skip the trapped instruction) and
 * returns 1 (handled). Caller (el2_trap) should dispatch here for guest
 * (kind>>2==2) sync exceptions with EC==0x18, before the generic fault
 * record, and return without falling through when this returns 1. */
int gtrace_handle_sysreg(struct el2_frame *frame);

/* Handle an ESR_EL2.EC==0x16 (HVC) trap taken from the EL1 guest via our
 * VBAR_EL1 trampoline (see gtrace_vbar_el1()). ISS[15:0] is the HVC
 * immediate = which of the 16 EL1 vector-table entries fired. Reads the
 * guest's EL1 fault state (ELR_EL1/ESR_EL1/FAR_EL1/SPSR_EL1 — EL2 can read
 * EL1 registers directly, no trap), records (vector, ELR_EL1, ESR_EL1.EC,
 * FAR_EL1) into the trace ring as two paired event slots, and returns 1
 * (handled). Does NOT advance frame->elr and does NOT attempt to resume
 * the guest — an early, un-vectored EL1 fault usually means the guest is
 * genuinely stuck; recording it is the point, and the ~6s WDT will reset
 * the board so the ring can be read with `md.l`. Caller (el2_trap) should
 * dispatch here for guest (kind>>2==2) sync exceptions with EC==0x16,
 * before the generic fault record. */
int gtrace_handle_hvc(struct el2_frame *frame);

/* Physical/virtual address of the 2KB-aligned, 16-entry (0x80 bytes each)
 * EL1 vector table gtrace_init() built out of bare `hvc #<index>`
 * instructions — one per architectural vector (sync/irq/fiq/serr x
 * sp0/spx/lower-64/lower-32). The integrator does:
 *     msr vbar_el1, <gtrace_vbar_el1()>
 * before entering the guest, so ANY EL1 exception the guest takes before
 * it installs its own VBAR_EL1 traps to EL2 (as HVC, EC==0x16) instead of
 * silently looping at address 0. */
uint64_t gtrace_vbar_el1(void);

#endif /* BZDOS_GTRACE_H */
