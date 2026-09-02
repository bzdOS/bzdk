/* SPDX-License-Identifier: BSD-2-Clause */

/* smp.c — SMP bring-up + per-core CNTP tick for the bzdOS EL2 microkernel.
 * See smp.h for the design rationale (why secondaries enable the MMU, the A53
 * SMPEN coherency gotcha, the breadcrumb layout). This file owns:
 *   - smp_boot_config[] / smp_stacks[] storage (referenced by start.S),
 *   - smp_init(): capture primary config + PSCI CPU_ON cores 1..3,
 *   - smp_secondary_main(): per-core C entry (tick + idle),
 *   - smp_timer_irq(): per-core CNTP handler (independent of gic_timer.c's
 *     single-core module state),
 *   - smp_heartbeat(): the 0x50000900 breadcrumb.
 *
 * Freestanding, no libc: <stdint.h> plus our own headers only.
 */
#include <stdint.h>
#include "smp.h"
#include "exceptions.h"
#include "timer.h"
#include "dbgmon.h"
#include "usbacm.h"
#include "musb.h"   /* emac_status_line_maybe(): inject EMAC diagnostics into
                     * the USB-ACM TX stream — the only channel alive when
                     * EMAC/dbgmon is dark */
#include "emac.h"   /* emac_link_watchdog() self-heal */
#include "wdt.h"
#include "hv_addrmap.h"   /* HVMAP_DBGTOOLS_HEARTBEAT: CPU1 debug-loop heartbeat */
#include "hwbp.h"          /* hwbp_set_wp_el2(): per-core EL2 self-watch arm */
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */
#ifdef HV_HDMI
#include "hud.h"    /* hud_update(): live HUD refresh on CPU1 (HV_HDMI build) */
#include "hdmi.h"   /* hdmi_phy_locked()/hdmi_relock(): PHY lock-loss defense */
#endif

/* The shared guest-frame snapshot + debug-core flag live in el2_exc.c. */
extern struct el2_frame g_last_guest_frame;
extern void el2_snapshot_guest_frame(struct el2_frame *out);  /* seqlock read (H8) */
extern volatile uint32_t dbg_core_active;

/* GDB-stub CPU1 RSP hosting (ROADMAP B2, docs/gdbstub-integration.md §5). The
 * cross-core stop-state globals live in el2_exc.o (always linked). gdb_channel
 * + the two stub entry points live in gdbstub.o, which the dbg/gdb builds link
 * but repl/fbsd/zephyr/hdmi (also using smp.o) do NOT — so they're WEAK: 0
 * there, and the `&gdb_channel` guard below keeps the whole gdb path inert. */
extern volatile uint32_t gdb_stop_pending;
extern volatile uint32_t gdb_stop_signal;
extern volatile uint32_t gdb_resume_act;
extern volatile uint32_t gdb_channel __attribute__((weak));
extern int  gdbstub_poll(struct el2_frame *guest) __attribute__((weak));
extern void gdbstub_on_debug_event(struct el2_frame *guest, int signal) __attribute__((weak));

/* ISOLATION TEST flag. Default 0 = the debug core runs EMAC/dbgmon normally
 * (the production path). Set to 1 (over the net) to skip EMAC/dbgmon on CPU1
 * and tell apart "CPU1 wedges in dbgmon_service" from "the guest resets the
 * board": with EMAC skipped, if the board STAYS resident the wedge was
 * dbgmon_service; if it still resets ~20-70s the reset is guest-initiated. */
volatile uint32_t dbg_isolate_no_emac = 0;

/* DIAGNOSTIC: when 1, CPU0 does NOT enter the guest (main_dbg.c) — only the
 * hypervisor + CPU1 debug core run, to test the debug core free of guest
 * interference. */
volatile uint32_t dbg_no_guest = 0;

/* DIAGNOSTIC: when 0, CPU1 does NOT pet the HW WDOG (relies on CPU0's
 * progress-gated wdt_pet). Isolates whether CPU1's WDOG write is what freezes
 * it once the guest touches the timer/WDOG block. Set 1 to restore CPU1 owning
 * the WDOG. */
volatile uint32_t dbg_cpu1_wdog = 1;

/* DIAGNOSTIC: when 0, CPU1 does NOT run the debug loop — it parks in WFI like
 * cpu2/3, touching NO shared hardware. Isolates "cores merely being ON breaks
 * the guest" (still resets) vs "the debug-loop activity breaks it" (guest then
 * boots to its old 26 KB). smp_init still PSCI-CPU_ON's the cores either way. */
volatile uint32_t dbg_core_enable = 1;

/* Gate the USB-ACM console poll on the CPU1 debug core. Default 1: the MUSB TX
 * path is now fully non-blocking — musb_putc() only appends to a staging ring
 * and musb_poll() pushes at most one bounded 64-byte packet per call (only if
 * the EP1-IN FIFO is free), so a host that stops draining /dev/ttyACM can no
 * longer pin CPU1 the way the old musb_tx_flush_now() ~200k-iter/byte spin did.
 * dbgmon/EMAC therefore stay serviced whether or not a USB host is attached.
 * Left runtime-toggllable (set 0 over the net) purely to silence the gadget if
 * ever desired. */
volatile uint32_t dbg_usbacm = 1;

