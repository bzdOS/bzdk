/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgmon.c — LIVE hypervisor debug monitor for the bzdOS EL2 hypervisor.
 *
 * See dbgmon.h for the big picture. In one line: dbgmon_service() is called
 * from the EL2 tick handler with the guest's just-saved `struct el2_frame`,
 * services a tiny non-blocking line-oriented console (same console_* hooks
 * repl.c uses), executes at most one fully-received command per call, and
 * returns so the guest resumes. Nothing here ever blocks or spins waiting
 * for more input -- it is running in interrupt/tick context with the guest
 * (and everything else) preempted.
 *
 * Freestanding: no libc. Only <stdint.h> + "exceptions.h" (for struct
 * el2_frame). All printing/parsing helpers below are hand-rolled and do NOT
 * call into repl.c -- this file is fully self-contained, per the assignment
 * (dbgmon.c/.h are the only two files this module owns).
 */
#include <stdint.h>   /* HVMAP_SD_BC, HVMAP_SD_TESTBUF */
#include "exceptions.h"
#include "dbgmon.h"
#include "vcpu2.h"
#include "vcpu1.h"
#include "vcpu3.h"
#include "vinput.h"
#include "hv_addrmap.h"
#include "hv_addrmap.h"

/* ── weak fallbacks for optional providers ──────────────────────────────────
 * dbgmon.c is linked into targets that do NOT link fbdump.o, sd_bio.o or the
 * fault-injection half of emmc_bio.o -- `zephyr` is one, and it had not linked
 * for some time as a result (five undefined references at final link, all from
 * commands in this file). Rather than delete the commands or fork the file,
 * provide weak definitions: a target that links the real object overrides
 * these, and one that does not gets a command that reports "unavailable in
 * this build" instead of a build failure. Same idiom as el2_exc.c's weak
 * gic_timer_irq()/dbgmon_service().
 *
 * Verified: `make dbg` still resolves all five to the real implementations
 * (they are in DBG_OBJS), so no behaviour changes in the build people use. */
__attribute__((weak)) int fbdump_send(uint32_t pa, uint32_t len)
{
	(void)pa; (void)len;
	return -1;
}

__attribute__((weak)) int sd_bio_init(void)
{
	return -1;
}

__attribute__((weak)) int sd_bio_read(uint32_t lba, uint64_t buf_pa)
{
	(void)lba; (void)buf_pa;
	return -1;
}

__attribute__((weak)) int sd_bio_write(uint32_t lba, uint64_t buf_pa)
{
	(void)lba; (void)buf_pa;
	return -1;
}

__attribute__((weak)) void emmc_bio_fault_inject(uint32_t every, uint32_t min_lba,
                                                 uint32_t point,
                                                 uint32_t legacy_wait)
{
	(void)every; (void)min_lba; (void)point; (void)legacy_wait;
}



/* ------------------------------------------------------------------ *
 * Console hooks -- same extern contract repl.c uses. main_dbg.c (owned by
 * the architect) wires these to the EMAC network console. NON-BLOCKING:
 * console_getc() returns -1 immediately if no byte is available.
 * ------------------------------------------------------------------ */
extern int  console_getc(void);
extern void console_putc(int c);
extern void console_capture_start(char *buf, int cap);
extern int  console_capture_end(char **out);
extern void console_poll(void);
extern void console_flush(void);

/* emac.c's cold-boot PHY-lottery test lever (emac.h — externs, not an
 * include, because dbgmon.c stays deliberately self-contained and some
 * targets that link it do not link emac.o; those targets never execute the
 * wdtest command, so the reference only needs to RESOLVE at link time in
 * the builds that can reach it — same pattern as console_flush above). */
extern void emac_wd_test_mode(int on);

/* Guest single-step toggle, implemented in el2_exc.c (linked into the same
 * debugger build). Returns the new enabled state (1=on, 0=off). Passing the
 * live guest frame lets it arm PSTATE.SS on the eret that resumes the guest. */
extern int el2_ss_toggle(struct el2_frame *frame);

/* CPU1's OWN guest frame (el2_exc.c), for the gr1/sr1 commands below --
 * vcpu1.h's named follow-up now that vcpu1.c's third vCPU has a hardware
 * track record. Deliberately NOT the same storage `frame` (this function's
 * own parameter) points at: `frame`/g_last_guest_frame is, and stays,
 * CPU0's FreeBSD guest (see vcpu1.h: "gr/sr STILL MEAN CPU0's GUEST"),
 * even on a call that originated from CPU1's own tick. This extern pair is
 * the separate slot el2_exc.c publishes CPU1's own frame into.
 * el2_snapshot_guest_frame_cpu1() does the same seqlock-retry copy
 * el2_snapshot_guest_frame() does for the CPU0 slot; g_cpu1_guest_frame_seq
 * doubles as the "has CPU1 ever captured a frame at all" flag -- see
 * cmd_gr1()/cmd_sr1() for why that distinction has to be explicit instead of
 * just printing whatever is in the (possibly never-written) struct. */
extern volatile uint32_t g_cpu1_guest_frame_seq;
extern void el2_snapshot_guest_frame_cpu1(struct el2_frame *out);

/* Hardware breakpoints/watchpoints (hwbp.c) + backtrace (backtrace.c), both
 * linked into the debugger build. dbgmon stays self-contained: extern decls
 * only, no hwbp.h/backtrace.h include. */
extern int  hwbp_set(int idx, uint64_t va, int is_write_wp);
extern int  hwbp_clear(int idx, int is_write_wp);
extern void hwbp_clear_all(void);

/* sd_bio.c — SD-card block I/O. Linked into dbg/gdb and, until this command
 * existed, called from NOWHERE: its own header says "dbgmon `call` on CPU1 is
 * this file's only caller today", i.e. the intended way to exercise it was to
 * resolve the symbol with nm and `call` the address. That is exactly the
 * practice dbgtools.h exists to warn about -- an nm-resolved address against a
 * stale on-disk ELF silently jumps into garbage. Hence a real command. */
extern int sd_bio_init(void);
extern int sd_bio_read(uint32_t lba, uint64_t buf_pa);
extern int sd_bio_write(uint32_t lba, uint64_t buf_pa);

/* eMMC write fault injection (emmc_bio.c). extern-decl only, per this file's
 * self-contained convention. See emmc_bio.h for the full rationale: it makes
 * write failures happen on demand so that ebio_fail_settle()'s card-idle wait --
 * the safeguard that stops a retried CMD24 landing on a still-programming card
 * -- can actually be executed and observed. Since the corruption fix there are
 * no real failures left to enter that path. */
extern void emmc_bio_fault_inject(uint32_t every, uint32_t min_lba,
                                  uint32_t point, uint32_t legacy_wait);

/* Guest console RX injection (vconsole.c): push one host byte into the
 * guest's virtual UART0 RX ring. Used by the `poweroff` command to type a
 * clean-shutdown line into the guest. extern-decl only, per this file's
 * self-contained convention (no vconsole.h include). */
extern void vconsole_rx_push(uint8_t c);
extern int  hwbp_list_bp(uint64_t *va, int *en);
extern int  hwbp_list_wp(uint64_t *va, int *en);
extern int  backtrace_walk(uint64_t pc, uint64_t fp, uint64_t lr,
			   uint64_t *out, int max);

/* bmc.c — software-BMC management-plane verbs. Extern decl only, per this
 * file's self-contained discipline (no bmc.h include). */
extern void bmc_dispatch(char **argv, int argc, struct el2_frame *frame);

/* dbgtools.c — pause-before-entry gate setters (2026-07-26). Extern decl
 * only, per this file's self-contained discipline (no dbgtools.h include).
 * Durable (dc civac + dsb) writes -- see dbgtools.h for why the generic `w`
 * command (cmd_write_word() below, a plain store) isn't used for these two
 * specifically: they must survive a WARM RESET that may follow shortly
 * after. */
extern void dbgtools_hold_set(void);
extern void dbgtools_release_set(void);

/* fbdump.c — raw-Ethernet framebuffer streamer (ethertype 0x88B9). Extern
 * decl only, per this file's self-contained discipline (no fbdump.h
 * include). See fbdump.h for the framing and why this exists instead of `d`
 * or netcon. */
extern int fbdump_send(uint32_t pa, uint32_t len);

/* ------------------------------------------------------------------ *
 * Tiny freestanding I/O + string helpers (mirrors repl.c's style, but is
 * an independent copy -- dbgmon.c does not include or call repl.c).
 * ------------------------------------------------------------------ */

/* BZDBG capture sink. The sink itself lives in main_dbg.c's console_putc
 * (console_capture_start/end): capturing at THAT level grabs output from
 * other modules too (bmc_dispatch's `help` tree), not just this file's
 * helpers -- the 2026-08-27 first-cut limitation. dc_putc remains as the
 * call-site wrapper so all 49 console_putc call sites here route through
 * one place; with the sink armed the console layer captures, and with it
 * disarmed console_putc lands on the EMAC TX path as always. */
static char  g_capture_buf[4096];
static int   g_capture_on;         /* 0 = off */

static void dc_putc(int c)
{
	console_putc(c);   /* capture, when armed, happens inside console_putc */
}

static void cputs(const char *s)
{
	while (*s)
		dc_putc((int)(unsigned char)*s++);
}

static void newline(void)
{
	dc_putc('\r');
	dc_putc('\n');
}

static char hexdig(unsigned v)
{
	v &= 0xfu;
	return (char)(v < 10 ? ('0' + v) : ('a' + (v - 10)));
}

static void print_hex8(uint8_t v)
{
	dc_putc(hexdig(v >> 4));
	dc_putc(hexdig(v));
}

static void print_hex32(uint32_t v)
{
	print_hex8((uint8_t)(v >> 24));
	print_hex8((uint8_t)(v >> 16));
	print_hex8((uint8_t)(v >> 8));
	print_hex8((uint8_t)v);
}

/* Full 64-bit hex, always all 16 digits -- used for register dumps where a
 * fixed width makes the columns line up. */
static void print_hex64(uint64_t v)
{
	print_hex32((uint32_t)(v >> 32));
	print_hex32((uint32_t)v);
}

/* Address/long print: 32-bit-wide if it fits, else full 64-bit -- used for
 * memory addresses (matches repl.c's `r`/`rb`/`d` look). */
static void print_addr(unsigned long v)
{
	if ((uint64_t)v >> 32)
		print_hex32((uint32_t)((uint64_t)v >> 32));
	print_hex32((uint32_t)v);
}

static int streq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

/* Parse a hex string (optional 0x/0X prefix) into *out. Returns 1 on
 * success, 0 if the token is empty or contains a non-hex character --
 * never crashes on garbage input. */
static int parse_hex(const char *s, unsigned long *out)
{
	unsigned long v = 0;
	int any = 0;

	if (!s)
		return 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (; *s; s++) {
		char c = *s;
		unsigned d;

		if (c >= '0' && c <= '9')
			d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (unsigned)(c - 'A' + 10);
		else
			return 0;
		v = (v << 4) | d;
		any = 1;
	}
	if (!any)
		return 0;
	*out = v;
	return 1;
}

static void err(const char *msg)
{
	cputs("error: ");
	cputs(msg);
	newline();
}

/* Split line in place into up to MAX_TOKENS NUL-terminated tokens on runs
 * of spaces/tabs. Returns the token count. Bounded (no allocation, no
 * recursion) -- safe to call from interrupt context. */
