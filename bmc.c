/* SPDX-License-Identifier: BSD-2-Clause */

/* bmc.c — "software BMC" management-plane dispatch for the bzdOS EL2
 * hypervisor. See bmc.h for the big picture and the transport/threading model.
 *
 * In one line: bmc_dispatch() is called by dbgmon.c's exec_line() when the
 * first token of a console line is "bmc"; it routes the verb to a handler that
 * calls an EXISTING hypervisor primitive (reboot_clean, the vconsole rings, the
 * dbg_* flags, the EXC/GICT/SMP1/UART/FF1V breadcrumbs, the A64 thermal
 * sensor) and prints a plain-text reply. Nothing here owns a transport, a
 * device driver (bar an optional thermal read), or any long-lived state beyond
 * the destructive-verb safety gate. Non-blocking + bounded, like dbgmon —
 * this runs on the CPU1 debug core with the guest preempted.
 *
 * SELF-CONTAINED I/O: mirrors dbgmon.c's style — its own tiny freestanding
 * print/parse helpers over the same extern console_* hooks, no libc.
 */
#include <stdint.h>
#include "exceptions.h"
#include "bmc.h"
#include "vconsole.h"   /* VCONSOLE_BUF_BASE/SIZE -- never copy them, see below */ /* HVMAP_VCPM_* -- the postmortem carry-over lane      */
#include "hv_addrmap.h"
#include "cntpct.h"

/* ------------------------------------------------------------------ *
 * Console hooks — identical extern contract dbgmon.c/repl.c use. main_dbg.c
 * wires these to the EMAC (and, via usbacm.c, the USB-ACM) console. NON-
 * BLOCKING: console_getc() returns -1 immediately if no byte is available.
 * ------------------------------------------------------------------ */
extern int  console_getc(void);
extern void console_putc(int c);
extern void console_flush(void);

/* ------------------------------------------------------------------ *
 * Existing primitives this façade drives. Extern decls only (bmc.c stays
 * self-contained, no cross-module header pull-in beyond bmc.h/exceptions.h),
 * exactly as dbgmon.c declares the hwbp and backtrace helpers.
 * ------------------------------------------------------------------ */

/* reboot.c — clean USB-gadget disconnect + WDOG reboot to U-Boot (noreturn). */
extern void reboot_clean(void);

/* wdt.c — dead-man watchdog controls + the debug-core reset gate.
 * wdt_debug_hold is a fixed-address macro (HVMAP_WDT_DEBUG_HOLD, see
 * wdt.h/hv_addrmap.h), not a plain linked global anymore -- pulling in
 * wdt.h (rather than a local `extern volatile uint32_t wdt_debug_hold;`)
 * is what makes that macro visible here. */
#include "wdt.h"
extern void wdt_arm(void);
extern void wdt_disarm(void);

/* vconsole.c — guest virtual-UART bridge rings. */
extern void vconsole_rx_push(uint8_t c);        /* host -> guest keystroke     */
extern int  vconsole_tx_tee_getc(uint8_t *out); /* guest -> host TX tee byte   */

/* smp.c / el2_exc.c — the live runtime debug flags a mgmt plane exposes. */
extern volatile uint32_t dbg_usbacm;
extern volatile uint32_t dbg_core_enable;
extern volatile uint32_t dbg_cpu1_wdog;
extern volatile uint32_t dbg_isolate_no_emac;
extern volatile uint32_t dbg_no_guest;
extern volatile uint32_t dbg_block_reset;

/* el2_exc.c — the shared guest register snapshot (guest PC / liveness). */
extern struct el2_frame g_last_guest_frame;
extern void el2_snapshot_guest_frame(struct el2_frame *out);  /* seqlock read (H8) */

/* axp803.c — AXP803 PMIC battery telemetry over the RSB bus (rsb.c). See
 * axp803.h for the full register-map citation/confidence breakdown. Extern
 * decl only, per this file's self-contained discipline (no axp803.h pull-in
 * beyond the two struct/constant names we actually use). */
struct axp803_health {
	uint32_t vbat_mv, ichg_ma, idischg_ma, ts_mv, status, chip_ok;
};
extern int  axp803_init(void);
extern void axp803_read_health(struct axp803_health *out);
#define BMC_BATT_PRESENT   (1u << 0)
#define BMC_BATT_CHARGING  (1u << 1)
#define BMC_BATT_VBUS      (1u << 2)
#define BMC_BATT_DIE_HOT   (1u << 3)
#define BMC_BATT_CHIP_OK   (1u << 4)

/* ------------------------------------------------------------------ *
 * Breadcrumb windows we READ (documented owners in parentheses). Fixed
 * addresses per the tree convention; we do not include their headers, we just
 * know the layout — same discipline dbgmon.c's cmd_t()/cmd_ff() use.
 * ------------------------------------------------------------------ */
