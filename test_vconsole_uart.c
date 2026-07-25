/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vconsole_uart.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the trap-and-emulate 16550 UART register-decode/emulation logic
 * in vconsole.c.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "vconsole.c"` WITH STUBS
 * ============================================================================
 * vconsole.c was read in full before writing this file. Its register
 * decode/emulation core (the page-offset fold at line 415, the IIR/LSR/USR/
 * MSR/THR value-synthesis if/else chains at lines 417-507) is pure integer
 * logic driven entirely by (register offset, direction, latched IER/pend
 * state) -- exactly the "pure part" the task asked to extract. It is not
 * ISOLATED pure logic in the source, though: vconsole_handle_fault() also
 * threads through:
 *   - "mrs %0, hpfar_el2"                                  (line 372)
 *   - vc_store32/vc_load32/vc_store_byte's "dc civac, %0\n\tdsb sy"
 *                                                            (lines 241/257)
 *   - vc_rx_getc()/vconsole_rx_push()/vc_txtee_push()'s "dmb ishld"/"dsb sy"
 *     cross-core ring barriers                              (lines 72-108,136-171)
 * None of those are gcc builtins on x86_64. More importantly, the task
 * explicitly scopes this mirror to "the pure part, not the cross-core rings
 * or MMIO faulting": the HPFAR-based IPA reconstruction and the el2_frame/
 * ESR.ISS decode (ISV/WNR/SRT -- lines 382-402) are the "MMIO faulting"
 * half (how a stage-2 fault becomes "guest wants register X"), and the RX-
 * injection/TX-tee rings (lines 40-171) are the "cross-core rings" half --
 * both excluded per the task's own framing. What's mirrored here is
 * everything AFTER the ESR is decoded and BEFORE the ring producer/consumer
 * internals: given a page offset and (for writes) a value, what does the
 * emulated 16550 do and what does it decide vc_uart_ier/vc_txrdy_pend
 * should become. The rings themselves are replaced by a two-state test
 * double (a pending-byte flag + a queued byte), documented at its
 * definition below, that exercises the SAME boolean vc_rx_pending()
 * contract the real emulation logic branches on -- not a mirror of the
 * ring's head/tail/wrap arithmetic, which test_vblk_ring.c/test_vnet_ring.c
 * already establish the convention for elsewhere and which is explicitly
 * out of scope here.
 *
 * THE BUGS THIS FILE IS AIMED AT (both documented in vconsole.c's own
 * comments, found live on hardware):
 *   1. IIR-always-0 infinite loop (vconsole.c:206-212): the first cut
 *      synthesized IIR=0 for "no interrupt pending", but ns8250_clrint()
 *      loops `while ((iir & IIR_NOPEND) == 0)` -- IIR_NOPEND is bit0, so an
 *      all-zero IIR reads as "still pending" forever. test_iir_nopend_is_
 *      bit0_not_zero() below pins UART_IIR_NOPEND == 0x01 AND that the
 *      no-sources-enabled path returns exactly that value, never 0.
 *   2. THRE-interrupt LEVEL-vs-EDGE bug (vconsole.c:196-204): the first
 *      version returned TXRDY on EVERY IIR read while ETXRDY was enabled
 *      (level), so ns8250_clrint()'s drain-until-NOPEND loop never
 *      terminated. test_iir_txrdy_edge_latch_one_shot() below reproduces
 *      the exact two-reads-in-a-row sequence that bug failed.
 *   3. Multi-UART page-fold (vconsole.c:404-415): UART0/1/2 share one 4 KiB
 *      stage-2 page at sub-offsets 0x000/0x400/0x800; without `& 0x3FF`
 *      folding, UART1/2 register reads/writes fall through to the "any
 *      other register" default and desynchronize from the emulated state
 *      machine. test_page_fold_maps_uart0_1_2() below pins this directly.
 *
 * Build: gcc -o test_vconsole_uart test_vconsole_uart.c && ./test_vconsole_uart
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Mirrored register offsets/bit values -- verbatim from vconsole.c:27-37
 * and :172-175/206-212.
 * ------------------------------------------------------------------ */
#define UART_REG_THR   0x00u   /* vconsole.c:27 */
#define UART_REG_IER   0x04u   /* vconsole.c:28 */
#define UART_REG_IIR   0x08u   /* vconsole.c:29 */
#define UART_REG_LCR   0x0Cu   /* vconsole.c:30 */
#define UART_REG_MCR   0x10u   /* vconsole.c:31 */
#define UART_REG_LSR   0x14u   /* vconsole.c:32 */
#define UART_REG_MSR   0x18u   /* vconsole.c:33 */
#define UART_REG_USR   0x7Cu   /* vconsole.c:34 */

#define UART_LSR_THRE_TEMT   0x60u   /* vconsole.c:36 */
#define UART_LSR_DR          0x01u   /* vconsole.c:37 */

#define UART_IIR_RXRDY       0x04u   /* vconsole.c:172 */
#define UART_IIR_TXRDY       0x02u   /* vconsole.c:173 */
#define UART_IER_ERXRDY      0x01u   /* vconsole.c:174 */
#define UART_IER_ETXRDY      0x02u   /* vconsole.c:175 */

/* vconsole.c:206-212 -- bit0 = "no interrupt pending". A synthesized 0
 * (rather than this) is EXACTLY bug #1 above: ns8250_clrint() loops while
 * (iir & IIR_NOPEND) == 0, so an all-zero IIR never satisfies the loop. */
#define UART_IIR_NOPEND      0x01u

/* MSR bits: DCD|DSR|CTS permanently asserted (vconsole.c:488-498), so a tty
 * open() without CLOCAL never blocks forever in the carrier wait. */
#define UART_MSR_ASSERTED    0xB0u

/* ------------------------------------------------------------------ *
 * Mirrored state -- verbatim from vconsole.c:194/204 (module-scope latches,
 * one instance per emulated UART page in the real source -- see vc_uart_ier's
 * own comment: "one per emulated uart is overkill", so a single instance is
 * faithful here too).
 * ------------------------------------------------------------------ */
static uint32_t vc_uart_ier;
static uint32_t vc_txrdy_pend;

/* ------------------------------------------------------------------ *
 * Test-only scaffolding for the RX ring's BOOLEAN CONTRACT -- NOT a mirror
 * of vc_rx_pending()/vc_rx_getc()'s actual ring (head/tail/wrap, cross-core
 * barriers -- vconsole.c:57-77), which is explicitly out of scope per the
 * task ("not the cross-core rings"). This is a minimal two-state double
 * (pending flag + one queued byte) that lets tests drive the SAME
 * "is a host-injected byte waiting" boolean the mirrored emulation logic
 * below branches on, exactly as vc_rx_pending()'s return value would.
 * ------------------------------------------------------------------ */
static int mock_rx_has_byte;
static uint8_t mock_rx_byte;

static void mock_rx_push(uint8_t c)
{
	mock_rx_has_byte = 1;
	mock_rx_byte = c;
}

static int mock_rx_pending(void)
{
	return mock_rx_has_byte;
}

static uint8_t mock_rx_getc(void)
{
	mock_rx_has_byte = 0;
	return mock_rx_byte;
}

/* Resets both the mirrored emulation state (vc_uart_ier/vc_txrdy_pend) AND
 * the mock RX double above, so every test starts from a single, clean,
 * well-defined state with one call. */
static void vc_reset_state(void)
{
	vc_uart_ier = 0;
	vc_txrdy_pend = 0;
	mock_rx_has_byte = 0;
	mock_rx_byte = 0;
}

/* ------------------------------------------------------------------ *
 * vc_fold_reg() -- mirrors vconsole.c:404-415's page-offset fold verbatim.
 * off is (addr - UART0_BASE); UART0/1/2 alias the same 16550 register
 * layout at sub-page offsets 0x000/0x400/0x800 (see the comment at
 * vconsole.c:406-414), so `& 0x3FF` collapses all three onto one register
 * window.
 * ------------------------------------------------------------------ */
static uint32_t
vc_fold_reg(uint32_t off)
{
	return off & 0x3FFu;
}

/* ------------------------------------------------------------------ *
 * vc_emulate_write() -- mirrors vconsole.c:417-444's write-side branch
 * verbatim (already-decoded `reg`/`val` in place of the ESR SRT/x[srt]
 * decode at vconsole.c:394/419, which is "MMIO faulting", out of scope).
 * `captured` receives the THR byte (mirrors vconsole_capture_byte() being
 * called -- vconsole.c:424 -- but not the capture ring itself, which is
 * pure telemetry storage, not emulation logic); pass NULL to ignore it.
 * ------------------------------------------------------------------ */
static void
vc_emulate_write(uint32_t reg, uint64_t val, uint8_t *captured)
{
	if (reg == UART_REG_THR) {
		if (captured)
			*captured = (uint8_t)(val & 0xffu);
		vc_txrdy_pend = 1;
	} else if (reg == UART_REG_IER) {
		uint32_t newier = (uint32_t)(val & 0xffu);
		if ((newier & UART_IER_ETXRDY) && !(vc_uart_ier & UART_IER_ETXRDY))
			vc_txrdy_pend = 1;
		vc_uart_ier = newier;
	}
	/* Every other write (IIR-FCR/LCR/MCR) is silently ignored, matching
	 * vconsole.c:442-444. */
}

/* ------------------------------------------------------------------ *
 * vc_emulate_read() -- mirrors vconsole.c:445-507's read-side branch
 * verbatim, with vc_rx_pending()/vc_rx_getc() replaced by the mock_rx_*
 * test double described above (same boolean contract, no ring internals).
 * ------------------------------------------------------------------ */
static uint64_t
vc_emulate_read(uint32_t reg)
{
	uint64_t val = 0;

	if (reg == UART_REG_THR) {
		val = mock_rx_pending() ? (uint64_t)mock_rx_getc() : 0;
	} else if (reg == UART_REG_LSR) {
		val = UART_LSR_THRE_TEMT;
		if (mock_rx_pending())
			val |= UART_LSR_DR;
	} else if (reg == UART_REG_USR) {
		val = 0;
	} else if (reg == UART_REG_IIR) {
		if ((vc_uart_ier & UART_IER_ERXRDY) && mock_rx_pending()) {
			val = UART_IIR_RXRDY;
		} else if ((vc_uart_ier & UART_IER_ETXRDY) && vc_txrdy_pend) {
			vc_txrdy_pend = 0;
			val = UART_IIR_TXRDY;
		} else {
			val = UART_IIR_NOPEND;
		}
	} else if (reg == UART_REG_MSR) {
		val = UART_MSR_ASSERTED;
	} else {
		val = 0;
	}
	return val;
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* BUG #1 regression: with no interrupt source enabled (IER==0, the state
 * ns8250_clrint() probes in at UART attach time), an IIR read must return
 * exactly UART_IIR_NOPEND (bit0 set), NEVER 0 -- a synthesized 0 makes
 * `while ((iir & IIR_NOPEND) == 0)` loop forever. */
static void test_iir_nopend_is_bit0_not_zero(void)
{
	vc_reset_state();
	assert(UART_IIR_NOPEND == 0x01u);

	uint64_t v = vc_emulate_read(UART_REG_IIR);
	assert(v == UART_IIR_NOPEND);
	assert(v != 0);
	assert((v & UART_IIR_NOPEND) != 0);   /* the exact bit ns8250_clrint() tests */
}

/* IIR still reads NOPEND even with data waiting/THR just written, as long
 * as the guest never enabled the corresponding IER bit -- our model gates
 * every reported source on IER, matching vconsole.c:480-487. */
static void test_iir_nopend_when_sources_not_enabled(void)
{
	vc_reset_state();
	mock_rx_push('X');
	vc_emulate_write(UART_REG_THR, 'Y', NULL);   /* would arm txrdy_pend, but IER==0 */

	uint64_t v = vc_emulate_read(UART_REG_IIR);
	assert(v == UART_IIR_NOPEND);
}

/* BUG #3 regression: UART0/1/2 share one page at sub-offsets 0x000/0x400/
 * 0x800; every register offset must fold identically regardless of which
 * UART's block it came from. Without the `& 0x3FF` fold, UART1/2 reads
 * silently miss every register check and fall to the "unknown register"
 * default. */
static void test_page_fold_maps_uart0_1_2(void)
{
	const uint32_t uarts[3] = { 0x000u, 0x400u, 0x800u };
	const uint32_t regs[8] = {
		UART_REG_THR, UART_REG_IER, UART_REG_IIR, UART_REG_LCR,
		UART_REG_MCR, UART_REG_LSR, UART_REG_MSR, UART_REG_USR,
	};

	for (unsigned r = 0; r < 8; r++) {
		uint32_t expect = regs[r];
		for (unsigned u = 0; u < 3; u++) {
			uint32_t off = uarts[u] + regs[r];
			assert(vc_fold_reg(off) == expect);
		}
	}

	/* Concretely: UART1's IIR (page offset 0x408) must behave exactly like
	 * UART0's IIR (0x008), not fall through to the "any other read"
	 * default (which vconsole.c would give it pre-fold: val=0, itself
	 * indistinguishable from the IIR-always-0 bug on the secondary UART). */
	vc_reset_state();
	uint32_t reg_uart1_iir = vc_fold_reg(0x400u + UART_REG_IIR);
	assert(reg_uart1_iir == UART_REG_IIR);
	uint64_t v = vc_emulate_read(reg_uart1_iir);
	assert(v == UART_IIR_NOPEND);
	assert(v != 0);
}

/* LSR always reports THRE|TEMT (transmitter permanently ready, so FreeBSD's
 * busy-wait-for-tx-ready loop never blocks -- vconsole.c:461-468) and OR's
 * in DR exactly when a host-injected byte is waiting. */
static void test_lsr_thre_temt_and_dr(void)
{
	vc_reset_state();
	mock_rx_has_byte = 0;
	uint64_t v = vc_emulate_read(UART_REG_LSR);
	assert(v == UART_LSR_THRE_TEMT);           /* 0x60, DR clear */
	assert((v & UART_LSR_DR) == 0);

	mock_rx_push('Q');
	v = vc_emulate_read(UART_REG_LSR);
	assert(v == (UART_LSR_THRE_TEMT | UART_LSR_DR));   /* 0x61 */
}

/* USR (Allwinner busy-status register) always reads 0 ("never busy") --
 * vconsole.c:469-471. */
static void test_usr_always_zero(void)
{
	vc_reset_state();
	assert(vc_emulate_read(UART_REG_USR) == 0);
	mock_rx_push('Z');
	assert(vc_emulate_read(UART_REG_USR) == 0);   /* unaffected by RX state */
}

/* MSR reports DCD|DSR|CTS permanently asserted (0xB0) so a tty open()
 * without CLOCAL never sleeps forever in the carrier wait -- the
 * userland-hang bug documented at vconsole.c:490-497. */
static void test_msr_reports_carrier_asserted(void)
{
	vc_reset_state();
	assert(vc_emulate_read(UART_REG_MSR) == UART_MSR_ASSERTED);
	assert((UART_MSR_ASSERTED & 0x80u) != 0);   /* DCD */
	assert((UART_MSR_ASSERTED & 0x20u) != 0);   /* DSR */
	assert((UART_MSR_ASSERTED & 0x10u) != 0);   /* CTS */
}

/* THR write: captures the transmitted byte and arms the THRE edge
 * (vc_txrdy_pend=1) unconditionally, regardless of IER (the edge fires
 * every real THR-empties event; whether it's ever REPORTED depends on
 * ETXRDY, tested separately) -- vconsole.c:421-426. */
static void test_thr_write_captures_and_arms_edge(void)
{
	vc_reset_state();
	uint8_t captured = 0;
	vc_emulate_write(UART_REG_THR, 0x141, &captured);   /* low byte only */
	assert(captured == 0x41);          /* 'A', high bits truncated */
	assert(vc_txrdy_pend == 1);
}

/* RBR (THR's read-side alias) dequeues exactly one host-injected byte when
 * pending, else reads 0 -- vconsole.c:449-460. */
static void test_rbr_read_dequeues_only_when_pending(void)
{
	vc_reset_state();
	assert(vc_emulate_read(UART_REG_THR) == 0);   /* nothing pending */

	mock_rx_push(0x55);
	uint64_t v = vc_emulate_read(UART_REG_THR);
	assert(v == 0x55);
	/* Consumed: a second read with nothing new pending reads 0 again, and
	 * LSR.DR must have dropped too (same underlying pending flag). */
	assert(vc_emulate_read(UART_REG_THR) == 0);
	assert((vc_emulate_read(UART_REG_LSR) & UART_LSR_DR) == 0);
}

/* IIR priority: RXRDY (enabled + pending) beats TXRDY (enabled + pended) --
 * vconsole.c:480-484 checks RXRDY first. RX being reported must NOT consume
 * the still-latched TX edge (it's a separate source; only an actual IIR
 * report of TXRDY consumes vc_txrdy_pend, per vconsole.c:482-484). */
static void test_iir_rxrdy_priority_over_txrdy(void)
{
	vc_reset_state();
	vc_uart_ier = UART_IER_ERXRDY | UART_IER_ETXRDY;
	vc_txrdy_pend = 1;
	mock_rx_push('R');

	uint64_t v = vc_emulate_read(UART_REG_IIR);
	assert(v == UART_IIR_RXRDY);
	assert(vc_txrdy_pend == 1);   /* untouched -- RXRDY path never clears it */

	/* Once RX drains (mock_rx_pending() false), the still-pending TX edge
	 * surfaces on the next read. */
	mock_rx_getc();
	v = vc_emulate_read(UART_REG_IIR);
	assert(v == UART_IIR_TXRDY);
	assert(vc_txrdy_pend == 0);   /* consumed by this read */
}

/* BUG #2 regression: THRE must be an EDGE, not a LEVEL. One THR write (or
 * one IER-enable-while-empty transition) must produce TXRDY on exactly the
 * NEXT IIR read and NOPEND on every read after that until another edge
 * occurs. The first buggy version returned TXRDY on every read while
 * ETXRDY was set, so ns8250_clrint()'s "read IIR until NOPEND" loop spun
 * forever. */
static void test_iir_txrdy_edge_latch_one_shot(void)
{
	vc_reset_state();
	vc_uart_ier = UART_IER_ETXRDY;

	uint8_t captured;
	vc_emulate_write(UART_REG_THR, 'z', &captured);   /* fires the edge */
	assert(vc_txrdy_pend == 1);

	uint64_t v1 = vc_emulate_read(UART_REG_IIR);
	assert(v1 == UART_IIR_TXRDY);          /* first read: edge reported */

	uint64_t v2 = vc_emulate_read(UART_REG_IIR);
	assert(v2 == UART_IIR_NOPEND);         /* THE regression: NOT TXRDY again */

	uint64_t v3 = vc_emulate_read(UART_REG_IIR);
	assert(v3 == UART_IIR_NOPEND);         /* stays clear, doesn't re-arm itself */
}

/* Enabling ETXRDY while THR is (always, in this model) already empty must
 * immediately arm the edge -- vconsole.c:436-440's rationale: this is what
 * wakes a tty that armed the interrupt AFTER its last THR write, rather
 * than requiring one more THR write to ever see TXRDY. Re-writing the SAME
 * IER value (ETXRDY already set) must NOT re-arm it. */
static void test_ier_enable_etxrdy_arms_edge_once(void)
{
	vc_reset_state();
	assert(vc_txrdy_pend == 0);

	vc_emulate_write(UART_REG_IER, UART_IER_ETXRDY, NULL);
	assert(vc_uart_ier == UART_IER_ETXRDY);
	assert(vc_txrdy_pend == 1);            /* armed by the 0->1 transition */

	/* Consume it. */
	uint64_t v = vc_emulate_read(UART_REG_IIR);
	assert(v == UART_IIR_TXRDY);
	assert(vc_txrdy_pend == 0);

	/* Re-writing the identical IER value again (no 0->1 edge on ETXRDY)
	 * must NOT re-arm -- next IIR read stays NOPEND. */
	vc_emulate_write(UART_REG_IER, UART_IER_ETXRDY, NULL);
	assert(vc_txrdy_pend == 0);
	assert(vc_emulate_read(UART_REG_IIR) == UART_IIR_NOPEND);
}

/* Writes to any register other than THR/IER (IIR-as-FCR, LCR, MCR) are
 * silently accepted no-ops: they must not perturb vc_uart_ier/vc_txrdy_pend
 * or any subsequent read -- vconsole.c:442-444. */
static void test_other_register_writes_are_ignored(void)
{
	vc_reset_state();
	vc_uart_ier = UART_IER_ETXRDY;
	vc_txrdy_pend = 0;

	vc_emulate_write(UART_REG_LCR, 0x03, NULL);
	vc_emulate_write(UART_REG_MCR, 0x0B, NULL);
	vc_emulate_write(UART_REG_IIR, 0x01, NULL);   /* FCR alias on write */

	assert(vc_uart_ier == UART_IER_ETXRDY);   /* unchanged */
	assert(vc_txrdy_pend == 0);               /* unchanged */
	assert(vc_emulate_read(UART_REG_IIR) == UART_IIR_NOPEND);
}

/* ==================================================================== *
 * main() — runs every test, reports pass/fail, exits nonzero on failure.
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "iir_nopend_is_bit0_not_zero",           test_iir_nopend_is_bit0_not_zero },
	{ "iir_nopend_when_sources_not_enabled",   test_iir_nopend_when_sources_not_enabled },
	{ "page_fold_maps_uart0_1_2",              test_page_fold_maps_uart0_1_2 },
	{ "lsr_thre_temt_and_dr",                  test_lsr_thre_temt_and_dr },
	{ "usr_always_zero",                       test_usr_always_zero },
	{ "msr_reports_carrier_asserted",          test_msr_reports_carrier_asserted },
	{ "thr_write_captures_and_arms_edge",      test_thr_write_captures_and_arms_edge },
	{ "rbr_read_dequeues_only_when_pending",   test_rbr_read_dequeues_only_when_pending },
	{ "iir_rxrdy_priority_over_txrdy",         test_iir_rxrdy_priority_over_txrdy },
	{ "iir_txrdy_edge_latch_one_shot",         test_iir_txrdy_edge_latch_one_shot },
	{ "ier_enable_etxrdy_arms_edge_once",      test_ier_enable_etxrdy_arms_edge_once },
	{ "other_register_writes_are_ignored",     test_other_register_writes_are_ignored },
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

	printf("---- vconsole UART emulation tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
