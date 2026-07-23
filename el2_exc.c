/* el2_exc.c — EL2 trap handler for the bzdOS microkernel.
 *
 * Called from el2_common (exceptions.S) on every EL2 exception. Its job for
 * now is the debugging bedrock: capture WHAT faulted (ESR/ELR/FAR + regs) into
 * a dedicated, cache-coherent DRAM breadcrumb so a post-mortem `md.l` (or the
 * live network REPL) shows the exact fault instead of the board dying
 * silently. Later this same entry point becomes the RTOS trap: an IRQ/FIQ
 * here will drive the preemptive scheduler and the generic-timer tick.
 *
 * Breadcrumb window: 0x50000400 (distinct from MUSB 0x50000000, EMAC
 * 0x50000100, REPL 0x50000300). Same dc-civac+dsb store as the rest so the
 * record survives a WDT reset with the D-cache on.
 *   [0]  magic 0x4558_4331 ("EXC1")
 *   [1]  count  (total exceptions seen)
 *   [2]  kind   (last vector index)
 *   [3]  esr    (low 32)
 *   [4]  elr    (low 32)   [5] elr (high 32)
 *   [6]  far    (low 32)   [7] far (high 32)
 *   [8]  x0(lo) [9] x1(lo) [10] x30(lo)  (a few regs for context)
 */
#include <stdint.h>
#include "exceptions.h"
#include "guest.h"
#include "sched.h"
#include "vconsole.h"
#include "gtrace.h"
#include "hwbp.h"
#include "firstfault.h"
#include "onebp.h"
#include "wdt.h"
#include "smp.h"
#include "vblk_emmc.h"
#include "vnet_emac.h"   /* vnet_mmio_fault() -- ROADMAP C1 virtio-net-over-EMAC */
#include "reboot.h"
#include "backtrace.h"
#include "flightrec.h"
#include "coredump.h"

/* Shared snapshot of the guest register frame, written by CPU0 on every
 * guest-group trap and read by the SMP debug core (CPU1) so it can serve
 * gr/sr over EMAC even while CPU0 is wedged inside the guest. Caches are
 * coherent across cores (SMPEN set in start.S) so a plain global suffices. */
struct el2_frame g_last_guest_frame;
/* Set to 1 by the debug core (smp_secondary_main, CPU1) once it owns the
 * EMAC/dbgmon channel. While set, CPU0 does NOT call dbgmon_service itself —
 * only the debug core talks to EMAC, so the two never race the port. */
volatile uint32_t dbg_core_active;

/* When 1, intercept guest PSCI SYSTEM_RESET/OFF (don't actually reset the SoC)
 * so the board stays alive for inspection — the SMP debug core keeps EMAC up.
 * Default 1: forwarding the guest's SYSTEM_RESET drops the board to a hard SoC
 * reset that fell off the USB bus entirely (confirmed 2026-07-18), so we NEVER
 * forward it; the guest thinks the reset succeeded and we keep the board live. */
volatile uint32_t dbg_block_reset = 1;

/* When 1 (default), a guest PSCI SYSTEM_OFF (0x84000008) is treated as an
 * INTENTIONAL clean poweroff and honored with a clean warm reset back to
 * U-Boot (reboot_clean) instead of the dbg_block_reset "fake success, stay
 * alive" path. SYSTEM_OFF is only ever issued by `shutdown -p`/`halt -p`,
 * which run the full rc shutdown sequence FIRST — sync + unmount, so the
 * on-disk UFS fs_clean flag is already 1 by the time the SMC reaches us.
 * Doing a controlled WDOG warm reset here (supervisor reloads a fresh
 * HV+guest) is the DURABLE fix for the fs_clean re-dirtying problem: the one
 * intentional shutdown path now leaves the filesystem clean, exactly as real
 * hardware would on `shutdown -p`, instead of us hand-patching the superblock
 * (ufs_clean.py) after every hard reset. Clear this over the net if a debug
 * session wants the old catch-and-stay-alive behavior on SYSTEM_OFF too.
 * NOTE: SYSTEM_RESET (0x84000009) is deliberately NOT auto-honored — it is
 * ambiguous (an intentional `reboot` vs a panic auto-reboot) and stays on the
 * dbg_block_reset stay-alive path so crash-reboots can still be inspected. */
