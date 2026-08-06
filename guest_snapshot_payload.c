/* SPDX-License-Identifier: BSD-2-Clause */

/* guest_snapshot_payload.c — minimal EL1 guest for exercising
 * snapshot_save()/snapshot_restore() under QEMU `virt` (ROADMAP D1).
 *
 * The ONLY thing this payload does is increment a loop counter in a fixed,
 * EL2-readable DRAM word (see guest_snapshot_payload.h for exactly why that
 * address, not guest.c's own GUEST_BC_BASE). That single counter is the
 * whole observability mechanism the exercise needs: el2_exc_snapshot_qemu.c
 * samples it at every tick, so a `snap` taken at tick N followed by a
 * `restore` at tick M (M > N) is provable from the SERIAL LOG ALONE —
 * the counter should be seen climbing past its snapshot-time value, then
 * (if restore actually re-entered the guest at the saved PC/state with the
 * saved DRAM) drop back down near the snapshot-time value and climb again
 * from there, rather than continuing on from wherever it was at the moment
 * `restore` was called.
 *
 * Reuses guest_config()/guest_enter() from guest.c completely unmodified —
 * both are fully generic AArch64 EL1 setup, same as guest_qemu_payload.c's
 * qemu_guest_start() already does for the plain (non-snapshot) QEMU target.
 * Caches are OFF end to end on this build (SCTLR_EL1 RES1-only per
 * guest_config(); EL2 itself runs with its own MMU off — see
 * start_qemu.S's header), so a plain volatile store here is visible to EL2
 * with no cache-maintenance needed, exactly like main_qemu.c's own
 * S2_AT_PAR_LO breadcrumb read.
 */
#include <stdint.h>
#include "guest.h"
#include "guest_snapshot_payload.h"

static inline void
sbc_wr(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(SNAPGP_BC_BASE + (uint32_t)i * 4u);
	*p = v;
}

static inline uint32_t
sbc_rd(int i)
{
	volatile uint32_t *p = (volatile uint32_t *)(SNAPGP_BC_BASE + (uint32_t)i * 4u);
	return *p;
}

static inline uint64_t
read_currentel(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return v >> 2;   /* CurrentEL.EL is bits [3:2] */
}

/* Private EL1 stack, owned entirely by this file — same 16 KiB sizing as
 * every other minimal payload in this tree (guest.c's guest_el1_stack,
 * guest_qemu_payload.c's qemu_guest_stack): a payload that only loops and
 * stores a word needs nothing more. */
#define STACK_WORDS (16384 / 8)
static uint64_t snap_guest_stack[STACK_WORDS] __attribute__((aligned(16)));

static void snap_guest_entry(void) __attribute__((noreturn));

static void
snap_guest_entry(void)
{
	sbc_wr(SNAPGP_BC_LOOP_IDX, 0u);
	sbc_wr(SNAPGP_BC_EL_IDX, (uint32_t)read_currentel());
	sbc_wr(SNAPGP_BC_MAGIC_IDX, SNAPGP_BC_MAGIC);   /* written LAST so a
	                                                  * reader that checks
	                                                  * magic first never
	                                                  * sees a torn record */

	for (;;) {
		uint32_t cur = sbc_rd(SNAPGP_BC_LOOP_IDX);
		sbc_wr(SNAPGP_BC_LOOP_IDX, cur + 1u);
	}
}

void
snapshot_guest_start(void)
{
	uint64_t sp_top = (uint64_t)&snap_guest_stack[STACK_WORDS];

	guest_config();
	guest_enter((uint64_t)snap_guest_entry, sp_top);
	__builtin_unreachable();
}