/* ESCALATION, DEFAULT ON since 2026-08-27: once emac_link_watchdog() gives
 * up (bounded self-heal exhausted, EMAC has NEVER accepted a single RX frame
 * despite ~6 retries over ~48 s), CPU1 sets wdt_debug_hold — which (with
 * wdt_pet() now honoring it too, see wdt.c) stops BOTH watchdog-pet paths
 * and lets the HW WDOG reboot the board to U-Boot within ~16 s, where the
 * persistent chimpd auto-reloads. Turns "every dead-EMAC boot costs a
 * physical power-cycle" into an automatic bounded recovery. Was opt-in
 * until the self-heal path was trusted live; the trust evidence arrived
 * [MEASURED 2026-08-27: a cold-boot PHY-lottery boot stayed EMAC-dark
 * through the full retry budget and only the (then-manual) break-glass
 * recovered it], so the default flipped: a cable-left-unplugged board now
 * reboots on a visible ~90 s cycle instead of sitting dark until a human
 * notices. Togglable to 0 over the net if a quiet dark board is ever
 * preferred. */
volatile uint32_t dbg_emac_watchdog_reboot = 1;

/* HDMI PHY re-lock defense (HV_HDMI builds), DEFAULT ON. hdmi_init() brings the
 * pipeline up with the PHY locked, but ~1 s into guest boot FreeBSD's axp8xx
 * PMIC driver disables AXP803 dldo1 (vcc-hdmi-dsi, the PHY's hvcc-supply) as
 * "unused" — cutting PHY power — so PHY_STATUS bit7 drops to 0. When CPU1 sees
 * that (hdmi_phy_locked()==0), it calls hdmi_relock(), which RECLAIMS THE RSB
 * BUS AND RE-ENABLES dldo1 before re-running the PHY bring-up — the missing
 * piece that makes it actually re-lock (root cause found + fixed live
 * 2026-07-25: dldo1 off, not clock-gating). Rate-limited to at most one attempt
 * per 65536 loop passes; once it re-locks, hdmi_phy_locked()==1 and it stops
 * firing, so the ~110 ms cost is paid only while the signal is actually down.
 * Set 0 over the net to disable (e.g. to leave the RSB bus entirely to the
 * guest). */
volatile uint32_t dbg_hdmi_relock = 1;

/* Periodic EMAC-health status line injected into the USB-ACM console — the
 * ONLY channel proven alive when EMAC/dbgmon goes dark. Reads emac.c's own
 * breadcrumb window directly (0x50000100, layout documented in emac.c), the
 * same by-address pattern dbgmon.c uses for other modules' breadcrumbs.
 * Silent once EMAC has accepted at least one RX frame, so a healthy board
 * never sees this mixed into the guest's console output. */
#define EMAC_BC_BASE 0x50000100UL
static uint64_t g_emac_status_last_print;

static void put_hex32(uint32_t v)
{
	static const char hexd[] = "0123456789abcdef";
	int i;
	for (i = 28; i >= 0; i -= 4)
		musb_putc(hexd[(v >> i) & 0xfu]);
}

static uint32_t g_emac_last_rx;        /* last rx_count we saw change   */
static uint64_t g_emac_last_rx_ts;     /* when it last changed (ticks)  */

static void emac_status_line_maybe(void)
{
	volatile uint32_t *bc = (volatile uint32_t *)EMAC_BC_BASE;
	uint64_t freq, now, period;

	/* Banner condition: EMAC hasn't accepted a frame for 30+ s. The first
	 * version required rx_count==0 FOREVER, which stayed silent when EMAC
	 * took a few frames early and THEN went dark (observed live 2026-07-21:
	 * dead channel, guest hung, and not one banner to diagnose it with). */
	{
		uint64_t f = timer_freq();
		uint64_t t = timer_now();
		if (!f)
			f = 24000000ull;
		if (bc[7] != g_emac_last_rx) {
			g_emac_last_rx = bc[7];
			g_emac_last_rx_ts = t;
			return;                    /* traffic flowing: healthy */
		}
		if (g_emac_last_rx_ts == 0)
			g_emac_last_rx_ts = t;     /* first call: baseline */
		if (t - g_emac_last_rx_ts < f * 30ull)
			return;                    /* quiet <30 s: not alarming yet */
	}

	freq = timer_freq();
	if (!freq)
		freq = 24000000ull;
	now = timer_now();
	period = freq * 10ull;   /* ~10 s */
	if (g_emac_status_last_print && now - g_emac_status_last_print < period)
		return;
	g_emac_status_last_print = now;

	musb_puts("\r\n[EMAC-WDT] stage="); put_hex32(bc[1]);
	musb_puts(" link=");                put_hex32(bc[4]);
	musb_puts(" speed=");               put_hex32(bc[5]);
	musb_puts(" rx=");                  put_hex32(bc[7]);
	musb_puts(" tx=");                  put_hex32(bc[6]);
	musb_puts(" mdio_ok=");             put_hex32(bc[17]);
	musb_puts(" reinit=");              put_hex32(bc[18]);
	musb_puts("\r\n");
	musb_flush();
}

/* ------------------------------------------------------------------ *
 * Storage referenced by start.S (must be non-static, plain symbols).
 * ------------------------------------------------------------------ */