volatile uint32_t dbg_clean_off = 1;

/* 0x50000400 — the DOCUMENTED exc window that the READERS (hud.c, dbgmon.c
 * cmd_ff DBGMON_EXC_BASE) already expect. The writer was wrongly pointing at
 * SRAM 0x00018100 (a) mismatching those readers and (b) getting wiped by the
 * BROM/U-Boot on a warm WDT reset so EL2-fault post-mortems read garbage. DRAM
 * 0x50000400 survives the warm reset and matches the readers. */
#define EXC_BC_BASE 0x50000400UL
#define EXC_MAGIC   0x45584331u   /* "EXC1" */

static uint32_t exc_count;

/* B3 crash-forensics flood guard (see the call site below for the full
 * rationale): hard cap on how many coredump_send() streams (each up to
 * COREDUMP_MAX_TOTAL bytes) we're willing to emit in a single boot, as a
 * backstop behind the same-fault latch. Picked comfortably above "one" (so a
 * genuine handful of distinct faults during a debugging session all get a
 * coredump) but nowhere near "unbounded". */
#define CORE_DUMPS_PER_BOOT 4u

static inline void exc_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(EXC_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Optional best-effort console line. Weak so a build without a console
 * (e.g. a pure fault-catcher) still links; main_*.c provides the real one. */
__attribute__((weak)) void exc_report_line(const struct el2_frame *f)
{
	(void)f;
}

/* Timer-IRQ handler, provided by gic_timer.c. Weak fallback so a build
 * without the GIC lane still links (the call is then a no-op). */
__attribute__((weak)) void gic_timer_irq(struct el2_frame *f)
{
	(void)f;
}

/* Live debug monitor, provided by dbgmon.c in the debugger build. Weak
 * fallback so the REPL/fbsd builds (no dbgmon.o) still link — called only on
 * a guest tick to service the network debugger against the live guest frame. */
__attribute__((weak)) void dbgmon_service(struct el2_frame *f)
{
	(void)f;
}

/* virtio-net-over-EMAC MMIO device (vnet_emac.c, ROADMAP C1). Weak fallback
 * (same pattern as gic_timer_irq/dbgmon_service above) so the `gdb`/`fbsd`
 * builds -- which don't link vnet_emac.o -- still link; the call then always
 * reports "not my window" and every existing image's behavior is unchanged. */
__attribute__((weak)) int vnet_mmio_fault(struct el2_frame *frame)
{
	(void)frame;
	return 0;
}

/* ------------------------------------------------------------------ *
 * EL2 software single-step of the guest.
 *
 * Mechanism: MDCR_EL2.TDE=1 routes EL1/EL0 debug exceptions to EL2; guest
 * MDSCR_EL1.SS=1 + PSTATE.SS=1 (SPSR_EL2 bit21 on eret) makes each guest
 * instruction generate a Software Step exception taken to EL2 with
 * ESR_EL2.EC==0x32 (step from a lower EL). el2_trap dispatches EC==0x32 to
 * el2_ss_handle(), which records (guest PC, guest SP) into a wrapping ring,
 * re-arms PSTATE.SS, and erets — so the guest advances one instruction at a
 * time and we log the exact PC where SP first crosses _etext / the first
 * data abort occurs.
 *
 * Ring: fixed DRAM window at 0x50002800, magic "SST1". Header 8 words, then
 * SS_SLOTS entries of 4 words each: pc(lo,hi), sp(lo,hi). 8 + 256*4 = 1032
 * words (0x1020 bytes) -> 0x50002800..0x50003820. NOTE: that range overlaps
 * the HDMI breadcrumb at 0x50003000, but hdmi.o/hud.o are NOT part of the
 * debugger (DBG_OBJS) build that owns single-step, so there is no runtime
 * collision here — flagged as a landmine for any future build that links
 * both single-step and the HDMI pipeline.
 *
 * Header words: [0]=magic [1]=total steps [2]=head (next slot idx, wraps)
 *               [3]=enabled flag  [4..7]=reserved.
 *
 * Enabled via the dbgmon `ss` command (el2_ss_toggle) right before/while the
 * guest runs; OFF by default. */
#define SS_BASE    0x50002800UL
#define SS_MAGIC   0x53535431u   /* "SST1" */
#define SS_SLOTS   256u
#define SS_HDR     8u            /* header words before the slot array */

#define SPSR_SS_BIT (1ull << 21) /* PSTATE.SS  (single-step active) */
#define SPSR_D_BIT  (1ull << 9)  /* PSTATE.D   (debug-exception mask) */

static int      el2_ss_on;       /* single-step currently enabled */
static uint32_t el2_ss_head;     /* next slot index 0..SS_SLOTS-1 */
static uint32_t el2_ss_total;    /* total steps recorded */

static inline void ss_wr(uint32_t widx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(SS_BASE + (uint32_t)widx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static void ss_ring_reset(void)
{
	uint32_t i;

	for (i = 0; i < SS_HDR + SS_SLOTS * 4u; i++)
		ss_wr(i, 0u);
	el2_ss_head = 0;
	el2_ss_total = 0;
	ss_wr(0, SS_MAGIC);
}

static inline void ss_set_mdscr(int on)
{
	uint64_t v;

	__asm__ volatile("mrs %0, mdscr_el1" : "=r"(v));
	if (on)
		v |= 1ull;           /* MDSCR_EL1.SS (bit0) */
	else
		v &= ~1ull;
	__asm__ volatile("msr mdscr_el1, %0\n\tisb" :: "r"(v) : "memory");
}

static inline void ss_set_tde(int on)
{
	uint64_t v;

	__asm__ volatile("mrs %0, mdcr_el2" : "=r"(v));
	if (on)
		v |= (1ull << 8);    /* MDCR_EL2.TDE (bit8): debug exceptions -> EL2 */
	else
		v &= ~(1ull << 8);
	__asm__ volatile("msr mdcr_el2, %0\n\tisb" :: "r"(v) : "memory");
}

/* Record one (guest PC, guest SP) sample and re-arm PSTATE.SS for the next
 * step. ELR_EL2 (frame->elr) already points at the NEXT guest instruction —
 * do NOT advance it. */
static void el2_ss_handle(struct el2_frame *frame)
{
	uint64_t sp1;
	uint32_t base;

	__asm__ volatile("mrs %0, sp_el1" : "=r"(sp1));

	base = SS_HDR + el2_ss_head * 4u;
	ss_wr(base + 0u, (uint32_t)frame->elr);
	ss_wr(base + 1u, (uint32_t)(frame->elr >> 32));
	ss_wr(base + 2u, (uint32_t)sp1);
	ss_wr(base + 3u, (uint32_t)(sp1 >> 32));

	el2_ss_head = (el2_ss_head + 1u) % SS_SLOTS;
	el2_ss_total++;
	ss_wr(1, el2_ss_total);
	ss_wr(2, el2_ss_head);

	/* Re-arm: SPSR_EL2.SS captured 0 (step completed); set it back to 1 and
	 * keep debug unmasked so the next eret single-steps again. */
	frame->spsr |= SPSR_SS_BIT;
	frame->spsr &= ~SPSR_D_BIT;
}

/* Toggle guest single-step. Called from dbgmon's `ss` command on the tick
 * path, so `frame` is the guest frame el2_common will eret back into: setting
 * PSTATE.SS in frame->spsr makes stepping start on that eret. Returns the new
 * enabled state (1=on, 0=off). */
int el2_ss_toggle(struct el2_frame *frame)
{
	if (!el2_ss_on) {
		ss_ring_reset();
		ss_set_mdscr(1);
		ss_set_tde(1);
		if (frame) {
			frame->spsr |= SPSR_SS_BIT;   /* step on the next eret */
			frame->spsr &= ~SPSR_D_BIT;   /* unmask debug exceptions */
		}
		el2_ss_on = 1;
	} else {
		ss_set_mdscr(0);
		ss_set_tde(0);
		if (frame) {
			frame->spsr &= ~SPSR_SS_BIT;  /* stop stepping */
			frame->spsr |= SPSR_D_BIT;    /* restore debug mask */
		}
		el2_ss_on = 0;
	}
	ss_wr(3, (uint32_t)el2_ss_on);
	return el2_ss_on;
}

void el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);

	/* Dead-man's switch: pet the hardware watchdog on EVERY EL2 exception.
	 * The WDT is armed (~16s) in main_dbg.c. As long as EL2 is servicing
	 * ANYTHING (timer tick, vconsole trap, any guest fault, a host dbgmon
	 * command), we're alive and stay resident for live debugging. Only a
	 * TOTAL wedge — EL2 stops taking exceptions entirely, i.e. the exact
	 * "EMAC dead, board unreachable" state that used to require a physical
	 * power-cycle — lets the watchdog fire, rebooting to U-Boot where a
	 * persistent chimpd catches it and auto-reloads. Bounded ~16s downtime,
	 * no human, no manual reset. See SESSION-RULES.md R2/R3. */
	wdt_pet();

	/* Snapshot the guest frame for the SMP debug core (CPU1) — only when that
	 * core is actually up (dbg_core_active). Inert/zero-overhead otherwise. */
	if (dbg_core_active && (kind >> 2) == 2u)
		g_last_guest_frame = *frame;

	/* Recovery for cmd_call(): if a dbgmon-invoked function faults with an
	 * EL2 sync exception, don't let it kill the hypervisor. Restore the
	 * return address from x30 (the BLR's saved LR) and set x0=0xDEAD so
	 * cmd_call reports the fault. The callee's frame is abandoned. */
	{
		extern volatile int dbgmon_call_active;
		if (dbgmon_call_active &&
		    t == EL2_KIND_SYNC && (kind >> 2) != 2u) {
			dbgmon_call_active = 0;
			frame->x[0] = 0xDEADull;
			frame->elr = frame->x[30];
			exc_report_line(frame);
			return;
		}
	}

	/* IRQ / FIQ: this is the RTOS tick path — hand straight to the GIC timer
	 * handler (it ACKs GICC_IAR, samples jitter, re-arms, and EOIs). We do
	 * NOT touch the fault breadcrumb here (that would be overwritten 1000x/s)
	 * and NEVER advance ELR — the interrupted instruction must resume. */
	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
		/* If this interrupt was taken FROM a lower EL (group 2 = the EL1
		 * guest), it's the hypervisor preempting the guest — count it. */
		if ((kind >> 2) == 2u)
			guest_note_preempt();
		gic_timer_irq(frame);      /* ACK/EOI + jitter first (must EOI) */
		if ((kind >> 2) == 2u && !dbg_core_active)
			dbgmon_service(frame); /* live debugger (only if the SMP debug core
			                        * isn't the one owning EMAC) */
		sched_tick(frame);         /* preemptive switch; erets away when a
		                            * scheduler is running, else no-op fallback */
		return;
	}

	/* Guest early-phase tracing (gtrace): a lower-EL sync trap that is a
	 * TVM system-register access (EC==0x18, the guest bringing up its own MMU)
	 * or a trampoline HVC (EC==0x16, an early EL1 fault we made visible) is
	 * handled/recorded here so we SEE how far locore got — before the generic
	 * fault path. */
	if ((kind >> 2) == 2u && (kind & 3u) == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;
		/* SMC from the guest (trapped by HCR_EL2.TSC=1). Log the PSCI function
		 * id to a ring at 0x50000200 [0]=count, [1..15]=last-15 fnids so a
		 * post-reset `md` shows the guest's PSCI call sequence. Intercept
		 * SYSTEM_RESET(0x84000009)/SYSTEM_OFF(0x84000008): DON'T actually reset —
		 * report success and resume, so the board stays alive and we can see
		 * whether the guest was the one resetting it. Forward everything else
		 * (CPU_ON/AFFINITY_INFO/etc.) to the real EL3 PSCI so guest SMP still
		 * behaves normally. */
		if (ec == 0x17u) {
			uint64_t fnid = frame->x[0];
			volatile uint32_t *r = (volatile uint32_t *)0x50000200UL;
			uint32_t idx = r[0] + 1u;
			r[0] = idx;
			r[1u + ((idx - 1u) & 0xFu)] = (uint32_t)fnid;
			__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(r) : "memory");
			/* B4 flight recorder: every guest PSCI SMC, interleaved with
			 * the fault/IRQ/virtio/console timeline — a0=fnid, a1=ELR (the
			 * guest PC that issued the SMC). The bespoke ring above
			 * (0x50000200) already keeps a PSCI-only history; this call
			 * additionally places the event on the shared generic
			 * timeline so a post-mortem can see e.g. "PSCI SYSTEM_RESET
			 * right before/after this fault", not PSCI in isolation. */
			flightrec_log(FLTR_K_TRAP, fnid, frame->elr);
			/* SYSTEM_OFF (0x84000008): operator-initiated clean poweroff.
			 * The rc shutdown path has already synced + unmounted (fs_clean
			 * is 1 on disk), so honor it with a CLEAN warm reset to U-Boot —
			 * the supervisor reloads a fresh HV+guest onto an already-clean
			 * filesystem. This is the durable fix for the fs_clean saga (see
			 * dbg_clean_off's comment). MUST raise wdt_debug_hold first, or the
			 * SMP debug core (CPU1) keeps petting the HW WDOG via
			 * wdt_debug_kick() and defeats the ~2s timer reboot_clean() arms
			 * here on CPU0. reboot_clean() drops the USB gadget cleanly and
			 * never returns. */
			if (fnid == 0x84000008ull && dbg_clean_off) {
				r[1u + ((idx - 1u) & 0xFu)] = 0x0FF0FF0Fu; /* bc: clean-off */
				__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(r) : "memory");
				wdt_debug_hold = 1;   /* release both pet paths so WDOG fires */
				reboot_clean();       /* USB drop + ~2s WDOG warm reset; no return */
			}
			if ((fnid == 0x84000009ull || fnid == 0x84000008ull) &&
			    dbg_block_reset) {
				frame->x[0] = 0;          /* PSCI SUCCESS, but no real reset */
				frame->elr += 4u;
				return;
			}
			{
				register uint64_t x0 __asm__("x0") = frame->x[0];
				register uint64_t x1 __asm__("x1") = frame->x[1];
				register uint64_t x2 __asm__("x2") = frame->x[2];
				register uint64_t x3 __asm__("x3") = frame->x[3];
				__asm__ volatile("smc #0"
				    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
				frame->x[0] = x0; frame->x[1] = x1;
				frame->x[2] = x2; frame->x[3] = x3;
			}
			frame->elr += 4u;
			return;
		}
		/* Software Step (EC==0x32, from a lower EL) — our single-step of the
		 * guest. Record (PC,SP), re-arm PSTATE.SS, resume. Handled before the
		 * generic fault record so the step is not mistaken for a fault. */
		if (ec == 0x32u) {
			el2_ss_handle(frame);
			return;
		}
		/* First-fault probe: a stage-2 abort (instr EC 0x20/0x21 or data
		 * 0x24/0x25) on the guest's unmapped EL1 vector page carries the
		 * ORIGINAL, still-unmasked EL1 fault — latch it, remap, resume. */
		if ((ec == 0x20u || ec == 0x21u || ec == 0x24u || ec == 0x25u) &&
		    firstfault_handle(frame))
			return;
		/* Hardware breakpoint (EC 0x30) / watchpoint (EC 0x34) taken from the
		 * guest, routed to EL2 by MDCR_EL2.TDE. hwbp_handle records the hit and
		 * one-shot-disables the slot so the guest makes forward progress on
		 * eret; returns 1 if it owned this exception. Do NOT advance ELR. */
		if ((ec == 0x30u || ec == 0x34u) && hwbp_handle(frame, frame->esr))
			return;
		if (ec == 0x18u && gtrace_handle_sysreg(frame))
			return;
		/* One-shot software breakpoint (onebp) claims its own HVC imm
		 * first — gtrace_handle_hvc() unconditionally treats EC==0x16
		 * as its own EL1-vector-trampoline HVC and would misinterpret
		 * a plain instrumentation HVC as a bogus "first fault". */
		if (ec == 0x16u && onebp_handle_hvc(frame))
			return;
		if (ec == 0x16u && gtrace_handle_hvc(frame))
			return;
	}

	/* Guest (lower-EL) synchronous DATA ABORT (ESR EC==0x24): first try the
	 * virtual UART console — a stage-2 fault on the unmapped UART0 page means
	 * the FreeBSD guest is doing console I/O. vconsole emulates it (captures
	 * transmitted bytes, returns "ready" on status reads, advances ELR) and
	 * returns 1 if handled — then this is normal guest progress, NOT a fault,
	 * so we return without recording. Only genuinely unhandled aborts fall
	 * through to the fault record below. */
	if ((kind >> 2) == 2u && (kind & 3u) == EL2_KIND_SYNC &&
	    (((uint32_t)(frame->esr >> 26)) & 0x3fu) == 0x24u) {
		if (vconsole_handle_fault(frame)) {
			if (!dbg_core_active)
				dbgmon_service(frame);
			return;
		}
		/* Next, the eMMC-backed virtio-blk device at 0x0A000000. Same
		 * "handled -> return without recording" contract as vconsole.
		 * vblk_mmio_fault() returns 0 for any abort outside its 0x200-byte
		 * window, so calling it unconditionally on every guest data abort is
		 * safe; disjoint from the UART0 page, so order vs vconsole is
		 * arbitrary. A QueueNotify write here drains the ring straight to the
		 * real eMMC and injects INTID 82. See docs/virtio-blk-design.md. */
		if (vblk_mmio_fault(frame)) {
			if (!dbg_core_active)
				dbgmon_service(frame);
			return;
		}
		/* Finally, the virtio-net-over-EMAC multiplexer at 0x0A001000
		 * (ROADMAP C1 — see vnet_emac.h). Same "handled -> return without
		 * recording" contract; vnet_mmio_fault() returns 0 for any abort
		 * outside its own 0x200-byte window (disjoint from vconsole's UART0
		 * page and vblk's 0x0A000000 window), so calling it unconditionally
		 * here is safe. A QueueNotify(1) write here drains the transmitq
		 * straight to the real EMAC (muxed with the debug-protocol traffic —
		 * see vnet_emac.c's TX ethertype filter) and injects VNET_INTID. */
		if (vnet_mmio_fault(frame)) {
			if (!dbg_core_active)
				dbgmon_service(frame);
			return;
		}
	}

	/* Synchronous / SError: record the fault for post-mortem. HPFAR_EL2 holds
	 * the faulting guest IPA on a stage-2 fault — the key datum for "where
	 * did the FreeBSD guest touch unmapped memory".
	 *
	 * BUG FIX (2026-07-23, found live while investigating the first-ever
	 * userland-transition fault storm past vtbd0p3): HPFAR_EL2 bits[39:4]
	 * hold FIPA[47:12] (the faulting IPA's page-frame-number, i.e. the
	 * field sits at bit position 4 of the register but represents address
	 * bit position 12) — reconstructing the real address needs a NET left
	 * shift of 8 from the register value with its low 4 reserved bits
	 * masked off (equivalently: extract the field with >>4, then place it
	 * at bits[47:12] with <<12, netting <<8 against the masked raw value).
	 * This matches the standard KVM/Linux idiom `(hpfar & mask) << 8`. The
	 * previous `<< 4` here computed an IPA exactly 16x (one hex digit) too
	 * small, silently mis-locating every stage-2-fault diagnostic this
	 * breadcrumb ever reported. Confirmed by manually walking the guest's
	 * own stage-1 tables (TTBR1_EL1) for a live repro and finding the true
	 * faulting page's PA matched the OLD field's value << 4 exactly. */
	{
		uint64_t hpfar;
		__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
		exc_bc(0, EXC_MAGIC);
		exc_bc(1, ++exc_count);
		exc_bc(2, (uint32_t)kind);
		exc_bc(3, (uint32_t)frame->esr);
		exc_bc(4, (uint32_t)frame->elr);
		exc_bc(5, (uint32_t)(frame->elr >> 32));
		exc_bc(6, (uint32_t)frame->far);
		exc_bc(7, (uint32_t)(frame->far >> 32));
		exc_bc(8, (uint32_t)frame->x[0]);
		exc_bc(9, (uint32_t)frame->x[1]);
		exc_bc(10, (uint32_t)frame->x[30]);
		exc_bc(11, (uint32_t)((hpfar & 0xFFFFFFFFF0ULL) << 8)); /* IPA low */
		exc_bc(12, (uint32_t)(((hpfar & 0xFFFFFFFFF0ULL) << 8) >> 32));

		/* AD-HOC diagnostic capture (2026-07-23, first-ever userland-
		 * transition fault storm investigation): the guest's own EL1 MMU
		 * config at fault time. These are CPU0's OWN banked EL1 registers
		 * (this code runs ON the faulting core, CPU0, in response to ITS
		 * OWN trap -- unlike dbgmon's `sr` command, which is serviced by
		 * CPU1 and would read CPU1's own, irrelevant, always-zero EL1 bank).
		 * A trap to EL2 does NOT touch the guest's EL1-banked registers, so
		 * these are genuinely the guest's live page-table-walk config at
		 * the moment of this specific fault. Words 13-18, comfortably
		 * inside the free 0x50000400-0x500 window (next breadcrumb, jitter/
		 * TIMR, starts at 0x50000500). */
		{
			uint64_t ttbr0, ttbr1, tcr;
			__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
			__asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
			__asm__ volatile("mrs %0, tcr_el1"   : "=r"(tcr));
			exc_bc(13, (uint32_t)ttbr0);
			exc_bc(14, (uint32_t)(ttbr0 >> 32));
			exc_bc(15, (uint32_t)ttbr1);
			exc_bc(16, (uint32_t)(ttbr1 >> 32));
			exc_bc(17, (uint32_t)tcr);
			exc_bc(18, (uint32_t)(tcr >> 32));
		}
	}

	/* B3 crash-forensics: capture a frame-pointer backtrace into the BTR1
	 * ring (backtrace.c) for every fault recorded above — previously
	 * backtrace_walk() only ran on-demand (dbgmon's `bt` command), so a
	 * board that rebooted before an operator typed `bt` had no backtrace at
	 * all. pc=ELR, fp=x29, lr=x30, same triple panic.c/dbgmon.c use;
	 * defensive by construction (never faults on a bad fp). */
	{
		uint64_t bt_out[16];
		(void)backtrace_walk(frame->elr, frame->x[29], frame->x[30],
		                     bt_out, 16);
	}

	/* B3 crash-forensics: stream a bounded ELF coredump to the host
	 * (coredump_send(), coredump.c/.h) for a GENUINE guest panic, but not
	 * for every iteration of a re-fault storm. coredump_send() was left
	 * uncalled when it was first written (see commit 2624ae5's message)
	 * precisely because guest synchronous faults never advance ELR (the
	 * ELR-advance rule is right below, at the bottom of this function) —
	 * a guest wedged re-executing the same unmapped instruction re-traps
	 * at the IDENTICAL (esr,elr) every single time until the WDT reboots
	 * us, and firing coredump_send() unconditionally here would turn one
	 * bug into a flood of redundant up-to-384-KiB transfers.
	 *
	 * Two bounded guards, belt-and-braces:
	 *   (a) same-fault latch (cd_last_esr/cd_last_elr): skip if this
	 *       fault's (esr,elr) is identical to the last one we already
	 *       streamed a coredump for. An identical repeat of the exact
	 *       same trap IS the re-fault-storm signature described above,
	 *       not a new panic — so it costs nothing beyond the guest's own
	 *       original bug, already an infinite loop by construction.
	 *   (b) hard per-boot cap (CORE_DUMPS_PER_BOOT, above): defense in
	 *       depth in case a pathological guest loop varies esr/elr
	 *       slightly between iterations (e.g. a differing FAR on each
	 *       pass) and would otherwise slip past guard (a) — mirrors
	 *       coredump.c's own "everything here is bounded" design
	 *       (COREDUMP_MAX_TOTAL, per-region caps, bounded TX retries) and
	 *       flightrec.c's fixed-size-ring philosophy for the sibling
	 *       post-mortem instrument.
	 * Both guards are plain function-local statics (.bss): a WDT warm
	 * reboot clears them, so a genuinely NEW panic in a later boot is
	 * never held back by a previous boot's cap or latch — the intent is
	 * "once per genuine panic", not "once ever".
	 *
	 * Guest faults only: (kind>>2)==2u is the lower-EL (guest) group,
	 * exactly the same test g_last_guest_frame's snapshot above uses. Our
	 * own EL2-level sync self-faults (e.g. the deliberate brk self-test)
	 * advance ELR and return at the bottom of this function instead of
	 * looping, so they were never the flood risk and aren't "guest
	 * panics" this milestone is about. */
	if ((kind >> 2) == 2u) {
		static uint32_t cd_dumps_sent;
		static uint64_t cd_last_esr = ~0ULL;
		static uint64_t cd_last_elr = ~0ULL;

		if (cd_dumps_sent < CORE_DUMPS_PER_BOOT &&
		    (frame->esr != cd_last_esr || frame->elr != cd_last_elr)) {
			cd_last_esr = frame->esr;
			cd_last_elr = frame->elr;
			cd_dumps_sent++;
			coredump_send(frame, (uint64_t *)0, 0);
		}
	}

	/* B4 flight recorder: one event per recorded fault into the FLTR ring
	 * (flightrec.c) — a0=ESR (what kind of fault), a1=ELR (where). Small,
	 * additive; the fuller config-table generalization (every trap type,
	 * IRQ injects, virtio ops, console bytes) is future work — see
	 * flightrec.h. */
	flightrec_log(FLTR_K_FAULT, frame->esr, frame->elr);

	exc_report_line(frame);

	/* Advance ELR ONLY for a synchronous exception taken from OUR level (EL2,
	 * group != 2) — e.g. the deliberate brk self-test — so we return alive.
	 * A fault FROM the guest (lower EL, group 2) must NOT have its ELR touched:
	 * we record it and leave it; the guest re-faults (or a WDT armed before
	 * entering the guest resets us so we can read this breadcrumb). Advancing a
	 * guest's ELR would silently corrupt the guest instruction stream. */
	if ((kind & 3u) == EL2_KIND_SYNC && (kind >> 2) != 2u)
		frame->elr += 4u;
}
