/* SPDX-License-Identifier: BSD-2-Clause */

/* vcpu2.c — hand CPU2 to the FreeBSD guest as a second vCPU.
 *
 * See vcpu2.h for the design and for why CPU1 is not a candidate. This file is
 * deliberately shaped like zguest_cpu3.c: publish real zeros first, park on
 * `wfe`, then a one-way entry sequence. The differences from that file are all
 * consequences of one fact — this is the SAME guest as CPU0, not a second one:
 *
 *   - stage2_enable(), not stage2_zephyr_init()/_enable(): the tables already
 *     exist and are shared; only this core's banked VTCR/VTTBR need arming.
 *   - gic_timer_cpuif_init(): the guest's AP expects its own PPIs, so this
 *     core's GIC CPU interface has to be open. CPU0 does this in main_dbg.c.
 *   - No isolation self-check. There is no partition to verify here: a second
 *     vCPU of one guest is *supposed* to see exactly what vCPU0 sees. (For
 *     Zephyr on CPU3 the self-check is the whole point, because there the two
 *     guests must NOT see each other.)
 */
#include <stdint.h>
#include "vcpu2.h"
#include "hv_addrmap.h"
#include "smp.h"
#include "stage2.h"
#include "guest.h"
#include "kload.h"
#include "gic_timer.h"

/* Default off. Overridable at BUILD time because the guest issues its PSCI
 * CPU_ON during its own early boot, long before anything can reach dbgmon over
 * the network -- so a runtime-only knob could never arm this in time for the
 * request that matters. `make dbg VCPU2=1` builds with it pre-armed; the
 * dbgmon `vcpu2 off` command still works for turning it off afterwards. */
#ifndef VCPU2_DEFAULT_ON
#define VCPU2_DEFAULT_ON 0
#endif
volatile uint32_t dbg_vcpu2 = VCPU2_DEFAULT_ON;

static volatile uint32_t g_req;         /* set by vcpu2_request(), consumed once */
static volatile uint64_t g_entry;
static volatile uint64_t g_ctxid;
static volatile uint32_t g_handed;      /* 1 once the core has left for EL1 */
static volatile uint32_t g_parked;      /* 1 once the core reaches its wfe loop */
static uint32_t g_requests, g_refused;

static inline void bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_VCPU2_BC +
	    (uint32_t)i * 4u);

	*p = v;
	/* Clean to PoC: this window is read from OTHER cores and from the host
	 * over EMAC, and a Normal-WB store that stays in this core's cache is
	 * exactly the "guest DRAM word is not a liveness probe" trap this
	 * project has already been bitten by. */
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/*
 * purpose:     Accept or refuse a guest PSCI CPU_ON aimed at affinity 2.
 * input:       entry_pa   — the guest's requested entry point (its x2)
 *              context_id — the guest's context ID (its x3), delivered in x0
 * output:      1 if accepted (caller answers PSCI SUCCESS), 0 if refused
 *              (caller answers ALREADY_ON, i.e. the pre-existing behaviour)
 * sideEffects: records the request, sets the start flag, `sev` to wake the
 *              parked core, updates breadcrumbs
 *
 * Called from el2_exc.c's PSCI filter, on the GUEST's core (CPU0) — not on
 * CPU2. Everything it touches is either volatile or only read by CPU2 after it
 * observes g_req, and the `dsb` before the `sev` orders those writes ahead of
 * the wake-up.
 */
int vcpu2_request(uint64_t entry_pa, uint64_t context_id)
{
	uint32_t why;

	g_requests++;
	bc(2, g_requests);

	if (!dbg_vcpu2)
		why = 1u;               /* feature gate off */
	else if (g_handed)
		why = 2u;               /* already a guest core; truly ALREADY_ON */
	else if (!g_parked)
		why = 3u;               /* has not reached its park loop yet */
	else
		why = 0u;               /* accept */

	bc(7, why);
	if (why != 0u) {
		g_refused++;
		bc(6, g_refused);
		return 0;
	}

	g_entry = entry_pa;
	g_ctxid = context_id;
	bc(3, (uint32_t)entry_pa);
	bc(4, (uint32_t)(entry_pa >> 32));
	bc(5, (uint32_t)context_id);
	bc(1, 2u);                      /* request accepted */

	g_req = 1u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
	return 1;
}