uint64_t smp_boot_config[SMP_CFG_N];
uint8_t  smp_stacks[SMP_MAX_CPUS][SMP_STACK_SIZE] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ *
 * Per-core private state. 64-byte aligned per entry to keep each core's
 * mutable fields on their own cache line (no false sharing between cores).
 * ------------------------------------------------------------------ */
struct smp_percpu {
	uint64_t next_deadline;   /* absolute CNTP CVAL for this core's next tick */
	uint64_t period_ticks;    /* tick period in counter ticks                 */
	uint64_t ticks;           /* CNTP ticks handled by this core (secondaries) */
	uint64_t heartbeat;       /* bumped every sched tick (all cores)          */
} __attribute__((aligned(64)));

static struct smp_percpu g_percpu[SMP_MAX_CPUS];

/* ------------------------------------------------------------------ *
 * Breadcrumb (0x50000900 "SMP1"). Same dc-civac+dsb store as the rest of
 * the tree so a `bc 0x50000900` over the net (or an md.l post-reset) sees it.
 * ------------------------------------------------------------------ */
static inline void smp_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)SMP_BC_BASE + (uint32_t)i * 4u);

	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Like smp_bc but NO `dsb sy` — only dc civac to push the value to PoC so it's
 * still readable post-reset, WITHOUT the blocking barrier. DIAGNOSTIC: tests
 * whether `dsb sy` is what wedges the CPU1 debug loop once the guest runs. */
static inline void smp_bc_nodsb(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)((uintptr_t)SMP_BC_BASE + (uint32_t)i * 4u);

	*p = v;
	__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
}

/* Online bitmap kept in a plain global so smp_num_online() is cheap; mirrored
 * to breadcrumb word 1. Set with a release store from each secondary. */
static volatile uint32_t g_online;

/* ------------------------------------------------------------------ *
 * GIC-400 + CNTP register access. Bases match gic_timer.c (cited there against
 * sun50i-a64.dtsi): GICD 0x01c81000, GICC 0x01c82000. We touch ONLY the
 * per-core (banked) pieces here — the distributor's global enable (GICD_CTLR)
 * was already set by the primary's gic_timer_init(); GICD_IGROUPR0 /
 * ISENABLER0 / IPRIORITYR for PPIs (INTID 0..31) are banked PER CORE, so each
 * secondary must (re)program its own copy for INTID 30.
 * ------------------------------------------------------------------ */
#define GICD_BASE SOC_A64_GICD_BASE
#define GICC_BASE SOC_A64_GICC_BASE

#define GICD_IGROUPR0    (*(volatile uint32_t *)(GICD_BASE + 0x080))
#define GICD_ISENABLER0  (*(volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_IPRIO30     (*(volatile uint8_t  *)(GICD_BASE + 0x400 + 30))

#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00c))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

#define TIMER_INTID       30u        /* CNTP non-secure physical timer PPI */
#define GIC_SPURIOUS_MIN  1020u
#define IAR_INTID(iar)    ((iar) & 0x3ffu)

#define CNTP_CTL_ENABLE   (1u << 0)  /* [0]=ENABLE, [1]=IMASK, [2]=ISTATUS */

#define HCR_EL2_IMO       (1ull << 4)

static inline void write_cntp_cval(uint64_t v)
{
	__asm__ volatile("msr cntp_cval_el0, %0" :: "r"(v) : "memory");
}

static inline void write_cntp_ctl(uint32_t v)
{
	__asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)v) : "memory");
}

/* ------------------------------------------------------------------ *
 * smp_init — runs on CPU0.
 * ------------------------------------------------------------------ */

/* PSCI CPU_ON (SMC64). x0=fn id 0xC4000003, x1=target MPIDR, x2=entry PA,
 * x3=context id. Returns the PSCI status in x0 (0 success, negative error). */
static int64_t psci_cpu_on(uint64_t target_mpidr, uint64_t entry_pa, uint64_t ctx)
{
	register uint64_t x0 __asm__("x0") = 0xC4000003ULL; /* PSCI_CPU_ON_64 */
	register uint64_t x1 __asm__("x1") = target_mpidr;
	register uint64_t x2 __asm__("x2") = entry_pa;
	register uint64_t x3 __asm__("x3") = ctx;

	__asm__ volatile("smc #0"
	                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
	                 :
	                 : "memory");
	return (int64_t)x0;
}

/* Clean the captured-config cache lines to the Point of Coherency so a
 * secondary reading smp_boot_config[] with its MMU still OFF (Device/
 * non-cacheable access) sees the values CPU0 just wrote in its D-cache. */
