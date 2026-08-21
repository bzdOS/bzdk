/* SPDX-License-Identifier: BSD-2-Clause */

/* gdbstub_hw.c — hardware breakpoint / watchpoint lane for the GDB stub.
 *
 * See gdbstub_hw.h for the contract and docs/gdbstub-design.md §5 for the
 * design. This module owns the mapping GDB Z1/z1 (hw exec bp) and Z2/Z3/Z4
 * (watch/rwatch/awatch) <-> the hwbp.c DBGB/DBGW breakpoint/watchpoint slots,
 * plus the "which slot fired / what address" logic the stop-reply needs.
 *
 * It layers on hwbp.c rather than duplicating it:
 *   - Type 1 (hw breakpoint) uses hwbp_set(slot, va, 0) / hwbp_clear(slot, 0)
 *     verbatim — an instruction address match is exactly what hwbp.c programs.
 *   - Type 2/3/4 (watchpoint) uses hwbp_set(slot, va, 1) to get the baseline
 *     arming (DBGWVR aligned, MDE+TDE on, hwbp shadow/breadcrumb updated), then
 *     PATCHES DBGWCR<slot>.LSC to select read/access (hwbp_set hard-codes
 *     store-only). That patch is the only DBG* register write we issue directly.
 *
 * Freestanding: <stdint.h> + "exceptions.h" only. No libc, no FP/SIMD.
 *
 * NOTE (board verification): the LSC-patch approach assumes hwbp_set() has
 * already left DBGWCR<slot> enabled with BAS=0xff; we only flip bits[4:3]. If a
 * future hwbp.c changes the WCR field layout this must track it. Marked TODO
 * where the exact A64 watchpoint length/BAS semantics need a live check.
 *
 * ------------------------------------------------------------------------
 * CONFIRMED BUG (2026-07-26, see gdbstub-hwbp-wrong-core memory) + FIX.
 * ------------------------------------------------------------------------
 * DBGBVR/DBGBCR/DBGWVR/DBGWCR are per-PE BANKED registers. gdbstub_hw_insert()
 * / _remove() are only ever called from gdbstub.c's dispatch(), which is only
 * ever called from smp.c's CPU1 debug-service loop (both call sites:
 * gdbstub_on_debug_event() and gdbstub_poll()) — CPU1 never hosts the FreeBSD
 * guest. This module used to call hwbp_set()/hwbp_clear()/patch_wp_lsc()
 * DIRECTLY, i.e. on CPU1 — arming CPU1's own debug unit, which never executes
 * guest code, so the guest could never trip it. Every hbreak/watch/rwatch/
 * awatch silently never fired.
 *
 * Fix: route every real DBG*-register write through the SAME cross-core
 * handshake style already proven for the CPU0-stop/CPU1-resume direction
 * (gdb_stop_pending/gdb_resume_act, el2_exc.c/smp.c) — but in the OPPOSITE
 * direction (CPU1 asks CPU0 to do something). This only works while CPU0 is
 * ALREADY PARKED in el2_exc.c's `while (gdb_resume_act == 0xffffffffu) wfe;`
 * loop (a real GDB stop is active), because that is the only place CPU0 is
 * guaranteed to be sitting in EL2 code we can safely extend to run one more
 * op before it resumes — see hwop_run() below and el2_exc.c's gdb_hw_op_*
 * block comment. When CPU0 is instead running the guest freely (the common
 * "set a breakpoint ahead of time" case), transparently interrupting it would
 * need a new SGI/IPI path (this board's GIC has none today) — DELIBERATELY
 * NOT built this pass (real bare-metal EL2 code, no OS safety net; a
 * mis-built async-IRQ-into-guest path risks hanging or corrupting the guest,
 * which is worse than "hbreak silently doesn't fire"). hwop_run() detects
 * that case (gdb_stop_pending == 0) and refuses cleanly (-2) instead of
 * arming the wrong core or guessing. Callers needing a breakpoint that fires
 * on a NOT-yet-reached kernel address should use software `break` (Z0, fixed
 * separately — see gdbstub.c's resolve()) — hbreak/watch set while CPU0 is
 * already parked at a prior stop (e.g. "stop here, then also watch this
 * address, then continue") DOES work correctly with this fix. */
