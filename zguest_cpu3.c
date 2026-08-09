/* SPDX-License-Identifier: BSD-2-Clause */

/* zguest_cpu3.c — implementation. See zguest_cpu3.h for the full lifecycle
 * and staging-address rationale.
 *
 * Freestanding: <stdint.h> only, no libc.
 */
#include <stdint.h>
#include "zguest_cpu3.h"
#include "zload2.h"
#include "stage2_zephyr.h"
#include "vconsole.h"
#include "guest.h"
#include "kload.h"
#include "hv_addrmap.h"

/* ------------------------------------------------------------------ *
 * Staging addresses -- see zguest_cpu3.h's header comment for the full
 * "why inside Zephyr's own slice, not FreeBSD's gigabyte" rationale. Both
 * halves of stage2_zephyr.h's ZSTAGE2_DRAM_BASE/ZSTAGE2_DRAM_SIZE window.
 * ------------------------------------------------------------------ */
#define ZG3_PA_BASE       (ZSTAGE2_DRAM_BASE)                    /* lower 16 MiB */
#define ZG3_ELF_STAGE_PA  (ZSTAGE2_DRAM_BASE + (ZSTAGE2_DRAM_SIZE / 2u)) /* upper 16 MiB */

/* ------------------------------------------------------------------ *
 * Breadcrumb window: HVMAP_ZGUEST3_BC (0x50070000, "ZG3_") -- see
 * hv_addrmap.h.
 *   [0] magic   0x5A47335F ("ZG3_")
 *   [1] state   1 = parked (waiting for `zboot`), 2 = start requested/
 *               booting, 3 = about to enter the guest (last word written
 *               before kload_enter(), which never returns), 0xBAD1 =
 *               zload2_parse_and_place() failed, 0xBAD2 = isolation
 *               selfcheck failed (halted, never entered the guest)
 *   [2] entry_pa_lo  resolved physical entry address, low 32 bits
 * ------------------------------------------------------------------ */
#define ZG3_MAGIC 0x5A47335Fu   /* "ZG3_" */

enum {
	ZG3_MAGIC_IDX = 0,
	ZG3_STATE_IDX,
	ZG3_ENTRY_PA_IDX,
};

static inline void
zg3_bc(int i, uint32_t v)
{
	volatile uint32_t *p =
		(volatile uint32_t *)(HVMAP_ZGUEST3_BC + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Start-request flag -- plain cross-core-visible word, no dc civac needed
 * (live, never-persisted state; SMPEN cache coherency is enough, same
 * convention as vconsole.c's RX-ring head/tail). Set only by
 * zguest_cpu3_start_set(), consulted only by the wfe-poll loop below. */
static volatile uint32_t g_zguest3_start_req;

void
zguest_cpu3_start_set(void)
{
	g_zguest3_start_req = 1u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

void
zephyr_cpu3_run(void)
{
	uint64_t entry_pa = 0;

	zg3_bc(ZG3_MAGIC_IDX, ZG3_MAGIC);
	zg3_bc(ZG3_STATE_IDX, 1u);   /* parked, waiting for `zboot` */

	/* wfe-poll wait loop -- same shape as dbgtools.c's hold/release gate
	 * (main_dbg.c's dbgtools_hold_get() loop): spin on `wfe`, woken by the
	 * `sev` in zguest_cpu3_start_set(). Deliberately does NOT touch any
	 * watchdog here (see zguest_cpu3.h's header comment: CPU1 alone owns
	 * the HW WDOG, unconditionally; this core doing nothing WDOG-related
	 * while parked is correct, matching CPU3's do-nothing state in every
	 * other Makefile target). */
	while (!g_zguest3_start_req)
		__asm__ volatile("wfe" ::: "memory");

	zg3_bc(ZG3_STATE_IDX, 2u);   /* start requested -- booting */

	/* ---- One-way Zephyr boot sequence -------------------------------- */
	if (!zload2_parse_and_place(ZG3_ELF_STAGE_PA, ZG3_PA_BASE, &entry_pa)) {
		zg3_bc(ZG3_STATE_IDX, 0xBAD1u);
		for (;;)
			__asm__ volatile("wfi" ::: "memory");
	}
	zg3_bc(ZG3_ENTRY_PA_IDX, (uint32_t)entry_pa);

	stage2_zephyr_init();
	stage2_zephyr_enable();

	/* Hardware isolation self-check MUST pass before this core ever runs
	 * a single Zephyr instruction -- see stage2_zephyr.h's contract. A
	 * failure means a hole in the partition (FreeBSD's DRAM or real MMIO
	 * reachable from CPU3's stage-2 regime); per this project's own
	 * "safety invariant violated -> fail loud and stopped" convention
	 * (e.g. main_dbg.c's dbg_no_guest / dbgtools hold-gate diagnostics),
	 * halt here rather than silently proceeding. */
	if (!stage2_zephyr_isolation_selfcheck()) {
		zg3_bc(ZG3_STATE_IDX, 0xBAD2u);
		for (;;)
			__asm__ volatile("wfi" ::: "memory");
	}

	/* Console up before Zephyr can possibly print anything. */
	vconsole_init_chan1();

	/* Existing, unmodified, core-agnostic guest-entry contract (guest.h). */
	guest_config();

	zg3_bc(ZG3_STATE_IDX, 3u);   /* about to enter the guest */

	/* x0: Zephyr's own reset code never reads it (don't-care, same as
	 * main_zephyr.c's kload_enter() call). SP_EL1: Zephyr's __reset
	 * overwrites SP_EL1 with its own computed stack before any C code
	 * runs, so this value is a placeholder too (same as main_zephyr.c's
	 * SP_EL1_PLACEHOLDER) -- ZG3_PA_BASE chosen only so the call site
	 * reads sensibly, its actual value confirmed not to matter. */
	kload_enter(entry_pa, 0, ZG3_PA_BASE);
}
