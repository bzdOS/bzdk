/* SPDX-License-Identifier: BSD-2-Clause */

/* vcpu1.c — hand CPU1 to the FreeBSD guest as a third vCPU, EXPERIMENTAL.
 *
 * See vcpu1.h for the full design and the honest tradeoff (a hardware-level
 * wedge of this specific core is no longer covered; a software-level guest
 * hang on it is, via the same IMO=1 unmaskable-tick mechanism CPU0 already
 * relies on). Structurally identical to vcpu2.c's park-and-enter sequence;
 * the two differences are:
 *
 *   - This core needs its OWN periodic CNTP tick armed before entering the
 *     guest (CPU2/vcpu2.c has none — nothing there depended on one). Without
 *     it, IMO=1 alone buys nothing: there would be no periodic physical IRQ
 *     to route, so EL2 would never regain control on this core at all.
 *   - The tick's own handler (gic_timer_irq(), gic_timer.c) is where the
 *     watchdog kick + dbgmon service actually happen, gated on
 *     `smp_cpu_id() == SMP_DEBUG_CPU && dbg_vcpu1` — not in this file, and
 *     not in a foreground loop, because the whole point is that they must
 *     run even while this core is busy executing guest code.
 */
#include <stdint.h>
#include "vcpu1.h"
#include "hv_addrmap.h"
#include "smp.h"
#include "stage2.h"
#include "guest.h"
#include "kload.h"
#include "gic_timer.h"
#include "vgic.h"    /* vgic_init() — see the call site: without it this core's
                      * guest gets no interrupts at all under IMO=1 */

/* Owned by el2_exc.c. Not a status bit but the EMAC/dbgmon single-owner mutex —
 * see where this file sets it for why that distinction cost a debug channel. */
extern volatile uint32_t dbg_core_active;

/* Default off. Overridable at build time for the same reason vcpu2.c's
 * VCPU2_DEFAULT_ON exists: the guest issues its PSCI CPU_ON during its own
 * early boot, before dbgmon is reachable over the network to arm this at
 * runtime. `make dbg VCPU1=1` pre-arms it; `vcpu1 off` at the dbgmon prompt
 * still works afterward. */
#ifndef VCPU1_DEFAULT_ON
#define VCPU1_DEFAULT_ON 0
#endif
volatile uint32_t dbg_vcpu1 = VCPU1_DEFAULT_ON;

/* Tick period for CPU1's own CNTP arm. 10 ms, matching main_dbg.c's own
 * gic_timer_arm_preserving_cntvoff(10000u) choice for CPU0's
 * vtimer_mask_watchdog recovery tick — no reason for this core's watchdog-
 * kick/dbgmon cadence to differ, and reusing the already-measured period
 * means no new jitter/timing unknowns to characterize. */
#define VCPU1_TICK_PERIOD_US 10000u

static volatile uint32_t g_req;
static volatile uint64_t g_entry;
static volatile uint64_t g_ctxid;
static volatile uint32_t g_handed;
static volatile uint32_t g_parked;
static uint32_t g_requests, g_refused;

static inline void bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_VCPU1_BC +
	    (uint32_t)i * 4u);

	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/*
 * purpose:     Accept or refuse a guest PSCI CPU_ON aimed at affinity 1.
 * input:       entry_pa   — the guest's requested entry point (its x2)
 *              context_id — the guest's context ID (its x3), delivered in x0
 * output:      1 if accepted (caller answers PSCI SUCCESS), 0 if refused
 * sideEffects: records the request, sets the start flag, `sev` to wake the
 *              parked core, updates breadcrumbs
 *
 * Called from el2_exc.c's PSCI filter, on the GUEST's core (CPU0) — not on
 * CPU1. Byte-for-byte the same shape as vcpu2_request(); see that file for
 * the memory-ordering rationale (dsb before sev).
 */
int vcpu1_request(uint64_t entry_pa, uint64_t context_id)
{
	uint32_t why;

	g_requests++;
	bc(2, g_requests);

	if (!dbg_vcpu1)
		why = 1u;
	else if (g_handed)
		why = 2u;
	else if (!g_parked)
		why = 3u;
	else
		why = 0u;

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
	bc(1, 2u);

	g_req = 1u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
	return 1;
}

/*
 * purpose:     CPU1's loop once this feature is enabled: park, then become a
 *              guest vCPU with its own watchdog-safe tick armed.
 * input:       none
 * output:      never returns once a request is accepted; returns immediately
 *              if the gate is off, so the caller falls through to the
 *              existing SMP_DEBUG_CPU tight-loop branch instead
 * sideEffects: arms this core's stage-2 regime, GIC CPU interface, and its
 *              own periodic CNTP tick, then erets to EL1
 */
