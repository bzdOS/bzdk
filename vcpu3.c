/* SPDX-License-Identifier: BSD-2-Clause */

/* vcpu3.c — hand CPU3 to the FreeBSD guest as a third (or fourth, alongside
 * vcpu1.c/vcpu2.c) vCPU.
 *
 * See vcpu3.h for the design, and for the mutual-exclusion mechanism against
 * zguest_cpu3.c (the `dual` target's Zephyr guest) -- bzdos_cpu3_owner below.
 *
 * Deliberately a copy of vcpu2.c's structure, WITH vcpu2.c's own fix already
 * folded in from the start (vgic_init() + the EL2 IRQ/FIQ unmask), rather
 * than shipped without it and patched later -- see vcpu2.c's fix comment for
 * the full mechanism (missing vgic_init() => GICH never initialized on this
 * core => every physical interrupt taken here, including the smp_rendezvous
 * IPI, is accepted at the GIC and never reaches the guest => wedge on the
 * first cross-call). Repeating that omission a third time was the one
 * mistake this file was written specifically to not make.
 */
#include <stdint.h>
#include "vcpu3.h"
#include "hv_addrmap.h"
#include "smp.h"
#include "stage2.h"
#include "guest.h"
#include "kload.h"
#include "gic_timer.h"
#include "vgic.h"

/* MUTUAL EXCLUSION WITH zguest_cpu3.c, ENFORCED AT LINK TIME.
 *
 * See vcpu3.h's header comment for the runtime hazard this prevents (CPU3
 * parking forever in vcpu3_run(), silently orphaning the `dual` target's
 * `zboot` feature, if both were ever linked together with vcpu3 armed).
 * zguest_cpu3.c defines the SAME symbol with the SAME non-weak linkage; if
 * both objects are ever listed in one Makefile target's object list, the
 * final link step fails with `multiple definition of bzdos_cpu3_owner`
 * instead of producing a binary that only misbehaves once booted. Not
 * `static`, not `weak` -- a genuine collision is the entire point. The value
 * itself is never read by anything; it exists to be linked, not consulted. */
const char *const bzdos_cpu3_owner = "vcpu3";

/* Default off. Same build-time-only reasoning as VCPU1/VCPU2: the guest
 * issues its PSCI CPU_ON during its own early boot, before dbgmon is
 * reachable over the network to arm this at runtime. `make dbg VCPU3=1`
 * pre-arms it; `vcpu3 off` at the dbgmon prompt still works afterward. */
#ifndef VCPU3_DEFAULT_ON
#define VCPU3_DEFAULT_ON 0
#endif
volatile uint32_t dbg_vcpu3 = VCPU3_DEFAULT_ON;

static volatile uint32_t g_req;         /* set by vcpu3_request(), consumed once */
static volatile uint64_t g_entry;
static volatile uint64_t g_ctxid;
static volatile uint32_t g_handed;      /* 1 once the core has left for EL1 */
static volatile uint32_t g_parked;      /* 1 once the core reaches its wfe loop */
static uint32_t g_requests, g_refused;

static inline void bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_VCPU3_BC +
	    (uint32_t)i * 4u);

	*p = v;
	/* Clean to PoC -- see vcpu2.c's identical comment on why this must not
	 * be a plain Normal-WB store left in this core's own cache. */
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/*
 * purpose:     Accept or refuse a guest PSCI CPU_ON aimed at affinity 3.
 * input:       entry_pa   — the guest's requested entry point (its x2)
 *              context_id — the guest's context ID (its x3), delivered in x0
 * output:      1 if accepted (caller answers PSCI SUCCESS), 0 if refused
 *              (caller answers ALREADY_ON, i.e. the pre-existing behaviour)
 * sideEffects: records the request, sets the start flag, `sev` to wake the
 *              parked core, updates breadcrumbs
 *
 * Called from el2_exc.c's PSCI filter, on the GUEST's core — not on CPU3.
 * Byte-for-byte the same shape as vcpu2_request(); see that file for the
 * memory-ordering rationale (dsb before sev).
 */
