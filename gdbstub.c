/* gdbstub.c — GDB Remote Serial Protocol stub for the bzdOS EL2 hypervisor.
 *
 * See gdbstub.h for the big picture and the caller contract. This file is
 * fully self-contained: <stdint.h> + "exceptions.h" only, all hex/framing
 * helpers hand-rolled, no libc, no FP/SIMD (-mgeneral-regs-only clean). It
 * touches ONLY the byte transport hooks (gdb_getc/putc/flush) and the guest
 * `struct el2_frame *` the debugger passes in.
 *
 * ---------------------------------------------------------------------------
 * PYTHON HOST BRIDGE SPEC (gdb <-> raw Ethernet)
 * ---------------------------------------------------------------------------
 * gdb/lldb speak RSP over a TCP socket; the board speaks it over ethertype
 * 0x88B5 console frames (the SAME channel dbgmon uses — so a gdb build must
 * run gdbstub_poll() on the tick path INSTEAD OF dbgmon_service(), never both,
 * and gdbstub prints no banner). Wire framing == the existing console framing
 * (see repl-cmd.py), i.e. RAW GDB BYTES in the Ethernet payload, no length
 * prefix of our own:
 *
 *   host -> board : dst=ff:ff:ff:ff:ff:ff  src=<host MAC>  etype=0x88B5
 *                   payload = the raw RSP bytes to deliver (NO trailing '\r';
 *                   zero-pad to the 46-byte minimum — the board's emac RX
 *                   stops at the first NUL, and RSP text never contains NUL).
 *   board -> host : dst=ff:ff:ff:ff:ff:ff  src=02:bd:05:00:00:01 etype=0x88B5
 *                   payload = raw RSP bytes; host strips trailing NUL padding
 *                   (payload.split(b"\0",1)[0]) — RSP text has no embedded NUL.
 *
 * Minimal bridge (mirrors repl-cmd.py's AF_PACKET plumbing; run as root):
 *
 *   #!/usr/bin/env python3
 *   import socket, struct, select
 *   IFACE="br0"; ETYPE=0x88B5
 *   BOARD=bytes.fromhex("02bd05000001"); BCAST=b"\xff"*6
 *   raw=socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETYPE))
 *   raw.bind((IFACE, ETYPE)); SRC=raw.getsockname()[4]
 *   def to_board(data):
 *       for i in range(0, len(data), 256):           # keep chunks < 1 frame
 *           p=data[i:i+256]
 *           if len(p)<46: p=p+b"\x00"*(46-len(p))
 *           raw.send(BCAST+SRC+struct.pack("!H",ETYPE)+p)
 *   srv=socket.socket(socket.AF_INET, socket.SOCK_STREAM)
 *   srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
 *   srv.bind(("0.0.0.0",1234)); srv.listen(1)
 *   print("gdb: target remote :1234"); tcp,_=srv.accept(); tcp.setblocking(False)
 *   while True:
 *       r,_,_=select.select([tcp, raw],[],[],1.0)
 *       if tcp in r:
 *           d=tcp.recv(4096)
 *           if not d: break
 *           to_board(d)                                # raw RSP -> board
 *       if raw in r:
 *           f=raw.recv(2048)
 *           if len(f)<14 or struct.unpack("!H",f[12:14])[0]!=ETYPE: continue
 *           if f[6:12]!=BOARD: continue
 *           pl=f[14:].split(b"\x00",1)[0]              # strip NUL padding
 *           if pl: tcp.sendall(pl)                     # board RSP -> gdb
 *
 * Then:  aarch64-linux-gnu-gdb microkernel-dbg.elf  ->  target remote :1234
 *        (lldb: `gdb-remote 1234`). Because the payload is raw RSP, GDB's own
 *        acks/retransmits work end to end; the bridge is a dumb byte pipe.
 * ---------------------------------------------------------------------------
 */
#include <stdint.h>
#include "exceptions.h"
#include "gdbstub.h"
#include "gdbstub_hw.h"

/* Byte transport — provided by main_dbg.c (wired to emac). gdb_getc() MUST
 * pump the EMAC RX ring (e.g. { emac_poll(); return emac_getc(); }) so bytes
 * flow while we spin in the command loop with IRQs masked. */