void vcpu1_run(void)
{
	int i;

	if (!dbg_vcpu1)
		return;                 /* caller falls back to the debug-core loop */

	for (i = 0; i < VCPU1_BC_NWORDS; i++)
		bc(i, 0);
	bc(0, VCPU1_MAGIC);

	bc(1, 1u);
	g_parked = 1u;
	__asm__ volatile("dsb sy" ::: "memory");

	/* Park. Does NOT touch the hardware watchdog here -- once this core is
	 * handed to the guest, the watchdog kick moves to the tick path
	 * (gic_timer_irq(), gated on dbg_vcpu1) so it survives whatever the
	 * guest is doing on this core. While still parked here, nothing has
	 * been handed over yet, so there is nothing new to protect against --
	 * the OLD debug-core loop (smp.c) hasn't started either, since this
	 * function is called BEFORE that branch. A parked-forever core (gate
	 * armed but the guest never asks for affinity 1 — e.g. no cpu@1 in the
	 * DTB) means CPU1 does nothing at all, including no watchdog kick,
	 * until the operator notices and runs `vcpu1 off`. Named, not hidden:
	 * arming this gate is a commitment, not a free trial. */
	while (!g_req)
		__asm__ volatile("wfe" ::: "memory");

	g_handed = 1u;
	bc(1, 3u);

	/* CLAIM OWNERSHIP OF EMAC/dbgmon FOR THIS CORE — the fix for the
	 * "debug channel goes dark a few minutes after the guest boots and never
	 * comes back" regression (found and root-caused 2026-08-26).
	 *
	 * smp.c calls this function BEFORE its own `dbg_core_active = 1`, and this
	 * function never returns once a request is accepted — so without the write
	 * below, dbg_core_active stays 0 for the entire life of the board while
	 * vcpu1 is armed. That flag is not a status bit, it is a MUTEX: el2_exc.c
	 * has six `if (!dbg_core_active) dbgmon_service(frame);` call sites, one on
	 * each of the guest's MMIO trap paths (vconsole/vgicd/vblk/vnet/vblk_sd),
	 * whose whole purpose is "CPU0 services dbgmon only while no debug core
	 * exists". With the flag stuck at 0 and CPU1's tick ALSO calling
	 * dbgmon_service(), emac_poll() ends up running concurrently on two cores
	 * — and emac.c's own header states the RX side is safe only because that
	 * cannot happen: "emac_poll() is only ever invoked from ONE core at a time
	 * ... so g_rx_slot/rx_ring are single-owner at all times", explicitly
	 * citing this very flag as the gate. Two owners corrupt g_rx_slot and the
	 * console byte ring's head/tail permanently, which is exactly the observed
	 * signature: the channel works while the guest is still booting (few
	 * traps), dies once the guest is genuinely active, and never recovers no
	 * matter how quiet the link goes, while stateless paths serviced inside
	 * emac_poll() itself (dbgtools' raw peek, hvdbg's address-independent
	 * wdt_reset) keep answering. It also silently disabled the guest-frame
	 * snapshot at el2_exc.c's `if (dbg_core_active && smp_cpu_id() == 0u ...)`,
	 * so gr/sr/gdbstub would have been reporting stale registers.
	 *
	 * WHY HERE and not at the top of this function: while merely PARKED (no
	 * CPU_ON seen yet — the whole guest-boot window) this core services
	 * nothing, so CPU0 servicing dbgmon from its traps is the CORRECT and
	 * necessary behaviour, and claiming the flag early would black out the
	 * channel for the entire boot. The claim belongs exactly here, on the
	 * accepted path, where the periodic tick that takes over the duty is armed
	 * a few instructions below. The gap between this store and the first tick
	 * is microseconds. */
	dbg_core_active = 1u;
	__asm__ volatile("dsb sy" ::: "memory");

	/* ---- one-way entry into the guest, at EL1 ------------------------ */

	stage2_arm_secondary();

	{
		uint64_t hcr;

		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr |= (1ull << 4) | (1ull << 3);   /* IMO, FMO */
		hcr |= (1ull << 19);                /* TSC */
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
	}

	gic_timer_cpuif_init();

	/* THE ROOT-CAUSE FIX for the "Release APs...done." wedge (2026-08-26).
	 *
	 * Without this call CPU1's guest receives ZERO interrupts — not a slow or
	 * jittery delivery, none at all — and the whole guest deadlocks, on BOTH
	 * cores, the first time FreeBSD needs an IPI. Why, in the order the pieces
	 * actually chain:
	 *
	 *   1. The live policy is HCR_EL2.IMO=1/FMO=1 (main_dbg.c's "interrupt-
	 *      virtualization milestone"): EL2 owns every physical IRQ/FIQ, and
	 *      the ONLY route from a physical interrupt to the guest is
	 *      vgic_inject_hw() tying it to a GICH List Register.
	 *   2. GICH (GICH_HCR, the GICH_LR list registers, GICH_VMCR) is
	 *      per-PE-banked — this file's
	 *      own header and gic_timer.c both already state that — and
	 *      vgic_active() reads THIS core's vg_percpu slot (vgic.c). So a core
	 *      that never ran its own vgic_init() has GICH_HCR.En == 0 and
	 *      vgic_active() == 0.
	 *   3. gic_timer_irq()'s forwarding block is gated on exactly that
	 *      (`if (vgic_active() && ...)`, gic_timer.c). With it false, every
	 *      INTID this core takes falls through to the legacy per-INTID
	 *      handling and is dropped instead of injected.
	 *
	 * The symptom that made this hard to see: FreeBSD's AP-release rendezvous
	 * is a PURE shared-memory handshake (aps_ready/aps_started, dsb+sev/wfe —
	 * no interrupt involved), so CPU1 comes up perfectly and the guest even
	 * prints "Release APs...done." It then wedges at the NEXT step,
	 * smp_after_idle_runnable()'s smp_rendezvous(), whose IPI CPU0 duly writes
	 * to GICD_SGIR and which arrives at CPU1's *physical* interface, traps to
	 * EL2 here, and dies for want of a virtual interface to be injected into.
	 * Two earlier hypotheses (vgicd.c masking that SGIR write; a CPU0<->CPU1
	 * cache-coherency failure in the handshake) were both wrong, and the
	 * evidence that rules them out is the same in both cases: CPU1's EL2 side
	 * — the tick, the watchdog kick, dbgmon over EMAC — stays fully alive and
	 * responsive for the entire wedge, so neither the core nor its coherency
	 * is broken; only the guest ON it is starved.
	 *
	 * ORDERING, and it matters (same order main_dbg.c uses for CPU0):
	 * gic_timer_cpuif_init() above opens the shared CPU-interface registers
	 * vgic needs; this call must come BEFORE the tick is armed and long before
	 * `daifclr` below, so EL2 never takes a physical IRQ on this core without
	 * somewhere (a List Register) and a policy (HW-mode injection) ready for
	 * it. vgic_init() also writes CNTVOFF_EL2 = 0, which DELIBERATELY
	 * overrides the "preserving" intent of gic_timer_arm_preserving_cntvoff()
	 * further down: two vCPUs of one SMP guest must share one virtual
	 * timebase, so CPU1's CNTVOFF has to match the zero CPU0 already runs
	 * with (main_dbg.c zeroes it explicitly too) — a per-core CNTVOFF would
	 * hand FreeBSD two disagreeing clocks, which is its own class of bug.
	 * Placing this before the arm is what makes the arm preserve the RIGHT
	 * value rather than whatever this core happened to boot with. */
	vgic_init();

	/* Real, event-driven MUSB "mc" SPI (musb.h's MUSB_IRQ_INTID), targeted
	 * at CPU1 alone — see gic_timer.c's musb_irq_arm_cpu1() for the design
	 * and its UNVERIFIED-on-hardware assumptions. This is what lets the
	 * USB-ACM console bridge (usbacm.c) be serviced the instant the real
	 * hardware asserts an event, instead of only every VCPU1_TICK_PERIOD_US
	 * (10 ms) via the tick path below — see usbacm.h's threading-model
	 * comment for why a fixed 10 ms period is the wrong fit for USB
	 * full-speed frame timing. Must run before daifclr below (harmless if
	 * IRQs are still masked at that point: the SPI simply latches pending
	 * at the distributor, same non-eventful-until-unmasked behavior
	 * gic_timer_init()'s own diagnostic busy-wait already relies on for
	 * TIMER_INTID). */
	musb_irq_arm_cpu1();

	/* THE addition vcpu2.c does not need: without this core's own periodic
	 * CNTP tick, IMO=1 has nothing to route -- no physical IRQ means EL2
	 * never regains control here at all, and dbg_vcpu1's whole watchdog/
	 * dbgmon mitigation (gic_timer_irq(), gated on smp_cpu_id()==
	 * SMP_DEBUG_CPU) would simply never run. Preserving-CNTVOFF variant,
	 * same as main_dbg.c's own CPU0 arm: this core's guest vCPU gets its
	 * own virtual timebase, untouched by whatever CNTVOFF_EL2 happened to
	 * already hold. */
	gic_timer_arm_preserving_cntvoff(VCPU1_TICK_PERIOD_US);

	{
		uint64_t v;

		__asm__ volatile("mrs %0, vtcr_el2"  : "=r"(v)); bc(8, (uint32_t)v);
		__asm__ volatile("mrs %0, vttbr_el2" : "=r"(v));
		bc(9,  (uint32_t)v);
		bc(10, (uint32_t)(v >> 32));
		__asm__ volatile("mrs %0, hcr_el2"   : "=r"(v)); bc(11, (uint32_t)v);
		__asm__ volatile("mrs %0, sctlr_el1" : "=r"(v)); bc(12, (uint32_t)v);
	}

	guest_config();

	/* Unmask EL2 IRQ+FIQ on THIS core now that the tick is armed and
	 * stage-2/HCR are set -- from this instruction on, a physical timer
	 * IRQ on CPU1 traps to EL2 regardless of what the guest code entered
	 * below does to its own PSTATE.I. This is the actual mechanism the
	 * whole mitigation in vcpu1.h rests on. */
	__asm__ volatile("msr daifclr, #3" ::: "memory");

	kload_enter(g_entry, g_ctxid, g_entry);
}