#include <stdint.h>
#include "exceptions.h"
#include "gdbstub_hw.h"

/* From hwbp.c (already declared in hwbp.h, re-declared here to avoid pulling in
 * the whole header set; signatures MUST match hwbp.h). */
extern int  hwbp_set(int idx, uint64_t va, int is_write_wp);
extern int  hwbp_clear(int idx, int is_write_wp);

/* Cross-core hw-op request globals, defined in el2_exc.c (always linked) —
 * see its block comment for the full handshake contract. Plain (non-weak)
 * extern: this file is only ever linked together with el2_exc.o. */
extern volatile uint32_t gdb_stop_pending;   /* 1 while CPU0 is parked      */
extern volatile uint32_t gdb_hw_op_pending;
extern volatile int32_t  gdb_hw_op_kind;
extern volatile int32_t  gdb_hw_op_slot;
extern volatile uint64_t gdb_hw_op_va;
extern volatile uint32_t gdb_hw_op_lsc;
extern volatile int32_t  gdb_hw_op_result;

enum {
	HWOP_BP_SET   = 0,
	HWOP_BP_CLEAR = 1,
	HWOP_WP_SET   = 2,
	HWOP_WP_CLEAR = 3,
};

/* Bounded spin guard for hwop_run()'s wait below. CPU0, once parked, services
 * a queued op within a handful of instructions after its very next `wfe`
 * wake — this cap is generous headroom, not a tuned timing budget, and exists
 * only so a CPU1 caller can never hang forever if something is wrong (per the
 * task's own "never spin forever" requirement). */
#define HWOP_SPIN_GUARD 10000000L

/* Post a hw-op request to CPU0 and wait (bounded) for it to complete — but
 * ONLY if CPU0 is actually parked right now (gdb_stop_pending != 0): that is
 * the ONLY state in which CPU0 is guaranteed to be sitting in the wfe loop
 * that services gdb_hw_op_*, per this file's header comment. If CPU0 is
 * running the guest instead, there is no safe way to reach it from here today
 * — refuse with -2 rather than touch its banked debug registers from the
 * wrong core (the original bug) or silently do nothing while claiming
 * success. Returns: gdb_hw_op_result (>= 0, hwbp_set/hwbp_clear's own
 * 0=success/-1=bad-index convention) on a serviced request, or -2 if CPU0
 * wasn't parked / the request timed out. */
static int hwop_run(int kind, int slot, uint64_t va, uint32_t lsc)
{
	long guard;

	if (!gdb_stop_pending)
		return -2;                      /* CPU0 not parked -- refuse cleanly */

	gdb_hw_op_slot   = slot;
	gdb_hw_op_va     = va;
	gdb_hw_op_lsc    = lsc;
	gdb_hw_op_result = -2;
	__asm__ volatile("dsb sy" ::: "memory");
	gdb_hw_op_kind = kind;
	__asm__ volatile("dsb sy" ::: "memory");
	gdb_hw_op_pending = 1u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");   /* poke CPU0's wfe */

	for (guard = 0; guard < HWOP_SPIN_GUARD; guard++) {
		if (!gdb_hw_op_pending)
			break;
		__asm__ volatile("wfe" ::: "memory");
	}
	if (gdb_hw_op_pending)
		return -2;                      /* timed out: never touched HW state */

	return gdb_hw_op_result;
}

/* A64 slot maxima (hwbp.c clamps the live count from ID_AA64DFR0_EL1; we use
 * the architectural maxima for our shadow arrays and let hwbp_set return -1 for
 * a slot the hardware doesn't implement). */
#define HW_MAX_BP 6
#define HW_MAX_WP 4

#define SPSR_D_BIT (1ull << 9)   /* PSTATE.D — debug-exception mask */

/* ESR_EL2.EC values for guest debug exceptions routed to EL2 by MDCR_EL2.TDE. */
#define EC_BREAKPT 0x30u         /* breakpoint from a lower EL   */
#define EC_WATCHPT 0x34u         /* watchpoint from a lower EL   */

/* DBGWCR.LSC (bits[4:3]) load/store control per GDB watch type. */
#define LSC_LOAD   0x1u          /* rwatch  */
#define LSC_STORE  0x2u          /* watch   */
#define LSC_BOTH   0x3u          /* awatch  */

