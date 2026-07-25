/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgmon.h — LIVE hypervisor debug monitor for the bzdOS EL2 hypervisor.
 *
 * WHY THIS EXISTS: the guest (a real FreeBSD/EL1 kernel) is preempted every
 * CNTP tick (HCR_EL2.IMO=1 routes the physical timer IRQ to EL2). That
 * preemption lands in el2_common/el2_trap with a FULL snapshot of the guest
 * context already saved into a `struct el2_frame` (see exceptions.h) --
 * x0..x30, ELR_EL2 (== the guest's PC), SPSR_EL2 (== guest PSTATE), etc.
 *
 * dbgmon plugs into that instant: the tick handler calls dbgmon_service()
 * with the just-saved frame, we service a tiny non-blocking command console
 * over the same console_* hooks repl.c uses (which main_dbg.c wires to the
 * EMAC network console), and then return so the guest resumes exactly where
 * it left off. Because this happens every tick, the "debugger" is live and
 * interactive without ever stopping, resetting, or single-stepping the
 * guest -- you just read a fresh `gr`/`sr`/`gva`/... snapshot each time the
 * tick fires (typically far faster than a human can type).
 *
 * IMPORTANT: dbgmon_service() runs ON THE TICK PATH, i.e. in interrupt
 * context with the guest fully preempted but nothing else suspended (no
 * scheduler, no watchdog handling happens here). It MUST be non-blocking and
 * bounded: it drains whatever console input is already buffered, and if that
 * completes a line, executes exactly one command and replies -- then always
 * returns. It never spins waiting for more input. Console TX is likewise
 * bounded: replies are built from short, fixed-size formatting calls (no
 * unbounded loops other than caller-supplied lengths on `r`/`rb`/`d`, which
 * the command parser itself should be given sane values for -- this module
 * does not impose an additional cap beyond what fits one service() call's
 * time budget, matching repl.c's own dump commands).
 */
#ifndef BZDOS_DBGMON_H
#define BZDOS_DBGMON_H

#include "exceptions.h"

/* Print the one-line startup banner + first prompt. Call this exactly once,
 * after the console hooks are wired up, before the guest starts ticking. */
void dbgmon_init(void);

/* Call this every guest tick (kind>>2==2, i.e. a lower-EL IRQ that preempted
 * the EL1 guest), AFTER el2_common has saved the guest context and the GIC
 * timer IRQ has been acknowledged/rearmed. `guest_frame` is the just-saved
 * struct el2_frame for the guest that is about to resume when this function
 * returns -- `gr`/`sr`/`gva` all report the guest's state as of THIS tick.
 *
 * Non-blocking: drains whatever console RX is currently available (bounded),
 * and if a full line has accumulated, executes ONE command and replies with
 * a fresh "dbg> " prompt. If no full line is available yet, does nothing
 * further and returns immediately so the guest resumes. Never crashes on a
 * bad command or bad hex -- reports an error and re-prompts instead. */
void dbgmon_service(struct el2_frame *guest_frame);

#endif /* BZDOS_DBGMON_H */
