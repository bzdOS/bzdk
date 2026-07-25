/* SPDX-License-Identifier: BSD-2-Clause */

/* gdbstub_hw.h — hardware breakpoint / watchpoint lane for the GDB stub.
 *
 * The existing gdbstub.c implements the RSP core and SOFTWARE breakpoints
 * (Z0/z0 = patched BRK). It returns an empty packet for Z1/z1 (hardware
 * breakpoints) and Z2/Z3/Z4 (watchpoints). This companion module fills that
 * gap by mapping those GDB packets onto the existing hwbp.c DBGB/DBGW
 * (breakpoint/watchpoint) machinery — WITHOUT editing gdbstub.c or hwbp.c
 * (both are pre-existing).
 *
 * How it plugs in (see docs/gdbstub-integration.md for the exact edits):
 *   - gdbstub.c::dispatch(), in the `Z`/`z` cases, calls gdbstub_hw_insert()/
 *     gdbstub_hw_remove() for type 1..4 instead of replying empty.
 *   - gdbstub.c::send_stop() (or the el2_trap divert) calls
 *     gdbstub_hw_stop_reason() to build the richer T-reply that names the
 *     hardware breakpoint / watchpoint that fired (hwbreak/watch/rwatch/awatch),
 *     which GDB needs to report *why* it stopped and, for watchpoints, WHICH
 *     address was hit.
 *   - qSupported gains "hwbreak+;swbreak+".
 *
 * WHY a hardware lane at all (SW BRK already works):
 *   - HW breakpoints stop on execution of PC in memory that BRK cannot patch
 *     (read-only / execute-in-place guest text), and are invisible to a guest
 *     that checksums its own code.
 *   - Watchpoints (data breakpoints) have NO software equivalent here — they
 *     are the only way to answer "who wrote address X" without single-stepping
 *     millions of instructions.
 *
 * Slot economy: the Allwinner A64 (Cortex-A53) implements 6 breakpoint + 4
 * watchpoint slots (clamped at runtime from ID_AA64DFR0_EL1 inside hwbp.c).
 * GDB may ask for more than exist; we return E-replies past capacity so GDB
 * falls back to software breakpoints on its own.
 *
 * PSTATE.D caveat: hardware bp/wp/single-step are DEBUG exceptions and are
 * masked while the guest runs with PSTATE.D=1 (early FreeBSD locore). Software
 * BRK (Z0) is NOT masked and is the early-boot workhorse. gdbstub_hw_insert()
 * refuses (E-reply) when the stopped frame shows CPSR.D==1 so GDB reports a
 * clear error instead of arming a slot that can never fire. See
 * docs/gdbstub-design.md §7.
 *
 * Freestanding: <stdint.h> + "exceptions.h" only. No libc, no FP/SIMD.
 */
#ifndef BZDOS_GDBSTUB_HW_H
#define BZDOS_GDBSTUB_HW_H

#include <stdint.h>
#include "exceptions.h"

/* GDB Z/z "type" field values (RSP: Z<type>,addr,kind). */
enum {
	GDB_BP_SW      = 0,   /* handled inside gdbstub.c (BRK patch)      */
	GDB_BP_HW      = 1,   /* hardware execution breakpoint  -> DBGB    */
	GDB_WP_WRITE   = 2,   /* write watchpoint  (GDB `watch`)  -> DBGW  */
	GDB_WP_READ    = 3,   /* read  watchpoint  (GDB `rwatch`) -> DBGW  */
	GDB_WP_ACCESS  = 4,   /* access watchpoint (GDB `awatch`) -> DBGW  */
};

/* Insert a hardware breakpoint (type 1) or watchpoint (type 2/3/4).
 *   type  : one of GDB_BP_HW / GDB_WP_WRITE / GDB_WP_READ / GDB_WP_ACCESS.
 *   addr  : guest VA as GDB gave it.
 *   kind  : for type 1 the instruction size (4; 2 for a thumb guest — N/A here).
 *           for type 2/3/4 the watch LENGTH in bytes (1,2,4,8).
 *   frame : the stopped guest frame (used to check CPSR.D — see PSTATE.D note).
 * Returns 1 on success, 0 on "no free slot / over capacity" (GDB then uses a
 * software breakpoint), -1 on "refused: debug masked (CPSR.D=1)". The caller
 * (gdbstub.c dispatch) turns 1 -> "OK", 0 -> "" (empty = unsupported so GDB
 * retries as Z0), -1 -> "E01". */
int gdbstub_hw_insert(int type, uint64_t addr, int kind, struct el2_frame *frame);

/* Remove a previously inserted hw bp/wp matching (type,addr). Idempotent:
 * removing an unknown one is a no-op success (returns 1). */
int gdbstub_hw_remove(int type, uint64_t addr);

/* Disarm every hw bp/wp this module owns (called on GDB detach/kill). */
void gdbstub_hw_clear_all(void);

/* Given a debug-exception frame, decide whether it is one of OUR hardware
 * bp/wp and, if so, write a GDB stop-reason fragment into `out` (NUL-terminated,
 * needs ~40 bytes) and return 1. The fragment is the part AFTER "T05", e.g.
 *   "hwbreak:;"                 (hardware execution breakpoint, EC==0x30)
 *   "watch:ffff000012340008;"   (write watchpoint hit, addr = FAR_EL2)
 *   "rwatch:<addr>;" / "awatch:<addr>;"
 * Returns 0 if this frame's ESR EC is not a hw bp/wp we own (caller then uses
 * the plain "T05thread:01;" reply). Does NOT clear the slot — under GDB
 * ownership hw bp/wp are STICKY until an explicit z packet (unlike hwbp.c's
 * one-shot hwbp_handle()); the slot is re-armed automatically on the next
 * resume because its shadow entry persists. */
int gdbstub_hw_stop_reason(struct el2_frame *frame, char *out);

/* 1 if this module currently owns at least one armed hw bp/wp — lets the
 * el2_trap divert know it must route EC==0x30/0x34 to the stub (and NOT to
 * hwbp.c's one-shot handler) while GDB is attached. */
int gdbstub_hw_active(void);

#endif /* BZDOS_GDBSTUB_HW_H */