/* 8 (was 4): the bare minimum for "bmc" to dispatch stayed at 4, but that
 * only leaves room for a 2-word tail after "bmc <verb>" -- too little for
 * `bmc con inject <text...>` (bmc.c's bmc_con_inject re-joins whatever
 * argv it is handed). Bumped per docs/bmc-integration.md's explicit note.
 * Every other command still just ignores the extra slots it doesn't need. */
#define MAX_TOKENS 8

static int tokenize(char *line, char *tok[MAX_TOKENS])
{
	int n = 0;
	char *p = line;

	while (*p && n < MAX_TOKENS) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		tok[n++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (*p)
			*p++ = '\0';
	}
	return n;
}

/* ------------------------------------------------------------------ *
 * System-register readers. All of these are the guest's (EL1's) banked
 * architectural state; reading them with `mrs` FROM EL2 (us) reads the real
 * hardware register directly -- no trap, no guest involvement, and it
 * reflects the guest's live state as of this tick. (Guest MSR/MRS to the
 * TVM-covered subset of these DOES trap to EL2 elsewhere in this hypervisor
 * -- see gtrace.c -- but that is orthogonal to us reading them here.)
 * ------------------------------------------------------------------ */

#define RDSYSREG(name) ({ uint64_t _v; __asm__ volatile("mrs %0, " #name : "=r"(_v)); _v; })

/* ------------------------------------------------------------------ *
 * Command implementations.
 * ------------------------------------------------------------------ */

#define WORDS_PER_LINE 4
#define BYTES_PER_LINE 16

/* gr: dump the guest's GPRs + ELR (guest PC) + SPSR straight from the
 * el2_frame the tick handler just saved. This is the headline command --
 * it shows exactly where the EL1 guest is executing and its full register
 * state, live, without stopping it. */
/* Defined below; cmd_sd() hexdumps the sector it just read with it. */
static void cmd_read_bytes(unsigned long addr, uint32_t n);

/* ------------------------------------------------------------------ *
 * `sd` — exercise the SD-card driver (sd_bio.c).
 *
 * WHY THIS EXISTS: sd_bio.c is a careful, register-for-register port of the
 * hardware-verified emmc_bio.c (same SMHC IP), but it has never executed. It is
 * linked into dbg/gdb and called from nowhere. When a microSD card arrives, this
 * is what says whether the driver works -- and if it does not, WHERE it stopped,
 * because `sd init` decodes sd_bio's own breadcrumb window rather than just
 * reporting an errno.
 *
 * The breadcrumb decode is the point. sd_bio_init() stamps slot 0 with a
 * distinct negative value at each of its seven failure points, and the
 * interesting values (CMD8 response, ACMD41 OCR, block-addressing, the card's
 * RCA) into slots 2..6. A bare "rc=-5" says nothing; "ACMD41 never left busy"
 * says what to look at.
 * ------------------------------------------------------------------ */
static void sd_show_bc(void)
{
	const volatile uint32_t *bc = (const volatile uint32_t *)HVMAP_SD_BC;
	uint32_t st = bc[0];

	cputs("  sdbc[0] fail-stage = "); print_hex32(st);
	switch (st) {
	case 0u:            cputs("  (no failure recorded)");                break;
	case 0xffffffffu:   cputs("  (-1 clk_update before 400kHz)");        break;
	case 0xfffffffeu:   cputs("  (-2 clk_update after 400kHz)");         break;
	case 0xfffffffdu:   cputs("  (-3 CMD0 GO_IDLE / SEND_INIT)");        break;
	case 0xfffffffcu:   cputs("  (-4 ACMD41 command error)");            break;
	case 0xfffffffbu:   cputs("  (-5 ACMD41 never left busy: no card?)"); break;
	case 0xfffffffau:   cputs("  (-6 CMD2 ALL_SEND_CID)");               break;
	case 0xfffffff9u:   cputs("  (-7 CMD3 SEND_RELATIVE_ADDR)");         break;
	default:            cputs("  (unrecognised)");                       break;
	}
	cputs("\r\n  sdbc[2] CMD8 resp  = "); print_hex32(bc[2]);
	cputs("\r\n  sdbc[3] CMD8 ok    = "); print_hex32(bc[3]);
	cputs("\r\n  sdbc[4] OCR        = "); print_hex32(bc[4]);
	cputs("\r\n  sdbc[5] block-addr = "); print_hex32(bc[5]);
	cputs("  (1 = SDHC/SDXC, 0 = byte-addressed SDSC)");
	cputs("\r\n  sdbc[6] card RCA   = "); print_hex32(bc[6]);
	cputs("\r\n");
}

/* `fi` -- arm/disarm eMMC write fault injection. See emmc_bio.h.
 *
 * Deliberately requires the LBA floor to be typed out every time rather than
 * defaulting it. Injection can corrupt real data, and the floor is the only
 * thing standing between this command and the guest's root filesystem; a default
 * would be a default that someone eventually forgets to override. The guest's
 * unused swap partition starts at 12863488 (0xC44000), which is the intended
 * target.
 *
 *   fi off
 *   fi <every> <min_lba_hex> [legacy]
 *
 * legacy=1 selects the OLD iteration-bounded card-idle wait in the settle, so
 * corruption should REAPPEAR; omit it (or 0) for the timed wait, where it should
 * not. Running both is what validates the fix instead of merely exercising it. */
static void cmd_fi(char **tok, int nt)
{
	unsigned long every = 0, min_lba = 0, point = 0, legacy = 0;

	if (nt < 2) {
		err("usage: fi off | fi <every> <min_lba_hex> <point> [legacy]");
		return;
	}
	if (streq(tok[1], "off")) {
		emmc_bio_fault_inject(0, 0, 0, 0);
		cputs("fi: DISARMED\r\n");
		return;
	}
	if (nt < 4) {
		err("usage: fi <every> <min_lba_hex> <point> [legacy]  (min_lba mandatory)");
		return;
	}
	if (!parse_hex(tok[1], &every) || every == 0) {
		err("bad every (hex, nonzero)");
		return;
	}
	if (!parse_hex(tok[2], &min_lba)) {
		err("bad min_lba (hex)");
		return;
	}
	if (nt >= 4 && !parse_hex(tok[3], &point)) {
		err("bad point (0 = after DATA_OVER, 1 = data-phase wait)");
		return;
	}
	if (nt >= 5 && !parse_hex(tok[4], &legacy)) {
		err("bad legacy flag");
		return;
	}

	emmc_bio_fault_inject((uint32_t)every, (uint32_t)min_lba,
	                      (uint32_t)point, (uint32_t)legacy);
	cputs("fi: ARMED every=");
	print_hex32((uint32_t)every);
	cputs(" min_lba=");
	print_hex32((uint32_t)min_lba);
	cputs(legacy ? " wait=LEGACY(iteration-bounded)" : " wait=TIMED");
	cputs(point ? " point=DATA-PHASE(words still in FIFO)\r\n"
	            : " point=AFTER-DATA_OVER(all words delivered)\r\n");
}

static void cmd_sd(char **tok, int nt)
{
	unsigned long lba = 0;
	int rc;

	if (nt < 2) {
		err("usage: sd <init|read <lba>|write <lba> CONFIRM>");
		return;
	}

	if (streq(tok[1], "init")) {
		rc = sd_bio_init();
		cputs("sd init: rc="); print_hex32((uint32_t)rc);
		cputs(rc == 0 ? "  OK\r\n" : "  FAILED\r\n");
		sd_show_bc();
		return;
	}

	if (streq(tok[1], "read")) {
		if (nt < 3 || !parse_hex(tok[2], &lba)) {
			err("usage: sd read <lba>");
			return;
		}
		rc = sd_bio_read((uint32_t)lba, (uint64_t)HVMAP_SD_TESTBUF);
		cputs("sd read lba="); print_hex32((uint32_t)lba);
		cputs(" rc="); print_hex32((uint32_t)rc);
		if (rc != 0) { cputs("  FAILED\r\n"); sd_show_bc(); return; }
		cputs("  OK, first 64 bytes:\r\n");
		cmd_read_bytes(HVMAP_SD_TESTBUF, 64u);
		return;
	}

	if (streq(tok[1], "write")) {
		/* Destructive and irreversible on a real card, so it takes an explicit
		 * literal rather than relying on the operator having read the help.
		 * Writes whatever HVMAP_SD_TESTBUF currently holds -- normally a buffer
		 * a prior `sd read` filled, which makes read-write-read a real
		 * round-trip test rather than a blind write of stack garbage. */
		if (nt < 4 || !parse_hex(tok[2], &lba) || !streq(tok[3], "CONFIRM")) {
			err("usage: sd write <lba> CONFIRM   (writes HVMAP_SD_TESTBUF; destructive)");
			return;
		}
		rc = sd_bio_write((uint32_t)lba, (uint64_t)HVMAP_SD_TESTBUF);
		cputs("sd write lba="); print_hex32((uint32_t)lba);
		cputs(" rc="); print_hex32((uint32_t)rc);
		cputs(rc == 0 ? "  OK\r\n" : "  FAILED\r\n");
		if (rc != 0) sd_show_bc();
		return;
	}

	err("usage: sd <init|read <lba>|write <lba> CONFIRM>");
}

static void cmd_gr(struct el2_frame *f)
{
	int i;

	if (!f) {
		err("no frame");
		return;
	}
	for (i = 0; i < 31; i++) {
		dc_putc('x');
		if (i >= 10)
			dc_putc('0' + (i / 10));
		dc_putc('0' + (i % 10));
		dc_putc('=');
		print_hex64(f->x[i]);
		dc_putc(((i % 3) == 2) ? '\r' : ' ');
		if ((i % 3) == 2)
			dc_putc('\n');
	}
	newline();
	cputs("elr(pc)="); print_hex64(f->elr);
	cputs(" spsr=");   print_hex64(f->spsr);
	newline();
	cputs("esr=");     print_hex64(f->esr);
	cputs(" far=");    print_hex64(f->far);
	cputs(" kind=");   print_hex64(f->kind);
	newline();
}

/* sr: dump the guest's live EL1 system-register state -- MMU/exception/
 * timer config -- via direct `mrs` reads from EL2. */
static void cmd_sr(void)
{
	cputs("ELR_EL1=");   print_hex64(RDSYSREG(ELR_EL1));   newline();
	cputs("ESR_EL1=");   print_hex64(RDSYSREG(ESR_EL1));   newline();
	cputs("FAR_EL1=");   print_hex64(RDSYSREG(FAR_EL1));   newline();
	cputs("SCTLR_EL1="); print_hex64(RDSYSREG(SCTLR_EL1)); newline();
	cputs("TCR_EL1=");   print_hex64(RDSYSREG(TCR_EL1));   newline();
	cputs("TTBR0_EL1=");print_hex64(RDSYSREG(TTBR0_EL1));  newline();
	cputs("TTBR1_EL1=");print_hex64(RDSYSREG(TTBR1_EL1));  newline();
	cputs("MAIR_EL1=");  print_hex64(RDSYSREG(MAIR_EL1));  newline();
	cputs("VBAR_EL1=");  print_hex64(RDSYSREG(VBAR_EL1));  newline();
	cputs("SP_EL1=");    print_hex64(RDSYSREG(SP_EL1));    newline();
	cputs("SPSR_EL1=");  print_hex64(RDSYSREG(SPSR_EL1));  newline();
	cputs("CNTV_CTL_EL0=");print_hex64(RDSYSREG(CNTV_CTL_EL0)); newline();
	cputs("CNTP_CTL_EL0=");print_hex64(RDSYSREG(CNTP_CTL_EL0)); newline();
	cputs("CurrentEL=");  print_hex64(RDSYSREG(CurrentEL)); newline();
}

