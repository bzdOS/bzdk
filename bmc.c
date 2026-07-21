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

/* wdt.c — dead-man watchdog controls + the debug-core reset gate. */
extern void wdt_arm(void);
extern void wdt_disarm(void);
extern volatile uint32_t wdt_debug_hold;   /* set !=0 => CPU1 stops petting => HW WDOG fires */

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

/* ------------------------------------------------------------------ *
 * Breadcrumb windows we READ (documented owners in parentheses). Fixed
 * addresses per the tree convention; we do not include their headers, we just
 * know the layout — same discipline dbgmon.c's cmd_t()/cmd_ff() use.
 * ------------------------------------------------------------------ */
#define BMC_GICT_BASE   0x50000800UL   /* gic_timer.c "GICT": [1/2]=ticks lo/hi */
#define BMC_EXC_BASE    0x50000400UL   /* el2_exc.c  "EXC1": [1]=count [2]=kind [3]=esr */
#define BMC_SMP_BASE    0x50000900UL   /* smp.c      "SMP1": [1]=online [6..9]=heartbeats */
#define BMC_UART_BASE   0x50000f00UL   /* vconsole.h "UART": [1]=total_bytes [2]=faults */
#define BMC_FFV_BASE    0x50005800UL   /* firstfault.c "FF1V": [1]=count            */
#define BMC_UART_RINGBUF 0x50000f10UL  /* vconsole.h captured-console byte buffer   */

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
	uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v)); return v;
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

/* Previous tick counter, to compute the liveness delta between snapshots. */
static uint32_t bmc_prev_tick_lo;

struct bmc_health *bmc_health_snapshot(struct bmc_health *out)
{
	volatile uint32_t *gict = (volatile uint32_t *)BMC_GICT_BASE;
	volatile uint32_t *exc  = (volatile uint32_t *)BMC_EXC_BASE;
	volatile uint32_t *smp  = (volatile uint32_t *)BMC_SMP_BASE;
	volatile uint32_t *uart = (volatile uint32_t *)BMC_UART_BASE;
	volatile uint32_t *ffv  = (volatile uint32_t *)BMC_FFV_BASE;
	uint64_t up = rd_cntpct();

	out->magic        = BMC_HEALTH_MAGIC;
	out->version      = (BMC_PROTO_MAJOR << 16) | BMC_PROTO_MINOR;
	out->uptime_lo    = (uint32_t)up;
	out->uptime_hi    = (uint32_t)(up >> 32);
	out->tick_lo      = gict[1];
	out->tick_hi      = gict[2];
	out->tick_delta   = gict[1] - bmc_prev_tick_lo;
	bmc_prev_tick_lo  = gict[1];
	out->exc_count    = exc[1];
	out->last_exc_kind= exc[2];
	out->last_exc_esr = exc[3];
	out->guest_pc_lo  = (uint32_t)g_last_guest_frame.elr;
	out->guest_pc_hi  = (uint32_t)(g_last_guest_frame.elr >> 32);
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

	/* Latch to the BMC1 breadcrumb, word-for-word (struct order == word order). */
	{
		const uint32_t *w = (const uint32_t *)out;
		unsigned i;
		for (i = 0; i < sizeof(*out) / 4u; i++)
			bmc_bc(i, w[i]);
	}
	return out;
}

static void bmc_print_health(void)
{
	struct bmc_health h;
	bmc_health_snapshot(&h);

	cputs("BMC health v"); pdec(BMC_PROTO_MAJOR); console_putc('.'); pdec(BMC_PROTO_MINOR); nl();
	cputs("  uptime_cnt=0x"); ph64(((uint64_t)h.uptime_hi << 32) | h.uptime_lo); nl();
	cputs("  ticks=0x");   ph64(((uint64_t)h.tick_hi << 32) | h.tick_lo);
	cputs(" delta=");      pdec(h.tick_delta);
	cputs(h.tick_delta ? "  (timer LIVE)\r\n" : "  (timer STALLED)\r\n");
	cputs("  guest_pc=0x"); ph64(((uint64_t)h.guest_pc_hi << 32) | h.guest_pc_lo); nl();
	cputs("  exc_count="); pdec(h.exc_count);
	cputs(" last_kind=0x");ph32(h.last_exc_kind);
	cputs(" last_esr=0x"); ph32(h.last_exc_esr); nl();
	cputs("  online=0x");  ph32(h.online_map);
	cputs(" hb=[");        pdec(h.hb_cpu0); console_putc(' '); pdec(h.hb_cpu1);
	console_putc(' ');     pdec(h.hb_cpu2); console_putc(' '); pdec(h.hb_cpu3); cputs("]\r\n");
	cputs("  console_bytes="); pdec(h.cons_bytes);
	cputs(" faults=");         pdec(h.cons_faults);
	cputs(" ffv=");            pdec(h.ffv_count); nl();
	cputs("  temp_mC=");   pdec(h.temp_mc);
	cputs(" flags=0x");    ph32(h.flags);
	cputs(" wdt_hold=");   pdec(h.wdt_hold); nl();
	cputs("  (record latched @0x"); ph32((uint32_t)BMC_HEALTH_BASE); cputs(")\r\n");
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

/* `bmc con read [n]` — dump up to n bytes of the guest console CAPTURE ring as
 * raw ASCII (post-mortem log). Bounded; defaults to a screenful. */
static void bmc_con_read(uint32_t n)
{
	uint32_t total = rd32(BMC_UART_BASE + 4u);   /* UART word[1] = total_bytes */
	uint32_t i;
	if (n == 0 || n > total) n = total;
	if (n > 0x1000u) n = 0x1000u;                /* keep one service() bounded */
	cputs("--- guest console ("); pdec(n); cputs("/"); pdec(total); cputs(" bytes) ---\r\n");
	for (i = 0; i < n; i++) {
		uint8_t b = *(volatile uint8_t *)(BMC_UART_RINGBUF + i);
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
	cputs("  bmc flags               list debug flags + values\r\n");
	cputs("  bmc flag <name> <0|1>   set a debug flag            [ARMED]\r\n");
	cputs("  bmc con read [n]        dump guest console capture ring\r\n");
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
	if (streq(verb, "flags"))  { bmc_flags_list(); return; }
	if (streq(verb, "flag")) {
		unsigned long v;
		if (argc < 3 || !parse_hex(argv[2], &v)) { cputs("usage: bmc flag <name> <0|1>\r\n"); return; }
		bmc_flag_set(argv[1], v);
		return;
	}
	if (streq(verb, "con")) {
		if (argc < 2) { cputs("usage: bmc con <read|inject|tee> ...\r\n"); return; }
		if (streq(argv[1], "read")) {
			unsigned long n = 0; if (argc >= 3) parse_hex(argv[2], &n);
			bmc_con_read((uint32_t)n); return;
		}
		if (streq(argv[1], "inject")) { bmc_con_inject(&argv[2], argc - 2); return; }
		if (streq(argv[1], "tee")) {
			unsigned long n = 0; if (argc >= 3) parse_hex(argv[2], &n);
			bmc_con_tee((uint32_t)n); return;
		}
		cputs("usage: bmc con <read|inject|tee> ...\r\n");
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
	bmc_prev_tick_lo = rd32(BMC_GICT_BASE + 4u);
	bmc_health_snapshot(&h);           /* lay down BMC1 magic + first record */
	cputs("bmc: software-BMC mgmt plane ready ('bmc help')\r\n");
}
