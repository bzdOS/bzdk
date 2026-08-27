/* SPDX-License-Identifier: BSD-2-Clause */

/* test_wdogtrap.c — hosted (x86_64, plain gcc, no cross-compiler) unit tests
 * for the POLICY logic in wdogtrap.c: the WDOG-range refuse check and the
 * PC5-field-forcing math for PIO_PC_CFG0. This is the CCU/PIO/WDOG page trap
 * described in docs/wdog-ccu-pio-stage2.md and wdogtrap.h — a stage-2 trap
 * that is compiled into every real-hardware target (wdogtrap.o now sits next
 * to vgicd.o in DBG_OBJS/DUAL_OBJS/GDB_OBJS/FBSD_OBJS/ZEPHYR_OBJS/REPL_OBJS)
 * but is only ever REACHED once STAGE2_TRAP_WDOG_PAGE (stage2.c) is defined,
 * which no shipping target does today. This file proves the POLICY MATH is
 * correct independent of that flag, exactly like test_vgicd.c does for
 * vgicd.c's own policy math.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "wdogtrap.c"`
 * ============================================================================
 * Same convention test_vgicd.c/test_vgic_pendq.c document, for the same
 * reason: wdogtrap_handle_fault() is entangled with real AArch64 state
 * (`mrs hpfar_el2`, `volatile` MMIO loads/stores against a physical address,
 * `dc civac`/`dsb sy` breadcrumbs, smp_cpu_id()'s MRS against MPIDR_EL1) that
 * cannot and should not run on an x86_64 host. What this file mirrors is
 * deliberately narrow: the WDOG-range classification and the PC5 nibble-
 * forcing arithmetic — pure integer math, verbatim from wdogtrap.c (line
 * references in each test) — with the MMIO/breadcrumb/ESR-decode plumbing
 * left OUT rather than stubbed. This proves the policy; it does NOT and
 * cannot prove the trap dispatch, the real HPFAR/FAR reconstruction, or
 * anything about real CCU/PIO/WDOG hardware — that needs a board, which this
 * pass deliberately did not touch (see docs/wdog-ccu-pio-stage2.md).
 *
 * Every constant here was re-checked against the live tftpboot DTB
 * (`dtc -I dtb -O dts bananapi-min.dtb`) before being written: `clock@1c20000`
 * reg size 0x400, `pinctrl@1c20800` reg size 0x400, `watchdog@1c20ca0` reg
 * size 0x20 (status "disabled"), all inside the one page 0x1C20000-0x1C20FFF.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Constants, hand-transcribed from soc_a64.h / wdogtrap.c (NOT included —
 * see the header comment above). All offsets are relative to the page base
 * WDOGTRAP_PAGE_BASE == SOC_A64_CCU_BASE == 0x01C20000.
 * ------------------------------------------------------------------ */
#define WDOG_OFF_BASE  0xCA0u   /* SOC_A64_WDOG_NODE_BASE - page base */
#define WDOG_OFF_END   0xCC0u   /* WDOG_OFF_BASE + SOC_A64_WDOG_NODE_SIZE (0x20) */
#define PC_CFG0_OFF    0x848u   /* SOC_A64_PIO_PC_CFG0 - page base */
#define PC5_BYTE_OFF   0x84Au   /* PC_CFG0_OFF + 2 — the byte holding PC5's nibble */

/* ------------------------------------------------------------------ *
 * WDOG-range classification — verbatim from wdogtrap.c's
 * wdogtrap_handle_fault(), the `if (off >= WDOG_OFF_BASE && off <
 * WDOG_OFF_END)` check. No nbytes/overlap math needed here — see that file's
 * comment on why a plain start-offset check is sufficient for THIS range
 * (both boundaries are 4-byte aligned and Device-nGnRE accesses are always
 * naturally aligned, so no access can straddle either edge).
 * ------------------------------------------------------------------ */
static int
in_wdog_range(uint32_t off)
{
	return off >= WDOG_OFF_BASE && off < WDOG_OFF_END;
}