/* gr1/sr1: the vcpu1.h follow-up -- gr/sr, but for CPU1's OWN guest vCPU
 * (vcpu1.c's experimental third vCPU) instead of CPU0's FreeBSD guest.
 * These are ADDITIVE: gr/sr above are untouched, still read exactly the
 * `frame` this function was called with (CPU0's snapshot, always -- see the
 * extern block above), never g_cpu1_guest_frame. Mixing the two would be
 * exactly the "confidently wrong diagnosis" class of bug the CPU0 capture
 * gate's own comment (el2_exc.c) warns about, just moved into dbgmon
 * instead of el2_trap.
 *
 * Both commands share one problem gr/sr never had: CPU1 might have NEVER
 * captured a frame at all -- either dbg_vcpu1 is off (CPU1 runs no guest
 * code, so it cannot fault into the capture site) or it is armed but simply
 * hasn't trapped yet since boot. A frame of zeros in that state is
 * indistinguishable, byte-for-byte, from a real capture where the guest
 * genuinely had every GPR at zero -- exactly the "publish real zeros" trap
 * this project's breadcrumb-window discipline exists to avoid elsewhere.
 * g_cpu1_guest_frame_seq is the tell (see its own comment in el2_exc.c): it
 * starts at 0 and only the capture site ever advances it, so both commands
 * check it FIRST and refuse to print a frame at all when it's still 0,
 * rather than let a reader mistake "never captured" for "captured, and
 * it's zero". */
static void cmd_gr1(void)
{
	struct el2_frame f;
	int i;

	if (!g_cpu1_guest_frame_seq) {
		cputs("gr1: CPU1 has never captured a guest frame (vcpu1 is ");
		cputs(dbg_vcpu1 ? "armed, but hasn't trapped from EL1 yet)\r\n"
				 : "off -- CPU1 runs no guest code)\r\n");
		return;
	}
	el2_snapshot_guest_frame_cpu1(&f);
	for (i = 0; i < 31; i++) {
		dc_putc('x');
		if (i >= 10)
			dc_putc('0' + (i / 10));
		dc_putc('0' + (i % 10));
		dc_putc('=');
		print_hex64(f.x[i]);
		dc_putc(((i % 3) == 2) ? '\r' : ' ');
		if ((i % 3) == 2)
			dc_putc('\n');
	}
	newline();
	cputs("elr(pc)="); print_hex64(f.elr);
	cputs(" spsr=");   print_hex64(f.spsr);
	newline();
	cputs("esr=");     print_hex64(f.esr);
	cputs(" far=");    print_hex64(f.far);
	cputs(" kind=");   print_hex64(f.kind);
	newline();
}

/* sr1: CPU1's own live EL1 sysregs, same fields and same direct `mrs` reads
 * as cmd_sr() above, for the SAME reason cmd_sr() reads them directly --
 * plus one property cmd_sr() does NOT have. cmd_sr() runs wherever
 * exec_line() happened to be called from, which is CPU1 today but is not
 * guaranteed by anything in this file (see the "per-PE registers are
 * banked" rule in this project's CLAUDE.md: a debug-channel register read
 * is serviced by whichever core answers it, not necessarily the guest's
 * own core -- CPU0's guest and CPU1's dbgmon are, in general, different
 * cores). sr1 does not have that problem BY CONSTRUCTION: this function can
 * only ever observe a nonzero g_cpu1_guest_frame_seq (see the gate below)
 * once CPU1 itself has taken a lower-EL trap, and dbgmon_service() is only
 * ever invoked on CPU1 once dbg_core_active is set (el2_exc.c's six
 * `if (!dbg_core_active) dbgmon_service(frame);` fallback calls on CPU0
 * stop firing the moment it is) -- and vcpu1_run() claims dbg_core_active
 * for itself BEFORE it ever enters EL1 (vcpu1.h's "THAT WAS TRUE ONLY AFTER
 * ONE MORE FIX" section). So any window in which sr1 has something real to
 * report is also a window in which this function is provably executing on
 * CPU1 -- the same core whose EL1 register file the `mrs` reads below
 * return. That chain is worth restating if either invariant ever moves:
 * dbg_core_active's claim-before-entry ordering, or the fallback guard that
 * stops CPU0 from calling dbgmon_service once it's set. */
static void cmd_sr1(void)
{
	if (!g_cpu1_guest_frame_seq) {
		cputs("sr1: CPU1 has never captured a guest frame (vcpu1 is ");
		cputs(dbg_vcpu1 ? "armed, but hasn't trapped from EL1 yet)\r\n"
				 : "off -- CPU1 runs no guest code)\r\n");
		return;
	}
	cputs("ELR_EL1=");   print_hex64(RDSYSREG(ELR_EL1));   newline();
	cputs("ESR_EL1=");   print_hex64(RDSYSREG(ESR_EL1));   newline();
	cputs("FAR_EL1=");   print_hex64(RDSYSREG(FAR_EL1));   newline();
	cputs("SCTLR_EL1="); print_hex64(RDSYSREG(SCTLR_EL1)); newline();
	cputs("TCR_EL1=");   print_hex64(RDSYSREG(TCR_EL1));   newline();
	cputs("TTBR0_EL1=");print_hex64(RDSYSREG(TTBR0_EL1));  newline();
	cputs("TTBR1_EL1=");print_hex64(RDSYSREG(TTBR1_EL1));  newline();
	cputs("MAIR_EL1=");  print_hex64(RDSYSREG(MAIR_EL1));  newline();
	cputs("VBAR_EL1=");  print_hex64(RDSYSREG(VBAR_EL1));  newline();
	cputs("SP_EL1=");    print_hex64(RDSYSREG(SP_EL1));    newline();
	cputs("SPSR_EL1=");  print_hex64(RDSYSREG(SPSR_EL1));  newline();
	cputs("CNTV_CTL_EL0=");print_hex64(RDSYSREG(CNTV_CTL_EL0)); newline();
	cputs("CNTP_CTL_EL0=");print_hex64(RDSYSREG(CNTP_CTL_EL0)); newline();
	cputs("CurrentEL=");  print_hex64(RDSYSREG(CurrentEL)); newline();
}

/* r <addr> [n]: read n 32-bit words of PHYSICAL memory (flat EL2 map --
 * any PA is directly readable/writable, guest or host). */
/* Make a memory read HONEST.
 *
 * cmd_read_words()/cmd_read_bytes() used a plain volatile load. That is fine for
 * EL2's own breadcrumbs, and quietly wrong for anything the GUEST wrote: this
 * code runs on CPU1, and a value the guest stored -- especially through a
 * non-cacheable mapping, which is exactly what dma_alloc_coherent() gives it --
 * does not invalidate CPU1's copy. So the debug channel could report a stale
 * line, most dangerously as ZEROS, and zeros read like "nothing is there".
 *
 * Cost of not doing this, 2026-08-18: walking the Mali-400 GPU page tables from
 * EL2 reported the entire page directory empty, which would have been read as a
 * driver that never maps anything. The tables are guest-written coherent memory;
 * the reading could not be trusted either way.
 *
 * civac, NOT ivac. `dc ivac` discards a dirty line without writing it back, so an
 * `r` aimed at EL2's own recently-written data would DESTROY it -- a debug
 * command must never be able to corrupt the thing it inspects. `dc civac` cleans
 * first, then invalidates, which is also what every breadcrumb writer in this
 * tree already uses.
 *
 * This is the project's own rule made operational: per-PE state and cached copies
 * lie, and a read from the wrong core has produced confidently wrong diagnoses
 * here more than once. */
static inline void dbg_read_fresh(unsigned long addr)
{
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(addr) : "memory");
}

static void cmd_read_words(unsigned long addr, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if ((i % WORDS_PER_LINE) == 0) {
			if (i)
				newline();
			print_addr(addr + (unsigned long)i * 4u);
			dc_putc(':');
		}
		dc_putc(' ');
		dbg_read_fresh(addr + (unsigned long)i * 4u);
		print_hex32(*(volatile uint32_t *)(addr + (unsigned long)i * 4u));
	}
	newline();
}

/* rb <addr> [n]: read n bytes of PHYSICAL memory. */
static void cmd_read_bytes(unsigned long addr, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if ((i % BYTES_PER_LINE) == 0) {
			if (i)
				newline();
			print_addr(addr + i);
			dc_putc(':');
		}
		dc_putc(' ');
		dbg_read_fresh(addr + i);
		print_hex8(*(volatile uint8_t *)(addr + i));
	}
	newline();
}

/* d <addr> <len>: hex + ASCII dump. */
static void cmd_dump(unsigned long addr, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i += BYTES_PER_LINE) {
		uint32_t j;

		print_addr(addr + i);
		dc_putc(':');
		dc_putc(' ');
		for (j = 0; j < BYTES_PER_LINE; j++) {
			if (i + j < len)
				print_hex8(*(volatile uint8_t *)(addr + i + j));
			else
				cputs("  ");
			dc_putc(' ');
		}
		dc_putc('|');
		for (j = 0; j < BYTES_PER_LINE && i + j < len; j++) {
			uint8_t b = *(volatile uint8_t *)(addr + i + j);
			dc_putc((b >= 0x20 && b < 0x7f) ? (int)b : '.');
		}
		dc_putc('|');
		newline();
	}
}

/* CACHE MAINTENANCE ON THE DEBUG WRITE PATH — do not remove.
 *
 * Both writers below used to be a bare store. That made every host-side write
 * land in THIS core's (CPU1's) cache and, for DRAM, frequently never reach
 * memory at all. The failure is silent and actively deceptive:
 *
 *   - a read-back immediately after hits the same cache and returns the value
 *     you just wrote, so the write LOOKS durable — hvdbg's own
 *     write_word_verified() is fooled for exactly this reason;
 *   - a warm reset (which deliberately preserves DRAM, so every breadcrumb in
 *     this tree survives one) discards the dirty line, and the ORIGINAL DRAM
 *     content reappears.
 *
 * Found 2026-08-01 the expensive way. Trying to identify what kept refilling
 * four breadcrumb windows, I zeroed them from the host, confirmed the zeros,
 * rebooted, and watched the old text return — byte-for-byte identical, every
 * time. That reads exactly like an active writer, and I chased one through
 * three wrong theories (stale DRAM, U-Boot, the guest) and then a fourth (a
 * DMA master) after an EL2 watchpoint armed on all four cores recorded ZERO
 * hits. The watchpoint was right: nothing ever wrote those bytes. Nothing had
 * ever erased them either. The byte-identical "reappearance" was the tell —
 * a writer must reproduce the text, an un-erased region merely keeps it.
 *
 * Every breadcrumb writer in this tree (vc_store32, vblk_bc, vnet_bc, ...)
 * already does exactly this dc civac + dsb sy; the debug console was the one
 * write path that did not, which is why it could not be trusted for the one
 * job it exists to do. See docs/war-stories.md §12. */