int vcpu3_request(uint64_t entry_pa, uint64_t context_id)
{
	uint32_t why;

	g_requests++;
	bc(2, g_requests);

	if (!dbg_vcpu3)
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
 * purpose:     CPU3's loop once this feature is enabled: park, then become a
 *              guest vCPU.
 * input:       none
 * output:      never returns once a request is accepted (kload_enter is
 *              noreturn); returns immediately if the gate is off, so the
 *              caller falls through to zephyr_cpu3_run() instead (the
 *              `dual` target's Zephyr path, or the plain WFI park
 *              everywhere else).
 * sideEffects: arms this core's stage-2 regime, GIC CPU interface and vGIC,
 *              then erets to EL1
 */
void vcpu3_run(void)
{
	int i;

	if (!dbg_vcpu3)
		return;                 /* caller falls back to zephyr_cpu3_run() */

	/* Real zeros first -- see vcpu2.c's identical comment on why an
	 * all-zero read must unambiguously mean "never got here". */
	for (i = 0; i < VCPU3_BC_NWORDS; i++)
		bc(i, 0);
	bc(0, VCPU3_MAGIC);

	bc(1, 1u);
	g_parked = 1u;
	__asm__ volatile("dsb sy" ::: "memory");

	/* Park. Does NOT touch the watchdog: CPU1 alone owns the WDOG,
	 * unconditionally, and this core sitting idle while parked is the same
	 * do-nothing state CPU3 has in every build where this feature is off.
	 * `wfe` (not `wfi`) is what vcpu3_request()'s `sev` wakes. */
	while (!g_req)
		__asm__ volatile("wfe" ::: "memory");

	g_handed = 1u;
	bc(1, 3u);

	/* ---- one-way entry into the guest, at EL1 ------------------------ */

	/* NOT PORTED FROM vcpu1.c, DELIBERATELY -- same reasoning as vcpu2.c's
	 * identical note: `dbg_core_active`, the musb/hdmi IRQ targeting, and
	 * the periodic tick are CPU1-only duties (EMAC/dbgmon single-owner
	 * mutex, USB-ACM console bridge, HDMI HUD/vblank, and the watchdog
	 * cadence that carries them) with no bearing on, or equivalent for,
	 * this core. CPU1 keeps all of it regardless of whether CPU3 is a
	 * guest vCPU. */

	/* Same tables CPU0/CPU2 use. VTTBR_EL2/VTCR_EL2 are banked per-PE, so
	 * stage2_arm_secondary() programs THIS core's registers against the
	 * existing global tables (built once, by stage2_init(), on the boot
	 * core) and then enables -- see vcpu2.c's comment for the measured
	 * 5.7-million-fault consequence of skipping this. */
	stage2_arm_secondary();

	/* HCR_EL2 is banked too, and main_dbg.c sets these bits on CPU0 only.
	 * Same three bits, same values, as CPU0/CPU1/CPU2 -- one guest, one
	 * policy, on every core that runs it. */
	{
		uint64_t hcr;

		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr |= (1ull << 4) | (1ull << 3);   /* IMO, FMO */
		hcr |= (1ull << 19);                /* TSC */
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
	}

	/* The guest's AP will enable its own virtual timer and expect its
	 * PPIs; that needs this core's GIC CPU interface open. */
	gic_timer_cpuif_init();

	/* THE FIX vcpu2.c NEEDED AND DID NOT HAVE UNTIL 2026-08-27, PRESENT
	 * HERE FROM THE START. See vcpu2.c's fix comment for the complete
	 * derivation (GICH per-PE banking, vgic_active() reading THIS core's
	 * own vg_percpu slot, and the "Release APs...done." wedge this
	 * prevents). Must run BEFORE this core's own IRQ+FIQ unmask below, for
	 * the same reason: EL2 must never take a physical IRQ on this core
	 * before it has somewhere (a List Register) and a policy (HW-mode
	 * injection) ready for it. Also zeroes CNTVOFF_EL2, deliberately: every
	 * vCPU of one SMP guest must share one virtual timebase. */
	vgic_init();

	/* Publish what THIS core's banked registers actually hold, read on
	 * this core -- see vcpu2.c's comment on why a debug-channel read of
	 * these would return the wrong core's copy. */
	{
		uint64_t v;

		__asm__ volatile("mrs %0, vtcr_el2"  : "=r"(v)); bc(8, (uint32_t)v);
		__asm__ volatile("mrs %0, vttbr_el2" : "=r"(v));
		bc(9,  (uint32_t)v);
		bc(10, (uint32_t)(v >> 32));
		__asm__ volatile("mrs %0, hcr_el2"   : "=r"(v)); bc(11, (uint32_t)v);
		__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v)); bc(12, (uint32_t)v);
	}

	/* Core-agnostic EL1 configuration, unchanged and shared with every
	 * other vCPU entry path in this tree. */
	guest_config();

	/* Unmask EL2 IRQ+FIQ now that vgic_init() and this core's own HCR_EL2
	 * write are both in place, immediately before handing off -- see
	 * vcpu1.c's/vcpu2.c's identical call for why this must happen and in
	 * this order (no window where a physical interrupt could arrive on
	 * this core with nowhere configured to take it). */
	__asm__ volatile("msr daifclr, #3" ::: "memory");

	/* x0 = the context ID the guest passed to PSCI CPU_ON. SP_EL1 is a
	 * placeholder: FreeBSD's mpentry sets up its own stack before any C
	 * code runs. */
	kload_enter(g_entry, g_ctxid, g_entry);
}
