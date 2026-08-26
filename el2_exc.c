/* SPDX-License-Identifier: BSD-2-Clause */

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
#include "hv_addrmap.h"
#include "vgicd.h"   /* vgicd_handle_fault() -- trapped GIC distributor */
#include "exceptions.h"
#include "trace.h"
#include "vcpu2.h"
#include "vcpu1.h"
#include "profiler.h"
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
#include "vinput.h"      /* vinput_mmio_fault() -- virtual keyboard */
#include "vblk_sd.h"     /* vblk_sd_mmio_fault() -- virtio-blk over microSD */
#include "stage2.h"      /* stage2_wx_fault() -- opt-in dynamic W^X, see
                           * stage2.h's STAGE2_WX_DYNAMIC block and
                           * docs/wx-enforcement.md. The prototype itself is
                           * only visible when STAGE2_WX_DYNAMIC is set (it
                           * defaults to 0), so the call site below is
                           * wrapped in the SAME #if -- an off build neither
                           * declares nor calls it. */
#ifdef HV_HDMI
#include "scanout.h"     /* scanout_mmio_fault() -- zero-copy GPU scanout flip
                           * doorbell, see docs/zero-copy-scanout.md. Guarded
                           * the same way main_dbg.c guards hdmi.h/hud.h: with
                           * no framebuffer there is nothing to flip. */
#endif
#include "reboot.h"
#include "backtrace.h"
#include "flightrec.h"
#include "coredump.h"

/* Shared snapshot of the guest register frame, written by CPU0 on every
 * guest-group trap and read by the SMP debug core (CPU1) so it can serve
 * gr/sr over EMAC even while CPU0 is wedged inside the guest. Caches are
 * coherent across cores (SMPEN set in start.S) so a plain global suffices. */
struct el2_frame g_last_guest_frame;
/* seqlock for g_last_guest_frame (review H8): CPU0 (sole writer) bumps this
 * odd before the struct copy and even after; CPU1 readers (dbgmon gr/sr via
 * smp.c, bmc telemetry) copy through el2_snapshot_guest_frame(), which retries
 * across a stable EVEN interval rather than reading a half-written frame.
 * Coherency across cores is guaranteed (SMPEN); this adds the missing
 * atomicity. */
volatile uint32_t g_last_guest_frame_seq;

/* ------------------------------------------------------------------ *
 * GDB-stub cross-core stop/resume handshake (ROADMAP B2, see
 * docs/gdbstub-integration.md §3/§4). A guest debug trap (SW BRK / completed
 * single-step / HW bp / watchpoint) fires here in el2_trap on CPU0, but the
 * RSP transport lives on the CPU1 debug core. These three cache-coherent
 * globals (SMPEN makes plain globals coherent, same as g_last_guest_frame)
 * are the handshake: CPU0 publishes a stop and parks; CPU1 runs the RSP loop
 * and writes back a resume decision + any register edits.
 *   gdb_stop_pending : CPU0 set, CPU1 clears once served.
 *   gdb_stop_signal  : GDB signal number (5 SIGTRAP).
 *   gdb_resume_act   : 0xffffffff "no decision yet"; CPU1 writes non-sentinel.
 * Defined here (always linked) so builds without gdbstub.o still resolve them;
 * the divert that USES them is gated on the weak gdbstub hooks below, which are
 * 0 in a build that doesn't link the stub — so this whole path is inert there. */
volatile uint32_t gdb_stop_pending;
volatile uint32_t gdb_stop_signal;
volatile uint32_t gdb_resume_act;

/* ------------------------------------------------------------------ *
 * GDB-stub hw-breakpoint/watchpoint cross-core op request — the OPPOSITE
 * direction of the handshake above (CPU1 asking CPU0 to do something,
 * instead of CPU0 publishing a stop for CPU1). See gdbstub-hwbp-wrong-core
 * memory: DBGBVR/DBGBCR/DBGWVR/DBGWCR are per-PE banked registers, so
 * gdbstub_hw.c's Z1..Z4 handlers (which ALWAYS run on CPU1 — same call
 * chain as the stop handshake above) must not arm them directly; they'd
 * silently arm CPU1's own debug unit, which never executes guest code.
 *
 * This only needs to work while CPU0 is ALREADY PARKED in the `wfe` loop
 * a few lines below (a real GDB stop is active — e.g. gdb sitting at a
 * breakpoint hit) — that is precisely the scope gdbstub_hw.c restricts
 * itself to (see its hwop_run()): if CPU0 is instead running the guest
 * freely, arming/clearing a real hardware debug register transparently
 * would require asynchronously interrupting it (a new SGI/IPI path), which
 * was deliberately NOT built this pass (see gdbstub_hw.c's block comment
 * for the full rationale) — gdbstub_hw_insert()/_remove() detect that case
 * themselves (gdb_stop_pending == 0) and refuse cleanly instead of using
 * this mechanism.
 *   gdb_hw_op_pending : CPU1 sets to post a request, CPU0 clears once done
 *                       (mirrors gdb_stop_pending, opposite direction/owner).
 *   gdb_hw_op_kind/slot/va/lsc : the request (see gdbstub_hw.c's HWOP_*).
 *   gdb_hw_op_result  : CPU0 writes hwbp_set()/hwbp_clear()'s return value
 *                       (0 == success) before clearing gdb_hw_op_pending.
 * Defined here (always linked, harmless/unused where gdbstub.o isn't linked)
 * so the wfe-loop extension below compiles in every build; gdbstub_hw_apply_op
 * (the weak hook that actually performs the op) is 0 in a build without
 * gdbstub_hw.o, so the `if` short-circuits and this is a no-op there. */
volatile uint32_t gdb_hw_op_pending;
volatile int32_t  gdb_hw_op_kind;
volatile int32_t  gdb_hw_op_slot;
volatile uint64_t gdb_hw_op_va;
volatile uint32_t gdb_hw_op_lsc;
volatile int32_t  gdb_hw_op_result;

/* Weak: resolve to the real gdbstub.o/gdbstub_hw.o implementations in the
 * dbg/gdb builds, to 0 in repl/fbsd/zephyr/hdmi (which link el2_exc.o but not
 * gdbstub.o) — the divert short-circuits on the null address there, leaving
 * hwbp.c's one-shot path and the fault breadcrumb exactly as before. */