static void dbg_write_flush(unsigned long addr)
{
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(addr) : "memory");
}

/* w <addr> <val>: write a 32-bit word of PHYSICAL memory. */
static void cmd_write_word(unsigned long addr, unsigned long val)
{
	*(volatile uint32_t *)addr = (uint32_t)val;
	dbg_write_flush(addr);
	cputs("wrote "); print_addr(addr); cputs(" = "); print_hex32((uint32_t)val); newline();
}

/* wb <addr> <val>: write a byte of PHYSICAL memory. */
static void cmd_write_byte(unsigned long addr, unsigned long val)
{
	*(volatile uint8_t *)addr = (uint8_t)val;
	dbg_write_flush(addr);
	cputs("wrote "); print_addr(addr); cputs(" = "); print_hex8((uint8_t)val); newline();
}

/* gva <addr>: translate a GUEST VIRTUAL address through the guest's own
 * stage-1 translation. `AT S1E1R, <Xt>` asks the MMU to perform exactly the
 * translation the guest's own EL1 MMU would for a read at that VA (using
 * the CURRENT TTBR0/1_EL1, TCR_EL1, SCTLR_EL1 -- i.e. the guest's live MMU
 * state, since we're the only software using these EL1 registers), leaving
 * the result in PAR_EL1: bit0 = F (1 = translation fault, bits[6:1] = FST
 * fault status code); if F==0, bits[47:12] = the output PA[47:12]. We then
 * OR in the low 12 bits of the VA (the page offset) to get the exact byte
 * address, and read the word there (any PA is directly readable under our
 * flat EL2 map) -- this is how you follow the guest's high-KVA (0xffff...)
 * pointers into real memory once FreeBSD's MMU is on. */
static void cmd_gva(unsigned long va)
{
	uint64_t par;

	__asm__ volatile("at s1e1r, %0" :: "r"((uint64_t)va) : "memory");
	__asm__ volatile("isb" ::: "memory");
	par = RDSYSREG(PAR_EL1);

	cputs("PAR_EL1="); print_hex64(par); newline();

	if (par & 1ULL) {
		cputs("FAULT fst=");
		print_hex8((uint8_t)((par >> 1) & 0x3fULL));
		newline();
		return;
	}

	{
		unsigned long pa = (unsigned long)((par & 0x000ffffffffff000ULL) |
						    ((uint64_t)va & 0xfffULL));
		cputs("pa="); print_addr(pa); newline();
		cputs("word="); print_hex32(*(volatile uint32_t *)pa); newline();
	}
}

/* t: summarize the trace/console rings -- read + print the key words of
 * each fixed DRAM breadcrumb window. Addresses/layouts per the project's
 * existing instruments (gtrace.h, vconsole.h, el2_exc.c); dbgmon does not
 * include those headers (keeps this file self-contained per the
 * assignment), it just knows the fixed addresses and word offsets. */
#define DBGMON_GTRC_BASE  0x50002000UL   /* gtrace.h: magic/event_count/last_sctlr/fault_count */
_Static_assert(DBGMON_GTRC_BASE == HVMAP_LOW_GTRACE,
               "DBGMON_GTRC_BASE drifted from hv_addrmap.h -- the map owns this address");
#define DBGMON_UART_BASE  0x50000f00UL   /* vconsole.h: magic/total_bytes/fault_count/reserved */
_Static_assert(DBGMON_UART_BASE == HVMAP_LOW_VCONSOLE_HDR,
               "DBGMON_UART_BASE drifted from hv_addrmap.h -- the map owns this address");
#define DBGMON_EXC_BASE   0x50000400UL   /* el2_exc.c: magic/count/kind/esr/... */

static void cmd_t(void)
{
	volatile uint32_t *g = (volatile uint32_t *)DBGMON_GTRC_BASE;
	volatile uint32_t *u = (volatile uint32_t *)DBGMON_UART_BASE;
	volatile uint32_t *e = (volatile uint32_t *)DBGMON_EXC_BASE;

	cputs("GTRC  magic="); print_hex32(g[0]);
	cputs(" events=");     print_hex32(g[1]);
	cputs(" last_sctlr="); print_hex32(g[2]);
	cputs(" faults=");     print_hex32(g[3]);
	newline();

	cputs("UART  magic="); print_hex32(u[0]);
	cputs(" bytes=");      print_hex32(u[1]);
	cputs(" faults=");     print_hex32(u[2]);
	newline();

	cputs("EXC   magic="); print_hex32(e[0]);
	cputs(" count=");      print_hex32(e[1]);
	cputs(" kind=");       print_hex32(e[2]);
	cputs(" esr=");        print_hex32(e[3]);
	newline();
	cputs("EXC   elr=");   print_hex32(e[4]);
	dc_putc(':'); print_hex32(e[5]);
	cputs(" far=");        print_hex32(e[6]);
	dc_putc(':'); print_hex32(e[7]);
	newline();

	{
		volatile uint32_t *ff = (volatile uint32_t *)0x50002400UL;
		volatile uint32_t *ss = (volatile uint32_t *)0x50002800UL;

		cputs("FFL1  magic="); print_hex32(ff[0]);
		cputs(" valid=");      print_hex32(ff[1]);
		cputs(" elr=");        print_hex32(ff[7]); dc_putc(':'); print_hex32(ff[6]);
		cputs(" far=");        print_hex32(ff[9]); dc_putc(':'); print_hex32(ff[8]);
		newline();
		cputs("SST1  magic="); print_hex32(ss[0]);
		cputs(" steps=");      print_hex32(ss[1]);
		cputs(" head=");       print_hex32(ss[2]);
		cputs(" on=");         print_hex32(ss[3]);
		newline();
	}
}

/* Short human name for an ESR EC (exception class) field — just the ones we
 * expect to see latched as the guest's first EL1 fault. Unknown -> "?". */
static const char *ec_name(uint32_t ec)
{
	switch (ec) {
	case 0x00: return "unknown";
	case 0x0e: return "illegal-state";
	case 0x15: return "svc64";
	case 0x16: return "hvc64";
	case 0x18: return "msr/mrs-trap";
	case 0x20: return "iabort-lower";
	case 0x21: return "iabort-cur";
	case 0x22: return "pc-align";
	case 0x24: return "dabort-lower";
	case 0x25: return "dabort-cur";
	case 0x26: return "sp-align";
	case 0x2f: return "serror";
	case 0x30: return "brkpt-lower";
	case 0x31: return "brkpt-cur";
	case 0x32: return "step-lower";
	case 0x33: return "step-cur";
	case 0x34: return "watchpt-lower";
	case 0x35: return "watchpt-cur";
	case 0x3c: return "brk";
	default:   return "?";
	}
}

/* ff: decode the FIRST-FAULT LATCH breadcrumb (gtrace.c, FFL1 @ 0x50002400).
 * This is the ORIGINAL guest EL1 fault frozen before the recursive-exception
 * storm masked it — EC name, ELR(PC), FAR, SP, SPSR, ESR + all GPRs. */
#define DBGMON_FF_BASE  0x50002400UL
_Static_assert(DBGMON_FF_BASE == HVMAP_LOW_FFL1,
               "DBGMON_FF_BASE drifted from hv_addrmap.h -- the map owns this address");
#define DBGMON_FF_MAGIC 0x46464C31u   /* "FFL1" */

static void cmd_ff(void)
{
	volatile uint32_t *f = (volatile uint32_t *)DBGMON_FF_BASE;
	uint32_t magic = f[0];
	uint32_t valid = f[1];
	uint64_t esr, elr, far, sp, spsr;
	uint32_t ec, i;

	if (magic != DBGMON_FF_MAGIC) {
		cputs("no first-fault latch (magic="); print_hex32(magic);
		cputs(", expect 46464c31)\r\n");
		return;
	}
	if (!valid) {
		cputs("first-fault latch armed, not yet triggered\r\n");
		return;
	}

	esr  = ((uint64_t)f[5]  << 32) | f[4];
	elr  = ((uint64_t)f[7]  << 32) | f[6];
	far  = ((uint64_t)f[9]  << 32) | f[8];
	sp   = ((uint64_t)f[11] << 32) | f[10];
	spsr = ((uint64_t)f[13] << 32) | f[12];
	ec   = (uint32_t)((esr >> 26) & 0x3fu);

	cputs("FIRST-FAULT LATCH (FFL1)\r\n");
	cputs("  vec=");  print_hex32(f[2]);
	cputs(" EC=0x");  print_hex8((uint8_t)ec);
	cputs(" ");       cputs(ec_name(ec));
	newline();
	cputs("  ESR_EL1=");  print_hex64(esr);  newline();
	cputs("  ELR_EL1=");  print_hex64(elr);  cputs(" (guest PC)\r\n");
	cputs("  FAR_EL1=");  print_hex64(far);  newline();
	cputs("  SP_EL1=");   print_hex64(sp);   newline();
	cputs("  SPSR_EL1="); print_hex64(spsr); newline();
	for (i = 0; i < 31u; i++) {
		uint64_t v = ((uint64_t)f[14u + i * 2u + 1u] << 32) | f[14u + i * 2u];
		dc_putc('x');
		if (i >= 10)
			dc_putc('0' + (i / 10));
		dc_putc('0' + (i % 10));
		dc_putc('=');
		print_hex64(v);
		dc_putc(((i % 3) == 2) ? '\r' : ' ');
		if ((i % 3) == 2)
			dc_putc('\n');
	}
	newline();
}

/* pt <ttbr_pa> <va>: OPTIONAL stage-1 walk. Assumes the common case (4KB
 * granule, up to 4 levels, table descriptors bit[1]=1 / bit[0]=1, block or
 * page descriptor otherwise) and just prints each level's raw descriptor
 * plus, at the leaf, the resulting output PA. Bounded to 4 levels (no
 * recursion beyond that -- can't loop forever even on a corrupt table). */
static void cmd_pt(unsigned long ttbr_pa, unsigned long va)
{
	unsigned long table = ttbr_pa & 0x0000fffffffff000UL;
	int level;

	for (level = 0; level <= 3; level++) {
		int shift = 39 - level * 9;
		unsigned long idx = (va >> shift) & 0x1ffUL;
		uint64_t desc = *(volatile uint64_t *)(table + idx * 8u);

		cputs("L"); dc_putc((char)('0' + level));
		cputs(" idx="); print_hex32((uint32_t)idx);
		cputs(" desc="); print_hex64(desc);
		newline();

		if (!(desc & 1ULL)) {
			cputs("invalid (not present)\r\n");
			return;
		}
		if (level == 3 || !(desc & 2ULL)) {
			unsigned long pa = (unsigned long)(desc & 0x0000fffffffff000ULL);
			cputs("leaf -> pa_base="); print_addr(pa); newline();
			return;
		}
		table = (unsigned long)(desc & 0x0000fffffffff000ULL);
	}
}

/* sr2: dump EL2 system registers (the hypervisor's own state). */
static void cmd_sr2(void)
{
	uint64_t v;
#define SR2(name) __asm__ volatile("mrs %0, " name : "=r"(v)); \
                   cputs(name "="); print_hex64(v); newline()
	SR2("cnthctl_el2");
	SR2("cntvoff_el2");
	SR2("hcr_el2");
	SR2("vttbr_el2");
	SR2("vtcr_el2");
	SR2("cptr_el2");
	SR2("sctlr_el2");
#undef SR2
}