static void clean_boot_config(void)
{
	uintptr_t start = (uintptr_t)&smp_boot_config[0];
	uintptr_t end   = start + sizeof(smp_boot_config);
	uintptr_t p;

	for (p = start & ~63ULL; p < end; p += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

void smp_init(void)
{
	uint64_t v;
	uint32_t i;

	/* Lay down magic + a fresh online map (CPU0 is online by definition). */
	g_online = 1u;                 /* bit0 = CPU0 */
	smp_bc(0, SMP_BC_MAGIC);
	smp_bc(1, g_online);

	/* --- Capture the primary's EL2/MMU configuration. A secondary reloads
	 * these verbatim so it runs the SAME translation regime (cacheable Normal,
	 * identical mappings) as CPU0 — the precondition for coherent shared state.
	 * SCTLR is captured LAST-in-order intent: the secondary writes it LAST to
	 * flip the MMU+caches on only after MAIR/TCR/TTBR0/VBAR/HCR are in place. */
	__asm__ volatile("mrs %0, mair_el2"  : "=r"(v)); smp_boot_config[SMP_CFG_MAIR]  = v;
	__asm__ volatile("mrs %0, tcr_el2"   : "=r"(v)); smp_boot_config[SMP_CFG_TCR]   = v;
	__asm__ volatile("mrs %0, ttbr0_el2" : "=r"(v)); smp_boot_config[SMP_CFG_TTBR0] = v;
	__asm__ volatile("mrs %0, vbar_el2"  : "=r"(v)); smp_boot_config[SMP_CFG_VBAR]  = v;
	__asm__ volatile("mrs %0, hcr_el2"   : "=r"(v));
	smp_boot_config[SMP_CFG_HCR] = v | HCR_EL2_IMO;  /* ensure IRQ->EL2 on secondaries */
	__asm__ volatile("mrs %0, sctlr_el2" : "=r"(v)); smp_boot_config[SMP_CFG_SCTLR] = v;

	clean_boot_config();

	/* --- PSCI CPU_ON for affinities 1..3. On the single-cluster A64 the
	 * target MPIDR is simply the aff0 value (aff1=aff2=aff3=0), i.e. i. Entry
	 * is _start_secondary's physical address (VA==PA identity here). Context id
	 * = cpuid (informational; the secondary re-derives its id from MPIDR). */
	for (i = 1; i < SMP_MAX_CPUS; i++) {
		int64_t rc = psci_cpu_on((uint64_t)i,
		                         (uint64_t)(uintptr_t)&_start_secondary,
		                         (uint64_t)i);
		smp_bc(2 + (int)(i - 1), (uint32_t)(int32_t)rc); /* words 2,3,4 */
	}

	/* --- Bounded wait for the secondaries to report online. Each has to set
	 * SMPEN, reload the MMU config, enable its MMU, init its CNTP and reach
	 * smp_secondary_main — a few hundred microseconds; give it generous slack
	 * and never hang if a core is dead (its bit just stays clear). */
	{
		uint64_t freq  = timer_freq();
		uint64_t start = timer_now();
		uint64_t limit = (freq ? freq : 24000000ull) / 10; /* ~100 ms */

		/* Busy-poll, NOT wfe: a bare `wfe` blocks until an event, and if every
		 * secondary faults BEFORE its `sev` (line ~299) no event ever arrives —
		 * so the timeout check (which sits before the wfe) is never re-reached
		 * and CPU0 hangs forever. Spin with a `yield` hint so the bounded
		 * timeout ALWAYS fires and smp_init returns even if no core comes up. */
		while ((g_online & 0xEu) != 0xEu) {          /* wait for bits 1,2,3 */
			if (timer_now() - start > limit)
				break;
			__asm__ volatile("yield" ::: "memory");
		}
	}

	smp_bc(1, g_online);
}

uint32_t smp_num_online(void)
{
	return (uint32_t)__builtin_popcount(g_online);
}

/* ------------------------------------------------------------------ *
 * Per-core CNTP tick.
 * ------------------------------------------------------------------ */

/* Program THIS core's banked GIC state + arm its own CNTP comparator. Touches
 * only per-core registers (see the register-block comment) so it cannot disturb
 * CPU0's already-running tick. */
static void smp_timer_init_secondary(uint32_t cpu, uint32_t period_us)
{
	uint64_t freq = timer_freq();
	uint64_t period_ticks;
	uint64_t now;
	uint64_t hcr;

	if (freq == 0)
		freq = 24000000ull;
	period_ticks = (freq * (uint64_t)period_us) / 1000000ull;
	if (period_ticks == 0)
		period_ticks = 1;

	g_percpu[cpu].period_ticks = period_ticks;
	g_percpu[cpu].ticks = 0;

	/* Banked PPI config for INTID 30 (this core's copy): Group 1 (NS -> IRQ),
	 * highest priority, forwarding enabled. Mirrors gic_timer.c's choices. */
	GICD_IGROUPR0  |= (1u << TIMER_INTID);
	GICD_IPRIO30    = 0x00u;
	GICD_ISENABLER0 = (1u << TIMER_INTID);

	/* This core's CPU interface: mask wide open, Group 1 forwarding on. */
	GICC_PMR  = 0xffu;
	GICC_CTLR = 0x3u;

	/* Route physical IRQ to EL2 on this core (idempotent with the captured HCR). */
	__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
	hcr |= HCR_EL2_IMO;
	__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");

	/* Arm the first interval on this core's own comparator. */
	now = timer_now();
	g_percpu[cpu].next_deadline = now + period_ticks;
	write_cntp_cval(g_percpu[cpu].next_deadline);
	write_cntp_ctl(CNTP_CTL_ENABLE);   /* ENABLE=1, IMASK=0 */
}

void smp_timer_irq(struct el2_frame *frame)
{
	uint32_t cpu   = smp_cpu_id();
	uint32_t iar   = GICC_IAR;
	uint32_t intid = IAR_INTID(iar);

	(void)frame;

	if (intid >= GIC_SPURIOUS_MIN)
		return;                        /* spurious: no EOI (GICv2) */

	if (intid != TIMER_INTID) {
		GICC_EOIR = iar;               /* not ours, but clear the active bit */
		return;
	}

	/* Re-arm from the PREVIOUS deadline (not "now") so handler latency doesn't
	 * accumulate into the period. This core's deadline is private — no race
	 * with the other cores' ticks, unlike gic_timer.c's shared global. */
	if (cpu < SMP_MAX_CPUS) {
		g_percpu[cpu].next_deadline += g_percpu[cpu].period_ticks;
		write_cntp_cval(g_percpu[cpu].next_deadline);
		g_percpu[cpu].ticks++;
	}
	write_cntp_ctl(CNTP_CTL_ENABLE);

	GICC_EOIR = iar;
}

/* ------------------------------------------------------------------ *
 * Heartbeat — called from sched_tick() on EVERY core, once per tick.
 * ------------------------------------------------------------------ */
void smp_heartbeat(uint32_t cpu, int current_task)
{
	if (cpu >= SMP_MAX_CPUS)
		return;
	g_percpu[cpu].heartbeat++;
	smp_bc(6 + (int)cpu, (uint32_t)g_percpu[cpu].heartbeat);   /* words 6..9  */
	smp_bc(10 + (int)cpu, (uint32_t)current_task);            /* words 10..13 */
}

/* ------------------------------------------------------------------ *
 * CPU2 async eMMC I/O offload hook (ROADMAP C2 — see vblk_async.h for the
 * full design). WEAK default: a plain WFI park, byte-for-byte the old
 * "unused secondary" behaviour this replaces. vblk_async.c (linked ONLY
 * into the dbg build's DBG_OBJS) provides a STRONG override that actually
 * runs the mailbox-draining loop; every other Makefile target
 * (stage0/net/repl/fbsd/gdb) does not link vblk_async.o and keeps this weak
 * park unchanged — the dispatch in smp_secondary_main() below therefore
 * never changes behaviour for those targets. (Several of those targets
 * already fail to link today for unrelated pre-existing reasons — missing
 * musb.o/dbgmon.o/etc. this file's OTHER calls also depend on — so this is
 * not a new fragility, just consistent with how the rest of smp.c already
 * has build-specific dependencies.)
 *
 * NOTE: linking vblk_async.o alone does not make the mailbox producer side
 * (vblk_emmc.c's vblk_async_post()) active — that additionally requires
 * g_vblk_async_ready, which THIS function sets, on the same "who's actually
 * running" logic. See vblk_async.h / vblk_emmc.h for why there must not be
 * an independent toggle for one half without the other. */
__attribute__((weak)) void vcpu2_run(void)
{
}

/* Third guest vCPU, EXPERIMENTAL — see vcpu1.h. Weak default: a no-op
 * return, so the caller (smp_secondary_main(), below) falls straight
 * through into the existing SMP_DEBUG_CPU tight loop, byte-for-byte
 * unchanged, on every target that doesn't link vcpu1.o. Same pattern as
 * vcpu2_run() immediately above. */
__attribute__((weak)) void vcpu1_run(void)
{
}

__attribute__((weak)) void vblk_async_cpu2_run(void)
{
	for (;;)
		__asm__ volatile("wfi" ::: "memory");
}

/* Fourth guest vCPU, on CPU3 — see vcpu3.h. Weak default: a no-op return, so
 * the caller (smp_secondary_main(), below) falls straight through into
 * zephyr_cpu3_run() (the `dual` target's real Zephyr path, or that
 * function's own weak WFI park everywhere else), on every target that
 * doesn't link vcpu3.o. Same pattern as vcpu1_run()/vcpu2_run() above, and
 * the SAME dispatch shape CPU2 already uses (try the FreeBSD-vCPU path
 * first, fall through to the pre-existing CPU3 behaviour if it declines).
 * vcpu3.o and zguest_cpu3.o are additionally mutually exclusive at LINK
 * TIME via bzdos_cpu3_owner — see vcpu3.h's header comment for why a
 * Makefile-only "don't link both" convention was judged insufficient here,
 * unlike CPU2's vcpu2.o/vblk_async.o pairing (vblk_async_cpu2_run() has no
 * feature of its own that a parked vcpu2 could silently orphan;
 * zephyr_cpu3_run()'s `zboot` does). */
__attribute__((weak)) void vcpu3_run(void)
{
}

/* ------------------------------------------------------------------ *
 * CPU3 Zephyr-guest hook (dual-guest milestone — see zguest_cpu3.h for the
 * full design). WEAK default: a plain WFI park, byte-for-byte the old
 * "unused secondary" behaviour this replaces — EXACT same pattern as
 * vblk_async_cpu2_run() above. zguest_cpu3.c (linked ONLY into the `dual`
 * target's DUAL_OBJS) provides a STRONG override that actually parks in a
 * wfe-poll loop waiting for a host-issued `zboot` (dbgmon.c) before ever
 * touching a Zephyr image; every other Makefile target (stage0/net/repl/
 * fbsd/dbg/gdb/hdmi/zephyr/every QEMU target) does not link zguest_cpu3.o
 * and keeps this weak park unchanged — the dispatch in smp_secondary_main()
 * below therefore never changes behaviour for those targets. */
__attribute__((weak)) void zephyr_cpu3_run(void)
{
	for (;;)
		__asm__ volatile("wfi" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * Secondary C entry — MMU already ON and coherent (set up in start.S).
 * ------------------------------------------------------------------ */
void smp_secondary_main(uint64_t cpuid)
{
	uint32_t cpu = (uint32_t)cpuid;

	/* Announce online with an atomic set of this core's bit. The A53 is
	 * ARMv8.0 (no LSE atomics), so use a load-acquire / store-release
	 * exclusive loop rather than a one-shot stset. Then SEV to wake CPU0's
	 * WFE wait in smp_init(). */
	{
		uint32_t bit = 1u << cpu;
		uint32_t old, ok;
		__asm__ volatile(
			"1:	ldaxr	%w0, [%3]\n"
			"	orr	%w0, %w0, %w2\n"
			"	stlxr	%w1, %w0, [%3]\n"
			"	cbnz	%w1, 1b\n"
			: "=&r"(old), "=&r"(ok)
			: "r"(bit), "r"(&g_online)
			: "memory");
	}
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
	smp_bc(1, g_online);

	/* EL2 SELF-WATCH, per-core arm (opt-in, see main_dbg.c's copy).
	 *
	 * DBGWVR/DBGWCR are BANKED PER PE. Arming from main_dbg.c covers CPU0 and
	 * ONLY CPU0, so a store from this core is invisible to it — which produced
	 * a confidently wrong "no CPU wrote it, must be DMA" reading on 2026-08-01
	 * before the gap was spotted. CPU1 in particular owns EMAC + usbacm, i.e.
	 * the console path, so it is the LIKELIEST writer of console bytes, and it
	 * was the one core guaranteed not to be watched.
	 *
	 * This tree has been bitten by per-PE banking twice before (see project
	 * memory: a GICH_HCR read that returned CPU1's bank, and banked GIC PPI
	 * state read from the wrong core). Arm every secondary too, so "our own
	 * instructions" means all of them. */
#if defined(EL2_SELFWATCH_ADDR) && (EL2_SELFWATCH_ADDR)
	hwbp_set_wp_el2(0, (uint64_t)(EL2_SELFWATCH_ADDR));
	smp_bc(8, 0x5e1f0000u | cpu);   /* selfwatch armed on this core */
#endif

	/* ---- CPU1 = dedicated EMAC/dbgmon DEBUG CORE ------------------------
	 * The whole point (fix for "the debugger dies when the guest wedges"):
	 * this core does NOTHING but poll EMAC and service dbgmon in a tight
	 * loop, INDEPENDENT of CPU0/the guest. IRQ stays MASKED (no timer, no
	 * sched) so nothing preempts the poll. dbgmon_service() polls the EMAC
	 * console internally (console_poll) and answers memory/breadcrumb reads
	 * and gr/sr (from the shared g_last_guest_frame snapshot) — so even when
	 * CPU0 is stuck spinning in the guest, this core keeps the board fully
	 * inspectable over the network. CPU0 sees dbg_core_active=1 and stops
	 * touching EMAC itself, so the two never race the MAC. */
	/* Third guest vCPU, EXPERIMENTAL (vcpu1.h) — checked BEFORE the debug-
	 * core branch below, same ordering vcpu2_run() already uses for CPU2.
	 * Returns immediately (falling through unchanged) unless dbg_vcpu1 is
	 * armed AND vcpu1.o is linked; noreturn once a request is accepted. */
	if (cpu == SMP_DEBUG_CPU)
		vcpu1_run();

	if (cpu == SMP_DEBUG_CPU && dbg_core_enable) {
		uint32_t iters = 0;
#ifdef HV_HDMI
		uint32_t last_relock_iters = 0;
#endif
		dbg_core_active = 1;
		__asm__ volatile("dsb sy" ::: "memory");
		for (;;) {
			if (dbg_cpu1_wdog)
				wdt_debug_kick();   /* own the HW WDOG (diag-gated) */
			/* Enforce the eMMC clock pinmux: PC5 (PIO PC_CFG0 @0x01C20848,
			 * nibble 5) must be function 3 (mmc2). FreeBSD's pinctrl muxes the
			 * other mmc2 pins but leaves PC5 at gpio, killing the eMMC clock ->
			 * OCR=0. Hold it at 3 (RMW preserves FreeBSD's other PC pins; write
			 * only when it drifted, so we don't fight the bus every iteration). */
			{
				volatile uint32_t *pc = (volatile uint32_t *)SOC_A64_PIO_PC_CFG0;
				uint32_t v = *pc;
				if (((v >> 20) & 0xFu) != 3u)
					*pc = (v & ~(0xFu << 20)) | (3u << 20);
			}
			smp_bc_nodsb(5, ++iters); /* pre-service counter (word 5), NO dsb */
			/* dbgtools heartbeat (hv_addrmap.h HVMAP_DBGTOOLS_HEARTBEAT):
			 * bumped UNCONDITIONALLY every pass of this loop, regardless
			 * of which branch below (gdb / dbgmon / neither) it takes —
			 * deliberately a SEPARATE word from SMP1's word[5]/`iters`
			 * above rather than reusing it: that one is "NO dsb" (best-
			 * effort, meant for a live `bc` read while things are
			 * healthy), whereas this one IS dc-civac+dsb'd so a raw,
			 * protocol-independent EMAC peek (dbgtools.c's
			 * ETHERTYPE_DBGRAW, answered straight from emac_poll() —
			 * see emac.c) always observes the true latest value, even
			 * mid-iteration. See dbgtools.h for the full rationale and
			 * its documented limitation. */
			{
				volatile uint32_t *hb = (volatile uint32_t *)
					HVMAP_DBGTOOLS_HEARTBEAT;
				*hb = iters;
				__asm__ volatile("dc civac, %0\n\tdsb sy"
				                  :: "r"(hb) : "memory");
			}
			/* Runtime kill-switch (default 0): when dbg_isolate_no_emac is
			 * set, dbgmon_service()/emac_poll() are skipped but the loop
			 * keeps running (usbacm below still drains the vconsole rings).
			 * NOT an experiment-in-flight: the isolation test this once
			 * served has concluded long ago; the flag survives as a
			 * diagnostic lever for "is the EMAC poll path itself the
			 * wedge?" questions. */
			if (&gdb_channel && gdb_channel) {
				/* GDB mode (ROADMAP B2): the `gdb` command routed this channel
				 * to the RSP stub. Two service points: (a) CPU0 parked in
				 * el2_trap on a bp/step/wp — run the command loop against the
				 * shared frame and release it; (b) guest running — poll for an
				 * async $cmd / Ctrl-C. Both bounded/non-blocking. The weak
				 * guard above means this is unreachable in a build without
				 * gdbstub.o. */
				if (gdb_stop_pending) {
					if (gdbstub_on_debug_event)
						gdbstub_on_debug_event(&g_last_guest_frame,
						                       (int)gdb_stop_signal);
					/* Clear BEFORE waking CPU0 (matches the handshake
					 * contract documented in el2_exc.c: "CPU0 set, CPU1
					 * clears once served"). Ownership used to sit with
					 * CPU0 instead (cleared after its wfe wake-up), which
					 * left a window where this loop could come back around
					 * to the check above while gdb_stop_pending was still
					 * 1 (CPU0 hadn't woken from wfe yet) and re-enter
					 * gdbstub_on_debug_event() a second time against the
					 * same stale frame — sending GDB an unsolicited extra
					 * stop-reply that desyncs its running/stopped state.
					 * Clearing here, before the resume signal even goes
					 * out, closes that window: by the time CPU0 can
					 * possibly run again, this flag already reads 0. */
					gdb_stop_pending = 0u;
					gdb_resume_act = 1u;   /* any non-sentinel resumes CPU0 */
					__asm__ volatile("dsb sy\n\tsev" ::: "memory");
				} else if (gdbstub_poll) {
					gdbstub_poll(&g_last_guest_frame);
				}
			} else if (!dbg_isolate_no_emac) {
				struct el2_frame snap;
				el2_snapshot_guest_frame(&snap);   /* consistent copy (H8) */
				dbgmon_service(&snap);
				/* Self-heal for "EMAC never saw a frame": bounded +
				 * rate-limited internally, ~one branch per iteration on
				 * the healthy path. Escalation to a WDOG self-reboot is
				 * opt-in (dbg_emac_watchdog_reboot above). internal-note
				 * wires the EMAC-dark detector to the BZDBG break-glass
				 * bg_seq {0x00,'~','B','Z','R','S','T',0x00} path
				 * (usbacm_force_breakglass) so an autonomous board-side
				 * EMAC-dark recovery needs no host typing. Both paths set
				 * the same wdt_debug_hold gate; usbacm_force_breakglass
				 * is the single chokepoint for break-glass semantics. */
				if (emac_link_watchdog() && dbg_emac_watchdog_reboot) {
					wdt_debug_hold = 1;
					usbacm_force_breakglass();
				}
			}
#ifdef HV_HDMI
			/* Live HUD refresh on the physical monitor. CPU1 owns the display
			 * (as it owns the debug console) since CPU0 is inside the guest.
			 * hud_update() repaints the whole 1080p frame — HUNDREDS of ms of
			 * CPU1 time — so it is BOTH delayed and heavily rate-limited as
			 * simple hygiene: the first ~500k loop passes go entirely to
			 * usbacm_poll()/dbgmon/eMMC-pinmux while the board is coming up, and
			 * thereafter the HUD repaints only every 128k-th pass — a few times
			 * a second, plenty for a status display, so a heavy repaint never
			 * dominates the other per-iteration servicing this loop does. Uses a
			 * consistent guest-frame snapshot (H8 seqlock), the same source
			 * dbgmon reads. (Note: an earlier version blamed this refresh for a
			 * missing /dev/ttyACM0 — that was actually host-side: the host
			 * usb_debug driver hijacking the HV's 1d6b:0010 CDC-ACM gadget, see
			 * the usb-debug-hijacks-ttyacm note. The gadget enumerates fine and
			 * the guest boots to root under HV_HDMI.) */
			if (iters > 500000u && (iters & 0x1ffffu) == 0) {
				struct el2_frame hud_snap;
				el2_snapshot_guest_frame(&hud_snap);
				hud_update(&hud_snap);
			}

			/* Real vblank, one MMIO read per pass. CPU1 owns the display,
			 * so it is also the right core to observe the panel: the
			 * latching TCON status bit means every vblank is counted
			 * exactly once, and the count + timestamp are published to
			 * the guest through the scanout register file so a guest KMS
			 * driver can stop inventing its own 60 Hz. See
			 * hdmi_vblank_poll(). Deliberately NOT gated on the
			 * 500000-iteration warm-up below: unlike the HUD repaint and
			 * the relock sequence, this neither draws nor reprograms
			 * anything, so there is nothing for it to fight during
			 * bring-up -- and the count is more useful the earlier it
			 * starts. */
			hdmi_vblank_poll();

			/* HDMI PHY lock-loss defense: CPU1 owns the display, so it also
			 * defends it. Live-board-confirmed (see hdmi_relock()'s comment)
			 * the guest never actually touches the CCU/DE2/TCON bits
			 * hdmi_init() programmed -- only the analog PHY's own lock
			 * status drops sometime after boot. The check itself is one
			 * cheap MMIO read every pass; only the (bounded, ~200ms) relock
			 * sequence is rate-limited, via last_relock_iters, so a monitor
			 * that's simply unplugged doesn't make CPU1 burn all its time
			 * retrying instead of servicing dbgmon/usbacm/the WDOG kick.
			 * Gated on the same 500000-iteration warm-up as the HUD repaint
			 * above so it never fights hdmi_init()'s own bring-up. */
			if (dbg_hdmi_relock && !hdmi_phy_locked() &&
			    (iters - last_relock_iters) > 65536u) {
				hdmi_relock();
				last_relock_iters = iters;
			}
#endif
			/* USB-OTG CDC-ACM interactive console bridge (usbacm.c):
			 * services the MUSB gadget and pumps both vconsole bridge
			 * rings, independent of the EMAC/dbgmon isolation flag above
			 * (a USB-only host has no EMAC path to fall back on, so this
			 * must keep running even with dbg_isolate_no_emac set). This
			 * is the ONLY place in the tree that touches MUSB registers
			 * once the hypervisor is up -- see usbacm.h's threading-model
			 * note for why that single-owner discipline matters. */
			if (dbg_usbacm) {
				usbacm_poll();   /* now fully non-blocking + bounded per call:
				                  * musb_putc only stages into a ring and
				                  * musb_poll pushes one 64-byte packet if the
				                  * EP1-IN FIFO is free, so a stalled host can no
				                  * longer starve dbgmon (see dbg_usbacm above). */
				emac_status_line_maybe(); /* EMAC-dark diagnostics over the
				                           * only-alive channel (USB-ACM) */
			}
			smp_bc_nodsb(6, iters);        /* post-service counter (word 6) */
			smp_bc_nodsb(7, (uint32_t)timer_now()); /* last CNTPCT (word 7) */
		}
	}

	/* ---- CPU2 = ROADMAP C2 async eMMC I/O offload core -------------------
	 * See vblk_async.h for the full design. Weak/strong linkage (above):
	 * a plain WFI park unless vblk_async.o is linked in (dbg build only),
	 * in which case this call runs the mailbox-draining loop and never
	 * returns. */
	if (cpu == 2) {
		/* Second guest vCPU, if armed. vcpu2_run() returns immediately
		 * when dbg_vcpu2 is 0, so the async-I/O worker below stays the
		 * default and this core behaves exactly as it always has until
		 * the feature is deliberately switched on. Weak, so targets
		 * that do not link vcpu2.o are unaffected. */
		vcpu2_run();
		vblk_async_cpu2_run();
		/* NOTREACHED — both the weak and strong definitions loop forever —
		 * but fall through to the shared park below defensively in case
		 * that invariant is ever broken by a future change. */
	}

	/* ---- CPU3 = dual-guest Zephyr core, OR a 4th guest vCPU (weak/strong,
	 * see above) ------------------------------------------------------
	 * Every existing target links only the weak zephyr_cpu3_run() (a plain
	 * WFI park, identical to the old inline loop this replaces); the `dual`
	 * target's zguest_cpu3.o overrides it with the real wfe-poll-for-
	 * `zboot` implementation. vcpu3_run() is tried FIRST, same ordering
	 * vcpu2_run() uses ahead of vblk_async_cpu2_run() for CPU2: it returns
	 * immediately when dbg_vcpu3 is 0, so zephyr_cpu3_run() stays the
	 * default and this core behaves exactly as it always has until the
	 * feature is deliberately switched on. Weak, so targets that do not
	 * link vcpu3.o are unaffected. vcpu3.o and zguest_cpu3.o are mutually
	 * exclusive at link time (bzdos_cpu3_owner, see vcpu3.h) — this
	 * dispatch never has to choose between two ALREADY-STRONG
	 * implementations at runtime. */
	if (cpu == 3) {
		vcpu3_run();
		zephyr_cpu3_run();
		/* NOTREACHED — see the CPU2 comment above for the same defensive
		 * fallthrough reasoning. */
	}

	/* Defensive shared park (unreachable for cpu==2/3 today; kept for any
	 * future core / as a last-resort catch-all). */
	for (;;)
		__asm__ volatile("wfi" ::: "memory");
}