/* ------------------------------------------------------------------ *
 * PC5 nibble-forcing — verbatim from wdogtrap.c's write path (the
 * `nbytes`/`bit_in_val` block). `val` is the FULL 32-bit value the guest's
 * store instruction carries (frame->x[srt] before any width truncation,
 * exactly as the real code reads it); `off`/`sas` describe the access the
 * same way ESR_EL2 does (sas: 0=byte,1=halfword,2/3=word). Returns the
 * value with PC5's nibble forced when the access's byte range includes
 * PC5_BYTE_OFF, and reports via *forced_out whether anything actually
 * changed (mirrors the g_pc5_reforced counter gate) and via *touches_pc5_out
 * whether the PC5-forcing branch ran at all (mirrors "this access is
 * classified as touching PC_CFG0's PC5 field, not the generic passthrough
 * bucket").
 * ------------------------------------------------------------------ */
static uint32_t
pc5_force(uint32_t off, uint32_t sas, uint32_t val, int *touches_pc5_out, int *forced_out)
{
	unsigned nbytes = (sas == 0) ? 1u : (sas == 1) ? 2u : 4u;

	if (off < PC5_BYTE_OFF + 1u && off + nbytes > PC5_BYTE_OFF) {
		unsigned bit_in_val = (PC5_BYTE_OFF - off) * 8u;
		uint32_t before = val;
		val = (val & ~(0xFu << (bit_in_val + 4u))) | (3u << (bit_in_val + 4u));
		if (touches_pc5_out) *touches_pc5_out = 1;
		if (forced_out) *forced_out = (val != before);
		return val;
	}
	if (touches_pc5_out) *touches_pc5_out = 0;
	if (forced_out) *forced_out = 0;
	return val;   /* unmodified — generic passthrough bucket */
}

/* ==================================================================== *
 * Tests: WDOG range
 * ==================================================================== */

/* The three registers wdt.c actually pokes (WDOG_CTRL 0xCB0, WDOG_CFG 0xCB4,
 * WDOG_MODE 0xCB8) must all classify as WDOG range. This is the load-bearing
 * case: these three offsets are exactly what wdt.c writes on every pet. */
static void test_wdog_range_covers_wdt_c_registers(void)
{
	assert(in_wdog_range(0xCB0u));   /* WDOG_CTRL */
	assert(in_wdog_range(0xCB4u));   /* WDOG_CFG  */
	assert(in_wdog_range(0xCB8u));   /* WDOG_MODE */
}

/* The DTB node's own declared span (`watchdog@1c20ca0`, reg size 0x20) is
 * WIDER than wdt.c's three registers — it also covers WDOG_IRQ_EN/IRQ_STA at
 * 0xCA0/0xCA4. Those must be denied too, even though nothing in this tree
 * writes them: a guest could still try. */
static void test_wdog_range_covers_full_dtb_node_span(void)
{
	assert(in_wdog_range(0xCA0u));   /* WDOG_IRQ_EN, first byte of the node */
	assert(in_wdog_range(0xCA4u));   /* WDOG_IRQ_STA */
	assert(in_wdog_range(0xCBFu));   /* last byte of the node (0xCA0+0x20-1) */
}

/* Exactly one byte before/after the node must NOT be denied — pins the
 * boundary so it cannot silently grow or shrink. 0xC9F is the last byte of
 * `timer@1c20c00`; 0xCC0 is the first unclassified byte past the node. */
static void test_wdog_range_boundaries_are_exclusive(void)
{
	assert(!in_wdog_range(0xC9Fu));
	assert(!in_wdog_range(0xCC0u));
}

/* A naturally-aligned word access starting just before the range (0xC9C,
 * covering 0xC9C-0xC9F) must NOT be classified as WDOG -- it cannot reach
 * into 0xCA0 at all, confirming the "no straddle" alignment argument
 * wdogtrap.c's comment makes rather than just asserting it. */
static void test_wdog_range_aligned_word_before_cannot_straddle(void)
{
	uint32_t off = 0xC9Cu;
	unsigned nbytes = 4u;
	assert(!in_wdog_range(off));
	assert(off + nbytes <= WDOG_OFF_BASE);   /* the access ends before WDOG starts */
}