/* sw <name> <val>: write a system register LIVE.  Name table covers the
 * registers most useful for hypervisor debugging — toggle CNTHCTL to test
 * counter-access isolation, HCR for trap routing, etc. */
static void cmd_sw(const char *name, uint64_t val)
{
	/* Use the write+readback pattern so the caller sees what took effect
	 * (some bits are RAZ/WI in certain configurations). */
	#define W(name, asm_name) \
		if (streq(reg, name)) { \
			__asm__ volatile("msr " asm_name ", %0\n\tisb" :: "r"(val) : "memory"); \
			uint64_t rb; __asm__ volatile("mrs %0, " asm_name : "=r"(rb)); \
			cputs(asm_name " wr="); print_hex64(val); cputs(" rb="); print_hex64(rb); newline(); \
			return; \
		}
	const char *reg = name;
	W("cnthctl", "cnthctl_el2");
	W("hcr",     "hcr_el2");
	W("vtcr",    "vtcr_el2");
	W("vttbr",   "vttbr_el2");
	W("cptr",    "cptr_el2");
	W("sctlr2",  "sctlr_el2");
	W("sctlr1",  "sctlr_el1");
	W("vbar1",   "vbar_el1");
	W("tcr1",    "tcr_el1");
	W("ttbr0_1", "ttbr0_el1");
	W("ttbr1_1", "ttbr1_el1");
	#undef W
	err("unknown reg. try: cnthctl hcr vtcr vttbr cptr sctlr1 sctlr2 vbar1 tcr1");
}

/* call <pa> [x0] [x1] [x2] [x3]: call a hypervisor function at physical
 * address pa with up to 4 arguments (AAPCS x0-x3). Returns x0.  Runs at
 * EL2 on the current (tick-handler) stack.  Use to invoke any hypervisor
 * function live: musb_init, vconsole_dump, gic_timer_init, etc.
 *
 * Safety: pa MUST be within the hypervisor's .text section (validated via
 * _text_start/_text_end from link.ld).  Additionally, call_active is set
 * so el2_exc.c can recover if the callee faults (returns 0xDEAD). */
extern char _text_start[], _text_end[];
volatile int dbgmon_call_active = 0;

static void cmd_call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	uint64_t ret;
	typedef uint64_t (*fn4_t)(uint64_t, uint64_t, uint64_t, uint64_t);

	if (fn < (uint64_t)(unsigned long)_text_start ||
	    fn >= (uint64_t)(unsigned long)_text_end) {
		cputs("reject: pa "); print_addr(fn);
		cputs(" outside .text ["); print_addr((unsigned long)_text_start);
		cputs(","); print_addr((unsigned long)_text_end); cputs(")\r\n");
		return;
	}

	cputs("calling "); print_addr(fn); cputs("(0x"); print_hex64(a0);
	cputs(", 0x"); print_hex64(a1); cputs(", 0x"); print_hex64(a2);
	cputs(", 0x"); print_hex64(a3); cputs(")\r\n");
	console_flush();

	dbgmon_call_active = 1;
	ret = ((fn4_t)(unsigned long)fn)(a0, a1, a2, a3);
	dbgmon_call_active = 0;

	cputs("ret = 0x"); print_hex64(ret);
	if (ret == 0xDEADull) cputs(" (FAULT — callee crashed, recovered)");
	newline();
}

/* patch <pa> <word>: write a 32-bit instruction/data word to physical
 * memory AND flush the I-cache for that line, so the change is visible to
 * the instruction stream immediately. Use for hot-patching code without
 * a board reset. */
extern void scrub_accept(uint64_t pa) __attribute__((weak));

static void cmd_patch(uint64_t pa, uint32_t word)
{
	volatile uint32_t *p = (volatile uint32_t *)(unsigned long)pa;
	*p = word;
	__asm__ volatile(
		"dc cvau, %0\n\t"
		"dsb ish\n\t"
		"ic ivau, %0\n\t"
		"dsb ish\n\t"
		"isb"
		:: "r"(p) : "memory");
	if (scrub_accept)
		scrub_accept(pa);   /* intentional: keep the scrubber from undoing it */
	cputs("patched "); print_addr(pa); cputs(" = "); print_hex32(word); newline();
}

/* poweroff: type a clean-shutdown line into the guest console. The guest's
 * rc shutdown path syncs + unmounts (UFS fs_clean -> 1 on disk) and then
 * issues PSCI SYSTEM_OFF, which el2_trap() honors (dbg_clean_off) with a clean
 * warm reset to U-Boot — so the supervisor reloads onto an already-clean
 * filesystem. This is the durable, "no hand-patching the superblock" way to
 * cycle the board. Requires a context that can run `shutdown` (a root shell /
 * single-user prompt); a leading newline flushes any partial line first. */
static void cmd_poweroff(void)
{
	static const char line[] = "\nshutdown -p now\n";
	const char *p;

	for (p = line; *p; p++)
		vconsole_rx_push((uint8_t)*p);
	cputs("injected 'shutdown -p now' -> guest unmounts, PSCI SYSTEM_OFF, "
	      "HV clean warm-reset to U-Boot (fs stays clean)\r\n");
}

static void cmd_help(void)
{
	cputs("bzdOS live hv-debugger commands:\r\n");
	cputs("  gr                 guest GPRs x0..x30 + ELR(PC) + SPSR, this tick (CPU0)\r\n");
	cputs("  sr                 guest EL1 sysregs (SCTLR/TCR/TTBRn/MAIR/VBAR/...) (CPU0)\r\n");
	cputs("  gr1                CPU1's OWN guest GPRs (vcpu1, EXPERIMENTAL; 'never captured'\r\n");
	cputs("                     if vcpu1 is off or hasn't trapped yet -- see vcpu1.h)\r\n");
	cputs("  sr1                CPU1's OWN guest EL1 sysregs, same vcpu1 caveat as gr1\r\n");
	cputs("  r  <addr> [n]      read n phys words (default n=1)\r\n");
	cputs("  rb <addr> [n]      read n phys bytes (default n=1)\r\n");
	cputs("  sd init            bring up the SD card, decode sd_bio breadcrumbs\r\n");
	cputs("  sd read <lba>      read one sector, hexdump it\r\n");
	cputs("  sd write <lba> CONFIRM   write the scratch buffer back [DESTRUCTIVE]\r\n");
	cputs("  fi <every> <min_lba> <point> [legacy]  eMMC write fault inject [DESTRUCTIVE]\r\n");
	cputs("  fi off             disarm fault injection\r\n");
	cputs("  d  <addr> <len>    hex+ascii dump\r\n");
	cputs("  gva <addr>         translate guest VA via AT S1E1R, read the word\r\n");
	cputs("  w  <addr> <val>    write phys word\r\n");
	cputs("  wb <addr> <val>    write phys byte\r\n");
	cputs("  t                  trace/console ring summary (GTRC/UART/EXC/FFL1/SST1)\r\n");
	cputs("  ff                 decode the first-fault latch (original guest fault)\r\n");
	cputs("  ss                 toggle guest single-step (ring @ 0x50002800)\r\n");
	cputs("  bp <va>            arm a hw instruction breakpoint on guest VA\r\n");
	cputs("  wp <va>            arm a hw write watchpoint on guest VA\r\n");
	cputs("  bpc                clear all hw breakpoints/watchpoints\r\n");
	cputs("  bpl                list hw breakpoints/watchpoints\r\n");
	cputs("  bt                 guest frame-pointer backtrace (BTR1 @ 0x50000700)\r\n");
	cputs("  pt <ttbr_pa> <va>  walk a stage-1 page table (best-effort)\r\n");
	cputs("  sr2                EL2 sysregs (CNTHCTL/HCR/VTCR/VTTBR/CPTR/SCTLR_EL2)\r\n");
	cputs("  sw <name> <val>    write sysreg LIVE (cnthctl hcr vtcr vttbr cptr sctlr1/2 ...)\r\n");
	cputs("  call <pa> [x0..x3] call hypervisor function at PA, return x0\r\n");
	cputs("  patch <pa> <word>  write instruction + I-cache flush (hot-patch)\r\n");
	cputs("  poweroff | off     inject 'shutdown -p now' -> clean fs + warm reset\r\n");
	cputs("  bmc <verb> ...     software-BMC mgmt plane (bmc help)\r\n");
	cputs("  hold               arm pause-before-guest-entry (takes effect NEXT warm reset)\r\n");
	cputs("  release            release a currently-held pause-before-guest-entry\r\n");
	cputs("  phycycle           DISABLED (EL2 RSB races the guest's iichb1 — use guest-side phycut)\r\n");
	cputs("  wdtest on|off      software 'PHY never trains' — exercises the give-up/escalation ladder\r\n");
	cputs("  zboot              start the 2nd guest on CPU3 (dual build; stage its ELF first)\r\n");
	cputs("  zstage             re-run the 2nd guest's copy-in from the landing window\r\n");
	cputs("  zunhalt            re-arm CPU3 after a failed zboot (0xBAD1/0xBAD2), no reload\r\n");
	cputs("  h | ?              this help\r\n");
}

/* bp <va>: arm the next free hardware INSTRUCTION breakpoint on guest VA. */
static void cmd_bp(unsigned long va, struct el2_frame *f)
{
	uint64_t bva[8]; int ben[8];
	int nbp = hwbp_list_bp(bva, ben);
	int i, slot = -1;

	for (i = 0; i < nbp; i++)
		if (!ben[i]) { slot = i; break; }
	if (slot < 0) { err("no free breakpoint slot (bpc to clear)"); return; }
	if (hwbp_set(slot, (uint64_t)va, 0)) { err("hwbp_set failed"); return; }
	if (f) f->spsr &= ~(1ull << 9);   /* unmask guest PSTATE.D, like `ss` */
	cputs("bp["); dc_putc('0' + slot); cputs("] @ ");
	print_addr(va); cputs(" armed\r\n");
}

/* wp <va>: arm the next free hardware WRITE watchpoint on guest VA. */
static void cmd_wp(unsigned long va, struct el2_frame *f)
{
	uint64_t wva[8]; int wen[8];
	int nwp = hwbp_list_wp(wva, wen);
	int i, slot = -1;

	for (i = 0; i < nwp; i++)
		if (!wen[i]) { slot = i; break; }
	if (slot < 0) { err("no free watchpoint slot (bpc to clear)"); return; }
	if (hwbp_set(slot, (uint64_t)va, 1)) { err("hwbp_set failed"); return; }
	if (f) f->spsr &= ~(1ull << 9);
	cputs("wp["); dc_putc('0' + slot); cputs("] write @ ");
	print_addr(va); cputs(" armed\r\n");
}

/* bpc: clear all hardware breakpoints + watchpoints. */
static void cmd_bpc(void)
{
	hwbp_clear_all();
	cputs("all bp/wp cleared\r\n");
}

