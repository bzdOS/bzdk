/* SPDX-License-Identifier: BSD-2-Clause */

/* hwbp.h — EL2-controlled hardware breakpoints + watchpoints on the guest.
 *
 * The bzdOS hypervisor already single-steps the guest (el2_exc.c, MDSCR_EL1.SS
 * + MDCR_EL2.TDE). Single-step is exhaustive but glacial: catching "the guest
 * first writes into rodata at 0xa90000" or "guest PC reaches parse_boot_param"
 * that way means logging millions of instructions. Hardware breakpoints and
 * watchpoints catch exactly those events for free — the CPU raises a debug
 * exception the instant the PC matches (breakpoint) or the addressed byte is
 * accessed (watchpoint), and MDCR_EL2.TDE routes it to EL2 so WE see it first.
 *
 * Mechanism (AArch64 self-hosted debug, guest = Non-secure EL1/EL0):
 *   - DBGBVR<n>_EL1 / DBGBCR<n>_EL1  program instruction breakpoints (address
 *     match, BAS=0xf, PMC=0b11 → EL1&EL0, E=1).
 *   - DBGWVR<n>_EL1 / DBGWCR<n>_EL1  program data watchpoints (LSC selects
 *     load/store/both, BAS byte-select, PAC=0b11 → EL1&EL0, E=1).
 *   - Guest MDSCR_EL1.MDE=1 enables monitor-mode debug (bp/wp/vector-catch).
 *   - MDCR_EL2.TDE=1 routes the resulting debug exception to EL2 (shared with
 *     the single-step lane — see the TDE note in hwbp.c).
 *
 * All of these EL1 registers are banked; from EL2 we `mrs`/`msr` them directly
 * (exactly as the single-step code writes MDSCR_EL1), so arming/clearing needs
 * no guest cooperation and takes effect on the next guest eret.
 *
 * On a hit, hwbp_handle() records a breadcrumb (which slot, PC, FAR, EC) at
 * 0x50000600 ("HWBP") and DISABLES the offending slot (one-shot) so the guest
 * can make forward progress on eret instead of re-faulting forever — the
 * breadcrumb (and a fresh `gr` on the next tick) is where you inspect the hit.
 */
#ifndef BZDOS_HWBP_H
#define BZDOS_HWBP_H

#include <stdint.h>
#include "exceptions.h"

/* Cortex-A53 (Allwinner A64) implements 6 breakpoints + 4 watchpoints. These
 * are the architectural maxima we compile switch-arms for; the actual count is
 * clamped at runtime from ID_AA64DFR0_EL1. */
#define HWBP_MAX_BP 6
#define HWBP_MAX_WP 4

/* Program slot `idx` to match guest VA `va`.
 *   is_write_wp == 0 → instruction breakpoint  (bank DBGB*, idx in 0..nbp-1)
 *   is_write_wp != 0 → data write watchpoint    (bank DBGW*, idx in 0..nwp-1)
 * Enables MDE (guest) + TDE (EL2 routing) as a side effect. Returns 0 on
 * success, -1 on a bad index. */
int hwbp_set(int idx, uint64_t va, int is_write_wp);

/* Disable a slot. `is_write_wp` selects the bank, same convention as hwbp_set.
 * Idempotent; bad index returns -1. */
int hwbp_clear(int idx, int is_write_wp);

/* Arm a WRITE watchpoint matching EL2 — i.e. watch what the HYPERVISOR ITSELF
 * writes, not the guest. hwbp_set()'s watchpoints match Non-secure EL1&EL0 only
 * and cannot see an EL2 store at all. A hit arrives as a current-EL sync
 * exception; el2_trap() logs it (FLTR_K_FAULT: a0=ESR, a1=ELR) and steps over
 * it, so the recorded ELR names the storing instruction. Resolve with
 * addr2line against microkernel-dbg.elf. See WCR_ARM_EL2 in hwbp.c. */
int hwbp_set_wp_el2(int idx, uint64_t va);

/* Disable every breakpoint and watchpoint (used by the `bpc` command). */
void hwbp_clear_all(void);

/* Re-arm any slot the guest disarmed behind our back, and publish DBGBCR0 as
 * the hardware actually reads it. Must run ON THE GUEST'S CORE -- DBGB*_EL1 are
 * banked. See the comment on the definition for the measurement that made this
 * necessary. */
void hwbp_reassert(void);

/* Called from el2_trap for a lower-EL synchronous debug exception. Inspects
 * ESR_EL2.EC: 0x30/0x31 = breakpoint, 0x34/0x35 = watchpoint. If it is one of
 * ours it records the breadcrumb, one-shot-disables the hitting slot, and
 * returns 1 (handled — caller must NOT advance ELR). Returns 0 for anything
 * else (including software-step 0x32/0x33, handled by the single-step lane). */
int hwbp_handle(struct el2_frame *frame, uint64_t esr);

/* Query helpers for the `bpl` list command. Fill caller arrays (length
 * HWBP_MAX_BP / HWBP_MAX_WP); *_va[i] is the armed VA, *_en[i] the enable bit.
 * Return the number of implemented slots. */
int hwbp_list_bp(uint64_t *va, int *en);
int hwbp_list_wp(uint64_t *va, int *en);

#endif /* BZDOS_HWBP_H */
