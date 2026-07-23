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

/* The shared guest-frame snapshot + debug-core flag live in el2_exc.c. */
extern struct el2_frame g_last_guest_frame;
extern volatile uint32_t dbg_core_active;

/* ISOLATION TEST flag (default 1 = skip EMAC/dbgmon on the debug core). Lets us
 * tell apart "CPU1 wedges in dbgmon_service" from "the guest resets the board":
 * with EMAC skipped, if the board STAYS resident the wedge was dbgmon_service;
 * if it still resets ~20-70s the reset is guest-initiated. Toggle over the net. */
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

/* OPT-IN escalation (default OFF): once emac_link_watchdog() gives up
 * (bounded self-heal exhausted, EMAC has NEVER accepted a single RX frame
 * despite ~6 retries over ~48 s), setting this to 1 makes CPU1 set
 * wdt_debug_hold — which (with wdt_pet() now honoring it too, see wdt.c)
 * stops BOTH watchdog-pet paths and lets the HW WDOG reboot the board to
 * U-Boot within ~16 s, where the persistent chimpd auto-reloads. Turns
 * "every dead-EMAC boot costs a physical power-cycle" into an automatic
 * bounded recovery. Left opt-in until the self-heal path is trusted live. */
volatile uint32_t dbg_emac_watchdog_reboot = 0;

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
#define GICD_BASE 0x01c81000UL
#define GICC_BASE 0x01c82000UL

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
__attribute__((weak)) void vblk_async_cpu2_run(void)
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
	if (cpu == SMP_DEBUG_CPU && dbg_core_enable) {
		uint32_t iters = 0;
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
				volatile uint32_t *pc = (volatile uint32_t *)0x01C20848UL;
				uint32_t v = *pc;
				if (((v >> 20) & 0xFu) != 3u)
					*pc = (v & ~(0xFu << 20)) | (3u << 20);
			}
			smp_bc_nodsb(5, ++iters); /* pre-service counter (word 5), NO dsb */
			/* ISOLATION TEST: dbgmon_service() TEMPORARILY DISABLED to prove
			 * whether the EMAC poll path is what wedges CPU1. If the board now
			 * stays resident forever (word5 huge, no reset), the wedge is in
			 * dbgmon_service; restore it once confirmed. */
			if (!dbg_isolate_no_emac) {
				dbgmon_service(&g_last_guest_frame);
				/* Self-heal for "EMAC never saw a frame": bounded +
				 * rate-limited internally, ~one branch per iteration on
				 * the healthy path. Escalation to a WDOG self-reboot is
				 * opt-in (dbg_emac_watchdog_reboot above). */
				if (emac_link_watchdog() && dbg_emac_watchdog_reboot)
					wdt_debug_hold = 1;
			}
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
		vblk_async_cpu2_run();
		/* NOTREACHED — both the weak and strong definitions loop forever —
		 * but fall through to the shared park below defensively in case
		 * that invariant is ever broken by a future change. */
	}

	/* CPU3 (and CPU2 defensively, see above): unused for now — park in WFI,
	 * out of the way. (The per-core preemptive-tick/sched path is
	 * intentionally NOT started; it's untested and only adds risk to the
	 * guest bring-up.) */
	for (;;)
		__asm__ volatile("wfi" ::: "memory");
}