/* BMC_GICT_BASE (0x50000800) is RETIRED -- do not read it, do not resurrect a
 * tick_lo/tick_hi/tick_delta read from it. Two independent, already-landed
 * findings elsewhere in this tree prove it cannot give a trustworthy tick
 * count over this channel:
 *   1. gic_timer.c no longer writes "GICT" there at all -- it moved to
 *      0x00018200 (gic_timer.c's own breadcrumb-window comment, "distinct
 *      from ... jitter/TIMR"). hv_addrmap.h's map comment still lists
 *      "GICT 0x800..." because migrating that legacy annotation is explicitly
 *      called out there as unfinished ("only the 0x50020000 I/O block ... is
 *      centralised so far") -- it is stale documentation, not current fact.
 *   2. hv_addrmap.h's own collision audit (2026-07-26) records that this
 *      address is ALIASED: "gic_timer.c's GICT (0x50000800) overlaps BTR1's
 *      tail (BTR1 runs to 0x50000820)" -- backtrace.c's BT_BC_BASE is
 *      0x50000700, size 0x120, i.e. words [64..71] of the *backtrace* ring
 *      physically occupy 0x50000800..0x5000081c. So the old code here
 *      (`gict[1]`, `gict[2]`) was not reading a stale-but-harmless sentinel
 *      like the other fields below -- it was reading LIVE backtrace-ring
 *      data and presenting it as a tick counter, which made `tick_delta`
 *      nonzero (and printed "(timer LIVE)") essentially any time the
 *      backtrace ring churned, regardless of whether the guest timer was
 *      healthy. That is a confidently WRONG reading, worse than the
 *      all-ones "n/a" sentinels this file already guards against below.
 *   3. Even the *correct* current address (0x00018200) is independently
 *      documented as unusable for a debug-channel read: gic_timer.c's own
 *      vtimer_mask_watchdog() comment (2026-07-29) says it "sits ... in
 *      GUEST-WRITABLE SRAM and read back as random bytes (magic not GICT)".
 * So there is currently no trustworthy tick source to point this at without
 * an address-map fix that is out of this file's scope (and needs board
 * verification this session cannot do). bmc_health_snapshot() below reports
 * NOTE 2026-08-20: the reason for retirement was the ALIASED address, not the
 * idea of publishing a tick. A collision-checked tick source now exists --
 * HVMAP_TRACE_RING (hv_addrmap.h, with _Static_asserts against its
 * neighbours), whose header carries total_events and the 24 MHz counter
 * frequency. Reviving a timer line here would mean bumping HEALTH_WORDS on
 * both sides (bmc.c and bmc_client.py) AND being careful that the ring counts
 * ALL events, not only ticks -- so it is left undone deliberately rather than
 * forgotten. The sentinel below stays the honest answer until then.
 *
 * the honest "no data" sentinel for tick_lo/tick_hi/tick_delta instead of
 * silently forwarding someone else's memory. Use hb_cpu0/hb_cpu1 (SMP1,
 * 0x50000900 -- unaffected by any of the above) for liveness; bzdctl.py
 * already prefers those over ticks for exactly this reason. */
#define BMC_EXC_BASE    0x50000400UL   /* el2_exc.c  "EXC1": [1]=count [2]=kind [3]=esr */
#define BMC_SMP_BASE    0x50000900UL   /* smp.c      "SMP1": [1]=online [6..9]=heartbeats */
#define BMC_UART_BASE   0x50000f00UL   /* vconsole.h "UART": [1]=total_bytes [2]=faults */
_Static_assert(BMC_UART_BASE == HVMAP_LOW_VCONSOLE_HDR,
               "BMC_UART_BASE drifted from hv_addrmap.h -- the map owns this address");
#define BMC_FFV_BASE    0x50005800UL   /* firstfault.c "FF1V": [1]=count            */
/* The captured-console byte buffer. Deliberately taken from vconsole.h rather
 * than hard-coded here: this line USED to read 0x50000f10 -- the address the
 * buffer had back when it sat immediately after the header -- and it was never
 * updated when vconsole.h moved it to its own explicit, non-adjacent base
 * (VCONSOLE_BUF_BASE, see that header for why). The result was that
 * `bmc con read` spent months reading whatever now lives just past the header
 * instead of the console ring, which is why `bzdctl.py console` reliably
 * returned ancient boot text: it was showing the ring's OLD location. That
 * misread cost the 2026-08-10 kldload investigation four passes, because the
 * guest's panic message was in the real ring the whole time and this command
 * could not see it. A private copy of another module's address is exactly how
 * that drift happened, so there is no private copy any more. */
#define BMC_UART_RINGBUF VCONSOLE_BUF_BASE
#define BMC_UART_RINGCAP VCONSOLE_BUF_SIZE

/* A64 THS (thermal sensor controller), 0x01C25000 — flat device-mapped, so an
 * EL2 read reaches it directly. THS0_DATA (@+0x80) holds the raw sample once
 * the controller is running (U-Boot/ATF leave it enabled on the A64). The A64
 * calibration is affine and NEGATIVE-slope; the sun50i-a64-ths coefficients
 * (Linux drivers/thermal/sun8i_thermal.c) give
 *     T_milliC = (2170 - raw) * 1000000 / 8560 . . . approx, VALIDATE on-board.
 * If the controller is not running (raw==0 or 0xFFF) we report 0 = n/a rather
 * than a bogus temperature. NOTE: this is the ONLY new hardware touch in the
 * whole BMC; every other verb is pure façade. */
#define A64_THS_BASE    0x01C25000UL
#define A64_THS0_DATA   (A64_THS_BASE + 0x80u)

/* ------------------------------------------------------------------ *
 * Tiny freestanding I/O + parse helpers (independent copy, dbgmon.c style).
 * ------------------------------------------------------------------ */
static void cputs(const char *s) { while (*s) console_putc((int)(unsigned char)*s++); }
static void nl(void) { console_putc('\r'); console_putc('\n'); }