/* ==================================================================== *
 * Tests: PC5 forcing
 * ==================================================================== */

/* THE EXPECTED-COMMON CASE: a full 32-bit RMW at the register's own base
 * (SAS=2/word), matching how a real pinctrl driver is expected to touch this
 * register. PC5's nibble (bits[23:20]) must end up forced to 0b0011
 * regardless of what the guest wrote there; every other nibble in the word
 * must survive untouched. */
static void test_pc5_word_write_at_base_forces_nibble_keeps_rest(void)
{
	/* guest tries to set every PC pin's function field to 0 (gpio_in),
	 * including PC5 -- exactly the FreeBSD pinctrl bug this whole trap
	 * exists to neutralise. */
	uint32_t guest_val = 0x00000000u;
	int touches = 0, forced = 0;
	uint32_t out = pc5_force(PC_CFG0_OFF, 2u /* word */, guest_val, &touches, &forced);

	assert(touches == 1);
	assert(forced == 1);
	assert(((out >> 20) & 0xFu) == 0x3u);   /* PC5 forced to func 3 */
	assert((out & ~(0xFu << 20)) == (guest_val & ~(0xFu << 20)));  /* rest untouched */
}

/* Same word write, but the guest ALREADY asked for func 3 on PC5 (the
 * well-behaved case, e.g. right after main_dbg.c's own boot-time set): the
 * value must come out identical and the "had to force it" counter gate must
 * NOT fire -- mirrors HVMAP_WDOGTRAP_BC word 5 staying 0 when nothing needed
 * correcting. */
static void test_pc5_word_write_already_func3_is_a_noop(void)
{
	uint32_t guest_val = (0x3u << 20);   /* only PC5 set, already func 3 */
	int touches = 0, forced = 0;
	uint32_t out = pc5_force(PC_CFG0_OFF, 2u, guest_val, &touches, &forced);

	assert(touches == 1);
	assert(forced == 0);
	assert(out == guest_val);
}

/* THE BYTE-PARTIAL CASE this test file exists to pin: a single-byte write
 * landing EXACTLY on PC5_BYTE_OFF (0x84A), not on PC_CFG0_OFF (0x848). An
 * offset-equality-only check (`off == PC_CFG0_OFF`) would miss this and let
 * the guest clear PC5 through the back door -- this is the exact gap fixed
 * before landing (see wdogtrap.c's comment on the write path). bit_in_val
 * must come out 0 (this byte IS PC5's byte), so the forced nibble sits at
 * bits[7:4] of the 8-bit access value. */
static void test_pc5_byte_write_at_pc5_byte_offset_is_caught(void)
{
	uint32_t guest_byte = 0x00u;   /* guest tries to clear PC5's nibble */
	int touches = 0, forced = 0;
	uint32_t out = pc5_force(PC5_BYTE_OFF, 0u /* byte */, guest_byte, &touches, &forced);

	assert(touches == 1);
	assert(forced == 1);
	assert(((out >> 4) & 0xFu) == 0x3u);   /* forced within the byte's own bits[7:4] */
}

/* A halfword write at PC5_BYTE_OFF (covers PC5_BYTE_OFF and the byte after
 * it) must also be caught, with the nibble forced at bit 4 of the 16-bit
 * value (bit_in_val == 0, same as the byte case, since the access STARTS at
 * PC5_BYTE_OFF). */
static void test_pc5_halfword_write_at_pc5_byte_offset_is_caught(void)
{
	uint32_t guest_half = 0x0000u;
	int touches = 0, forced = 0;
	uint32_t out = pc5_force(PC5_BYTE_OFF, 1u /* halfword */, guest_half, &touches, &forced);

	assert(touches == 1);
	assert(((out >> 4) & 0xFu) == 0x3u);
	(void)forced;
}

/* A halfword write at PC_CFG0_OFF (covers bytes 0-1, i.e. PC0-PC3's fields,
 * NOT PC5) must NOT be classified as touching PC5 -- confirms the overlap
 * math correctly excludes an access that stays entirely below PC5's byte. */
