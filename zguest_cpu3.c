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

/* Staging addresses (ZG3_PA_BASE / ZG3_ELF_STAGE_PA) now live in
 * zguest_cpu3.h — zstage.c needs the staging address too, and a second
 * private copy of it here would be free to drift. */

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
 *   [3] attempts     number of `zboot` attempts this core has begun, STICKY
 *                    across `zunhalt` re-arms (never reset) -- so a retry
 *                    cannot make it look like the first try
 *   [4] last_fail    the most recent failure code (0xBAD1/0xBAD2), also
 *                    STICKY: it survives a re-arm and a subsequent success,
 *                    so "it works now" never erases "it failed before"
 *   [5] rearms       number of times `zunhalt` released this core from a halt
 * ------------------------------------------------------------------ */
#define ZG3_MAGIC 0x5A47335Fu   /* "ZG3_" */

enum {
	ZG3_MAGIC_IDX = 0,
	ZG3_STATE_IDX,
	ZG3_ENTRY_PA_IDX,
	ZG3_ATTEMPTS_IDX,
	ZG3_LAST_FAIL_IDX,
	ZG3_REARMS_IDX,
	ZG3_NWORDS,
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

/* Re-arm request -- same conventions as the start flag above. Consulted only
 * by the halt loop, so setting it while CPU3 is parked or already running
 * Zephyr is harmless. */
static volatile uint32_t g_zguest3_rearm_req;

void
zguest_cpu3_start_set(void)
{
	g_zguest3_start_req = 1u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

void
zguest_cpu3_rearm_set(void)
{
	g_zguest3_rearm_req = 1u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* Sticky counters -- see the breadcrumb comment above. Held in registers-
 * turned-locals rather than re-read from the breadcrumb window, so a stray
 * write to that window by a debugger cannot corrupt the loop's own state. */
static uint32_t g_zg3_attempts;
static uint32_t g_zg3_rearms;

/* Enter the failure halt: publish why, then stop. Returns only if an operator
 * explicitly re-armed via `zunhalt` (dbgmon), in which case the caller loops
 * back round to the parked state and waits for a fresh `zboot`.
 *
 * The halt-loud property this preserves is spelled out in zguest_cpu3.h: CPU3
 * stops dead, publishes the reason, and does nothing further on its own
 * initiative. `fail` is recorded STICKY so that a later retry -- successful or
 * not -- cannot erase the evidence of this one. */
static void
zg3_halt_until_rearm(uint32_t fail)
{
	zg3_bc(ZG3_LAST_FAIL_IDX, fail);
	zg3_bc(ZG3_STATE_IDX, fail);

	for (;;) {
		if (g_zguest3_rearm_req) {
			g_zguest3_rearm_req = 0u;
			/* Discard any `zboot` issued WHILE halted. Without this,
			 * such a request stays latched and fires the instant the
			 * re-arm releases this core, so `zunhalt` alone silently
			 * starts an attempt -- observed live on 2026-08-10: the
			 * breadcrumb went straight from 0xBAD1 back to 0xBAD1
			 * with attempts incremented, and the parked state could
			 * never be seen at all. Two consequences, both bad: the
			 * operator gets an attempt they did not ask for in that
			 * moment, and the one state that proves the re-arm worked
			 * is unobservable. Clearing it makes the two commands mean
			 * exactly what they say -- `zunhalt` returns to waiting,
			 * a separate `zboot` tries again. */
			g_zguest3_start_req = 0u;
			g_zg3_rearms++;
			zg3_bc(ZG3_REARMS_IDX, g_zg3_rearms);
			return;
		}
		/* wfe, NOT wfi -- and this is not a style choice.
		 *
		 * The first version of this loop used `wfi`, reasoning that a
		 * halted core should sleep as cheaply as possible and would be
		 * woken by "any interrupt". That was wrong, and it was caught
		 * only on real hardware (2026-08-10): CPU3 takes NO interrupts
		 * whatsoever in this design -- it never arms a timer, never
		 * enables its GIC CPU interface, and runs with DAIF masked at
		 * EL2 -- so there is nothing to wake a `wfi` here, ever. And
		 * `sev`, which is what zguest_cpu3_rearm_set() sends, sets the
		 * event register and so wakes `wfe` only. The result was a
		 * permanent sleep: `zunhalt` was acknowledged over EMAC, the
		 * flag was set, and CPU3 slept through it (breadcrumb rearms
		 * stuck at 0 across repeated attempts).
		 *
		 * `wfe` is the mechanism the parked loop above already uses and
		 * that `zboot` has proven works. Waking on an unrelated `sev`
		 * from another core and re-checking one word costs nothing worth
		 * counting on a core whose entire job is to wait. */
		__asm__ volatile("wfe" ::: "memory");
	}
}

void
zephyr_cpu3_run(void)
{
	uint64_t entry_pa = 0;
	int i;

	/* Publish the whole window as REAL zeros before anything else, so that
	 * an all-zero read unambiguously means "this core never got here"
	 * rather than "it got here and found nothing" -- a distinction this
	 * project has drawn the wrong conclusion from before. */
	for (i = 0; i < ZG3_NWORDS; i++)
		zg3_bc(i, 0);
	zg3_bc(ZG3_MAGIC_IDX, ZG3_MAGIC);

	/* Outer loop exists ONLY to serve `zunhalt` (zguest_cpu3.h): a failed
	 * attempt lands in zg3_halt_until_rearm(), which returns only on an
	 * explicit operator re-arm. A SUCCESSFUL attempt never comes back here
	 * -- kload_enter() is noreturn -- so this is still the one-way boot
	 * sequence it always was. */
	for (;;) {
		zg3_bc(ZG3_STATE_IDX, 1u);   /* parked, waiting for `zboot` */

		/* wfe-poll wait loop -- same shape as dbgtools.c's hold/release
		 * gate (main_dbg.c's dbgtools_hold_get() loop): spin on `wfe`,
		 * woken by the `sev` in zguest_cpu3_start_set(). Deliberately
		 * does NOT touch any watchdog here (see zguest_cpu3.h's header
		 * comment: CPU1 alone owns the HW WDOG, unconditionally; this
		 * core doing nothing WDOG-related while parked is correct,
		 * matching CPU3's do-nothing state in every other Makefile
		 * target). */
		while (!g_zguest3_start_req)
			__asm__ volatile("wfe" ::: "memory");
		g_zguest3_start_req = 0u;   /* consume, so a re-arm re-parks */

		g_zg3_attempts++;
		zg3_bc(ZG3_ATTEMPTS_IDX, g_zg3_attempts);
		zg3_bc(ZG3_STATE_IDX, 2u);   /* start requested -- booting */

		/* ---- One-way Zephyr boot sequence ------------------------- */
		if (!zload2_parse_and_place(ZG3_ELF_STAGE_PA, ZG3_PA_BASE,
					    &entry_pa)) {
			zg3_halt_until_rearm(0xBAD1u);
			continue;
		}
		zg3_bc(ZG3_ENTRY_PA_IDX, (uint32_t)entry_pa);

		stage2_zephyr_init();
		stage2_zephyr_enable();

		/* Hardware isolation self-check MUST pass before this core ever
		 * runs a single Zephyr instruction -- see stage2_zephyr.h's
		 * contract. A failure means a hole in the partition (FreeBSD's
		 * DRAM or real MMIO reachable from CPU3's stage-2 regime); per
		 * this project's own "safety invariant violated -> fail loud
		 * and stopped" convention (e.g. main_dbg.c's dbg_no_guest /
		 * dbgtools hold-gate diagnostics), halt here rather than
		 * silently proceeding. Re-running init/enable on a retry is
		 * safe: both rebuild their tables from scratch and rewrite
		 * this core's own banked VTTBR_EL2/HCR_EL2. */
		if (!stage2_zephyr_isolation_selfcheck()) {
			zg3_halt_until_rearm(0xBAD2u);
			continue;
		}

		/* Console up before Zephyr can possibly print anything. */
		vconsole_init_chan1();

		/* Existing, unmodified, core-agnostic guest-entry contract
		 * (guest.h). */
		guest_config();

		zg3_bc(ZG3_STATE_IDX, 3u);   /* about to enter the guest */

		/* x0: Zephyr's own reset code never reads it (don't-care, same
		 * as main_zephyr.c's kload_enter() call). SP_EL1: Zephyr's
		 * __reset overwrites SP_EL1 with its own computed stack before
		 * any C code runs, so this value is a placeholder too (same as
		 * main_zephyr.c's SP_EL1_PLACEHOLDER) -- ZG3_PA_BASE chosen
		 * only so the call site reads sensibly, its actual value
		 * confirmed not to matter. */
		kload_enter(entry_pa, 0, ZG3_PA_BASE);
	}
}