static char hexdig(unsigned v) { v &= 0xfu; return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }
static void ph8(uint8_t v)  { console_putc(hexdig(v >> 4)); console_putc(hexdig(v)); }
static void ph32(uint32_t v){ ph8((uint8_t)(v>>24)); ph8((uint8_t)(v>>16)); ph8((uint8_t)(v>>8)); ph8((uint8_t)v); }
static void ph64(uint64_t v){ ph32((uint32_t)(v>>32)); ph32((uint32_t)v); }

/* Unsigned decimal, no leading zeros (used for human-friendly health text). */
static void pdec(uint32_t v)
{
	char b[10]; int i = 0;
	if (v == 0) { console_putc('0'); return; }
	while (v && i < 10) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
	while (i) console_putc(b[--i]);
}

static int streq(const char *a, const char *b)
{
	while (*a && *b) { if (*a != *b) return 0; a++; b++; }
	return *a == *b;
}

/* Parse hex (optional 0x) -> *out; 1 on success, 0 on empty/garbage. */
static int parse_hex(const char *s, unsigned long *out)
{
	unsigned long v = 0; int any = 0;
	if (!s) return 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	for (; *s; s++) {
		char c = *s; unsigned d;
		if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
		else return 0;
		v = (v << 4) | d; any = 1;
	}
	if (!any) return 0;
	*out = v; return 1;
}

static uint32_t rd32(unsigned long pa) { return *(volatile uint32_t *)pa; }

static inline uint64_t rd_cntpct(void)
{
	uint64_t v; v = cntpct_read();   /* cntpct.h: Allwinner counter erratum */ return v;
}

/* ------------------------------------------------------------------ *
 * Destructive-verb SAFETY GATE.
 *
 * The EMAC/USB-ACM channel is L2-local (a wire on br0) and unauthenticated —
 * this is intentionally a trust-the-LAN debug plane, not a cryptographic one.
 * But `reset`, `flag`, `wdt` etc. are DESTRUCTIVE (reboot_clean drops the board
 * to U-Boot; toggling dbg_core_enable can kill the very channel you are on), so
 * we require an explicit two-step ARM before any destructive verb, exactly like
 * a BMC's "chassis power" confirm:
 *
 *   bmc arm <nonce>       -> latches nonce + timestamp
 *   bmc reset             -> allowed once, if armed within BMC_ARM_WINDOW; the
 *                            arm is CONSUMED (one-shot) so a stray replayed
 *                            "reset" frame cannot fire on its own.
 *
 * The window is CNTPCT-based (real time) so a stale arm expires even if no
 * further commands arrive. This is a foot-gun guard, NOT security; a design
 * note in docs/bmc-design.md covers hardening (per-command HMAC over a shared
 * secret, or binding to the host's source MAC) if this ever leaves the lab.
 * ------------------------------------------------------------------ */
#define BMC_ARM_WINDOW_S   10u          /* arm valid for ~10 s */

static volatile uint32_t bmc_arm_nonce;     /* last armed nonce (0 = disarmed) */
static uint64_t          bmc_arm_at;        /* CNTPCT at arm time              */
static uint64_t          bmc_arm_ticks_win; /* BMC_ARM_WINDOW_S in ticks       */

static void bmc_arm(unsigned long nonce)
{
	uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	if (!f) f = 24000000ull;
	bmc_arm_ticks_win = f * (uint64_t)BMC_ARM_WINDOW_S;
	bmc_arm_nonce = (uint32_t)nonce ? (uint32_t)nonce : 1u;
	bmc_arm_at = rd_cntpct();
	cputs("armed: destructive verbs enabled for ");
	pdec(BMC_ARM_WINDOW_S); cputs("s\r\n");
}

/* Returns 1 and CONSUMES the arm if currently armed & fresh; else prints why
 * and returns 0. Every destructive handler calls this first. */
static int bmc_check_armed(void)
{
	if (bmc_arm_nonce == 0u) {
		cputs("refused: not armed (run 'bmc arm <nonce>' first)\r\n");
		return 0;
	}
	if (rd_cntpct() - bmc_arm_at > bmc_arm_ticks_win) {
		bmc_arm_nonce = 0u;
		cputs("refused: arm expired (re-run 'bmc arm <nonce>')\r\n");
		return 0;
	}
	bmc_arm_nonce = 0u;   /* one-shot consume */
	return 1;
}

/* ------------------------------------------------------------------ *
 * HEALTH — the structured status record (see struct bmc_health in bmc.h).
 * ------------------------------------------------------------------ */

/* Cache-coherent breadcrumb store, same dc-civac+dsb pattern as every other
 * lane so the record survives a warm WDT reset and is network-readable. */