/* ------------------------------------------------------------------ *
 * Shadow tables: what GDB asked us to arm, and which hwbp.c slot it maps to.
 * We need these because (a) z<type>,addr must find the slot again, and (b) a
 * watchpoint hit reports FAR_EL2 which we match back to an armed doubleword to
 * decide watch vs rwatch vs awatch in the stop reply.
 * ------------------------------------------------------------------ */
struct hw_ent {
	int      used;
	int      type;     /* GDB_BP_HW / GDB_WP_* */
	uint64_t addr;     /* as GDB gave it       */
	int      len;      /* watch length (bytes); 4 for a bp */
};

static struct hw_ent bp_slot[HW_MAX_BP];   /* index == DBGB slot */
static struct hw_ent wp_slot[HW_MAX_WP];   /* index == DBGW slot */

/* ------------------------------------------------------------------ *
 * Direct DBGWCR<slot> patch (only LSC bits) — the one place we touch a debug
 * register ourselves. The register NAME must be a literal, hence switch-arm.
 * ------------------------------------------------------------------ */
#define RD(reg)    ({ uint64_t _v; __asm__ volatile("mrs %0, " reg : "=r"(_v)); _v; })
#define WR(reg, v) __asm__ volatile("msr " reg ", %0" :: "r"((uint64_t)(v)) : "memory")

static uint64_t rd_wcr(int n)
{
	switch (n) {
	case 0: return RD("dbgwcr0_el1");
	case 1: return RD("dbgwcr1_el1");
	case 2: return RD("dbgwcr2_el1");
	case 3: return RD("dbgwcr3_el1");
	}
	return 0;
}

static void wr_wcr(int n, uint64_t v)
{
	switch (n) {
	case 0: WR("dbgwcr0_el1", v); break;
	case 1: WR("dbgwcr1_el1", v); break;
	case 2: WR("dbgwcr2_el1", v); break;
	case 3: WR("dbgwcr3_el1", v); break;
	}
	__asm__ volatile("isb" ::: "memory");
}

/* After hwbp_set(slot, va, 1) armed a store-watch, rewrite LSC for the GDB
 * type. BAS (bits[12:5]) is left as hwbp_set programmed it (0xff = whole
 * doubleword). For sub-doubleword lengths a tighter BAS would be more precise;
 * whole-doubleword is a safe superset (may over-trigger on neighbours in the
 * same 8-byte region). TODO: derive BAS from (addr&7, len) once verified on the
 * board that A64 honours partial BAS for our LSC values. */
static void patch_wp_lsc(int slot, uint32_t lsc)
{
	uint64_t wcr = rd_wcr(slot);
	wcr &= ~(0x3ull << 3);
	wcr |= ((uint64_t)(lsc & 0x3u)) << 3;
	wr_wcr(slot, wcr);
}

static uint32_t lsc_for_type(int type)
{
	if (type == GDB_WP_READ)   return LSC_LOAD;
	if (type == GDB_WP_ACCESS) return LSC_BOTH;
	return LSC_STORE;            /* GDB_WP_WRITE */
}

/* ------------------------------------------------------------------ *
 * Public: insert.
 * ------------------------------------------------------------------ */
