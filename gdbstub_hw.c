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
 */
#include <stdint.h>
#include "exceptions.h"
#include "gdbstub_hw.h"

/* From hwbp.c (already declared in hwbp.h, re-declared here to avoid pulling in
 * the whole header set; signatures MUST match hwbp.h). */
extern int  hwbp_set(int idx, uint64_t va, int is_write_wp);
extern int  hwbp_clear(int idx, int is_write_wp);

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
	int i, slot;

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
		if (hwbp_set(slot, addr, 0) != 0)
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
		if (hwbp_set(slot, addr, 1) != 0)   /* baseline store-watch + MDE/TDE */
			return 0;
		patch_wp_lsc(slot, lsc_for_type(type));   /* select read/write/access */
		wp_slot[slot].used = 1;
		wp_slot[slot].type = type;
		wp_slot[slot].addr = addr;
		wp_slot[slot].len  = (kind > 0) ? kind : 8;
		return 1;
	}

	return 0;                                /* unknown type -> empty reply */
}

/* ------------------------------------------------------------------ *
 * Public: remove.
 * ------------------------------------------------------------------ */
int gdbstub_hw_remove(int type, uint64_t addr)
{
	int i;

	if (type == GDB_BP_HW) {
		for (i = 0; i < HW_MAX_BP; i++)
			if (bp_slot[i].used && bp_slot[i].addr == addr) {
				hwbp_clear(i, 0);
				bp_slot[i].used = 0;
				return 1;
			}
		return 1;                            /* unknown -> no-op success */
	}
	/* any watch type */
	for (i = 0; i < HW_MAX_WP; i++)
		if (wp_slot[i].used && wp_slot[i].addr == addr) {
			hwbp_clear(i, 1);
			wp_slot[i].used = 0;
			return 1;
		}
	return 1;
}

void gdbstub_hw_clear_all(void)
{
	int i;
	for (i = 0; i < HW_MAX_BP; i++)
		if (bp_slot[i].used) { hwbp_clear(i, 0); bp_slot[i].used = 0; }
	for (i = 0; i < HW_MAX_WP; i++)
		if (wp_slot[i].used) { hwbp_clear(i, 1); wp_slot[i].used = 0; }
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