extern int  gdb_getc(void);
extern void gdb_putc(int c);
extern void gdb_flush(void);

/* ------------------------------------------------------------------ *
 * Small freestanding helpers.
 * ------------------------------------------------------------------ */

static char nyb(unsigned v)
{
	static const char t[] = "0123456789abcdef";
	return t[v & 0xf];
}

static int unhex(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int str_n_eq(const char *a, const char *b, int n)
{
	int i;
	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* Parse a big-endian hex number (as used for m/M/Z addresses & lengths),
 * advancing *pp past the digits. Returns the value; *pp stops at the first
 * non-hex char. */
static uint64_t parse_num(const char **pp)
{
	const char *p = *pp;
	uint64_t v = 0;
	int d;

	while ((d = unhex(*p)) >= 0) {
		v = (v << 4) | (uint64_t)d;
		p++;
	}
	*pp = p;
	return v;
}

/* ------------------------------------------------------------------ *
 * Debug sysreg / cache helpers (mirror el2_exc.c's single-step mechanism
 * and dbgmon/gtrace's self-modifying-code flush).
 * ------------------------------------------------------------------ */

#define SPSR_SS_BIT (1ull << 21)   /* PSTATE.SS  */
#define SPSR_D_BIT  (1ull << 9)    /* PSTATE.D   */

static void set_tde(int on)
{
	uint64_t v;
	__asm__ volatile("mrs %0, mdcr_el2" : "=r"(v));
	if (on) v |= (1ull << 8); else v &= ~(1ull << 8);
	__asm__ volatile("msr mdcr_el2, %0\n\tisb" :: "r"(v) : "memory");
}

static void set_mdscr_ss(int on)
{
	uint64_t v;
	__asm__ volatile("mrs %0, mdscr_el1" : "=r"(v));
	if (on) v |= 1ull; else v &= ~1ull;
	__asm__ volatile("msr mdscr_el1, %0\n\tisb" :: "r"(v) : "memory");
}

/* Clean-to-PoU + invalidate-I on one address after patching an instruction. */
static void isync_patch(unsigned long pa)
{
	__asm__ volatile(
		"dc cvau, %0\n\t"
		"dsb ish\n\t"
		"ic ivau, %0\n\t"
		"dsb ish\n\t"
		"isb"
		:: "r"(pa) : "memory");
}

/* Translate a guest VA through the guest's own stage-1 (AT S1E1R, exactly as
 * dbgmon's `gva`). On success writes the output PA and returns 1; on a
 * translation fault returns 0. Stage-2 is identity for this guest (IPA==PA),
 * so the stage-1 output is directly addressable under our flat EL2 map. */
static int gva_to_pa(uint64_t va, unsigned long *pa)
{
	uint64_t par;
	__asm__ volatile("at s1e1r, %0" :: "r"(va) : "memory");
	__asm__ volatile("isb" ::: "memory");
	__asm__ volatile("mrs %0, par_el1" : "=r"(par));
	if (par & 1ull)
		return 0;
	*pa = (unsigned long)((par & 0x000ffffffffff000ull) | (va & 0xfffull));
	return 1;
}

/* Map an address GDB gave us to a directly-addressable PA: try the guest
 * stage-1 first (guest VAs / high-KVA pointers); if that faults, treat the
 * address as a flat physical address (our EL2 kernel, or guest phys). */
static unsigned long resolve(uint64_t addr)
{
	unsigned long pa;
	if (gva_to_pa(addr, &pa))
		return pa;
	return (unsigned long)addr;   /* direct PA fallback */
}

/* ------------------------------------------------------------------ *
 * RSP framing.
 * ------------------------------------------------------------------ */

#define GDB_BUF 2048

static char     rx_buf[GDB_BUF];
static char     tx_buf[GDB_BUF];       /* last framed packet, for retransmit */
static int      tx_len;

/* Send an already-framed buffer and wait (bounded) for the '+' ack; on '-'
 * resend. Gives up after a large spin so a vanished host can't wedge us. */
static void tx_raw_and_ack(void)
{
	long guard;
	int i, c;

	for (guard = 0; guard < 8; guard++) {
		for (i = 0; i < tx_len; i++)
			gdb_putc((int)(unsigned char)tx_buf[i]);
		gdb_flush();

		/* Wait for +/-. */
		{
			long spin;
			for (spin = 0; spin < 4000000; spin++) {
				c = gdb_getc();
				if (c < 0)
					continue;
				if (c == '+')
					return;         /* acked */
				if (c == '-')
					break;          /* nak -> resend */
				/* stray byte (Ctrl-C, start of next pkt): ignore */
			}
			if (spin >= 4000000)
				return;             /* host gone — stop waiting */
		}
	}
}

/* Frame `payload` as $payload#cc and send it (with ack handling). Our payloads
 * are hex/ASCII and never contain $ # } * — no escaping needed on TX. */
static void gdb_send(const char *payload)
{
	unsigned sum = 0;
	int n = 0;

	tx_buf[n++] = '$';
	while (*payload && n < GDB_BUF - 4) {
		unsigned char ch = (unsigned char)*payload++;
		sum += ch;
		tx_buf[n++] = (char)ch;
	}
	tx_buf[n++] = '#';
	tx_buf[n++] = nyb(sum >> 4);
	tx_buf[n++] = nyb(sum);
	tx_len = n;
	tx_raw_and_ack();
}

static void gdb_send_empty(void) { gdb_send(""); }
static void gdb_send_ok(void)    { gdb_send("OK"); }

/* Receive one packet payload into rx_buf, NUL-terminated. Skips leading acks;
 * handles }-escaping and *-run-length in the body; verifies the checksum and
 * returns '+'/'-' to the host. Returns the decoded length, or -1 on a checksum
 * error (after sending '-', so the caller just retries). Blocks (spins on
 * gdb_getc) until a full packet arrives — used only while the guest is halted,
 * which is the correct "CPU stopped" state. */
/* Read the packet BODY (everything after '$'): the caller has already consumed
 * the opening '$'. See gdb_recv() for the scanning wrapper. */
static int gdb_recv_body(void)
{
	int c, len, expect, last;
	unsigned sum;

	len = 0;
	sum = 0;
	last = -1;
	for (;;) {
		c = gdb_getc();
		if (c < 0)
			continue;
		if (c == '#')
			break;
		sum += (unsigned)(c & 0xff);
		if (c == '}') {                 /* escape: next ^ 0x20 */
			int e;
			do { e = gdb_getc(); } while (e < 0);
			sum += (unsigned)(e & 0xff);
			e ^= 0x20;
			if (len < GDB_BUF - 1)
				rx_buf[len++] = (char)e;
			last = e;
		} else if (c == '*') {          /* run-length: repeat `last` */
			int rc, rep, k;
			do { rc = gdb_getc(); } while (rc < 0);
			sum += (unsigned)(rc & 0xff);
			rep = rc - 29;
			for (k = 0; k < rep && len < GDB_BUF - 1 && last >= 0; k++)
				rx_buf[len++] = (char)last;
		} else {
			if (len < GDB_BUF - 1)
				rx_buf[len++] = (char)c;
			last = c;
		}
	}
	rx_buf[len] = '\0';

	/* Two checksum hex digits. */
	{
		int h, l;
		do { h = gdb_getc(); } while (h < 0);
		do { l = gdb_getc(); } while (l < 0);
		expect = (unhex(h) << 4) | unhex(l);
	}

	if ((int)(sum & 0xff) != expect) {
		gdb_putc('-');
		gdb_flush();
		return -1;
	}
	gdb_putc('+');
	gdb_flush();
	return len;
}

/* Scan the wire for the '$' that opens a packet, then read its body. Blocks
 * (spins on gdb_getc) until a full packet arrives — used only while halted. */
static int gdb_recv(void)
{
	int c;
	for (;;) {
		c = gdb_getc();
		if (c < 0)
			continue;
		if (c == '$')
			return gdb_recv_body();
		/* '+', '-', 0x03 and stray bytes while stopped: ignore. */
	}
}

/* ------------------------------------------------------------------ *
 * Register access — GDB g-packet order: x0..x30, sp, pc, cpsr(32b).
 * ------------------------------------------------------------------ */

#define REG_SP   31
#define REG_PC   32
#define REG_CPSR 33
#define REG_MAX  34

static uint64_t reg_get(struct el2_frame *g, int n)
{
	if (n >= 0 && n <= 30)
		return g->x[n];
	if (n == REG_SP) {
		uint64_t v;
		__asm__ volatile("mrs %0, sp_el1" : "=r"(v));
		return v;
	}
	if (n == REG_PC)
		return g->elr;
	if (n == REG_CPSR)
		return (uint32_t)g->spsr;
	return 0;
}

static void reg_set(struct el2_frame *g, int n, uint64_t v)
{
	if (n >= 0 && n <= 30)
		g->x[n] = v;
	else if (n == REG_SP)
		__asm__ volatile("msr sp_el1, %0" :: "r"(v));
	else if (n == REG_PC)
		g->elr = v;
	else if (n == REG_CPSR)
		g->spsr = (g->spsr & ~0xffffffffull) | (v & 0xffffffffull);
}

/* Emit `bytes` bytes of `val` in little-endian hex into out; returns chars. */
static int emit_le(char *out, uint64_t val, int bytes)
{
	int i, n = 0;
	for (i = 0; i < bytes; i++) {
		unsigned b = (unsigned)((val >> (i * 8)) & 0xff);
		out[n++] = nyb(b >> 4);
		out[n++] = nyb(b);
	}
	return n;
}

/* Read `bytes` bytes of little-endian hex from in; returns the value. */
static uint64_t read_le(const char *in, int bytes)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < bytes; i++) {
		int hi = unhex(in[i * 2]);
		int lo = unhex(in[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			break;
		v |= (uint64_t)((hi << 4) | lo) << (i * 8);
	}
	return v;
}

static int reg_bytes(int n) { return (n == REG_CPSR) ? 4 : 8; }

/* ------------------------------------------------------------------ *
 * Software breakpoints.
 * ------------------------------------------------------------------ */

#define GDB_MAX_BP   16
#define BRK_INSTR    0xD420FA00u   /* BRK #0x7d0 */

static struct {
	int           used;
	uint64_t      addr;    /* as GDB gave it (VA or PA)   */
	unsigned long pa;      /* resolved physical address   */
	uint32_t      orig;    /* saved original instruction  */
} bp_tab[GDB_MAX_BP];

static int bp_insert(uint64_t addr)
{
	int i, free = -1;

	for (i = 0; i < GDB_MAX_BP; i++) {
		if (bp_tab[i].used && bp_tab[i].addr == addr)
			return 1;                       /* already set */
		if (!bp_tab[i].used && free < 0)
			free = i;
	}
	if (free < 0)
		return 0;

	{
		unsigned long pa = resolve(addr);
		volatile uint32_t *p = (volatile uint32_t *)pa;
		bp_tab[free].orig = *p;
		*p = BRK_INSTR;
		isync_patch(pa);
		bp_tab[free].used = 1;
		bp_tab[free].addr = addr;
		bp_tab[free].pa   = pa;
	}
	set_tde(1);   /* ensure guest BRK routes to EL2 */
	return 1;
}

static int bp_remove(uint64_t addr)
{
	int i;
	for (i = 0; i < GDB_MAX_BP; i++) {
		if (bp_tab[i].used && bp_tab[i].addr == addr) {
			volatile uint32_t *p = (volatile uint32_t *)bp_tab[i].pa;
			*p = bp_tab[i].orig;
			isync_patch(bp_tab[i].pa);
			bp_tab[i].used = 0;
			return 1;
		}
	}
	return 1;   /* removing an unknown bp is a no-op success for GDB */
}

static void bp_remove_all(void)
{
	int i;
	for (i = 0; i < GDB_MAX_BP; i++)
		if (bp_tab[i].used)
			bp_remove(bp_tab[i].addr);
}

/* ------------------------------------------------------------------ *
 * Stub state + step arming.
 * ------------------------------------------------------------------ */

static int gdb_attached;
static int gdb_stepping;

int gdbstub_step_active(void) { return gdb_stepping; }
int gdbstub_attached(void)    { return gdb_attached; }

static void arm_step(struct el2_frame *g)
{
	set_tde(1);
	set_mdscr_ss(1);
	if (g) {
		g->spsr |= SPSR_SS_BIT;    /* single-step on the eret */
		g->spsr &= ~SPSR_D_BIT;    /* unmask debug exceptions */
	}
	gdb_stepping = 1;
}

static void disarm_step(struct el2_frame *g)
{
	set_mdscr_ss(0);
	if (g) {
		g->spsr &= ~SPSR_SS_BIT;
		g->spsr |= SPSR_D_BIT;
	}
	gdb_stepping = 0;
}

/* ------------------------------------------------------------------ *
 * target.xml — advertise EXACTLY our g-packet register set so gdb's expected
 * layout matches (x0..x30, sp, pc, cpsr). Without this gdb would expect its
 * built-in aarch64 description (incl. v0..v31/fpsr/fpcr) and reject our g.
 * ------------------------------------------------------------------ */
static const char target_xml[] =
	"<?xml version=\"1.0\"?>"
	"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
	"<target version=\"1.0\">"
	"<architecture>aarch64</architecture>"
	"<feature name=\"org.gnu.gdb.aarch64.core\">"
	"<reg name=\"x0\" bitsize=\"64\"/><reg name=\"x1\" bitsize=\"64\"/>"
	"<reg name=\"x2\" bitsize=\"64\"/><reg name=\"x3\" bitsize=\"64\"/>"
	"<reg name=\"x4\" bitsize=\"64\"/><reg name=\"x5\" bitsize=\"64\"/>"
	"<reg name=\"x6\" bitsize=\"64\"/><reg name=\"x7\" bitsize=\"64\"/>"
	"<reg name=\"x8\" bitsize=\"64\"/><reg name=\"x9\" bitsize=\"64\"/>"
	"<reg name=\"x10\" bitsize=\"64\"/><reg name=\"x11\" bitsize=\"64\"/>"
	"<reg name=\"x12\" bitsize=\"64\"/><reg name=\"x13\" bitsize=\"64\"/>"
	"<reg name=\"x14\" bitsize=\"64\"/><reg name=\"x15\" bitsize=\"64\"/>"
	"<reg name=\"x16\" bitsize=\"64\"/><reg name=\"x17\" bitsize=\"64\"/>"
	"<reg name=\"x18\" bitsize=\"64\"/><reg name=\"x19\" bitsize=\"64\"/>"
	"<reg name=\"x20\" bitsize=\"64\"/><reg name=\"x21\" bitsize=\"64\"/>"
	"<reg name=\"x22\" bitsize=\"64\"/><reg name=\"x23\" bitsize=\"64\"/>"
	"<reg name=\"x24\" bitsize=\"64\"/><reg name=\"x25\" bitsize=\"64\"/>"
	"<reg name=\"x26\" bitsize=\"64\"/><reg name=\"x27\" bitsize=\"64\"/>"
	"<reg name=\"x28\" bitsize=\"64\"/><reg name=\"x29\" bitsize=\"64\"/>"
	"<reg name=\"x30\" bitsize=\"64\"/>"
	"<reg name=\"sp\" bitsize=\"64\" type=\"data_ptr\"/>"
	"<reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\"/>"
	"<reg name=\"cpsr\" bitsize=\"32\"/>"
	"</feature></target>";

/* Serve a qXfer:...:read:annex:offset,length window over `data`. */
static void xfer_reply(const char *data, int total, const char *args)
{
	const char *p = args;
	uint64_t off, len;
	int i, n;

	off = parse_num(&p);
	if (*p == ',') p++;
	len = parse_num(&p);

	if ((int)off >= total) {
		gdb_send("l");                 /* end of object */
		return;
	}
	n = total - (int)off;
	if ((uint64_t)n > len)
		n = (int)len;
	if (n > GDB_BUF - 4)
		n = GDB_BUF - 4;

	tx_buf[0] = ((int)off + n < total) ? 'm' : 'l';
	/* Reuse rx_buf as scratch is unsafe (holds current pkt); build inline. */
	{
		static char xb[GDB_BUF];
		xb[0] = tx_buf[0];
		for (i = 0; i < n; i++)
			xb[i + 1] = data[(int)off + i];
		xb[n + 1] = '\0';
		gdb_send(xb);
	}
}

/* ------------------------------------------------------------------ *
 * Stop reply.
 * ------------------------------------------------------------------ */
static void send_stop(int sig)
{
	char s[24];
	int n = 0;
	s[n++] = 'T';
	s[n++] = nyb((unsigned)sig >> 4);
	s[n++] = nyb((unsigned)sig);
	s[n++] = 't'; s[n++] = 'h'; s[n++] = 'r'; s[n++] = 'e';
	s[n++] = 'a'; s[n++] = 'd'; s[n++] = ':';
	s[n++] = '0'; s[n++] = '1'; s[n++] = ';';
	s[n]   = '\0';
	gdb_send(s);
}

/* ------------------------------------------------------------------ *
 * Command dispatch. Returns a GDB_RUN_* code; GDB_RUN_NONE means "handled,
 * stay stopped, keep reading". c/s/C/S/vCont set the guest PC if an address
 * was supplied and return CONTINUE/STEP. D/k return DETACH.
 * ------------------------------------------------------------------ */
static int dispatch(char *pkt, int len, struct el2_frame *g)
{
	char out[GDB_BUF];
	const char *p = pkt;

	gdb_attached = 1;
	if (len <= 0) { gdb_send_empty(); return GDB_RUN_NONE; }

	switch (pkt[0]) {

	case '?':                                  /* stop reason */
		send_stop(5);
		return GDB_RUN_NONE;

	case 'g': {                                /* read all registers */
		int n = 0, r;
		for (r = 0; r < REG_MAX; r++)
			n += emit_le(out + n, reg_get(g, r), reg_bytes(r));
		out[n] = '\0';
		gdb_send(out);
		return GDB_RUN_NONE;
	}

	case 'G': {                                /* write all registers */
		const char *d = pkt + 1;
		int r, off = 0;
		for (r = 0; r < REG_MAX; r++) {
			int b = reg_bytes(r);
			reg_set(g, r, read_le(d + off, b));
			off += b * 2;
		}
		gdb_send_ok();
		return GDB_RUN_NONE;
	}

	case 'p': {                                /* read one register */
		int r, n;
		p = pkt + 1;
		r = (int)parse_num(&p);
		if (r < 0 || r >= REG_MAX) { gdb_send("E01"); return GDB_RUN_NONE; }
		n = emit_le(out, reg_get(g, r), reg_bytes(r));
		out[n] = '\0';
		gdb_send(out);
		return GDB_RUN_NONE;
	}

	case 'P': {                                /* write one register: P n=val */
		int r;
		p = pkt + 1;
		r = (int)parse_num(&p);
		if (*p == '=') p++;
		if (r < 0 || r >= REG_MAX) { gdb_send("E01"); return GDB_RUN_NONE; }
		reg_set(g, r, read_le(p, reg_bytes(r)));
		gdb_send_ok();
		return GDB_RUN_NONE;
	}

	case 'm': {                                /* read memory: m addr,len */
		uint64_t addr, l;
		int n = 0;
		uint64_t i;
		p = pkt + 1;
		addr = parse_num(&p);
		if (*p == ',') p++;
		l = parse_num(&p);
		if (l > (GDB_BUF - 8) / 2)
			l = (GDB_BUF - 8) / 2;
		for (i = 0; i < l; i++) {
			unsigned long pa = resolve(addr + i);
			uint8_t b = *(volatile uint8_t *)pa;
			out[n++] = nyb(b >> 4);
			out[n++] = nyb(b);
		}
		out[n] = '\0';
		gdb_send(out);
		return GDB_RUN_NONE;
	}

	case 'M': {                                /* write memory: M addr,len:data */
		uint64_t addr, l, i;
		const char *d;
		p = pkt + 1;
		addr = parse_num(&p);
		if (*p == ',') p++;
		l = parse_num(&p);
		if (*p == ':') p++;
		d = p;
		for (i = 0; i < l; i++) {
			int hi = unhex(d[i * 2]);
			int lo = unhex(d[i * 2 + 1]);
			unsigned long pa;
			if (hi < 0 || lo < 0) break;
			pa = resolve(addr + i);
			*(volatile uint8_t *)pa = (uint8_t)((hi << 4) | lo);
			isync_patch(pa);           /* in case of code writes */
		}
		gdb_send_ok();
		return GDB_RUN_NONE;
	}

	case 'c':                                  /* continue [addr] */
		if (pkt[1]) { p = pkt + 1; g->elr = parse_num(&p); }
		return GDB_RUN_CONTINUE;

	case 'C':                                  /* continue with signal [;addr] */
		{
			const char *q = pkt + 1;
			(void)parse_num(&q);       /* skip signal */
			if (*q == ';') { q++; g->elr = parse_num(&q); }
		}
		return GDB_RUN_CONTINUE;

	case 's':                                  /* step [addr] */
		if (pkt[1]) { p = pkt + 1; g->elr = parse_num(&p); }
		return GDB_RUN_STEP;

	case 'S':                                  /* step with signal [;addr] */
		{
			const char *q = pkt + 1;
			(void)parse_num(&q);
			if (*q == ';') { q++; g->elr = parse_num(&q); }
		}
		return GDB_RUN_STEP;

	case 'Z': {                                /* insert breakpoint */
		int type = pkt[1] - '0';
		p = pkt + 2;
		if (*p == ',') p++;
		{
			uint64_t addr = parse_num(&p);
			int kind = 0;
			if (*p == ',') { p++; kind = (int)parse_num(&p); }
			if (type == 0) {            /* Z0,addr,kind (software) */
				if (bp_insert(addr)) gdb_send_ok();
				else                 gdb_send("E01");
			} else {                    /* HW bp / watchpoints */
				int r = gdbstub_hw_insert(type, addr, kind, g);
				if      (r == 1)  gdb_send_ok();
				else if (r == -1) gdb_send("E01"); /* refused: CPSR.D=1 */
				else              gdb_send_empty(); /* full -> GDB uses SW bp */
			}
		}
		return GDB_RUN_NONE;
	}

	case 'z': {                                /* remove breakpoint */
		int type = pkt[1] - '0';
		p = pkt + 2;
		if (*p == ',') p++;
		{
			uint64_t addr = parse_num(&p);
			if (type == 0) bp_remove(addr);
			else           gdbstub_hw_remove(type, addr);
			gdb_send_ok();
		}
		return GDB_RUN_NONE;
	}

	case 'H':                                  /* thread select: OK, 1 thread */
		gdb_send_ok();
		return GDB_RUN_NONE;

	case 'D':                                  /* detach */
		bp_remove_all();
		disarm_step(g);
		gdb_attached = 0;
		gdb_send_ok();
		return GDB_RUN_DETACH;

	case 'k':                                  /* kill — nothing to kill; resume */
		bp_remove_all();
		disarm_step(g);
		gdb_attached = 0;
		return GDB_RUN_DETACH;

	case 'q':                                  /* general query */
		if (str_n_eq(pkt, "qSupported", 10)) {
			gdb_send("PacketSize=1024;qXfer:features:read+;swbreak+;hwbreak+");
		} else if (str_n_eq(pkt, "qXfer:features:read:target.xml:", 31)) {
			xfer_reply(target_xml, (int)(sizeof(target_xml) - 1), pkt + 31);
		} else if (str_n_eq(pkt, "qAttached", 9)) {
			gdb_send("1");             /* attached to an existing process */
		} else if (str_n_eq(pkt, "qC", 2)) {
			gdb_send("QC01");
		} else if (str_n_eq(pkt, "qfThreadInfo", 12)) {
			gdb_send("m01");
		} else if (str_n_eq(pkt, "qsThreadInfo", 12)) {
			gdb_send("l");
		} else if (str_n_eq(pkt, "qHostInfo", 9)) {
			/* lldb: minimal aarch64 host descriptor. */
			gdb_send("cputype:16777228;cpusubtype:0;"
				 "ostype:none;endian:little;ptrsize:8;");
		} else {
			gdb_send_empty();
		}
		return GDB_RUN_NONE;

	case 'v':                                  /* multi-op resume */
		if (str_n_eq(pkt, "vCont?", 6)) {
			gdb_send("vCont;c;C;s;S");
			return GDB_RUN_NONE;
		}
		if (str_n_eq(pkt, "vCont;", 6)) {
			/* Take the FIRST action; single thread, so thread-id is moot. */
			char a = pkt[6];
			if (a == 's' || a == 'S') return GDB_RUN_STEP;
			if (a == 'c' || a == 'C') return GDB_RUN_CONTINUE;
			gdb_send_empty();
			return GDB_RUN_NONE;
		}
		gdb_send_empty();              /* vMustReplyEmpty, vKill, etc. */
		return GDB_RUN_NONE;

	default:
		gdb_send_empty();              /* unsupported -> empty packet */
		return GDB_RUN_NONE;
	}
}

/* Apply a resume decision to the debug sysregs / frame. */
static void apply(int action, struct el2_frame *g)
{
	if (action == GDB_RUN_STEP)
		arm_step(g);
	else
		disarm_step(g);                /* CONTINUE / DETACH: no stepping */
}

/* The stopped command loop: read + dispatch packets until GDB resumes. If
 * `first_open` is set, the opening '$' of the first packet was already consumed
 * by the caller (the poll path), so read its body directly first. */
static int command_loop_ex(struct el2_frame *g, int first_open)
{
	int first = first_open;

	for (;;) {
		extern void wdt_debug_kick(void);   /* CPU1-owned HW WDOG */
		int n;
		int act;

		wdt_debug_kick();                   /* keep the board alive while
						      * gdb dwells at a breakpoint  */
		n = first ? gdb_recv_body() : gdb_recv();
		first = 0;
		if (n < 0)
			continue;              /* bad checksum — GDB will resend */
		act = dispatch(rx_buf, n, g);
		if (act != GDB_RUN_NONE) {
			apply(act, g);
			return act;
		}
	}
}

static int command_loop(struct el2_frame *g)
{
	return command_loop_ex(g, 0);
}

/* ------------------------------------------------------------------ *
 * Public API.
 * ------------------------------------------------------------------ */

void gdbstub_init(void)
{
	int i;
	gdb_attached = 0;
	gdb_stepping = 0;
	tx_len = 0;
	for (i = 0; i < GDB_MAX_BP; i++)
		bp_tab[i].used = 0;
	/* Route guest BRK/step to EL2 up front so a breakpoint set before the
	 * first stop still traps. (MDSCR.SS stays 0 until we actually step.) */
	set_tde(1);
}

void gdbstub_on_debug_event(struct el2_frame *guest, int signal)
{
	char reason[48];

	disarm_step(guest);                    /* we have stopped; clear stepping */
	if (signal == 5 && gdbstub_hw_stop_reason(guest, reason)) {
		char s[64];
		int n = 0;
		s[n++] = 'T'; s[n++] = nyb((unsigned)signal >> 4); s[n++] = nyb((unsigned)signal);
		{ const char *r = reason; while (*r) s[n++] = *r++; }
		s[n++]='t';s[n++]='h';s[n++]='r';s[n++]='e';s[n++]='a';s[n++]='d';
		s[n++]=':';s[n++]='0';s[n++]='1';s[n++]=';'; s[n]='\0';
		gdb_send(s);
	} else {
		send_stop(signal);
	}
	command_loop(guest);                   /* arms `guest` before returning */
}

int gdbstub_poll(struct el2_frame *guest)
{
	int budget;

	/* Bounded scan of whatever RX is buffered right now. */
	for (budget = 0; budget < 512; budget++) {
		int c = gdb_getc();
		if (c < 0)
			break;
		if (c == '+' || c == '-')
			continue;              /* stray ack while running */
		if (c == 0x03) {               /* Ctrl-C: interrupt the guest */
			send_stop(2);          /* SIGINT */
			return command_loop(guest);
		}
		if (c == '$') {
			/* A real command arrived: GDB is interacting, so the target
			 * must present as stopped. We've already consumed the opening
			 * '$', so enter the command loop with first_open=1 to read
			 * THIS packet's body directly (no byte lost), then service all
			 * subsequent traffic until GDB resumes. */
			return command_loop_ex(guest, 1);
		}
		/* other bytes while running: ignore */
	}
	return GDB_RUN_NONE;
}