int gdbstub_hw_insert(int type, uint64_t addr, int kind, struct el2_frame *frame)
{
	int i, slot, r;

	/* Debug exceptions are masked while the guest runs with PSTATE.D=1 (early
	 * FreeBSD locore). Arming a slot then would silently never fire — refuse so
	 * GDB reports a clear error. Software BRK (Z0) is the early-boot path. */
	if (frame && (frame->spsr & SPSR_D_BIT))
		return -1;

	if (type == GDB_BP_HW) {
		/* Reuse: already armed at this addr? idempotent success. */
		for (i = 0; i < HW_MAX_BP; i++)
			if (bp_slot[i].used && bp_slot[i].addr == addr)
				return 1;
		slot = -1;
		for (i = 0; i < HW_MAX_BP; i++)
			if (!bp_slot[i].used) { slot = i; break; }
		if (slot < 0)
			return 0;                       /* full -> GDB falls back to SW bp */
		/* Arm the REAL DBGBVR/DBGBCR on CPU0 (see this file's header comment
		 * for why never here on CPU1). r==-2 means CPU0 isn't parked right
		 * now -- refuse cleanly rather than arm the wrong core. */
		r = hwop_run(HWOP_BP_SET, slot, addr, 0);
		if (r == -2)
			return -2;
		if (r != 0)
			return 0;                       /* slot not implemented on this core */
		bp_slot[slot].used = 1;
		bp_slot[slot].type = type;
		bp_slot[slot].addr = addr;
		bp_slot[slot].len  = 4;
		return 1;
	}

	if (type == GDB_WP_WRITE || type == GDB_WP_READ || type == GDB_WP_ACCESS) {
		for (i = 0; i < HW_MAX_WP; i++)
			if (wp_slot[i].used && wp_slot[i].addr == addr)
				return 1;
		slot = -1;
		for (i = 0; i < HW_MAX_WP; i++)
			if (!wp_slot[i].used) { slot = i; break; }
		if (slot < 0)
			return 0;
		/* CPU0-side applies hwbp_set(slot,addr,1) THEN the LSC patch, as one
		 * op (HWOP_WP_SET) -- see gdbstub_hw_apply_op() below. */
		r = hwop_run(HWOP_WP_SET, slot, addr, lsc_for_type(type));
		if (r == -2)
			return -2;
		if (r != 0)
			return 0;
		wp_slot[slot].used = 1;
		wp_slot[slot].type = type;
		wp_slot[slot].addr = addr;
		wp_slot[slot].len  = (kind > 0) ? kind : 8;
		return 1;
	}

	return 0;                                /* unknown type -> empty reply */
}

/* ------------------------------------------------------------------ *
 * Public: remove. Returns 1 on success (or "never armed" no-op), 0 if a
 * REAL armed slot exists but couldn't be safely cleared right now (CPU0 not
 * parked) -- the caller (gdbstub.c dispatch()) must report an error rather
 * than claim OK, since the shadow table is deliberately left `used` in that
 * case (the real hardware slot is still armed; state stays consistent so a
 * later retry, once CPU0 is parked again, can still find and clear it).
 * ------------------------------------------------------------------ */
int gdbstub_hw_remove(int type, uint64_t addr)
{
	int i;

	if (type == GDB_BP_HW) {
		for (i = 0; i < HW_MAX_BP; i++)
			if (bp_slot[i].used && bp_slot[i].addr == addr) {
				if (hwop_run(HWOP_BP_CLEAR, i, 0, 0) != 0)
					return 0;       /* CPU0 not parked -- leave armed+tracked */
				bp_slot[i].used = 0;
				return 1;
			}
		return 1;                            /* unknown -> no-op success */
	}
	/* any watch type */
	for (i = 0; i < HW_MAX_WP; i++)
		if (wp_slot[i].used && wp_slot[i].addr == addr) {
			if (hwop_run(HWOP_WP_CLEAR, i, 0, 0) != 0)
				return 0;
			wp_slot[i].used = 0;
			return 1;
		}
	return 1;
}

/* Best-effort: clears every real armed slot IF CPU0 happens to be parked
 * right now (the normal case when this runs from GDB's D/k, inside the same
 * command_loop() as a live stop). If CPU0 is NOT parked (e.g. a detach
 * issued from the Ctrl-C "fake stop" path in gdbstub_poll(), where CPU0 never
 * actually halted), a real hw slot cannot be safely cleared from here today
 * (same restriction as gdbstub_hw_remove()) and is left armed on CPU0 — it
 * will keep firing hwbp.c's own one-shot handler on the guest until the next
 * board reset. Still zeroes the shadow table either way (this is a
 * terminal detach/kill; nothing will ask "is it still armed?" again through
 * this module after this call), so at least gdbstub's OWN bookkeeping never
 * goes stale even in that edge case. */
void gdbstub_hw_clear_all(void)
{
	int i;
	for (i = 0; i < HW_MAX_BP; i++)
		if (bp_slot[i].used) { hwop_run(HWOP_BP_CLEAR, i, 0, 0); bp_slot[i].used = 0; }
	for (i = 0; i < HW_MAX_WP; i++)
		if (wp_slot[i].used) { hwop_run(HWOP_WP_CLEAR, i, 0, 0); wp_slot[i].used = 0; }
}