int gdbstub_attached(void)  __attribute__((weak));
int gdbstub_hw_active(void) __attribute__((weak));

/* Executes the queued gdb_hw_op_* request (CPU0 side, called from inside the
 * parked wfe loop below) — the actual hwbp_set()/hwbp_clear()/DBGWCR-LSC-patch
 * calls live in gdbstub_hw.c, which already has every piece needed (it used
 * to call them directly, on the wrong core; see its block comment). Weak so
 * builds without gdbstub_hw.o still link. */
void gdbstub_hw_apply_op(void) __attribute__((weak));

void el2_snapshot_guest_frame(struct el2_frame *out)
{
	uint32_t s1, s2;
	unsigned tries = 0;
	do {
		s1 = g_last_guest_frame_seq;
		__asm__ volatile("dsb ish" ::: "memory");
		*out = g_last_guest_frame;
		__asm__ volatile("dsb ish" ::: "memory");
		s2 = g_last_guest_frame_seq;
		/* Bounded: writes are per-guest-trap, never a continuous stream, so
		 * a stable even interval is reached almost immediately; the cap only
		 * guarantees a CPU1 reader can never spin forever. */
	} while (((s1 & 1u) || s1 != s2) && ++tries < 1000u);
}
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
 * SYSTEM_RESET (0x84000009): see dbg_clean_reset below. */
volatile uint32_t dbg_clean_off = 1;

/* When 1 (default since 2026-08-05), a guest PSCI SYSTEM_RESET (0x84000009) is
 * ALSO honored with a clean warm reset, for the same reasons as SYSTEM_OFF.
 *
 * This reverses an earlier deliberate decision, so here is why. SYSTEM_RESET
 * used to fall through to dbg_block_reset's "report success, reset nothing,
 * stay alive so a crash-reboot can be inspected" path. The cost of that was
 * measured on 2026-08-05: FreeBSD's cpu_reset() (arm64/vm_machdep.c:138-140)
 * asks the platform to reset and, when the call returns, spins in a two-
 * instruction `while(1)`. Lying to it therefore does not keep a guest
 * inspectable — it parks the guest FOREVER in that spin, PC/LR one instruction
 * apart, indistinguishable from a hang to anything watching from outside. That
 * is what an A2 cycle run reported as "guest never came back after reload", and
 * it makes unattended boot-reliability measurement impossible: any guest that
 * decides to reboot (e.g. a panic auto-reboot during boot) wedges permanently.
 *
 * The "so it can be inspected" argument does not survive contact with the
 * facts either: everything worth inspecting happened BEFORE the reset request
 * — the panic, the fault, the console output — and all of it lives in the
 * flight recorder, the console capture ring and the breadcrumb windows, ALL of
 * which deliberately survive a warm WDT reset (that is the entire design). So
 * honoring the reset loses no evidence and converts a permanent wedge into an
 * automatic recovery the supervisor can act on.
 *
 * This is NOT the 2026-07-18 failure mode that dbg_block_reset exists to
 * prevent. That was FORWARDING the guest's SMC to EL3, which hard-resets the
 * SoC and drops the board off the USB bus entirely. reboot_clean() is the
 * controlled path instead: it drops the MUSB pull-up first and then arms the
 * WDOG, and it is the same call SYSTEM_OFF has been using — proven on hardware
 * 2026-08-04, board landed in U-Boot and was caught by the supervisor in ~1s.
 *
 * Clear this over the net (`sw` / dbg flags) to get the old stay-alive-and-
 * inspect behaviour back for a session that specifically wants to catch a guest
 * mid-reboot-request. The PSCI ring at 0x50000200 records the fnid either way,
 * with 0x0FF0FF09 marking "clean reset honored" (0x0FF0FF0F = clean off). */
volatile uint32_t dbg_clean_reset = 1;

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

/* Second guest vCPU (vcpu2.c), linked only into the `dbg` target. Weak
 * fallbacks, same pattern as the stubs below, so every other target links
 * unchanged: the gate reads 0 and the request is always refused, which is
 * exactly the pre-existing behaviour (a guest CPU_ON for affinity 2 gets
 * ALREADY_ON). Defined here rather than in dbgmon.c because el2_exc.o is in
 * every target's object list, so one definition covers them all. */
__attribute__((weak)) volatile uint32_t dbg_vcpu2;

__attribute__((weak)) int vcpu2_request(uint64_t entry_pa, uint64_t context_id)
{
	(void)entry_pa; (void)context_id;
	return 0;
}

/* Third guest vCPU (vcpu1.c), EXPERIMENTAL — see vcpu1.h for the tradeoff.
 * Same weak-fallback pattern as dbg_vcpu2/vcpu2_request immediately above:
 * every target without vcpu1.o linked keeps refusing affinity 1, i.e. the
 * pre-existing ALREADY_ON behaviour, unchanged. */
__attribute__((weak)) volatile uint32_t dbg_vcpu1;

__attribute__((weak)) int vcpu1_request(uint64_t entry_pa, uint64_t context_id)
{
	(void)entry_pa; (void)context_id;
	return 0;
}

/* Event-trace ring (trace.c) and PC-sample profiler (profiler.c), linked only
 * into the `dbg` target. Weak fallbacks, same pattern as gic_timer_irq above,
 * so every other target links unchanged and pays one call to an empty
 * function. These feed the HUD's SCHED GANTT and PROFILE panels; without
 * trace.o/profiler.o those panels correctly report their windows as absent. */
__attribute__((weak)) void trace_emit(uint8_t type, uint8_t cpu, uint16_t arg,
                                      uint32_t ctx)
{
	(void)type; (void)cpu; (void)arg; (void)ctx;
}