/* bpl: list configured breakpoints + watchpoints. */
static void cmd_bpl(void)
{
	uint64_t bva[8], wva[8]; int ben[8], wen[8];
	int nbp = hwbp_list_bp(bva, ben);
	int nwp = hwbp_list_wp(wva, wen);
	int i;

	for (i = 0; i < nbp; i++) {
		cputs("bp["); dc_putc('0' + i); cputs("] ");
		cputs(ben[i] ? "EN  " : "--  "); print_hex64(bva[i]); newline();
	}
	for (i = 0; i < nwp; i++) {
		cputs("wp["); dc_putc('0' + i); cputs("] ");
		cputs(wen[i] ? "EN  " : "--  "); print_hex64(wva[i]);
		cputs(wen[i] ? " (write)\r\n" : "\r\n");
	}
}

/* bt: frame-pointer backtrace of the guest as of this tick. Dumps raw return
 * addresses (resolve host-side with addr2line/nm vs /opt/bzdos/tftpboot/kernel;
 * ring also mirrored to BTR1 @ 0x50000700). */
static void cmd_bt(struct el2_frame *f)
{
	uint64_t frames[32];
	int n, i;

	if (!f) { err("no frame"); return; }
	n = backtrace_walk(f->elr, f->x[29], f->x[30], frames, 32);
	cputs("guest backtrace ("); print_hex32((uint32_t)n); cputs(" frames):\r\n");
	for (i = 0; i < n; i++) {
		cputs("  #"); print_hex8((uint8_t)i); cputs(" ");
		print_hex64(frames[i]); newline();
	}
}

/* ffv: decode the FF1V breadcrumb (0x50005800) — the guest's ORIGINAL EL1
 * fault captured by unmapping its EL1 vector page in stage-2. */
#define DBGMON_FFV_BASE  0x50005800UL
#define DBGMON_FFV_MAGIC 0x46463156u   /* "FF1V" */

static void cmd_ffv(void)
{
	volatile uint32_t *f = (volatile uint32_t *)DBGMON_FFV_BASE;
	uint32_t magic = f[0], count = f[1];
	uint64_t esr, elr, far, spsr, sp;
	uint32_t ec, i;

	if (magic != DBGMON_FFV_MAGIC) {
		cputs("no vector first-fault (magic="); print_hex32(magic);
		cputs(", expect 46463156)\r\n");
		return;
	}
	if (!count) {
		cputs("vector first-fault armed, not yet triggered\r\n");
		return;
	}
	esr  = ((uint64_t)f[5]  << 32) | f[4];
	elr  = ((uint64_t)f[7]  << 32) | f[6];
	far  = ((uint64_t)f[9]  << 32) | f[8];
	spsr = ((uint64_t)f[11] << 32) | f[10];
	sp   = ((uint64_t)f[13] << 32) | f[12];
	ec   = (uint32_t)((esr >> 26) & 0x3fu);

	cputs("VECTOR FIRST-FAULT (FF1V)  hits="); print_hex32(count); newline();
	cputs("  vecoff=0x"); print_hex32(f[2]);
	cputs(" el2_ec=0x");  print_hex8((uint8_t)f[3]); newline();
	cputs("  EC=0x");     print_hex8((uint8_t)ec);
	cputs(" ");           cputs(ec_name(ec)); newline();
	cputs("  ESR_EL1=");  print_hex64(esr);  newline();
	cputs("  ELR_EL1=");  print_hex64(elr);  cputs(" (original guest PC)\r\n");
	cputs("  FAR_EL1=");  print_hex64(far);  newline();
	cputs("  SPSR_EL1="); print_hex64(spsr); newline();
	cputs("  SP_EL1=");   print_hex64(sp);   newline();
	for (i = 0; i < 31u; i++) {
		uint64_t v = ((uint64_t)f[14u + i * 2u + 1u] << 32) | f[14u + i * 2u];
		dc_putc('x');
		if (i >= 10)
			dc_putc('0' + (i / 10));
		dc_putc('0' + (i % 10));
		dc_putc('=');
		print_hex64(v);
		dc_putc(((i % 3) == 2) ? '\r' : ' ');
		if ((i % 3) == 2)
			dc_putc('\n');
	}
	newline();
}

/* ------------------------------------------------------------------ *
 * Dispatch one already-tokenized command line. Never crashes: unknown
 * command or bad hex just prints an error and returns.
 * ------------------------------------------------------------------ */