/* ------------------------------------------------------------------ *
 * CPU0-side executor: called from INSIDE el2_exc.c's parked wfe loop, i.e.
 * this runs on CPU0, the core that actually owns the guest's live debug
 * register bank -- the entire point of this module's fix (see the header
 * comment). Reads the gdb_hw_op_* request CPU1's hwop_run() just posted and
 * performs the exact same hwbp_set()/hwbp_clear()/patch_wp_lsc() sequence
 * this module used to run directly (on the wrong core). Writes
 * gdb_hw_op_result; does NOT touch gdb_hw_op_pending (el2_exc.c owns
 * clearing that, symmetric with how CPU1 owns clearing gdb_stop_pending in
 * the opposite-direction handshake).
 * ------------------------------------------------------------------ */
void gdbstub_hw_apply_op(void)
{
	int r;

	switch (gdb_hw_op_kind) {
	case HWOP_BP_SET:
		gdb_hw_op_result = hwbp_set(gdb_hw_op_slot, gdb_hw_op_va, 0);
		break;
	case HWOP_BP_CLEAR:
		gdb_hw_op_result = hwbp_clear(gdb_hw_op_slot, 0);
		break;
	case HWOP_WP_SET:
		r = hwbp_set(gdb_hw_op_slot, gdb_hw_op_va, 1);
		if (r == 0)
			patch_wp_lsc(gdb_hw_op_slot, gdb_hw_op_lsc);
		gdb_hw_op_result = r;
		break;
	case HWOP_WP_CLEAR:
		gdb_hw_op_result = hwbp_clear(gdb_hw_op_slot, 1);
		break;
	default:
		gdb_hw_op_result = -1;
		break;
	}
}

int gdbstub_hw_active(void)
{
	int i;
	for (i = 0; i < HW_MAX_BP; i++) if (bp_slot[i].used) return 1;
	for (i = 0; i < HW_MAX_WP; i++) if (wp_slot[i].used) return 1;
	return 0;
}

/* ------------------------------------------------------------------ *
 * Public: stop-reason for a hw bp/wp trap.
 * ------------------------------------------------------------------ */
static char nyb(unsigned v)
{
	static const char t[] = "0123456789abcdef";
	return t[v & 0xf];
}

/* Emit `addr` as big-endian hex (GDB stop-reason addresses are big-endian,
 * unlike the little-endian register/memory hex) with no leading zeros beyond
 * one nibble. Returns chars written. */
static int emit_be_hex(char *out, uint64_t v)
{
	int n = 0, started = 0, shift;
	for (shift = 60; shift >= 0; shift -= 4) {
		unsigned d = (unsigned)((v >> shift) & 0xf);
		if (d || started || shift == 0) {
			out[n++] = nyb(d);
			started = 1;
		}
	}
	return n;
}

static const char *append(char *out, int *n, const char *s)
{
	while (*s)
		out[(*n)++] = *s++;
	return s;
}

int gdbstub_hw_stop_reason(struct el2_frame *frame, char *out)
{
	uint32_t ec = (uint32_t)((frame->esr >> 26) & 0x3fu);
	int n = 0, i;

	if (ec == EC_BREAKPT) {
		/* Only claim it if we actually own an armed hw breakpoint (otherwise it
		 * may be a stray / non-GDB bp — let the caller use the plain reply). */
		if (!gdbstub_hw_active())
			return 0;
		append(out, &n, "hwbreak:;");
		out[n] = '\0';
		return 1;
	}

	if (ec == EC_WATCHPT) {
		uint64_t da = frame->far;          /* accessed data address */
		int slot = -1;
		for (i = 0; i < HW_MAX_WP; i++)
			if (wp_slot[i].used &&
			    (wp_slot[i].addr & ~7ull) == (da & ~7ull)) { slot = i; break; }
		if (slot < 0)
			return 0;                      /* not one of ours */
		/* Map DBGW LSC/type back to the GDB stop key. */
		if (wp_slot[slot].type == GDB_WP_READ)
			append(out, &n, "rwatch:");
		else if (wp_slot[slot].type == GDB_WP_ACCESS)
			append(out, &n, "awatch:");
		else
			append(out, &n, "watch:");
		n += emit_be_hex(out + n, da);
		out[n++] = ';';
		out[n]   = '\0';
		return 1;
	}

	return 0;
}
