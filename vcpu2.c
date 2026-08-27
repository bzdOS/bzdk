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
#include "vgic.h"    /* vgic_init() -- see the call site below. Added
                      * 2026-08-27; this core never had it before, and per
                      * RELEASE-0.0.2.md's own account of CPU1's identical bug
                      * ("`vcpu1_run()` never called `vgic_init()` on CPU1 ...
                      * that core had zero interrupts: no timer, no IPI") plus
                      * this file's own 0.0.1-era hardware result ("Verified —
                      * the guest enumerates CPU 1 ... affinity: 2, sets up
                      * IPIs, completes Release APs. That was as far as it
                      * went" -- RELEASE-0.0.2.md, "The headline"), CPU2 as
                      * shipped here almost certainly hit the exact same wedge
                      * CPU1 did, just never root-caused as such because
                      * VCPU2 defaults off and attention moved to CPU1. */

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

	/* THE FIX THIS FILE WAS MISSING (added 2026-08-27, ported from vcpu1.c's
	 * hardware-proven root-cause fix -- board-UNVERIFIED here; VCPU2 defaults
	 * off, see vcpu2.h, so this changes no default-build behaviour).
	 *
	 * Under this project's live HCR_EL2.IMO=1/FMO=1 policy (main_dbg.c; set
	 * again on THIS core three bits above), EL2 owns every physical
	 * interrupt and the ONLY route to the guest is vgic_inject_hw() tying it
	 * to a GICH List Register (vgic.c). GICH is per-PE-banked and
	 * vgic_active() reads the CALLING core's own vg_percpu slot -- a core
	 * that never ran its own vgic_init() has GICH_HCR.En == 0 and nr_lr == 0,
	 * so gic_timer_irq()'s forwarding block (`if (vgic_active() && ...)`)
	 * never triggers there, and the fallback legacy path's own
	 * vgic_inject_hw() call (gic_timer.c) finds no free List Register either
	 * (nr_lr == 0) and queues into a per-core pending queue that nothing
	 * ever drains, because the maintenance PPI that drains it
	 * (VGIC_MAINT_INTID) was also never armed here. Net effect: every
	 * physical interrupt this core takes -- in particular the smp_rendezvous
	 * IPI FreeBSD's SMP bring-up sends the instant it needs cross-call
	 * coordination -- is accepted at the GIC but never reaches the guest.
	 *
	 * That is exactly the "Release APs...done." wedge vcpu1.c's header
	 * documents finding and fixing for CPU1 (missing vgic_init() there too),
	 * and RELEASE-0.0.2.md records this file getting no further than that
	 * same line on real hardware during 0.0.1 -- before the root cause was
	 * understood. It was never revisited here because CPU1 became the
	 * default second vCPU and VCPU2 stayed an off-by-default experiment.
	 *
	 * Must run BEFORE this core's own IRQ+FIQ unmask below, same ordering
	 * vcpu1.c uses and for the same reason: EL2 must never take a physical
	 * IRQ on this core before it has somewhere (a List Register) and a
	 * policy (HW-mode injection) ready for it. Also zeroes CNTVOFF_EL2,
	 * deliberately: two vCPUs of one SMP guest must share one virtual
	 * timebase, and main_dbg.c already zeroes CPU0's copy explicitly. */
	vgic_init();

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

	/* Added alongside vgic_init() above, same derivation: secondaries boot
	 * with DAIF masked (start.S's _start_secondary reset state -- see
	 * vblk_async.c's header for the same fact stated about CPU2's other
	 * role) and nothing in this file's ORIGINAL sequence ever cleared it.
	 * main_dbg.c does this for CPU0 (`msr daifclr, #3`, right before its own
	 * guest entry) and vcpu1.c does it for CPU1, calling it out as "the
	 * actual mechanism the whole mitigation rests on" -- this core had no
	 * equivalent. Unmask EL2 IRQ+FIQ now that vgic_init() and this core's own
	 * HCR_EL2 write are both in place, immediately before handing off, so
	 * there is no window where a physical interrupt could arrive on this
	 * core with nowhere configured to take it. */
	__asm__ volatile("msr daifclr, #3" ::: "memory");

	/* x0 = the context ID the guest passed to PSCI CPU_ON, which is what
	 * DEN0022 says the newly-started core receives. SP_EL1 is a
	 * placeholder: FreeBSD's mpentry sets up its own stack before any C
	 * code runs, exactly as Zephyr's __reset does on CPU3. */
	kload_enter(g_entry, g_ctxid, g_entry);
}