static void exec_line(char *line, struct el2_frame *frame)
{
#if defined(PROD_NO_DBG)
	/* ROADMAP T5 prod lockout (docs/security-notes.md option 2,
	 * -DPROD_NO_DBG): compile the command DISPATCH out entirely. This is
	 * belt-and-suspenders alongside emac.c's RX-accept gate (which already
	 * stops 0x88B5 console frames from ever reaching the byte ring this
	 * function's caller reads from) -- with this flag set, even a line
	 * that somehow made it into the ring (e.g. some future caller of
	 * console_getc()'s underlying ring) is a no-op: no peek/poke/call/gdb,
	 * nothing acted on, ever. */
	(void)line;
	(void)frame;
	return;
#else
	char *tok[MAX_TOKENS];
	int nt = tokenize(line, tok);
	const char *cmd;
	unsigned long a0, a1;

	if (nt == 0)
		return;
	cmd = tok[0];

	if (streq(cmd, "gr")) {
		cmd_gr(frame);
		return;
	}
	if (streq(cmd, "sr")) {
		cmd_sr();
		return;
	}
	if (streq(cmd, "gr1")) {
		cmd_gr1();
		return;
	}
	if (streq(cmd, "sr1")) {
		cmd_sr1();
		return;
	}
	if (streq(cmd, "r")) {
		unsigned long n = 1;
		if (nt < 2 || !parse_hex(tok[1], &a0)) {
			err("usage: r <addr> [n]");
			return;
		}
		if (nt >= 3 && !parse_hex(tok[2], &n)) {
			err("bad count");
			return;
		}
		cmd_read_words(a0, (uint32_t)n);
		return;
	}
	if (streq(cmd, "sd")) {
		cmd_sd(tok, nt);
		return;
	}
	if (streq(cmd, "fi")) {
		cmd_fi(tok, nt);
		return;
	}
	if (streq(cmd, "rb")) {
		unsigned long n = 1;
		if (nt < 2 || !parse_hex(tok[1], &a0)) {
			err("usage: rb <addr> [n]");
			return;
		}
		if (nt >= 3 && !parse_hex(tok[2], &n)) {
			err("bad count");
			return;
		}
		cmd_read_bytes(a0, (uint32_t)n);
		return;
	}
	if (streq(cmd, "d")) {
		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: d <addr> <len>");
			return;
		}
		cmd_dump(a0, (uint32_t)a1);
		return;
	}
	if (streq(cmd, "fbdump")) {
		/* Stream a raw region out over ethertype 0x88B9 for the host to
		 * reassemble -- see fbdump.h for why neither `d`, netcon nor
		 * coredump can carry a framebuffer. Fire-and-forget: gaps are
		 * the host's problem, and it re-requests them as narrower
		 * ranges. */
		int r;

		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: fbdump <addr> <len>");
			return;
		}
		r = fbdump_send((uint32_t)a0, (uint32_t)a1);
		if (r < 0) {
			cputs(r == -1 ? "fbdump: outside guest DRAM\r\n" :
			      r == -2 ? "fbdump: too large\r\n" :
					"fbdump: zero length\r\n");
			return;
		}
		cputs("fbdump: frames=");
		print_hex64((uint64_t)r);
		cputs("\r\n");
		return;
	}
	if (streq(cmd, "gva")) {
		if (nt < 2 || !parse_hex(tok[1], &a0)) {
			err("usage: gva <addr>");
			return;
		}
		cmd_gva(a0);
		return;
	}
	if (streq(cmd, "w")) {
		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: w <addr> <val>");
			return;
		}
		cmd_write_word(a0, a1);
		return;
	}
	if (streq(cmd, "wb")) {
		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: wb <addr> <val>");
			return;
		}
		cmd_write_byte(a0, a1);
		return;
	}
	if (streq(cmd, "hold")) {
		/* Arm the pause-before-guest-entry gate (main_dbg.c, checked right
		 * before kload_enter()). Only takes effect if this write reaches
		 * DRAM before a SUBSEQUENT warm reset AND before main_dbg.c's
		 * check point on THIS boot hasn't happened yet -- i.e. call this,
		 * then hv.wdt_reset(), never the reverse. See dbgtools.h. */
		dbgtools_hold_set();
		cputs("hold armed (HVMAP_DBGTOOLS_HOLD=1) -- takes effect on the "
		      "NEXT warm reset, not this boot\r\n");
		return;
	}
	if (streq(cmd, "release")) {
		/* Let a currently-held CPU0 (main_dbg.c's hold loop) proceed into
		 * kload_enter(). No-op if nothing is currently held. */
		dbgtools_release_set();
		cputs("release signaled (HVMAP_DBGTOOLS_RELEASE=1)\r\n");
		return;
	}
	if (streq(cmd, "phycycle")) {
		/* DISABLED 2026-08-30 — see emac.c's note. ANY EL2 RSB access
		 * races the guest's iichb1/axp8xx driver (the guest owns the
		 * bus); the one live attempt wedged the guest's interrupt path
		 * (vtnet dead, cpu3 frozen). The physical PHY power-cut repro
		 * lives guest-side now: /tmp/phycut.c over /dev/iic1. */
		cputs("phycycle: disabled -- EL2 must not touch the RSB while the "
		      "guest owns it (measured wedge 2026-08-30). Use the guest-"
		      "side phycut (I2CRDWR /dev/iic1) instead.\r\n");
		return;
	}
	if (streq(cmd, "wdtest")) {
		/* Watchdog give-up/escalation ladder test: software "PHY refuses
		 * to train" — attempts 1..7 (~48 s), BC_STAGE_WD_GAVEUP, one-shot
		 * return 1 -> dbg_emac_watchdog_reboot -> wdt_debug_hold -> WDOG
		 * reboot -> U-Boot -> TFTP. The console NEVER goes down (MDIO is
		 * never touched), so the whole ladder is observable live and
		 * cancellable with `wdtest off` up to the give-up. */
		if (nt >= 2 && streq(tok[1], "off")) {
			emac_wd_test_mode(0);
			cputs("wdtest: off -- watchdog and phy_startup back to normal\r\n");
			return;
		}
		emac_wd_test_mode(1);
		cputs("wdtest: ON -- phy_startup now fails in software; the link "
		      "watchdog will re-kick 6x (~48 s), give up, and reboot via "
		      "WDOG+TFTP. Cancel with `wdtest off`.\r\n");
		return;
	}
	if (streq(cmd, "vcpu2")) {
		/* Arm/disarm the SECOND GUEST vCPU on CPU2 (vcpu2.c). Off by
		 * default: with the gate clear, CPU2 keeps doing async eMMC I/O
		 * and a guest PSCI CPU_ON for affinity 2 is answered
		 * ALREADY_ON exactly as before.
		 *
		 * Arming this alone changes nothing visible -- the guest also
		 * has to ASK, which means booting it with a device tree that
		 * advertises cpu@2. The shipped bananapi-min.dtb lists only
		 * cpu@0, which is why hw.ncpu has always read 1. */
		if (nt > 1 && streq(tok[1], "on")) {
			dbg_vcpu2 = 1u;
			cputs("vcpu2: ARMED -- CPU2 will become a guest vCPU on "
			      "the next guest CPU_ON for affinity 2\r\n");
		} else if (nt > 1 && streq(tok[1], "off")) {
			dbg_vcpu2 = 0u;
			cputs("vcpu2: disarmed (CPU2 stays an HV async-I/O "
			      "worker)\r\n");
		} else {
			volatile uint32_t *bc =
			    (volatile uint32_t *)HVMAP_VCPU2_BC;
			static const char *const st[] = {
				"not reached", "parked", "request accepted",
				"entering EL1"
			};
			static const char *const why[] = {
				"accepted", "gate off", "already handed over",
				"not parked yet"
			};
			uint32_t sv = bc[1], wv = bc[7];

			cputs("vcpu2: gate=");
			cputs(dbg_vcpu2 ? "ARMED" : "off");
			cputs("  state=");
			cputs(bc[0] == VCPU2_MAGIC
			      ? (sv < 4u ? st[sv] : "?")
			      : "window not published");
			cputs("  requests=");
			print_hex32(bc[2]);
			cputs(" refused=");
			print_hex32(bc[6]);
			cputs(" last=");
			cputs(wv < 4u ? why[wv] : "?");
			cputs("\r\n  usage: vcpu2 [on|off]\r\n");
		}
		return;
	}
	if (streq(cmd, "vcpu3")) {
		/* Arm/disarm the FOURTH GUEST vCPU on CPU3 (vcpu3.c). Same
		 * contract as `vcpu2` above -- off by default, arming it alone
		 * changes nothing visible until the guest is also booted with a
		 * device tree that advertises cpu@3. On the `dual` target this
		 * command is inert by construction: vcpu3.o is never linked
		 * there (vcpu3.c is link-time mutually exclusive with
		 * zguest_cpu3.c, see vcpu3.h), so dbg_vcpu3/vcpu3_request()
		 * resolve to el2_exc.c's weak always-refuse stubs and this
		 * reports "gate=off" / "window not published" no matter what. */
		if (nt > 1 && streq(tok[1], "on")) {
			dbg_vcpu3 = 1u;
			cputs("vcpu3: ARMED -- CPU3 will become a guest vCPU on "
			      "the next guest CPU_ON for affinity 3\r\n");
		} else if (nt > 1 && streq(tok[1], "off")) {
			dbg_vcpu3 = 0u;
			cputs("vcpu3: disarmed\r\n");
		} else {
			volatile uint32_t *bc =
			    (volatile uint32_t *)HVMAP_VCPU3_BC;
			static const char *const st[] = {
				"not reached", "parked", "request accepted",
				"entering EL1"
			};
			static const char *const why[] = {
				"accepted", "gate off", "already handed over",
				"not parked yet"
			};
			uint32_t sv = bc[1], wv = bc[7];

			cputs("vcpu3: gate=");
			cputs(dbg_vcpu3 ? "ARMED" : "off");
			cputs("  state=");
			cputs(bc[0] == VCPU3_MAGIC
			      ? (sv < 4u ? st[sv] : "?")
			      : "window not published");
			cputs("  requests=");
			print_hex32(bc[2]);
			cputs(" refused=");
			print_hex32(bc[6]);
			cputs(" last=");
			cputs(wv < 4u ? why[wv] : "?");
			cputs("\r\n  usage: vcpu3 [on|off]\r\n");
		}
		return;
	}
	if (streq(cmd, "type")) {
		/* Type text into the guest's virtual keyboard (vinput.c) — each
		 * token gets a synthesized space before it (tok[1] excepted), since
		 * tokenize() has already destroyed the original whitespace run
		 * lengths. No trailing Enter: chain with `key enter` when one is
		 * wanted, mirroring guest_sh.py's own console-inject convention of
		 * not assuming what the operator wants to happen next. */
		if (nt < 2) {
			err("usage: type <text...>");
			return;
		}
		for (int i = 1; i < nt; i++) {
			if (i > 1)
				vinput_send_ascii(' ');
			for (char *c = tok[i]; *c; c++)
				vinput_send_ascii(*c);
		}
		cputs("typed\r\n");
		return;
	}
	if (streq(cmd, "key")) {
		/* Raw key injection for anything `type` can't express (arrows, tab,
		 * enter, backspace, ctrl/alt as modifiers) or explicit press/release
		 * control. `key <name>` sends a full press+release; `key <name> 0|1`
		 * sends just that transition (for holding a modifier across other
		 * keys — not currently exposed as a combo, single-key-at-a-time is
		 * enough for v1). */
		static const struct { const char *name; uint16_t code; } named[] = {
			{ "enter", KEY_ENTER }, { "esc", KEY_ESC },
			{ "tab", KEY_TAB }, { "backspace", KEY_BACKSPACE },
			{ "space", KEY_SPACE },
			{ "up", KEY_UP }, { "down", KEY_DOWN },
			{ "left", KEY_LEFT }, { "right", KEY_RIGHT },
			{ "shift", KEY_LEFTSHIFT }, { "ctrl", KEY_LEFTCTRL },
			{ "alt", KEY_LEFTALT },
		};
		uint16_t code = 0;
		int found = 0;

		if (nt < 2) {
			err("usage: key <name> [0|1]  (name: enter esc tab "
			    "backspace space up down left right shift ctrl alt)");
			return;
		}
		for (uint32_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
			if (streq(tok[1], named[i].name)) {
				code = named[i].code;
				found = 1;
				break;
			}
		}
		if (!found) {
			err("unknown key name");
			return;
		}
		if (nt >= 3) {
			if (streq(tok[2], "0"))
				vinput_send_key(code, 0);
			else if (streq(tok[2], "1"))
				vinput_send_key(code, 1);
			else {
				err("usage: key <name> [0|1]");
				return;
			}
		} else {
			vinput_send_key(code, 1);
			vinput_send_key(code, 0);
		}
		cputs("ok\r\n");
		return;
	}
	if (streq(cmd, "vinput")) {
		/* Status, mirroring `vcpu2`'s own read-only report block: whether
		 * the guest has ever attached (magic/status), the two queues'
		 * ready bits, and the sent/dropped counters -- `dropped` climbing
		 * while `type`/`key` are in use means the guest hasn't posted
		 * eventq buffers yet (driver not attached / not DRIVER_OK), not a
		 * device bug. */
		volatile uint32_t *bc = (volatile uint32_t *)HVMAP_VINPUT_BC;

		cputs("vinput: ");
		if (bc[0] != 0x56494E31u) {
			cputs("window not published\r\n");
			return;
		}
		cputs("status=");
		print_hex32(bc[1]);
		cputs(" eventq_ready=");
		print_hex32(bc[2]);
		cputs(" statusq_ready=");
		print_hex32(bc[3]);
		cputs(" sent=");
		print_hex32(bc[4]);
		cputs(" dropped=");
		print_hex32(bc[5]);
		cputs(" irqs=");
		print_hex32(bc[6]);
		cputs("\r\n");
		return;
	}
	if (streq(cmd, "vcpu1")) {
		/* Arm/disarm the THIRD GUEST vCPU on CPU1 (vcpu1.c) — EXPERIMENTAL,
		 * see vcpu1.h for the full tradeoff before arming this on a board
		 * you care about recovering automatically: CPU1 stops being an
		 * independent crash-recovery witness and becomes a guest core
		 * whose watchdog kick/dbgmon service now depend on a GIC-priority
		 * tick surviving whatever the guest does SOFTWARE-side (it does,
		 * via the same IMO=1 mechanism CPU0 already relies on) but NOT a
		 * hardware-level wedge of this specific core (it does not).
		 *
		 * Off by default. The guest also has to ASK (cpu@1 in the device
		 * tree) — same two-sided gate as vcpu2. */
		if (nt > 1 && streq(tok[1], "on")) {
			dbg_vcpu1 = 1u;
			cputs("vcpu1: ARMED -- CPU1 will become a guest vCPU on "
			      "the next guest CPU_ON for affinity 1. CPU1 no longer "
			      "independently witnesses a wedge on this core -- see "
			      "vcpu1.h before relying on this.\r\n");
		} else if (nt > 1 && streq(tok[1], "off")) {
			dbg_vcpu1 = 0u;
			cputs("vcpu1: disarmed (CPU1 stays the debug/EMAC/watchdog "
			      "core)\r\n");
		} else {
			volatile uint32_t *bc =
			    (volatile uint32_t *)HVMAP_VCPU1_BC;
			static const char *const st[] = {
				"not reached", "parked", "request accepted",
				"entering EL1"
			};
			static const char *const why[] = {
				"accepted", "gate off", "already handed over",
				"not parked yet"
			};
			uint32_t sv = bc[1], wv = bc[7];

			cputs("vcpu1: gate=");
			cputs(dbg_vcpu1 ? "ARMED" : "off");
			cputs("  state=");
			cputs(bc[0] == VCPU1_MAGIC
			      ? (sv < 4u ? st[sv] : "?")
			      : "window not published");
			cputs("  requests=");
			print_hex32(bc[2]);
			cputs(" refused=");
			print_hex32(bc[6]);
			cputs(" last=");
			cputs(wv < 4u ? why[wv] : "?");
			cputs("\r\n  usage: vcpu1 [on|off]\r\n");
		}
		return;
	}
	if (streq(cmd, "zboot")) {
		/* Dual-guest milestone: wake CPU3's wfe-poll loop (zguest_cpu3.c)
		 * and start the one-way Zephyr boot sequence -- see
		 * zguest_cpu3.h's lifecycle comment for the full staging protocol
		 * (an operator writes the raw Zephyr ELF into DRAM at
		 * ZG3_ELF_STAGE_PA BEFORE issuing this command). Weak: no-op with
		 * a clear message in a build that doesn't link zguest_cpu3.o
		 * (every existing target today), same pattern as the `gdb`
		 * command's gdbstub_init weak guard just above. */
		extern void zguest_cpu3_start_set(void) __attribute__((weak));
		if (zguest_cpu3_start_set) {
			zguest_cpu3_start_set();
			cputs("zboot: CPU3 start requested\r\n");
		} else {
			cputs("zboot: not built with dual-guest support\r\n");
		}
		return;
	}
	if (streq(cmd, "zstage")) {
		/* Re-run the second guest's boot-time copy-in NOW, so a retry
		 * after a failed `zboot` has a supported way to get a corrected
		 * image to where zload2 reads it. Full rationale, and why writing
		 * straight to ZG3_ELF_STAGE_PA is NOT that way, in zstage.h.
		 * Supported retry order: stage into the landing window ->
		 * `zstage` -> `zunhalt` -> `zboot`. Weak-guarded exactly like
		 * `zboot`/`zunhalt`. */
		extern unsigned long zstage_restage(void) __attribute__((weak));
		if (zstage_restage) {
			unsigned long n = zstage_restage();
			if (n) {
				cputs("zstage: copied 0x");
				print_hex32((uint32_t)n);
				cputs(" bytes into the 2nd guest's slice\r\n");
			} else {
				cputs("zstage: nothing usable staged at the landing "
				      "window -- destination wiped (see ZSTG bc)\r\n");
			}
		} else {
			cputs("zstage: not built with dual-guest support\r\n");
		}
		return;
	}
	if (streq(cmd, "zunhalt")) {
		/* Release CPU3 from the halt it entered after a FAILED `zboot`
		 * (breadcrumb 0xBAD1 = image would not parse/place, 0xBAD2 =
		 * stage-2 isolation self-check failed), back to the parked state
		 * so a corrected image can be staged and `zboot` retried without
		 * reloading the whole board.
		 *
		 * This does NOT weaken the halt-loud safety property -- CPU3
		 * still stops dead and publishes why, and only a human who has
		 * read that breadcrumb can release it. See zguest_cpu3.h's
		 * zguest_cpu3_rearm_set() comment for the full argument, and note
		 * that re-arming does not fix whatever caused the failure: the
		 * same image will fail the same way. Weak-guarded exactly like
		 * `zboot` above. */
		extern void zguest_cpu3_rearm_set(void) __attribute__((weak));
		if (zguest_cpu3_rearm_set) {
			zguest_cpu3_rearm_set();
			cputs("zunhalt: CPU3 re-arm requested "
			      "(no-op unless halted)\r\n");
		} else {
			cputs("zunhalt: not built with dual-guest support\r\n");
		}
		return;
	}
	if (streq(cmd, "t")) {
		cmd_t();
		return;
	}
	if (streq(cmd, "gdb")) {
		/* ROADMAP B2: hand the EMAC 0x88B5 channel over to the GDB RSP stub.
		 * From here the CPU1 debug loop (smp.c) hosts gdbstub instead of this
		 * text monitor, and el2_trap diverts guest bp/step/wp to it — so a
		 * host `gdb; target remote :<port-bridge>` can drive the live guest.
		 * Weak: no-op in a build that somehow lacks gdbstub.o. */
		extern volatile uint32_t gdb_channel __attribute__((weak));
		extern void gdbstub_init(void) __attribute__((weak));
		if (&gdb_channel) {
			if (gdbstub_init)
				gdbstub_init();
			gdb_channel = 1u;
			__asm__ volatile("dsb sy" ::: "memory");
			cputs("gdb: RSP stub now owns this channel — "
			      "point `target remote` at the bridge.\r\n");
		} else {
			cputs("gdb: not built into this image.\r\n");
		}
		return;
	}
	if (streq(cmd, "ff")) {
		cmd_ff();
		return;
	}
	if (streq(cmd, "ffv")) { cmd_ffv(); return; }
	if (streq(cmd, "ss")) {
		int on = el2_ss_toggle(frame);
		volatile uint32_t *s = (volatile uint32_t *)0x50002800UL;
		cputs("single-step ");
		cputs(on ? "ON" : "OFF");
		cputs("  steps=");  print_hex32(s[1]);
		cputs(" head=");    print_hex32(s[2]);
		cputs("  (ring @ 0x50002800: r 50002808 <n>)\r\n");
		return;
	}
	if (streq(cmd, "pt")) {
		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: pt <ttbr_pa> <va>");
			return;
		}
		cmd_pt(a0, a1);
		return;
	}
	if (streq(cmd, "bp")) {
		if (nt < 2 || !parse_hex(tok[1], &a0)) { err("usage: bp <va>"); return; }
		cmd_bp(a0, frame);
		return;
	}
	if (streq(cmd, "wp")) {
		if (nt < 2 || !parse_hex(tok[1], &a0)) { err("usage: wp <va>"); return; }
		cmd_wp(a0, frame);
		return;
	}
	if (streq(cmd, "bpc")) { cmd_bpc(); return; }
	if (streq(cmd, "bpl")) { cmd_bpl(); return; }
	if (streq(cmd, "bt"))  { cmd_bt(frame); return; }
	if (streq(cmd, "sr2")) { cmd_sr2(); return; }
	if (streq(cmd, "sw")) {
		if (nt < 3 || !parse_hex(tok[2], &a1)) { err("usage: sw <name> <val>"); return; }
		cmd_sw(tok[1], (uint64_t)a1);
		return;
	}
	if (streq(cmd, "call")) {
		uint64_t a0v=0,a1v=0,a2v=0,a3v=0;
		if (nt < 2 || !parse_hex(tok[1], &a0)) { err("usage: call <pa> [x0] [x1] [x2] [x3]"); return; }
		if (nt > 2) parse_hex(tok[2], &a0v);
		if (nt > 3) parse_hex(tok[3], &a1v);
		if (nt > 4) parse_hex(tok[4], &a2v);
		if (nt > 5) parse_hex(tok[5], &a3v);
		cmd_call((uint64_t)a0, a0v, a1v, a2v, a3v);
		return;
	}
	if (streq(cmd, "patch")) {
		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: patch <pa> <word>"); return;
		}
		cmd_patch((uint64_t)a0, (uint32_t)a1);
		return;
	}
	if (streq(cmd, "poweroff") || streq(cmd, "off")) {
		cmd_poweroff();
		return;
	}
	if (streq(cmd, "h") || streq(cmd, "?")) {
		cmd_help();
		return;
	}
	if (streq(cmd, "bmc")) {
		/* Hand tokens AFTER "bmc" to the management-plane dispatcher. */
		bmc_dispatch(&tok[1], nt - 1, frame);
		return;
	}

	cputs("?\r\n");