static void test_pc5_halfword_write_at_base_does_not_touch_pc5(void)
{
	int touches = 0, forced = 0;
	uint32_t out = pc5_force(PC_CFG0_OFF, 1u /* halfword */, 0xBEEFu, &touches, &forced);

	assert(touches == 0);
	assert(forced == 0);
	assert(out == 0xBEEFu);   /* fully unmodified -- generic passthrough bucket */
}

/* A byte write at PC_CFG0_OFF+1 (PC1's field, byte 1) must likewise stay
 * outside the PC5 branch -- pins that only byte index 2 of the register is
 * special-cased, not "anywhere near PC_CFG0". */
static void test_pc5_byte_write_at_offset_plus1_does_not_touch_pc5(void)
{
	int touches = 0, forced = 0;
	uint32_t out = pc5_force(PC_CFG0_OFF + 1u, 0u /* byte */, 0x07u, &touches, &forced);

	assert(touches == 0);
	assert(out == 0x07u);
}

/* A word write straddling PC5_BYTE_OFF from an offset that is NOT the
 * register's own base (e.g. an access starting one byte in, off=0x849,
 * width 4, covering bytes 1..4 -- not naturally aligned and not something a
 * real driver would issue, but the overlap math must still classify it
 * correctly rather than silently under- or over-matching). bit_in_val should
 * be (0x84A-0x849)*8 == 8. */
static void test_pc5_overlap_math_handles_non_base_aligned_access(void)
{
	uint32_t guest_val = 0x00000000u;
	int touches = 0, forced = 0;
	uint32_t out = pc5_force(PC_CFG0_OFF + 1u, 2u /* word */, guest_val, &touches, &forced);

	assert(touches == 1);
	assert(((out >> (8 + 4)) & 0xFu) == 0x3u);   /* forced at bit_in_val(8)+4 */
}

/* ==================================================================== *
 * Tests: the two ranges never overlap (the ordering guarantee wdogtrap.c's
 * comment relies on -- WDOG is checked first and returns unconditionally,
 * which only matters as a SAFE ordering if the ranges are actually disjoint
 * to begin with).
 * ==================================================================== */
static void test_wdog_range_and_pc5_byte_are_disjoint(void)
{
	assert(WDOG_OFF_END <= PC_CFG0_OFF || PC5_BYTE_OFF + 1u <= WDOG_OFF_BASE);
}

/* ==================================================================== */

static const struct { const char *name; void (*fn)(void); } k_tests[] = {
	{ "wdog_range_covers_wdt_c_registers",              test_wdog_range_covers_wdt_c_registers },
	{ "wdog_range_covers_full_dtb_node_span",           test_wdog_range_covers_full_dtb_node_span },
	{ "wdog_range_boundaries_are_exclusive",            test_wdog_range_boundaries_are_exclusive },
	{ "wdog_range_aligned_word_before_cannot_straddle", test_wdog_range_aligned_word_before_cannot_straddle },
	{ "pc5_word_write_at_base_forces_nibble_keeps_rest", test_pc5_word_write_at_base_forces_nibble_keeps_rest },
	{ "pc5_word_write_already_func3_is_a_noop",         test_pc5_word_write_already_func3_is_a_noop },
	{ "pc5_byte_write_at_pc5_byte_offset_is_caught",    test_pc5_byte_write_at_pc5_byte_offset_is_caught },
	{ "pc5_halfword_write_at_pc5_byte_offset_is_caught", test_pc5_halfword_write_at_pc5_byte_offset_is_caught },
	{ "pc5_halfword_write_at_base_does_not_touch_pc5",  test_pc5_halfword_write_at_base_does_not_touch_pc5 },
	{ "pc5_byte_write_at_offset_plus1_does_not_touch_pc5", test_pc5_byte_write_at_offset_plus1_does_not_touch_pc5 },
	{ "pc5_overlap_math_handles_non_base_aligned_access", test_pc5_overlap_math_handles_non_base_aligned_access },
	{ "wdog_range_and_pc5_byte_are_disjoint",           test_wdog_range_and_pc5_byte_are_disjoint },
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- wdogtrap policy tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