static inline void bmc_bc(unsigned i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(BMC_HEALTH_BASE + (unsigned long)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Assemble the current dbg_* flags into the compact bitmap the record carries
 * and `bmc flags` prints. */
static uint32_t bmc_flag_bitmap(void)
{
	uint32_t f = 0;
	if (dbg_usbacm)          f |= BMC_FLAG_USBACM;
	if (dbg_core_enable)     f |= BMC_FLAG_CORE_ENABLE;
	if (dbg_cpu1_wdog)       f |= BMC_FLAG_CPU1_WDOG;
	if (dbg_isolate_no_emac) f |= BMC_FLAG_ISOLATE_NOEMAC;
	if (dbg_no_guest)        f |= BMC_FLAG_NO_GUEST;
	if (dbg_block_reset)     f |= BMC_FLAG_BLOCK_RESET;
	return f;
}

/* Read the A64 thermal sensor -> milli-°C, or 0 if unavailable. Best-effort;
 * see the coefficient note at A64_THS0_DATA. */
static uint32_t bmc_read_temp_mc(void)
{
	uint32_t raw = rd32(A64_THS0_DATA) & 0xFFFu;
	if (raw == 0u || raw == 0xFFFu)
		return 0u;                                  /* controller idle -> n/a */
	/* T_milliC = (2170 - raw) * 1000000 / 8560, clamped to non-negative. */
	{
		int32_t t = (int32_t)((int64_t)(2170 - (int32_t)raw) * 1000000 / 8560);
		return t < 0 ? 0u : (uint32_t)t;
	}
}

struct bmc_health *bmc_health_snapshot(struct bmc_health *out)
{
	volatile uint32_t *exc  = (volatile uint32_t *)BMC_EXC_BASE;
	volatile uint32_t *smp  = (volatile uint32_t *)BMC_SMP_BASE;
	volatile uint32_t *uart = (volatile uint32_t *)BMC_UART_BASE;
	volatile uint32_t *ffv  = (volatile uint32_t *)BMC_FFV_BASE;
	/* EXC1's magic (word[0]) is written lazily -- only inside the fault
	 * path (el2_exc.c), never proactively at init -- so on a board that
	 * has taken zero EL2 exceptions since cold boot, this window has
	 * simply never been touched. Gate on the magic exactly like ffv_count
	 * already does below, rather than trusting that untouched DRAM happens
	 * to read as all-ones. Unlike ffv_count/hb_cpuN, a bare 0 is the
	 * correct, unambiguous value here (EL2_KIND_SYNC == 0, so a defaulted
	 * last_exc_kind must never be forwarded as if it were a real SYNC
	 * event) -- callers show last_exc_kind/last_exc_esr only when
	 * exc_count != 0, so both "never written" and "genuinely zero
	 * exceptions" collapse to the same correct display: no last-fault
	 * detail, because there isn't one. */
	int exc_ok = (exc[0] == 0x45584331u);   /* "EXC1" */
	uint64_t up = rd_cntpct();

	out->magic        = BMC_HEALTH_MAGIC;
	out->version      = (BMC_PROTO_MAJOR << 16) | BMC_PROTO_MINOR;
	out->uptime_lo    = (uint32_t)up;
	out->uptime_hi    = (uint32_t)(up >> 32);
	/* tick_lo/tick_hi/tick_delta: RETIRED, see the BMC_GICT_BASE comment
	 * above -- the address this used to read is aliased with backtrace.c's
	 * live BTR1 ring, not a tick counter. Report the tree's standard
	 * "never written" sentinel rather than someone else's memory; the host
	 * side already renders this sentinel as "n/a" (bmc_client.py _avail). */
	out->tick_lo      = 0xffffffffu;
	out->tick_hi      = 0xffffffffu;
	out->tick_delta   = 0xffffffffu;
	out->exc_count    = exc_ok ? exc[1] : 0u;
	out->last_exc_kind= exc_ok ? exc[2] : 0u;
	out->last_exc_esr = exc_ok ? exc[3] : 0u;
	{
		struct el2_frame snap;
		el2_snapshot_guest_frame(&snap);       /* consistent copy (H8) */
		out->guest_pc_lo  = (uint32_t)snap.elr;
		out->guest_pc_hi  = (uint32_t)(snap.elr >> 32);
	}
	out->online_map   = smp[1];
	out->hb_cpu0      = smp[6];
	out->hb_cpu1      = smp[7];
	out->hb_cpu2      = smp[8];
	out->hb_cpu3      = smp[9];
	out->cons_bytes   = uart[1];
	out->cons_faults  = uart[2];
	out->temp_mc      = bmc_read_temp_mc();
	out->flags        = bmc_flag_bitmap();
	out->wdt_hold     = wdt_debug_hold;
	out->ffv_count    = (ffv[0] == 0x46463156u) ? ffv[1] : 0u;

	/* AXP803 battery telemetry (v1.1) — axp803_read_health() is bounded and
	 * self-zeroing if the chip was never confirmed present, so this is safe
	 * to call unconditionally every snapshot. */
	{
		struct axp803_health bh;
		axp803_read_health(&bh);
		out->vbat_mv     = bh.vbat_mv;
		out->ichg_ma     = bh.ichg_ma;
		out->idischg_ma  = bh.idischg_ma;
		out->batt_ts_mv  = bh.ts_mv;
		out->batt_status = bh.status;
		out->axp_ok      = bh.chip_ok;
	}

	/* Latch to the BMC1 breadcrumb, word-for-word (struct order == word order). */
	{
		const uint32_t *w = (const uint32_t *)out;
		unsigned i;
		for (i = 0; i < sizeof(*out) / 4u; i++)
			bmc_bc(i, w[i]);
	}
	return out;
}

/* Print one heartbeat: "idle/never" for the tree's all-ones "no data" sentinel
 * (a core with no heartbeat is not necessarily faulty -- CPU3 parks in WFI by
 * design and never posts one) rather than a 4294967295-looking fake count.
 * Mirrors bmc_client.py's hb_s formatting exactly, so the on-board text and
 * the host-decoded text never disagree about what a given value means. */
static void print_hb(uint32_t v)
{
	if (v == 0xffffffffu) cputs("idle"); else pdec(v);
}

static void bmc_print_health(void)
{
	struct bmc_health h;
	bmc_health_snapshot(&h);

	cputs("BMC health v"); pdec(BMC_PROTO_MAJOR); console_putc('.'); pdec(BMC_PROTO_MINOR); nl();
	cputs("  uptime_cnt=0x"); ph64(((uint64_t)h.uptime_hi << 32) | h.uptime_lo); nl();
	/* tick_lo/tick_hi/tick_delta are always the retired sentinel now (see
	 * bmc_health_snapshot()) -- say so plainly instead of computing a
	 * LIVE/STALLED verdict from a field that carries no real data. */
	cputs("  timer      n/a (tick breadcrumb retired -- see bmc.c BMC_GICT_BASE comment)\r\n");
	cputs("  guest_pc=0x"); ph64(((uint64_t)h.guest_pc_hi << 32) | h.guest_pc_lo); nl();
	/* exc_count == 0 means "no exceptions recorded" whether that is because
	 * none have happened or because the EXC1 breadcrumb was never written
	 * (bmc_health_snapshot() collapses both to 0) -- either way there is no
	 * last-fault detail to show, and last_exc_kind==0 is itself a real
	 * vector index (EL2_KIND_SYNC), so printing it here would look like a
	 * genuine SYNC fault that never occurred. */
	if (h.exc_count == 0u) {
		cputs("  exceptions none recorded\r\n");
	} else {
		cputs("  exc_count="); pdec(h.exc_count);
		cputs(" last_kind=0x");ph32(h.last_exc_kind);
		cputs(" last_esr=0x"); ph32(h.last_exc_esr); nl();
	}
	cputs("  online=0x");  ph32(h.online_map);
	cputs(" hb=[");        print_hb(h.hb_cpu0); console_putc(' '); print_hb(h.hb_cpu1);
	console_putc(' ');     print_hb(h.hb_cpu2); console_putc(' '); print_hb(h.hb_cpu3); cputs("]\r\n");
	cputs("  console_bytes="); pdec(h.cons_bytes);
	cputs(" faults=");         pdec(h.cons_faults);
	cputs(" ffv=");            pdec(h.ffv_count); nl();
	cputs("  temp_mC=");   pdec(h.temp_mc);
	cputs(" flags=0x");    ph32(h.flags);
	cputs(" wdt_hold=");   pdec(h.wdt_hold); nl();
	if (h.axp_ok) {
		/* chip_ok with every reading at zero means the PMIC answered but no
		 * battery telemetry came back (typically: no pack on the
		 * connector). Printing "vbat_mV=0" as a measurement invites a hunt
		 * for a flat battery that was never plugged in -- same distinction
		 * bmc_client.py's print_health() already makes. */
		if (!h.vbat_mv && !h.ichg_ma && !h.idischg_ma && !h.batt_ts_mv) {
			cputs("  battery: AXP803 present, no readings (no pack attached?)\r\n");
		} else {
			cputs("  battery: vbat_mV="); pdec(h.vbat_mv);
			cputs(" ichg_mA=");           pdec(h.ichg_ma);
			cputs(" idischg_mA=");        pdec(h.idischg_ma);
			cputs(" ts_mV=");             pdec(h.batt_ts_mv);
			cputs(" ["); if (h.batt_status & BMC_BATT_PRESENT)  cputs("present ");
			if (h.batt_status & BMC_BATT_CHARGING) cputs("charging ");
			if (h.batt_status & BMC_BATT_VBUS)     cputs("vbus ");
			if (h.batt_status & BMC_BATT_DIE_HOT)  cputs("DIE_HOT ");
			cputs("]\r\n");
		}
	} else {
		cputs("  battery: no AXP803 detected (RSB probe failed or absent)\r\n");
	}
	cputs("  (record latched @0x"); ph32((uint32_t)BMC_HEALTH_BASE); cputs(")\r\n");
}

/* ------------------------------------------------------------------ *
 * BATTERY domain — `bmc battery`: AXP803 telemetry alone (mirrors `bmc temp`).
 * ------------------------------------------------------------------ */
static void bmc_print_battery(void)
{
	struct axp803_health bh;
	axp803_read_health(&bh);

	if (!bh.chip_ok) {
		cputs("no AXP803 detected (RSB probe failed or absent)\r\n");
		return;
	}
	if (!bh.vbat_mv && !bh.ichg_ma && !bh.idischg_ma && !bh.ts_mv) {
		/* Same "present but no readings" case bmc_print_health() guards --
		 * see there for why a bare 0 must not be shown as a measurement. */
		cputs("AXP803 present, no readings (no pack attached?) status=0x");
		ph32(bh.status); cputs("\r\n");
		return;
	}
	cputs("vbat_mV=");    pdec(bh.vbat_mv);
	cputs(" ichg_mA=");   pdec(bh.ichg_ma);
	cputs(" idischg_mA=");pdec(bh.idischg_ma);
	cputs(" ts_mV=");     pdec(bh.ts_mv);
	cputs(" status=0x");  ph32(bh.status);
	cputs(" ["); if (bh.status & BMC_BATT_PRESENT)  cputs("present ");
	if (bh.status & BMC_BATT_CHARGING) cputs("charging ");
	if (bh.status & BMC_BATT_VBUS)     cputs("vbus ");
	if (bh.status & BMC_BATT_DIE_HOT)  cputs("DIE_HOT ");
	cputs("]\r\n");
	cputs("(ts_mV is the raw TS-pin millivolts, NOT a calibrated temperature"
	      " -- see axp803.h)\r\n");
}

/* ------------------------------------------------------------------ *
 * DEBUG-FLAGS domain.
 * ------------------------------------------------------------------ */
struct bmc_flag { const char *name; volatile uint32_t *cell; };

static const struct bmc_flag bmc_flags_tbl[] = {
	{ "usbacm",        &dbg_usbacm },
	{ "core_enable",   &dbg_core_enable },
	{ "cpu1_wdog",     &dbg_cpu1_wdog },
	{ "isolate_noemac",&dbg_isolate_no_emac },
	{ "no_guest",      &dbg_no_guest },
	{ "block_reset",   &dbg_block_reset },
};
#define BMC_NFLAGS (int)(sizeof(bmc_flags_tbl)/sizeof(bmc_flags_tbl[0]))

static void bmc_flags_list(void)
{
	int i;
	cputs("debug flags:\r\n");
	for (i = 0; i < BMC_NFLAGS; i++) {
		cputs("  "); cputs(bmc_flags_tbl[i].name);
		cputs(" = "); pdec(*bmc_flags_tbl[i].cell); nl();
	}
}

/* `bmc flag <name> <0|1>` — DESTRUCTIVE (can sever the debug channel), gated. */
static void bmc_flag_set(const char *name, unsigned long val)
{
	int i;
	if (!bmc_check_armed())
		return;
	for (i = 0; i < BMC_NFLAGS; i++) {
		if (streq(name, bmc_flags_tbl[i].name)) {
			*bmc_flags_tbl[i].cell = (uint32_t)val;
			cputs("flag "); cputs(name); cputs(" <- "); pdec((uint32_t)val); nl();
			return;
		}
	}
	cputs("unknown flag; try 'bmc flags'\r\n");
}

/* ------------------------------------------------------------------ *
 * CONSOLE domain — the guest console over the vconsole rings.
 * ------------------------------------------------------------------ */

/* `bmc con read [n]` — dump the LAST n bytes of the guest console CAPTURE ring
 * as raw ASCII (post-mortem log). Bounded; defaults to a screenful.
 *
 * THE TAIL, NOT THE HEAD, and this is the whole point of the command.
 * The previous version started at offset 0 and walked forward, which is wrong
 * twice over: once the ring has wrapped, offset 0 holds the OLDEST surviving
 * bytes rather than the start of the log, and even before a wrap a default
 * screenful showed the first n bytes instead of the most recent ones. Either
 * way an operator asking "what did the guest say just before it died?" got the
 * early boot banner. Combined with the wrong buffer address (see
 * BMC_UART_RINGBUF above) that made this command actively misleading during the
 * 2026-08-10 kldload investigation: the guest's panic backtrace was sitting in
 * the ring and four passes in a row concluded "silent hang" instead.
 *
 * total_bytes is a running count that is NOT clamped to the buffer size, so it
 * doubles as the write cursor: the newest byte is at (total-1) mod cap. Reading
 * n bytes backwards from there covers both the wrapped and not-yet-wrapped
 * cases with the same arithmetic, because when total <= cap the subtraction
 * lands exactly at total-n. */
static void bmc_con_read(uint32_t n)
{
	uint32_t total = rd32(BMC_UART_BASE + 4u);   /* UART word[1] = total_bytes */
	uint32_t cap = (uint32_t)BMC_UART_RINGCAP;
	uint32_t avail = (total < cap) ? total : cap;
	uint32_t start, i;

	if (n == 0 || n > avail) n = avail;
	if (n > 0x1000u) n = 0x1000u;                /* keep one service() bounded */

	/* Offset of the first byte to print, walking n back from the cursor. */
	start = (total - n) % cap;

	cputs("--- guest console (last "); pdec(n); cputs(" of ");
	pdec(avail); cputs(" held, "); pdec(total); cputs(" total) ---\r\n");
	for (i = 0; i < n; i++) {
		uint32_t off = (start + i) % cap;
		uint8_t b = *(volatile uint8_t *)(BMC_UART_RINGBUF + off);
		console_putc((b == '\n' || (b >= 0x20 && b < 0x7f)) ? (int)b : '.');
	}
	cputs("\r\n--- end ---\r\n");
}

/* `bmc con pm [n]` / `bmc con pmat <off> [n]` — dump the POSTMORTEM CARRY-OVER:
 * the console text of the run BEFORE this one, copied aside by vconsole_init()
 * before it reset the live ring. See hv_addrmap.h's VCPM lane comment for why
 * this exists; the short version is that after a crash that takes the whole
 * board down, reloading the hypervisor is the only way to get this channel back
 * and it is also what destroys the evidence — so the evidence is now saved
 * first, and this is how you read it.
 *
 * `pm` shows the TAIL (the last n bytes, i.e. what was printed just before the
 * end), which is the answer to the question anyone actually has. `pmat` takes
 * an explicit offset so the whole carry-over can be paged from the host without
 * this command having to stream 64 KiB inside one bounded service() call.
 *
 * NO WRAP ARITHMETIC HERE, deliberately: the lane is linearised at capture time
 * (offset 0 == oldest surviving byte, running forward to `len`). bmc_con_read()
 * above has to walk a live ring backwards from a cursor and the account of what
 * that cost when it was done wrong is in this file's own comments — a
 * postmortem reader is the last caller that should be re-deriving it. */
static void bmc_con_pm(uint32_t n, uint32_t off, int have_off)
{
	uint32_t len, prev_total, gen, faults, i;

	if (rd32(HVMAP_VCPM_HDR) != (uint32_t)HVMAP_VCPM_MAGIC) {
		cputs("con pm: no carry-over held. Either this is a cold boot "
		      "(nothing ran before us), or the run before this one was "
		      "built without the postmortem lane.\r\n");
		return;
	}

	len        = rd32(HVMAP_VCPM_HDR + 0x04u);
	prev_total = rd32(HVMAP_VCPM_HDR + 0x08u);
	gen        = rd32(HVMAP_VCPM_HDR + 0x0cu);
	faults     = rd32(HVMAP_VCPM_HDR + 0x18u);

	if (len > (uint32_t)HVMAP_VCPM_BUF_SIZE)
		len = (uint32_t)HVMAP_VCPM_BUF_SIZE;   /* never trust a stale word */

	if (n == 0u || n > 0x1000u)
		n = 0x1000u;                           /* keep one service() bounded */

	if (!have_off)
		off = (len > n) ? len - n : 0u;         /* default: the tail        */
	if (off >= len) {
		cputs("con pm: offset past the end (len="); pdec(len); cputs(")\r\n");
		return;
	}
	if (n > len - off)
		n = len - off;

	cputs("--- prev-run console, gen "); pdec(gen);
	cputs(", bytes ["); pdec(off); cputs(","); pdec(off + n);
	cputs(") of "); pdec(len); cputs(" held");
	/* How much the previous run printed that this copy does NOT contain. Worth
	 * stating outright: a reader looking for something and not finding it needs
	 * to know whether it was never printed or scrolled out of the ring. */
	if (prev_total > len) {
		cputs(" ("); pdec(prev_total - len);
		cputs(" earlier bytes lost to the ring wrap)");
	}
	cputs(", "); pdec(faults); cputs(" faults ---\r\n");

	for (i = 0; i < n; i++) {
		uint8_t b = *(volatile uint8_t *)(HVMAP_VCPM_BUF + off + i);
		console_putc((b == '\n' || (b >= 0x20 && b < 0x7f)) ? (int)b : '.');
	}
	cputs("\r\n--- end ---\r\n");
}

/* `bmc con inject <text...>` — push host keystrokes into the guest's virtual
 * UART0 RX ring (mountroot>, login, shell). argv points at the remaining
 * tokens; we re-join them with single spaces and append a newline, which is
 * what an interactive line needs. */
static void bmc_con_inject(char **argv, int argc)
{
	int t, k;
	for (t = 0; t < argc; t++) {
		const char *s = argv[t];
		if (t) vconsole_rx_push((uint8_t)' ');
		for (k = 0; s[k]; k++)
			vconsole_rx_push((uint8_t)s[k]);
	}
	vconsole_rx_push((uint8_t)'\r');   /* terminate the line for ns8250 RX poll */
	cputs("injected "); pdec((uint32_t)argc); cputs(" token(s) + CR\r\n");
}

/* `bmc con tee [n]` — drain up to n bytes currently pending in the guest->host
 * TX tee ring and echo them (a one-shot pull of live console; the streaming
 * loop lives host-side in bmc_client.py, which just calls this repeatedly). */
static void bmc_con_tee(uint32_t n)
{
	uint8_t b; uint32_t got = 0;
	if (n == 0) n = 256u;
	while (got < n && vconsole_tx_tee_getc(&b)) {
		console_putc((b == '\n' || (b >= 0x20 && b < 0x7f)) ? (int)b : '.');
		got++;
	}
	if (got == 0) cputs("(tee empty)\r\n"); else nl();
}

/* ------------------------------------------------------------------ *
 * POWER / RESET / WATCHDOG domain — all DESTRUCTIVE, all gated.
 * ------------------------------------------------------------------ */

/* `bmc reset` — clean reboot to U-Boot via reboot.c (drops the USB gadget
 * pull-up first, then arms the WDOG). NEVER RETURNS on success; the host
 * simply loses the link, which chimpd.py already treats as "board resetting". */
static void bmc_reset(void)
{
	if (!bmc_check_armed())
		return;
	cputs("resetting (reboot_clean)...\r\n");
	console_flush();
	reboot_clean();     /* noreturn */
}

/* `bmc wdt <hold|release|arm|disarm>`:
 *   hold    -> wdt_debug_hold=1 : CPU1 stops petting => HW WDOG fires ≤16 s
 *              (the "let it reset" path; gated).
 *   release -> wdt_debug_hold=0 : resume petting, board stays resident.
 *   arm     -> wdt.c wdt_arm()  : (re)start the dead-man window (gated).
 *   disarm  -> wdt.c wdt_disarm(): clear enable (keeps a wedge inspectable). */
static void bmc_wdt(const char *sub)
{
	if (streq(sub, "release")) { wdt_debug_hold = 0u; cputs("wdt: petting RESUMED\r\n"); return; }
	if (streq(sub, "disarm"))  { wdt_disarm();        cputs("wdt: DISARMED\r\n");        return; }
	/* hold/arm are destructive (lead to / permit a reset) — gate them. */
	if (streq(sub, "hold")) {
		if (!bmc_check_armed()) return;
		wdt_debug_hold = 1u; cputs("wdt: HELD (HW WDOG will fire <=16s)\r\n"); return;
	}
	if (streq(sub, "arm")) {
		if (!bmc_check_armed()) return;
		wdt_arm(); cputs("wdt: ARMED (dead-man window running)\r\n"); return;
	}
	cputs("usage: bmc wdt <hold|release|arm|disarm>\r\n");
}

/* ------------------------------------------------------------------ *
 * Verb table + top-level dispatch.
 * ------------------------------------------------------------------ */
static void bmc_help(void)
{
	cputs("bzdOS software-BMC verbs (v"); pdec(BMC_PROTO_MAJOR);
	console_putc('.'); pdec(BMC_PROTO_MINOR); cputs("):\r\n");
	cputs("  bmc ver                 protocol version\r\n");
	cputs("  bmc health              structured status record (+latch @0x50006000)\r\n");
	cputs("  bmc temp                SoC temperature (milli-C)\r\n");
	cputs("  bmc battery             AXP803 VBAT/IBAT/charge status (RSB)\r\n");
	cputs("  bmc flags               list debug flags + values\r\n");
	cputs("  bmc flag <name> <0|1>   set a debug flag            [ARMED]\r\n");
	cputs("  bmc con read [n]        dump guest console capture ring (this run)\r\n");
	cputs("  bmc con pm [n]          dump PREV run's console tail (survives reload)\r\n");
	cputs("  bmc con pmat <off> [n]  ...same, at an explicit offset, for paging\r\n");
	cputs("  bmc con inject <text>   type into guest console (RX inject)\r\n");
	cputs("  bmc con tee [n]         drain pending guest->host TX bytes\r\n");
	cputs("  bmc reset               clean reboot to U-Boot       [ARMED]\r\n");
	cputs("  bmc wdt <hold|release|arm|disarm>   watchdog control  [hold/arm ARMED]\r\n");
	cputs("  bmc arm <nonce>         enable destructive verbs for a short window\r\n");
	cputs("  [ARMED] verbs require a preceding 'bmc arm <nonce>'.\r\n");
	cputs("  Memory/register/breakpoint verbs live in dbgmon (gr/sr/r/w/bp/ff/...).\r\n");
}

void bmc_dispatch(char **argv, int argc, struct el2_frame *frame)
{
	const char *verb;
	(void)frame;   /* reserved: future verbs may want the live frame (e.g. gr echo) */

	if (argc < 1) { bmc_help(); return; }
	verb = argv[0];

	if (streq(verb, "ver")) {
		cputs("bmc proto "); pdec(BMC_PROTO_MAJOR); console_putc('.');
		pdec(BMC_PROTO_MINOR); nl();
		return;
	}
	if (streq(verb, "health")) { bmc_print_health(); return; }
	if (streq(verb, "temp"))   { cputs("temp_mC="); pdec(bmc_read_temp_mc()); nl(); return; }
	if (streq(verb, "battery")) { bmc_print_battery(); return; }
	if (streq(verb, "flags"))  { bmc_flags_list(); return; }
	if (streq(verb, "flag")) {
		unsigned long v;
		if (argc < 3 || !parse_hex(argv[2], &v)) { cputs("usage: bmc flag <name> <0|1>\r\n"); return; }
		bmc_flag_set(argv[1], v);
		return;
	}
	if (streq(verb, "con")) {
		if (argc < 2) { cputs("usage: bmc con <read|pm|pmat|inject|tee> ...\r\n"); return; }
		if (streq(argv[1], "read")) {
			unsigned long n = 0; if (argc >= 3) parse_hex(argv[2], &n);
			bmc_con_read((uint32_t)n); return;
		}
		if (streq(argv[1], "pm")) {
			unsigned long n = 0; if (argc >= 3) parse_hex(argv[2], &n);
			bmc_con_pm((uint32_t)n, 0u, 0); return;
		}
		if (streq(argv[1], "pmat")) {
			unsigned long o = 0, n = 0;
			if (argc < 3 || !parse_hex(argv[2], &o)) {
				cputs("usage: bmc con pmat <off> [n]\r\n"); return;
			}
			if (argc >= 4) parse_hex(argv[3], &n);
			bmc_con_pm((uint32_t)n, (uint32_t)o, 1); return;
		}
		if (streq(argv[1], "inject")) { bmc_con_inject(&argv[2], argc - 2); return; }
		if (streq(argv[1], "tee")) {
			unsigned long n = 0; if (argc >= 3) parse_hex(argv[2], &n);
			bmc_con_tee((uint32_t)n); return;
		}
		cputs("usage: bmc con <read|pm|pmat|inject|tee> ...\r\n");
		return;
	}
	if (streq(verb, "reset")) { bmc_reset(); return; }
	if (streq(verb, "wdt")) {
		if (argc < 2) { cputs("usage: bmc wdt <hold|release|arm|disarm>\r\n"); return; }
		bmc_wdt(argv[1]);
		return;
	}
	if (streq(verb, "arm")) {
		unsigned long n = 0; parse_hex(argc >= 2 ? argv[1] : "1", &n);
		bmc_arm(n);
		return;
	}
	if (streq(verb, "h") || streq(verb, "?") || streq(verb, "help")) { bmc_help(); return; }

	cputs("unknown bmc verb; 'bmc help'\r\n");
}

/* ------------------------------------------------------------------ *
 * Init.
 * ------------------------------------------------------------------ */
void bmc_init(void)
{
	struct bmc_health h;
	bmc_arm_nonce = 0u;                 /* start disarmed */
	axp803_init();                     /* probe the AXP803 over RSB (bounded; */
	                                    /* leaves chip_ok=0 on any failure)    */
	bmc_health_snapshot(&h);           /* lay down BMC1 magic + first record */
	cputs("bmc: software-BMC mgmt plane ready ('bmc help')\r\n");
}