__attribute__((weak)) void profiler_sample(uint64_t pc, int is_guest)
{
	(void)pc; (void)is_guest;
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

/* virtio-blk-over-eMMC MMIO device (vblk_emmc.c). Same weak-fallback pattern
 * as vnet_mmio_fault right above: the `repl`/`fbsd`/`zephyr` builds are
 * narrower milestones that never call vblk_init() and don't link
 * vblk_emmc.o/emmc_bio.o, so this stub keeps them linking -- the call then
 * always reports "not my window" (correct: nothing configured that window),
 * unchanged behavior for every build that DOES link the real vblk_emmc.o. */
__attribute__((weak)) int vblk_mmio_fault(struct el2_frame *frame)
{
	(void)frame;
	return 0;
}

/* virtio-input keyboard MMIO device (vinput.c). Same weak-fallback pattern as
 * vnet_mmio_fault/vblk_mmio_fault above so builds that don't link vinput.o
 * still link; the call then always reports "not my window". */
__attribute__((weak)) int vinput_mmio_fault(struct el2_frame *frame)
{
	(void)frame;
	return 0;
}

/* virtio-blk over the microSD card (vblk_sd.c). Same weak-fallback pattern
 * as vinput_mmio_fault immediately above. */
__attribute__((weak)) int vblk_sd_mmio_fault(struct el2_frame *frame)
{
	(void)frame;
	return 0;
}

/* Dual-guest catch-all MMIO absorber for CPU3/Zephyr (mmio_absorb.c). Same
 * weak-fallback pattern as vnet_mmio_fault/vblk_mmio_fault right above: only
 * the `dual` Makefile target links mmio_absorb.o; every other target keeps
 * linking with this stub, which always reports "not handled" -- harmless,
 * since the CPU3 dispatch branch that calls it (below) is itself dead code
 * on every target where CPU3 never traps at lower-EL (it just sits in
 * `wfi`, see smp.c). */
__attribute__((weak)) int mmio_absorb_fault(struct el2_frame *frame)
{
	(void)frame;
	return 0;
}

/* B3 crash-forensics ELF-over-EMAC stream (coredump.c, coredump_send()).
 * Weak fallback so the `repl`/`fbsd`/`zephyr` builds -- which never call
 * emac_init() and don't link coredump.o -- still link; a genuine guest
 * panic in those builds is still caught and recorded (flightrec_log() +
 * exc_report_line() below run regardless), it just isn't also streamed as
 * an ELF core over a network console those builds never bring up. */
__attribute__((weak)) void coredump_send(struct el2_frame *frame, uint64_t *regions, int nregions)
{
	(void)frame;
	(void)regions;
	(void)nregions;
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
_Static_assert(SS_BASE == HVMAP_LOW_SST1,
               "SS_BASE drifted from hv_addrmap.h -- the map owns this address");
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

/* ------------------------------------------------------------------ *
 * H3 — guest PSCI SMC filter (privilege-escalation hole closed).
 *
 * el2_trap() traps every guest SMC (HCR_EL2.TSC=1, EC==0x17) below. Until
 * this filter existed, every PSCI function EL2 didn't specifically
 * recognize (SYSTEM_OFF/SYSTEM_RESET) was forwarded straight through to
 * secure EL3 (BL31) with x1..x3 verbatim from the guest. EL2 is the highest
 * non-secure exception level on this SoC, so whatever EL BL31 warm-boots a
 * target core into on a PSCI "bring a core up" call is entirely up to
 * BL31/PSCI semantics, not something EL2 can police AFTER the fact — the
 * only safe point to stop a guest from parlaying a PSCI call into code
 * running above its own EL1 is HERE, before the `smc` instruction ever
 * executes.
 *
 * The concrete hole (finding H3): PSCI_CPU_ON (DEN0022 §5.5) takes a
 * guest-supplied target MPIDR (x1) and a guest-supplied entry_point_address
 * (x2 — "the address at which the core must commence execution"; the caller
 * does not get to say what EL that is, it's whatever EL PSCI_CPU_ON's own
 * warm-boot path uses on this platform, which is EL2 for every core this HV
 * itself CPU_ON's at boot — see smp.c). A FreeBSD/arm64 guest issuing
 * CPU_ON for an OFF core with x2 pointed at guest-controlled memory would
 * have that address executed AT EL2 — full hypervisor privilege from a
 * ring-1 guest. Today the hole is closed only BY ACCIDENT: smp_init()
 * already PSCI-CPU_ON's affinities 1..3 for its own use (CPU1 = EMAC/dbgmon
 * debug core, CPU2 = async eMMC I/O, CPU3 parked — see smp.c), so a guest
 * CPU_ON for any of them returns ALREADY_ON(-4) today — but nothing
 * enforced that on purpose, and there was no CPU_OFF interception, so a
 * guest (or a future multi-vCPU build) that first got one of those cores
 * OFF would immediately reopen the CPU_ON window. This filter makes the
 * policy explicit and no longer contingent on smp_init()'s boot-time
 * ordering being exactly what it is today.
 *
 * NOTE: the guest this HV runs today is single-vCPU as far as EL2 is
 * concerned — guest_enter() (guest.c/guest.h) is called exactly once, from
 * CPU0's own boot path (main_dbg.c et al.); smp_secondary_main() (smp.c)
 * never calls guest_enter() for CPU1..3, they run the HV's own debug-core /
 * async-I/O / idle loops forever. So the guest has never needed, and
 * structurally cannot use, a real PSCI CPU_ON to bring up a second vCPU —
 * refusing it here changes nothing about current working guest behavior.
 *
 * Policy (whitelist; everything not explicitly listed is refused):
 *   PSCI_VERSION, PSCI_FEATURES, AFFINITY_INFO{,_64}, MIGRATE{,_64},
 *   MIGRATE_INFO_TYPE, MIGRATE_INFO_UP_CPU{,_64}
 *     -> pure informational queries per DEN0022: none of them takes an
 *        entry-point/address parameter and none of them changes any core's
 *        power state, so forwarding x1..x3 to the real EL3 PSCI verbatim
 *        carries no privilege-escalation risk. Let BL31 answer exactly as
 *        it would without this HV in the picture.
 *   CPU_ON{,_64}
 *     -> NEVER forwarded. Cores 1..3 are permanently HV-owned (smp.c); core
 *        0 is the only guest vCPU and is already running. Answer
 *        ALREADY_ON(-4) for any of the 4 physical cores that actually exist
 *        on this SoC (matches true hardware state — not a lie) and
 *        INVALID_PARAMS(-2) for any other target, without ever reaching
 *        EL3 or even reading the guest's entry_point_address (x2).
 *   CPU_OFF, CPU_SUSPEND{,_64}
 *     -> NEVER forwarded. Neither takes a "target core" parameter in
 *        DEN0022 — both operate on the CALLING core, and the calling core
 *        for every guest SMC this HV ever traps IS core 0, simultaneously
 *        running this very hypervisor. CPU_OFF succeeding would power core
 *        0 off from under EL2 itself (self-inflicted total wedge);
 *        CPU_SUSPEND to a power-down state hands BL31 a guest-supplied
 *        wake-up entry_point_address for this SAME core's own warm-boot
 *        re-entry — structurally the same "guest picks the next PC at an
 *        elevated EL" hazard as CPU_ON, just against the running core
 *        instead of an idle one. Refuse both with DENIED(-3): no core is
 *        ever actually powered down or suspended, which also forecloses a
 *        future multi-vCPU build where an HV-owned core (1..3) could be
 *        talked into CPU_OFF'ing itself and reopening the CPU_ON window.
 *   SYSTEM_OFF, SYSTEM_RESET
 *     -> handled by the caller BEFORE this filter runs, unchanged
 *        (dbg_clean_off / dbg_block_reset) — control never reaches here for
 *        either fnid.
 *   anything else (unrecognized function ID)
 *     -> NOT_SUPPORTED(-1), the spec-correct answer for an unimplemented
 *        PSCI function, instead of blindly hosting an unknown, possibly
 *        address-bearing SMC out to EL3.
 *
 * Returns 1 and sets *ret to the PSCI return code when the call is decided
 * HERE (emulated or denied) — the caller must NOT execute the real `smc`
 * for it. Returns 0 when fnid is on the forward whitelist and safe to pass
 * to EL3 verbatim — the caller still owns doing that round-trip itself so
 * the existing x0..x3 smc-and-restore code doesn't have to move.
 */
#define PSCI_FN_VERSION            0x84000000ull
#define PSCI_FN_CPU_SUSPEND_32     0x84000001ull
#define PSCI_FN_CPU_SUSPEND_64     0xC4000001ull
#define PSCI_FN_CPU_OFF            0x84000002ull /* no _64 form: takes no address arg */
#define PSCI_FN_CPU_ON_32          0x84000003ull
#define PSCI_FN_CPU_ON_64          0xC4000003ull
#define PSCI_FN_AFFINITY_INFO_32   0x84000004ull
#define PSCI_FN_AFFINITY_INFO_64   0xC4000004ull
#define PSCI_FN_MIGRATE_32         0x84000005ull
#define PSCI_FN_MIGRATE_64         0xC4000005ull
#define PSCI_FN_MIGRATE_INFO_TYPE  0x84000006ull
#define PSCI_FN_MIGRATE_INFO_UP_32 0x84000007ull
#define PSCI_FN_MIGRATE_INFO_UP_64 0xC4000007ull
/* 0x84000008 (SYSTEM_OFF) / 0x84000009 (SYSTEM_RESET) intentionally NOT
 * listed here — el2_trap() special-cases both BEFORE calling this filter,
 * so control never reaches the switch below for either fnid. */
#define PSCI_FN_PSCI_FEATURES      0x8400000Aull

/* PSCI return codes, DEN0022 Table 5.1 (the subset this filter emits). */
#define PSCI_RET_SUCCESS         0
#define PSCI_RET_NOT_SUPPORTED   (-1)
#define PSCI_RET_INVALID_PARAMS  (-2)
#define PSCI_RET_DENIED          (-3)
#define PSCI_RET_ALREADY_ON      (-4)

/* x2/x3 are needed only by the CPU_ON case: DEN0022 gives x2 =
 * entry_point_address and x3 = context_id. They are passed in rather than read
 * from the frame so this function stays a pure decision over its arguments and
 * can be reasoned about (and tested) without a frame. */
static int psci_guest_filter(uint64_t fnid, uint64_t x1, uint64_t x2,
                             uint64_t x3, int64_t *ret)
{
	switch (fnid) {
	/* ---- informational / query-only: no address arg, no state change */
	case PSCI_FN_VERSION:
	case PSCI_FN_PSCI_FEATURES:
	case PSCI_FN_AFFINITY_INFO_32:
	case PSCI_FN_AFFINITY_INFO_64:
	case PSCI_FN_MIGRATE_32:
	case PSCI_FN_MIGRATE_64:
	case PSCI_FN_MIGRATE_INFO_TYPE:
	case PSCI_FN_MIGRATE_INFO_UP_32:
	case PSCI_FN_MIGRATE_INFO_UP_64:
		return 0; /* on the forward whitelist; caller does the real smc */

	/* ---- CPU_ON: never let the guest pick EL2's next PC on an off core */
	case PSCI_FN_CPU_ON_32:
	case PSCI_FN_CPU_ON_64: {
		uint64_t aff0   = x1 & 0xffull;
		uint64_t aff_hi = x1 & ~0xffull;

		/* Single-cluster quad-core A64: every real target's MPIDR has
		 * aff1==aff2==aff3==0 and aff0 in 0..3 (see smp.c's own
		 * psci_cpu_on(), which passes the bare core index as the
		 * whole target_mpidr). A nonzero aff_hi names a core that
		 * doesn't exist on this SoC. */
		if (aff_hi != 0 || aff0 > 3u) {
			*ret = PSCI_RET_INVALID_PARAMS;
			return 1;
		}
		/* Affinity 2 is the ONE core the guest may be given, and only
		 * when vcpu2.c is armed (dbg_vcpu2). vcpu2_request() decides:
		 * it returns 1 having recorded the guest's entry point and
		 * context ID and woken the parked core, and 0 to mean "no,
		 * behave exactly as before".
		 *
		 * This does NOT reopen finding H3. EL3 is still never asked,
		 * and the entry point is entered by kload_enter(), i.e. an
		 * `eret` to EL1 under this core's own stage-2 regime -- not
		 * warm-booted at EL2 the way a forwarded PSCI CPU_ON would be.
		 * The guest chooses where its own vCPU starts, at its own
		 * exception level, which is what PSCI is for. */
		if (aff0 == 2ull && vcpu2_request(x2, x3)) {
			*ret = PSCI_RET_SUCCESS;
			return 1;
		}

		/* Affinity 1 — EXPERIMENTAL, see vcpu1.h. Same shape as affinity 2
		 * above: vcpu1_request() alone decides, gated on dbg_vcpu1, and the
		 * same "no EL3, no warm-boot-at-EL2" argument applies unchanged —
		 * the guest still only ever picks where its OWN vCPU starts, at
		 * its own exception level. */
		if (aff0 == 1ull && vcpu1_request(x2, x3)) {
			*ret = PSCI_RET_SUCCESS;
			return 1;
		}

		/* Everything else: cores 0..3 really are all up (0 = this
		 * guest's own vCPU, running this very call; 1..3 = HV-owned,
		 * brought up by smp_init() at boot) — ALREADY_ON is the
		 * spec-correct AND truthful answer. This never reaches EL3, so
		 * the guest's entry_point_address (x2) is never executed. */
		*ret = PSCI_RET_ALREADY_ON;
		return 1;
	}

	/* ---- CPU_OFF / CPU_SUSPEND: never let the (guest-running) calling
	 * core actually power down or suspend — see the block comment above
	 * this function for the full rationale. */
	case PSCI_FN_CPU_OFF:
	case PSCI_FN_CPU_SUSPEND_32:
	case PSCI_FN_CPU_SUSPEND_64:
		*ret = PSCI_RET_DENIED;
		return 1;

	/* ---- not on the whitelist: refuse rather than forward blind ----- */
	default:
		*ret = PSCI_RET_NOT_SUPPORTED;
		return 1;
	}
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

	/* GUEST-BREAKPOINT KEEP-ALIVE (opt-in: -DGUEST_BP_ADDR=0x...).
	 *
	 * MDSCR_EL1.MDE (bit 15) is the master enable for breakpoints and
	 * watchpoints — and MDSCR_EL1 belongs to the GUEST. hwbp_set() sets MDE
	 * before guest entry, then FreeBSD initialises its own debug state and
	 * clears it, silently disarming everything. Observed exactly that on
	 * 2026-08-03: a breakpoint on trapsignal, armed on CPU0 (verified:
	 * armed-bp bitmap 0x1) with a correct address (verified against the
	 * DEPLOYED kernel's own symbol table, not just kernel.debug), recorded
	 * ZERO hits while growfs was demonstrably SIGSEGV-ing.
	 *
	 * So re-assert it. This runs on the GUEST'S core — the only place the
	 * value is not somebody else's bank, which is the trap that has now
	 * misled this project three times. Cost is one mrs+msr on a trap we are
	 * already taking, and only in a build that asked for it. */
#if defined(GUEST_BP_ADDR) && (GUEST_BP_ADDR)
	if ((kind >> 2) == 2u) {
		uint64_t mdscr, mdcr, oslsr;

		/* All three read HERE, on the guest's own core. Every one of them is
		 * banked per PE, and reading them over the debug channel gets CPU1's
		 * copy — the mistake that has now cost this project three separate
		 * wrong conclusions. */
		__asm__ volatile("mrs %0, mdscr_el1" : "=r"(mdscr));
		__asm__ volatile("mrs %0, mdcr_el2"  : "=r"(mdcr));
		__asm__ volatile("mrs %0, oslsr_el1" : "=r"(oslsr));

		/* Publish the raw values so each precondition is a READING, not a
		 * belief. Goes through hwbp.c, which OWNS this window: the hardcoded
		 * 0x50000638 this used to write was slots [14],[15],[16] of hwbp's own
		 * breadcrumb window, silently destroying its first-argument probe and
		 * its re-arm counter. Never address a neighbour's window by literal. */
		hwbp_publish_preconditions(mdscr, mdcr, oslsr);

		/* MDSCR_EL1.MDE (bit 15): master enable for breakpoints/watchpoints,
		 * and MDSCR_EL1 belongs to the guest — FreeBSD clears it during its
		 * own debug init, silently disarming us. Measured, not assumed. */
		if (!(mdscr & (1ull << 15))) {
			mdscr |= (1ull << 15);
			__asm__ volatile("msr mdscr_el1, %0\n\tisb" :: "r"(mdscr) : "memory");
		}
		/* MDCR_EL2.TDE (bit 8): route EL1/EL0 debug exceptions to EL2. Without
		 * it the breakpoint fires INTO THE GUEST, which handles it as its own
		 * debug exception and we never see a thing. hwbp_set() sets it before
		 * guest entry; re-assert in case anything since has cleared it. */
		if (!(mdcr & (1ull << 8))) {
			mdcr |= (1ull << 8);
			__asm__ volatile("msr mdcr_el2, %0\n\tisb" :: "r"(mdcr) : "memory");
		}
		/* OS LOCK — the one that actually silenced us, and the last thing I
		 * would have guessed. OSLSR_EL1.OSLK (bit 1) read 1 on hardware
		 * 2026-08-03: with the OS lock LOCKED the PE suppresses debug
		 * exceptions outright, no matter that MDE, TDE and an armed DBGBCR
		 * slot were all verified correct. That is very likely the real reason
		 * project memory records hardware breakpoints as "never fire" —
		 * three correct preconditions and one silent veto.
		 *
		 * Writing 0 to OSLAR_EL1 unlocks. FreeBSD locks it during its own
		 * debug init, so re-open it here, on the guest's core, every trap. */
		if (oslsr & (1ull << 1)) {
			uint64_t zero = 0;
			__asm__ volatile("msr oslar_el1, %0\n\tisb" :: "r"(zero) : "memory");
		}

		/* THE FIFTH BARRIER, and the one that made this whole block still
		 * not enough. MDE, TDE and the OS lock are all global gates; none of
		 * them is the SLOT. FreeBSD's dbg_monitor_init() zeroes DBGBCR/DBGBVR
		 * for every breakpoint and watchpoint as it brings a CPU up, so the
		 * slot itself goes dark while hwbp's bc[10] bitmap -- which is only
		 * its own bookkeeping variable, not a register read -- keeps
		 * reporting "armed".
		 *
		 * Measured 2026-08-18: a breakpoint on linux_dma_unmap_sg_attrs with
		 * all three gates verified open live and bitmap 0x1 recorded ZERO
		 * hits across a run whose own panic backtrace proves that function
		 * executed. That is what this call fixes; bc[17] now publishes
		 * DBGBCR0 as the hardware reads it, so "armed" is checkable instead
		 * of inferred. */
		hwbp_reassert();
	}
#endif

	/* Snapshot the guest frame for the SMP debug core (CPU1) — only when that
	 * core is actually up (dbg_core_active), and (dual-guest milestone)
	 * only for CPU0's FreeBSD guest: CPU3's Zephyr traps must never be able
	 * to overwrite this snapshot, which dbgmon.c's gr/sr/bt commands and
	 * gdbstub.c both read as "the" live guest frame. Every existing target
	 * only ever traps from CPU0 here anyway (CPU3 sits in `wfi`, see
	 * smp.c), so this added check is dead code for them -- smp_cpu_id() is
	 * a cheap `mrs mpidr_el1` either way. */
	if (dbg_core_active && smp_cpu_id() == 0u && (kind >> 2) == 2u) {
		/* seqlock publish (review H8): odd seq marks the copy in progress so
		 * a concurrent CPU1 reader retries instead of seeing a torn frame. */
		g_last_guest_frame_seq++;                        /* -> odd */
		__asm__ volatile("dsb ish" ::: "memory");
		/* CPU0 only. With a second guest vCPU on CPU2 (vcpu2.c) both cores
		 * take lower-EL traps, and this snapshot is what dbgmon's `gr`, the
		 * GDB stub's register reads and triage.py all present as "the
		 * guest". Letting CPU2 overwrite it would silently mix two vCPUs'
		 * register state into one report -- the same class of confusion as
		 * reading a banked register over the debug channel and getting
		 * CPU1's copy, which has produced several confidently wrong
		 * diagnoses in this project. */
		if (smp_cpu_id() == 0)
			g_last_guest_frame = *frame;
		__asm__ volatile("dsb ish" ::: "memory");
		g_last_guest_frame_seq++;                        /* -> even */
		__asm__ volatile("dsb ish" ::: "memory");
	}

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
		/* Trace/profile AFTER the EOI, never before: the ACK/EOI pair is
		 * the part with a deadline. One ring slot and one histogram
		 * bucket per tick, at the tick rate -- not per guest trap, which
		 * is the rate that would matter. group 2 == taken from the guest,
		 * so ELR_EL2 is a guest PC and the sample is attributed to the
		 * guest rather than to EL2's own idle loop. */
		trace_tick();
		profiler_sample(frame->elr, (kind >> 2) != 2u);
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

		/* Every guest sync trap, into the event ring, next to the
		 * flight-recorder entry below that already records the same
		 * thing for the post-mortem dump. Rate is the guest's own MMIO
		 * rate, which is measured in tens of thousands over a whole
		 * session -- not per instruction. */
		trace_trap((uint16_t)ec, (uint32_t)frame->elr);

		/* Flight-recorder sync-trap ring (E, 2026-07-25 vgic deep-dive): log
		 * EVERY guest sync trap — EC folded into a0's high bits so one glance
		 * separates TVM sysreg (0x18), data abort (0x24), HVC (0x16), etc. —
		 * interleaved in issue order with the FLTR_K_IRQ injects and the
		 * FLTR_K_TIMER CNTV samples, so a post-wedge dump reads as one ordered
		 * timeline. Consecutive-identical (esr,elr) traps are collapsed so a
		 * tight re-fault loop can't flush the ring's earlier context. */
		{
			static uint64_t sync_last_esr, sync_last_key;
			uint64_t key = frame->elr;
			uint32_t kind_log = FLTR_K_SYNC;

			/* For a DATA ABORT the ELR is worthless: it resolves to
			 * generic_bs_r_4/generic_bs_w_4, the bus_space MMIO leaves every
			 * arm64 driver funnels through, so it names no device. The faulting
			 * IPA does name one (0x0a000000=vblk, 0x0a001000=vnet, +0x50 =
			 * QueueNotify). Log that instead, under its own kind so no reader
			 * mistakes a1 for an ELR. See FLTR_K_DABT's comment for the evening
			 * this cost. Dedup then keys on (ESR,IPA) rather than (ESR,ELR), so
			 * a re-fault loop that moves to a NEW address is still recorded. */
			if (ec == 0x24u) {
				uint64_t hpfar;
				__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
				key = ((hpfar & 0xFFFFFFFFF0ULL) << 8) |
				      (frame->far & 0xFFFull);
				kind_log = FLTR_K_DABT;
			}

			if (frame->esr != sync_last_esr || key != sync_last_key) {
				sync_last_esr = frame->esr;
				sync_last_key = key;
				flightrec_log(kind_log,
				              ((uint64_t)ec << 32) | (uint32_t)frame->esr,
				              key);
			}
		}

		/* ---- GDB divert (ROADMAP B2): only while a host gdb is attached ----
		 * A guest software BRK (EC 0x3C), a completed single-step we own
		 * (EC 0x32), or a HW breakpoint/watchpoint gdb armed (EC 0x30/0x34)
		 * belongs to the RSP debugger, not hwbp.c's one-shot path or the fault
		 * breadcrumb. Publish the stop to the CPU1 debug core and PARK here
		 * until it resumes us; CPU1 runs the RSP command loop against the
		 * shared frame (edits land in g_last_guest_frame), then writes
		 * gdb_resume_act + sev. Weak-guarded: in a build without gdbstub.o
		 * gdbstub_attached is 0 (null address) and this is skipped entirely, so
		 * hwbp.c / the fault path below behave exactly as before. Must sit
		 * BEFORE hwbp_handle() so its one-shot handler can't clear a slot gdb
		 * wants sticky. */
		if (gdbstub_attached && gdbstub_attached() &&
		    (ec == 0x3Cu ||                                  /* guest SW BRK   */
		     ec == 0x32u ||                                  /* step complete  */
		     ((ec == 0x30u || ec == 0x34u) &&                /* HW bp / watch  */
		      gdbstub_hw_active && gdbstub_hw_active()))) {
			g_last_guest_frame = *frame;             /* snapshot for CPU1     */
			gdb_stop_signal = 5u;                    /* SIGTRAP               */
			gdb_resume_act  = 0xffffffffu;           /* "no decision yet"     */
			__asm__ volatile("dsb sy" ::: "memory");
			gdb_stop_pending = 1u;
			__asm__ volatile("sev" ::: "memory");    /* poke CPU1             */
			while (gdb_resume_act == 0xffffffffu) {   /* CPU1 runs RSP loop    */
				__asm__ volatile("wfe" ::: "memory");
				/* Service a queued hw bp/wp op (Z1..Z4/z1..z4) from CPU1
				 * WITHOUT leaving this parked loop — see the gdb_hw_op_*
				 * block comment above for why this must run HERE (on CPU0,
				 * the core that actually owns the guest's live debug
				 * register bank) rather than on CPU1 directly. Bounded: a
				 * single call, then straight back to the same wfe/condition
				 * check, so a request that (somehow) never arrives just
				 * means we keep waiting on gdb_resume_act exactly as before. */
				if (gdb_hw_op_pending && gdbstub_hw_apply_op) {
					gdbstub_hw_apply_op();
					__asm__ volatile("dsb sy" ::: "memory");
					gdb_hw_op_pending = 0u;
					__asm__ volatile("dsb sy\n\tsev" ::: "memory");
				}
			}
			*frame = g_last_guest_frame;             /* apply CPU1 reg edits  */
			/* gdb_stop_pending is CPU1's to clear (see smp.c debug-core
			 * loop), and it already did so before setting gdb_resume_act —
			 * i.e. before this wfe could ever wake up. Do NOT clear it here:
			 * that used to be CPU0's job, but CPU0 waking from wfe races
			 * against CPU1's own loop re-checking the flag, so clearing on
			 * the slow (CPU0) side let CPU1 spuriously re-enter the debug
			 * event handler against a stale frame. Single-writer-per-value
			 * now: CPU0 only ever sets this to 1, CPU1 only ever clears it. */
			/* CPU1 (gdbstub apply()) already set MDSCR_EL1.SS + SPSR.SS in the
			 * frame for STEP vs CONTINUE. Do NOT advance ELR — for a BRK the
			 * stub rewinds/reprograms the instruction itself. */
			return;
		}

		/* SMC from the guest (trapped by HCR_EL2.TSC=1). Log the PSCI function
		 * id to a ring at 0x50000200 [0]=count, [1..15]=last-15 fnids so a
		 * post-reset `md` shows the guest's PSCI call sequence. Intercept
		 * SYSTEM_RESET(0x84000009)/SYSTEM_OFF(0x84000008): DON'T actually reset —
		 * report success and resume, so the board stays alive and we can see
		 * whether the guest was the one resetting it. Everything else goes
		 * through psci_guest_filter() (H3 mitigation, see its block comment
		 * above): a small whitelist of informational calls is forwarded to
		 * the real EL3 PSCI verbatim, CPU_ON/CPU_OFF/CPU_SUSPEND are decided
		 * HERE and never reach EL3 (guest code must never pick EL2's next PC
		 * via a warm-boot entry address), and anything unrecognized gets
		 * PSCI NOT_SUPPORTED. */
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
			/* SYSTEM_RESET (0x84000009): honor it the same controlled
			 * way. See dbg_clean_reset's comment for why lying to
			 * cpu_reset() is worse than resetting -- it parks the guest
			 * in an eternal two-instruction spin that looks exactly like
			 * a hang, and the evidence a "stay alive" path was meant to
			 * preserve already survives the warm reset in DRAM. */
			if (fnid == 0x84000009ull && dbg_clean_reset) {
				r[1u + ((idx - 1u) & 0xFu)] = 0x0FF0FF09u; /* bc: clean-reset */
				__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(r) : "memory");
				wdt_debug_hold = 1;   /* let the WDOG actually fire */
				reboot_clean();       /* USB drop + WDOG warm reset; no return */
			}
			if ((fnid == 0x84000009ull || fnid == 0x84000008ull) &&
			    dbg_block_reset) {
				frame->x[0] = 0;          /* PSCI SUCCESS, but no real reset */
				frame->elr += 4u;
				return;
			}
			/* H3 mitigation: run every other guest PSCI call through the
			 * whitelist filter BEFORE it gets anywhere near the real `smc`
			 * below. A return of 1 means the filter already decided the
			 * outcome (emulated success or a denial) — resume the guest
			 * with that result and skip EL3 entirely. See
			 * psci_guest_filter()'s block comment for the full policy. */
			{
				int64_t psci_ret;

				if (psci_guest_filter(fnid, frame->x[1],
				                      frame->x[2], frame->x[3],
				                      &psci_ret)) {
					frame->x[0] = (uint64_t)psci_ret;
					frame->elr += 4u;
					return;
				}
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
#if STAGE2_WX_DYNAMIC
		/* Opt-in dynamic W^X (stage2.h, off by default — see
		 * docs/wx-enforcement.md): the SAME four ECs as the first-fault
		 * probe above (a stage-2 abort can only be one or the other —
		 * firstfault_handle() already returned 0, i.e. "not the vector
		 * page"), but this one is a general guest-DRAM W^X permission
		 * flip rather than a single fixed IPA. stage2_wx_fault() itself
		 * filters everything that isn't a DRAM PERMISSION fault back out
		 * (translation faults, non-DRAM IPAs, the two HV windows), so it
		 * is safe to try unconditionally here for every one of these ECs. */
		if ((ec == 0x20u || ec == 0x21u || ec == 0x24u || ec == 0x25u) &&
		    stage2_wx_fault(frame))
			return;
#endif
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

	/* Guest (lower-EL) synchronous DATA ABORT (ESR EC==0x24): route by which
	 * core is trapping. Every existing target only ever reaches this with
	 * cpu==0 (CPU3 sits in `wfi` in all of them, see smp.c), so the cpu==3
	 * branch below is dead code for them; the cpu!=3 branch is the ORIGINAL
	 * code, unchanged, just now inside an explicit else. */
	if ((kind >> 2) == 2u && (kind & 3u) == EL2_KIND_SYNC &&
	    (((uint32_t)(frame->esr >> 26)) & 0x3fu) == 0x24u) {
		if (smp_cpu_id() == 3u) {
			/* Dual-guest milestone: CPU3/Zephyr. Try the channel-1 virtual
			 * UART0 first (the one real, meaningfully-emulated device
			 * Zephyr's console needs — see vconsole.c/vconsole.h); anything
			 * else CPU3 could fault on (its stage-2 table has NO other
			 * carve-outs at all — see stage2_zephyr.c) falls to the
			 * generic MMIO absorber. Deliberately NOT calling
			 * dbgmon_service() here — that service point reads/writes
			 * g_last_guest_frame, which this milestone reserves for CPU0
			 * (see the snapshot guard above); CPU3 has no analogous live
			 * debug-core service in this design. */
			if (vconsole_handle_fault(frame, 1)) {
				return;
			}
			/* GICD is policed for BOTH partitions, not just the one we
			 * happen to distrust: the guarantee is symmetric or it is not a
			 * guarantee. Must come BEFORE mmio_absorb_fault(), which would
			 * otherwise swallow the access as read-as-zero/write-as-noop and
			 * silently drop a legitimate interrupt configuration. */
			if (vgicd_handle_fault(frame)) {
				return;
			}
			if (mmio_absorb_fault(frame)) {
				return;
			}
			/* Fell outside every known-safe range on CPU3 -- do NOT
			 * silently swallow (a real Zephyr-side bug, e.g. wandered
			 * outside its 32 MiB slice, must stay visible): fall through
			 * to the generic unhandled-fault path below, same as any
			 * other unclaimed abort. */
		} else {
			/* ORIGINAL code, unchanged: first try the virtual UART console
			 * — a stage-2 fault on the unmapped UART0 page means the
			 * FreeBSD guest is doing console I/O. vconsole emulates it
			 * (captures transmitted bytes, returns "ready" on status
			 * reads, advances ELR) and returns 1 if handled — then this is
			 * normal guest progress, NOT a fault, so we return without
			 * recording. Only genuinely unhandled aborts fall through to
			 * the fault record below. */
			if (vconsole_handle_fault(frame, 0)) {
				if (!dbg_core_active)
					dbgmon_service(frame);
				return;
			}
			/* The trapped GIC distributor page (vgicd.c). Same
			 * "handled -> return without recording" contract as vconsole:
			 * a guest touching GICD is normal interrupt setup, not a fault.
			 * Under IMO=0 the guest owns interrupt delivery, so this only
			 * polices the two affinity fields that can reach across the
			 * partition boundary -- see vgicd.h. */
			if (vgicd_handle_fault(frame)) {
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
			/* Next, the virtual keyboard at 0x0A003000 (vinput.h). Same
			 * "handled -> return without recording" contract as vblk/vnet
			 * above; vinput_mmio_fault() returns 0 for any abort outside its
			 * own 0x200-byte window, so calling it unconditionally here is
			 * safe. Events themselves are never driven from a guest write —
			 * they are injected by vinput_send_key()/vinput_send_ascii() from
			 * dbgmon.c's `type`/`key` commands, on CPU1. */
			if (vinput_mmio_fault(frame)) {
				if (!dbg_core_active)
					dbgmon_service(frame);
				return;
			}
			/* Next, virtio-blk over the microSD card at 0x0A004000
			 * (vblk_sd.h) — a SECOND, independent disk so /var can move off
			 * the boot-critical eMMC. Same "handled -> return without
			 * recording" contract as every device above. */
			if (vblk_sd_mmio_fault(frame)) {
				if (!dbg_core_active)
					dbgmon_service(frame);
				return;
			}
#ifdef HV_HDMI
			/* Finally, the zero-copy-scanout flip doorbell at 0x0A002000
			 * (ROADMAP: zero-copy GPU scanout -- see
			 * docs/zero-copy-scanout.md). Same "handled -> return without
			 * recording" contract as vconsole/vblk/vnet above;
			 * scanout_mmio_fault() returns 0 for any abort outside its own
			 * 0x100-byte window (disjoint from all three), so calling it
			 * unconditionally here is safe. Compiled and called only in
			 * HV_HDMI builds -- with no framebuffer there is nothing to
			 * flip, and this keeps every non-HV_HDMI target's link exactly
			 * as it was before this device existed. */
			if (scanout_mmio_fault(frame)) {
				if (!dbg_core_active)
					dbgmon_service(frame);
				return;
			}
#endif
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

		/* A1 isolation ENFORCEMENT report: if this stage-2 abort's faulting
		 * IPA lands in an HV DRAM window (hv-image 0x42000000..0x42200000 or
		 * hv-scratch 0x50000000..0x50200000), the guest just tried to touch
		 * hypervisor memory and was BLOCKED by the stage-2 partition instead
		 * of silently corrupting it — the whole point of milestone A1. Flag it
		 * distinctly (dedicated counter breadcrumb + its own flight-recorder
		 * kind) so a real violation is unmistakable, separate from an ordinary
		 * guest page fault. hpfar is still in scope from the block above. */
		{
			uint64_t ipa = (hpfar & 0xFFFFFFFFF0ULL) << 8;
			if ((ipa >= 0x42000000ULL && ipa < 0x42200000ULL) ||
			    (ipa >= 0x50000000ULL && ipa < 0x50200000ULL)) {
				static uint32_t hvviol_count;
				exc_bc(19, ++hvviol_count);
				flightrec_log(FLTR_K_HVVIOL, ipa, frame->elr);
			}
		}
	}

	/* B3 crash-forensics: capture a frame-pointer backtrace into the BTR1
	 * ring (backtrace.c) for every fault recorded above — previously
	 * backtrace_walk() only ran on-demand (dbgmon's `bt` command), so a
	 * board that rebooted before an operator typed `bt` had no backtrace at
	 * all. pc=ELR, fp=x29, lr=x30, same triple panic.c/dbgmon.c use;
	 * defensive by construction (never faults on a bad fp). Also resolve the
	 * first few frames to symbol+offset (backtrace_symbolize(), backtrace.c)
	 * into the separate BTS1 breadcrumb — best-effort/additive, a no-op past
	 * its own header stamp if no FreeBSD kernel has been kload_parse_elf()'d
	 * this boot; the raw BTR1 addresses above are unaffected either way. */
	{
		uint64_t bt_out[16];
		int bt_n = backtrace_walk(frame->elr, frame->x[29], frame->x[30],
		                          bt_out, 16);
		backtrace_symbolize(bt_out, bt_n);
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