#endif /* !PROD_NO_DBG */
}

/* ------------------------------------------------------------------ *
 * Public API.
 * ------------------------------------------------------------------ */

void dbgmon_init(void)
{
	cputs("bzdOS live hv-debugger -- h for help\r\n");
	cputs("dbg> ");
	console_flush();
}

/* Line buffer persists across ticks (static): each call to dbgmon_service()
 * appends whatever console RX is available right now onto it, and only
 * acts once a full line ('\r' or '\n') has arrived. Bounded per call by
 * DBGMON_MAX_RX_PER_TICK -- console_getc() itself is non-blocking (-1 when
 * empty) so the drain loop below terminates immediately once the RX FIFO is
 * empty; the extra cap just guards against a pathological flood of already-
 * buffered bytes eating an unbounded amount of tick time. */
#define DBGMON_LINE_MAX        128
#define DBGMON_MAX_RX_PER_TICK 64

/* Phase breadcrumb for localising a debug-core (CPU1) wedge inside this
 * function: word0 = phase, word1 = first 4 bytes of the command being exec'd.
 * DRAM (0x50000a00) so it survives a warm WDT reset for post-mortem `md`. */
static inline void dbgmon_phase(uint32_t ph, uint32_t cmd4)
{
	volatile uint32_t *p = (volatile uint32_t *)0x50000a00UL;
	p[0] = ph;
	p[1] = cmd4;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * BZDBG -- HV dbgmon lines arriving over USB-ACM (`~BZDBG<line>` from the
 * host, usbacm.c queues them). dbgmon_service() drains the queue BEFORE
 * its own console poll so the two input sources never interleave a line,
 * runs each line through the normal exec_line with the capture sink on,
 * and hands the captured text back to usbacm for the ~BZDBG< return path.
 * This is the EMAC-dark lifeline: the monitor stays fully drivable over
 * the USB console when the wire is down.
 * ------------------------------------------------------------------ */
#define BZDBG_LINE_MAX   DBGMON_LINE_MAX
/* BZDBG pipeline breadcrumbs, VCPU3_BC free tail @0x5009E4C0 (the pcore
 * attribution block ends at +0xC0): [0]=posted [1]=executed [2]=replies
 * handed to usbacm [3]=last stage stuck at (1=posted 2=executed 3=reply).
 * Readable over EMAC while the board lives -- this is how the BZDBG path
 * is debugged without printf access to itself. */
#define BZDBG_BC_BASE    (HVMAP_VCPU3_BC + 0xC0u)
static void bzdbg_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(BZDBG_BC_BASE + i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static char    bzdbg_line[BZDBG_LINE_MAX];
static volatile int bzdbg_pending = 0;

void dbgmon_bzdbg_post(const char *line)
{
	int i;
	for (i = 0; line[i] && i < BZDBG_LINE_MAX - 1; i++)
		bzdbg_line[i] = line[i];
	bzdbg_line[i] = '\0';
	__asm__ volatile("dsb sy" ::: "memory");
	bzdbg_pending = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	{
		static volatile uint32_t *bc =
		    (volatile uint32_t *)(BZDBG_BC_BASE + 0u);
		bc[0]++;
		bc[3] = 1;
		__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(&bc[0]) : "memory");
	}
}

/*usbacm.c reads these to drain the answer into the ACM TX ring. */
extern void el2_snapshot_guest_frame(struct el2_frame *out);  /* seqlock read (H8), el2_exc.c */
volatile int        bzdbg_reply_ready = 0;
volatile uint32_t   bzdbg_reply_len = 0;
const char         *bzdbg_reply_buf = g_capture_buf;

void dbgmon_bzdbg_poll(void)
{
	char *out;
	int   len;

	if (!bzdbg_pending)
		return;
	if (g_capture_on)
		return;   /* never re-enter while capturing */
	{
		static volatile uint32_t *bc =
		    (volatile uint32_t *)(BZDBG_BC_BASE + 0u);
		bc[1]++;
		bc[3] = 2;
		__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(&bc[1]) : "memory");
	}

	console_capture_start(g_capture_buf, (int)sizeof(g_capture_buf) - 1);
	{
		/* Same snapshot discipline as the tick path's dbgmon_service call:
		 * a consistent copy of CPU0's last guest frame, so gr/sr/gva keep
		 * their meaning regardless of which core is guesting. */
		struct el2_frame snap;
		el2_snapshot_guest_frame(&snap);
		exec_line(bzdbg_line, &snap);
	}
	len = console_capture_end(&out);
	bzdbg_reply_len = (uint32_t)len;

	__asm__ volatile("dsb sy" ::: "memory");
	{
		static volatile uint32_t *bc =
		    (volatile uint32_t *)(BZDBG_BC_BASE + 0u);
		bc[2]++;
		bc[3] = 3;
		__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(&bc[2]) : "memory");
	}
	bzdbg_reply_ready = 1;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
	bzdbg_pending = 0;
}

void dbgmon_service(struct el2_frame *guest_frame)
{
	static char line[DBGMON_LINE_MAX];
	static int len = 0;
	int drained = 0;

	dbgmon_phase(1, 0);
	dbgmon_bzdbg_poll();
	console_poll();
	dbgmon_phase(2, 0);

	while (drained < DBGMON_MAX_RX_PER_TICK) {
		int c = console_getc();

		if (c < 0)
			break;   /* nothing more buffered right now -- return, don't wait */
		drained++;

		if (c == '\r' || c == '\n') {
			line[len] = '\0';
			if (len > 0) {
				uint32_t c4 = (uint32_t)(uint8_t)line[0]
				    | ((uint32_t)(uint8_t)(len>1?line[1]:0) << 8)
				    | ((uint32_t)(uint8_t)(len>2?line[2]:0) << 16)
				    | ((uint32_t)(uint8_t)(len>3?line[3]:0) << 24);
				dbgmon_phase(3, c4);
				exec_line(line, guest_frame);
				dbgmon_phase(4, c4);
			}
			len = 0;
			cputs("dbg> ");
			console_flush();
			continue;
		}

		if (c == 0x7f || c == 0x08) {   /* backspace/DEL */
			if (len > 0)
				len--;
			continue;
		}

		if (len < DBGMON_LINE_MAX - 1) {
			line[len++] = (char)c;
		} else {
			/* Line too long: drop it rather than get stuck -- never let a
			 * runaway/garbage stream wedge the monitor. */
			len = 0;
			err("line too long");
			cputs("dbg> ");
		}
	}

	if (drained > 0)
		console_flush();
	dbgmon_phase(5, 0);
}