/*
 * purpose:     CPU2's loop once this feature is enabled: park, then become a
 *              guest vCPU.
 * input:       none
 * output:      never returns once a request is accepted (kload_enter is
 *              noreturn); returns immediately if the gate is off, so the caller
 *              can fall through to the async-I/O worker instead.
 * sideEffects: arms this core's stage-2 regime and GIC CPU interface, then
 *              erets to EL1
 */
void vcpu2_run(void)
{
	int i;

	if (!dbg_vcpu2)
		return;                 /* caller falls back to vblk_async */

	/* Real zeros first, so an all-zero read means "never got here" rather
	 * than "got here and found nothing" -- a distinction this project has
	 * drawn the wrong conclusion from before. */
	for (i = 0; i < VCPU2_BC_NWORDS; i++)
		bc(i, 0);
	bc(0, VCPU2_MAGIC);

	bc(1, 1u);
	g_parked = 1u;
	__asm__ volatile("dsb sy" ::: "memory");

	/* Park. Deliberately does NOT touch the watchdog: CPU1 alone owns the
	 * WDOG, unconditionally, and this core sitting idle while parked is the
	 * same do-nothing state CPU2 has in every build where vblk_async is not
	 * linked. `wfe` (not `wfi`) is what vcpu2_request()'s `sev` wakes. */
	while (!g_req)
		__asm__ volatile("wfe" ::: "memory");

	g_handed = 1u;
	bc(1, 3u);

	/* ---- one-way entry into the guest, at EL1 ------------------------ */

	/* Same tables CPU0 uses -- but note WHICH call. stage2_enable() alone
	 * only sets HCR_EL2.VM; VTCR_EL2 and VTTBR_EL2 are programmed by
	 * stage2_init(), which runs once on the boot core. Both are BANKED
	 * PER-PE, so this core had VM=1 against a ZERO VTTBR and its very first
	 * instruction fetch took a stage-2 translation fault at level 0
	 * (ESR 0x82000004) -- measured, 5.7 million times, on the first live
	 * attempt. stage2_arm_secondary() programs this PE's registers against
	 * the existing global tables and then enables. */
	stage2_arm_secondary();

	/* HCR_EL2 is banked too, and main_dbg.c sets these bits on CPU0 only.
	 * Without them this vCPU would take physical interrupts directly at EL1
	 * instead of through the vGIC (IMO/FMO), and its SMCs would go straight
	 * to EL3 unfiltered (TSC) -- bypassing the PSCI filter that is the whole
	 * reason a guest cannot escalate through CPU_ON. Same three bits, same
	 * values, so both vCPUs run under one policy. */
	{
		uint64_t hcr;

		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr |= (1ull << 4) | (1ull << 3);   /* IMO, FMO */
		hcr |= (1ull << 19);                /* TSC */
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
	}

	/* The guest's AP will enable its own virtual timer and expect its PPIs;
	 * that needs this core's GIC CPU interface open. gic_timer.c factored
	 * this out for exactly this kind of caller. */
	gic_timer_cpuif_init();

	/* Publish what THIS core's banked registers actually hold, read on this
	 * core. They cannot be checked from anywhere else: a read over the debug
	 * channel is serviced by CPU1 and returns CPU1's bank, which is exactly
	 * the mistake that has produced several confidently wrong diagnoses in
	 * this project. */
	{
		uint64_t v;

		__asm__ volatile("mrs %0, vtcr_el2"  : "=r"(v)); bc(8, (uint32_t)v);
		__asm__ volatile("mrs %0, vttbr_el2" : "=r"(v));
		bc(9,  (uint32_t)v);
		bc(10, (uint32_t)(v >> 32));
		__asm__ volatile("mrs %0, hcr_el2"   : "=r"(v)); bc(11, (uint32_t)v);
		__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v)); bc(12, (uint32_t)v);
	}

	/* Core-agnostic EL1 configuration, unchanged and shared with CPU0's
	 * path and with zguest_cpu3.c. */
	guest_config();

	/* x0 = the context ID the guest passed to PSCI CPU_ON, which is what
	 * DEN0022 says the newly-started core receives. SP_EL1 is a
	 * placeholder: FreeBSD's mpentry sets up its own stack before any C
	 * code runs, exactly as Zephyr's __reset does on CPU3. */
	kload_enter(g_entry, g_ctxid, g_entry);
}
