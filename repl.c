/* SPDX-License-Identifier: BSD-2-Clause */

/* repl.c — resident interactive command interpreter for the bzdOS microkernel.
 *
 * A line-oriented "hardware REPL": read a command line from the console, parse
 * it, execute it against live hardware/memory, print the result, and prompt for
 * the next one — forever, without ever reloading or resetting the board. This
 * is the tool that collapses our driver-debug loop from a ~90 s reflash cycle to
 * milliseconds: peek/poke MMIO, `c`all a function, or re-run MUSB bring-up
 * (`mi`/`mp`/`mpN`) live and watch the effect on the host immediately.
 *
 * I/O is done ONLY through the console_* hooks (see repl.h), which main_repl.c
 * backs with the EMAC raw-Ethernet console. MUSB is never used for the REPL's
 * own I/O — it is the device-under-test, so we must be able to drive it into any
 * state (even wedged) without losing our command channel.
 *
 * Freestanding: no libc, only <stdint.h>. All helpers (hex/dec printing, hex
 * parsing, string ops) are hand-rolled below.
 */
#include <stdint.h>
#include "repl.h"
#include "musb.h"
#include "wdt.h"
#include "exceptions.h"
#include "ring.h"
#include "alloc.h"
#include "timer.h"
#include "gic_timer.h"
#include "netcon.h"
#include "guest.h"
#include "sched.h"
#include "ktimer.h"
#include "ksync.h"
#include "wcet.h"
#include "stage2.h"
#include "kload.h"
#include "reboot.h"
#include "hud.h"

/* ------------------------------------------------------------------ *
 * Tiny freestanding I/O + string helpers (no libc available).
 * ------------------------------------------------------------------ */

/* console_putc over a NUL-terminated string. */
static void cputs(const char *s)
{
	while (*s)
		console_putc((int)(unsigned char)*s++);
}

static void newline(void)
{
	console_putc('\r');
	console_putc('\n');
}

static char hexdig(unsigned v)
{
	v &= 0xfu;
	return (char)(v < 10 ? ('0' + v) : ('a' + (v - 10)));
}

static void print_hex8(uint8_t v)
{
	console_putc(hexdig(v >> 4));
	console_putc(hexdig(v));
}

static void print_hex16(uint16_t v)
{
	print_hex8((uint8_t)(v >> 8));
	print_hex8((uint8_t)v);
}

static void print_hex32(uint32_t v)
{
	print_hex8((uint8_t)(v >> 24));
	print_hex8((uint8_t)(v >> 16));
	print_hex8((uint8_t)(v >> 8));
	print_hex8((uint8_t)v);
}

/* Print an address/long: 32-bit-wide if it fits, else full 64-bit. */
static void print_addr(unsigned long v)
{
	if (v >> 32)
		print_hex32((uint32_t)(v >> 32));
	print_hex32((uint32_t)v);
}

static void print_dec(uint32_t v)
{
	char buf[10];
	int i = 0;

	if (v == 0) {
		console_putc('0');
		return;
	}
	while (v > 0 && i < (int)sizeof(buf)) {
		buf[i++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (i > 0)
		console_putc(buf[--i]);
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

/* Parse a hex string (optional 0x/0X prefix) into *out. Returns 1 on success,
 * 0 if the token is empty or contains a non-hex character. */
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

/* ------------------------------------------------------------------ *
 * Command implementations.
 * ------------------------------------------------------------------ */

#define WORDS_PER_LINE 4   /* like U-Boot md.l */
#define BYTES_PER_LINE 16  /* like U-Boot md.b / a hex dump */

/* r/bc: read n 32-bit words from addr, U-Boot md.l style. */
static void cmd_read_words(unsigned long addr, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if ((i % WORDS_PER_LINE) == 0) {
			if (i)
				newline();
			print_addr(addr + (unsigned long)i * 4u);
			console_putc(':');
		}
		console_putc(' ');
		print_hex32(*(volatile uint32_t *)(addr + (unsigned long)i * 4u));
		wdt_pet();
	}
	newline();
}

/* rb: read n bytes from addr. */
static void cmd_read_bytes(unsigned long addr, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if ((i % BYTES_PER_LINE) == 0) {
			if (i)
				newline();
			print_addr(addr + i);
			console_putc(':');
		}
		console_putc(' ');
		print_hex8(*(volatile uint8_t *)(addr + i));
		wdt_pet();
	}
	newline();
}

/* d: hex + ASCII dump of len bytes. */
static void cmd_dump(unsigned long addr, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i += BYTES_PER_LINE) {
		uint32_t j;

		print_addr(addr + i);
		console_putc(':');
		console_putc(' ');
		for (j = 0; j < BYTES_PER_LINE; j++) {
			if (i + j < len)
				print_hex8(*(volatile uint8_t *)(addr + i + j));
			else
				cputs("  ");
			console_putc(' ');
		}
		console_putc('|');
		for (j = 0; j < BYTES_PER_LINE && i + j < len; j++) {
			uint8_t b = *(volatile uint8_t *)(addr + i + j);
			console_putc((b >= 0x20 && b < 0x7f) ? (int)b : '.');
		}
		console_putc('|');
		newline();
		wdt_pet();
	}
}

/* c: call the function at addr and print its return value. We call it through a
 * long(*)(void) prototype: this covers both a void function (whose "return" is
 * simply ignored/garbage, as with U-Boot `go`) and a long-returning one. */
static void cmd_call(unsigned long addr)
{
	long (*fn)(void) = (long (*)(void))addr;
	long ret;

	cputs("calling ");
	print_addr(addr);
	cputs(" ...");
	newline();
	console_flush();

	wdt_pet();
	ret = fn();
	wdt_pet();

	cputs("ret = 0x");
	print_addr((unsigned long)ret);
	newline();
}

static void cmd_help(void)
{
	cputs("bzdOS microkernel REPL commands (hex args):\r\n");
	cputs("  r  <addr> [n]     read n 32-bit words (default 1)\r\n");
	cputs("  rb <addr> [n]     read n bytes (default 1)\r\n");
	cputs("  w  <addr> <val>   write 32-bit word\r\n");
	cputs("  wb <addr> <val>   write byte\r\n");
	cputs("  wh <addr> <val>   write 16-bit halfword\r\n");
	cputs("  c  <addr>         call fn at addr, print return\r\n");
	cputs("  d  <addr> <len>   hex+ascii dump of len bytes\r\n");
	cputs("  mi                musb_init()  (re-run USB bring-up live)\r\n");
	cputs("  mp                musb_poll() once, print usb_ready\r\n");
	cputs("  mpN <n>           musb_poll() n times (bounded, pets WDT)\r\n");
	cputs("  bc [addr] [n]     dump DRAM breadcrumb (default 0x50000000, 24)\r\n");
	cputs("  h | ?             this help\r\n");
}

/* ------------------------------------------------------------------ *
 * Line parsing + dispatch.
 * ------------------------------------------------------------------ */

#define MAX_TOKENS 4

/* Split line in place into up to MAX_TOKENS NUL-terminated tokens on runs of
 * spaces/tabs. Returns the token count. */
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

/* Set by the `reset`/`reboot` command; honoured at the top of repl_run()'s
 * loop, which then stops petting the WDT so the board resets to U-Boot. */
static volatile int g_reboot = 0;

static void exec_line(char *line)
{
	char *tok[MAX_TOKENS];
	int nt = tokenize(line, tok);
	const char *cmd;
	unsigned long a0, a1;

	if (nt == 0)
		return;   /* blank line: just re-prompt */
	cmd = tok[0];

	/* ---- reads: r / rb ---- */
	if (streq(cmd, "r") || streq(cmd, "rb")) {
		unsigned long n = 1;
		if (nt < 2 || !parse_hex(tok[1], &a0)) {
			err("usage: r/rb <addr> [n]");
			return;
		}
		if (nt >= 3 && !parse_hex(tok[2], &n)) {
			err("bad count");
			return;
		}
		if (streq(cmd, "r"))
			cmd_read_words(a0, (uint32_t)n);
		else
			cmd_read_bytes(a0, (uint32_t)n);
		return;
	}

	/* ---- writes: w / wb / wh ---- */
	if (streq(cmd, "w") || streq(cmd, "wb") || streq(cmd, "wh")) {
		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: w/wb/wh <addr> <val>");
			return;
		}
		if (streq(cmd, "w"))
			*(volatile uint32_t *)a0 = (uint32_t)a1;
		else if (streq(cmd, "wb"))
			*(volatile uint8_t *)a0 = (uint8_t)a1;
		else
			*(volatile uint16_t *)a0 = (uint16_t)a1;
		/* echo back what landed */
		print_addr(a0);
		cputs(": ");
		if (streq(cmd, "w"))
			print_hex32(*(volatile uint32_t *)a0);
		else if (streq(cmd, "wb"))
			print_hex8(*(volatile uint8_t *)a0);
		else
			print_hex16(*(volatile uint16_t *)a0);
		newline();
		return;
	}

	/* ---- call ---- */
	if (streq(cmd, "c")) {
		if (nt < 2 || !parse_hex(tok[1], &a0)) {
			err("usage: c <addr>");
			return;
		}
		cmd_call(a0);
		return;
	}

	/* ---- dump ---- */
	if (streq(cmd, "d")) {
		if (nt < 3 || !parse_hex(tok[1], &a0) || !parse_hex(tok[2], &a1)) {
			err("usage: d <addr> <len>");
			return;
		}
		cmd_dump(a0, (uint32_t)a1);
		return;
	}

	/* ---- RTOS self-tests (deterministic; no scheduler/tick needed) ---- */
	if (streq(cmd, "ktt")) {
		ktimer_selftest();
		cputs("ktimer_selftest done (bc 0x50005600)\r\n");
		return;
	}
	if (streq(cmd, "kst")) {
		ksync_selftest();
		cputs("ksync_selftest done (bc 0x50005500)\r\n");
		return;
	}
	if (streq(cmd, "wct")) {
		wcet_selftest();
		cputs("wcet_selftest done (bc 0x50005400)\r\n");
		return;
	}

	/* ---- MUSB (device-under-test) controls ---- */
	if (streq(cmd, "mi")) {
		cputs("musb_init() ...");
		newline();
		console_flush();
		musb_init();
		cputs("musb_init done\r\n");
		return;
	}
	if (streq(cmd, "mp")) {
		int r = musb_poll();
		cputs("usb_ready = ");
		print_dec((uint32_t)(r ? 1 : 0));
		newline();
		return;
	}
	if (streq(cmd, "mpN")) {
		unsigned long n;
		int r = 0;
		unsigned long i;
		if (nt < 2 || !parse_hex(tok[1], &n)) {
			err("usage: mpN <n>");
			return;
		}
		for (i = 0; i < n; i++) {
			r = musb_poll();
			wdt_pet();   /* bounded loop, but keep the WDT fed */
		}
		cputs("polled 0x");
		print_addr(n);
		cputs(" times, usb_ready = ");
		print_dec((uint32_t)(r ? 1 : 0));
		newline();
		return;
	}

	/* ---- breadcrumb window ---- */
	if (streq(cmd, "bc")) {
		unsigned long addr = 0x50000000UL;
		unsigned long n = 24;
		if (nt >= 2 && !parse_hex(tok[1], &addr)) {
			err("bad addr");
			return;
		}
		if (nt >= 3 && !parse_hex(tok[2], &n)) {
			err("bad count");
			return;
		}
		cmd_read_words(addr, (uint32_t)n);
		return;
	}

	/* ---- remote reset: stop feeding the WDT so it returns us to U-Boot ---- */
	if (streq(cmd, "reset") || streq(cmd, "reboot")) {
		g_reboot = 1;   /* honoured at the top of repl_run()'s loop */
		return;
	}

	/* ---- reliable hot-reload / readback over netcon (ethertype 0x88B6,
	 * seq/ack/crc32 stop-and-wait — survives the lossy link). `nrx` receives
	 * a blob into memory (then `c <addr>` runs it); `ntx` reliably sends a
	 * memory range back to the host. These replace the raw `rx` for anything
	 * that must not drop a frame. */
	if (streq(cmd, "nrx")) {
		unsigned long addr = 0, len = 0;
		int n;
		if (nt < 3 || !parse_hex(tok[1], &addr) || !parse_hex(tok[2], &len)) {
			cputs("usage: nrx <addr> <len>\r\n");
			return;
		}
		cputs("nrx: receiving over netcon...\r\n");
		console_flush();
		n = netcon_recv((uint32_t)addr, (uint32_t)len, 100000u);
		/* make received code executable (clean D + invalidate I to PoU) */
		{
			unsigned long o;
			for (o = 0; o < (unsigned long)n; o += 64)
				__asm__ volatile("dc cvac, %0" :: "r"(addr + o) : "memory");
			__asm__ volatile("dsb ish");
			for (o = 0; o < (unsigned long)n; o += 64)
				__asm__ volatile("ic ivau, %0" :: "r"(addr + o) : "memory");
			__asm__ volatile("dsb ish; isb");
		}
		cputs("nrx got=");
		print_hex32((uint32_t)n);
		cputs("\r\n");
		return;
	}
	if (streq(cmd, "ntx")) {
		unsigned long addr = 0, len = 0;
		if (nt < 3 || !parse_hex(tok[1], &addr) || !parse_hex(tok[2], &len)) {
			cputs("usage: ntx <addr> <len>\r\n");
			return;
		}
		if (netcon_send((const uint8_t *)addr, (uint32_t)len))
			cputs("ntx ok\r\n");
		else
			cputs("ntx failed\r\n");
		return;
	}

	/* ---- EL2 exception self-test: install our vector table, then take a
	 * deliberate synchronous trap (brk #0). el2_trap records ESR/ELR/FAR to
	 * the 0x50000400 breadcrumb and advances ELR past the brk, so if the
	 * vector is correct we return alive and print "caught". Read the capture
	 * with `bc 0x50000400 c`. This proves we own EL2 traps — the RTOS bedrock. */
	if (streq(cmd, "exc")) {
		el2_install();
		cputs("installed EL2 vectors; firing brk #0...\r\n");
		console_flush();
		__asm__ volatile("brk #0");
		cputs("survived brk -> vector caught it (see bc 0x50000400)\r\n");
		return;
	}

	/* ---- RTOS-primitive self-tests (results in their breadcrumbs) ---- */
	if (streq(cmd, "rt")) {   /* lock-free ring self-test -> bc 0x50000600 */
		ring_selftest();
		cputs("ring_selftest done (bc 0x50000600)\r\n");
		return;
	}
	if (streq(cmd, "at")) {   /* static allocator self-test -> bc 0x50000700 */
		alloc_selftest();
		cputs("alloc_selftest done (bc 0x50000700)\r\n");
		return;
	}

	/* ---- start the preemptive RTOS tick + live jitter measurement ----
	 * `tick [period_us_hex]` (default 0x3e8 = 1 ms): install EL2 vectors, arm
	 * the CNTHP timer through the GIC, and unmask IRQ so ticks fire into
	 * el2_trap -> gic_timer_irq. Jitter lands in bc 0x50000500, tick stats in
	 * bc 0x50000800. `notick` re-masks IRQ. This is the determinism proof. */
	if (streq(cmd, "tick")) {
		unsigned long us = 0x3e8;
		if (nt >= 2) parse_hex(tok[1], &us);
		el2_install();
		gic_timer_init((uint32_t)us);
		/* Unmask BOTH IRQ and FIQ (daifclr #3): the timer PPI is now Group1
		 * (IRQ), but el2_trap routes FIQ->gic_timer_irq too, so clearing F as
		 * well is free insurance against the Group0/FIQ failure mode. */
		__asm__ volatile("msr daifclr, #3");   /* unmask IRQ+FIQ -> ticks begin */
		cputs("tick armed + IRQ unmasked (bc 0x50000800 ticks, 0x50000500 jitter)\r\n");
		return;
	}
	if (streq(cmd, "notick")) {
		__asm__ volatile("msr daifset, #2");    /* mask IRQ */
		cputs("IRQ masked\r\n");
		return;
	}

	/* ---- drop to the EL1 guest (hypervisor demo). guest_start_demo() is
	 * noreturn: the CPU runs at EL1 forever, our EL2 CNTP tick preempting it
	 * (bc 0x50000b00: [1]=guest counter climbs = EL1 ran, [2]=preempt count
	 * climbs = EL2 preempted). The REPL loop stops (we're at EL1), so NOTHING
	 * pets the WDT — we arm it to ~3s first so the board auto-returns to U-Boot,
	 * where repl-client reloads and we read the (DRAM-surviving) breadcrumb.
	 * Requires `tick` first (so the timer is delivering preemptions). */
	/* ---- start the fixed-priority preemptive RT scheduler demo. sched_start()
	 * is noreturn: tick-driven preemption runs the registered tasks; the REPL
	 * loop stops so nothing pets the WDT — arm ~3s so it auto-returns to U-Boot
	 * and we read bc 0x50000a00 (task id, switch count, per-task run_ticks —
	 * task 0 = highest priority should dominate). Requires `tick` first. */
	if (streq(cmd, "sstart")) {
		cputs("starting RT scheduler; ~3s then WDT->U-Boot (read bc 0x50000a00)\r\n");
		console_flush();
		*(volatile uint32_t *)0x01c20cb4UL = 1u;
		*(volatile uint32_t *)0x01c20cb8UL = 0x31u;   /* ~3s */
		*(volatile uint32_t *)0x01c20cb0UL = 0x14AFu;
		el2_install();
		sched_selftest_setup();
		sched_start();        /* noreturn */
		return;               /* unreachable */
	}

	/* ---- boot the REAL FreeBSD kernel as an EL1 guest under our hypervisor.
	 * Preconditions (set up by fbsd-boot.py via U-Boot BEFORE this REPL loaded):
	 * kernel ELF @0x44000000, DTB @0x4a000000, both already in DRAM via TFTP.
	 * Flow: parse ELF -> place segments contiguously @0x46000000 -> build the
	 * FreeBSD modinfo blob @0x4a100000 -> guest EL1 config (HCR.RW=1, SCTLR_EL1
	 * MMU off) -> stage-2 identity map on -> eret into the kernel's physical
	 * entry at EL1. noreturn. We arm a ~6s WDT first: when the kernel faults
	 * (first unmapped MMIO / DTB issue), el2_trap records ESR/ELR/FAR/HPFAR to
	 * 0x50000400 and the WDT returns us to U-Boot to read it. Honest first
	 * milestone: SEE the real kernel execute + where it needs the next thing. */
	if (streq(cmd, "fbsd")) {
		uint64_t mi, entry;
		if (!kload_parse_elf(0x44000000UL)) {
			cputs("fbsd: bad kernel ELF @0x44000000 (tftp it first)\r\n");
			return;
		}
		kload_place_segments(0x44000000UL, 0x46000000UL);
		/* DTB relocated from its TFTP spot (0x4a000000) into the kernel's
		 * early-map window at 0x47200000; modinfo at 0x47400000; mi is the
		 * modulep KVA to hand the kernel (x0). See kload.c EARLY-MAP. */
		mi = kload_build_modinfo(0x4a000000UL, 0x47200000UL, 0x47400000UL);
		entry = kload_entry_pa();
		guest_config();               /* HCR_EL2.RW=1, SCTLR_EL1 MMU off */
		stage2_init();
		stage2_enable();              /* HCR_EL2.VM=1, identity IPA=PA */
		cputs("fbsd: entering FreeBSD locore at EL1 (bc 0x50000d00 KLD1, fault->0x50000400)\r\n");
		console_flush();
		*(volatile uint32_t *)0x01c20cb4UL = 1u;
		*(volatile uint32_t *)0x01c20cb8UL = 0x61u;   /* WDT ~6s */
		*(volatile uint32_t *)0x01c20cb0UL = 0x14AFu;
		kload_enter(entry, mi, 0x4c000000UL);   /* SP_EL1 top; noreturn */
		return;                                  /* unreachable */
	}

	if (streq(cmd, "guest")) {
		cputs("dropping to EL1 guest; ~3s then WDT->U-Boot (read bc 0x50000b00)\r\n");
		console_flush();
		*(volatile uint32_t *)0x01c20cb4UL = 1u;      /* WDOG CFG: reset system */
		*(volatile uint32_t *)0x01c20cb8UL = 0x31u;   /* MODE: enable, ~3s */
		*(volatile uint32_t *)0x01c20cb0UL = 0x14AFu; /* CTRL: load interval */
		guest_start_demo();   /* noreturn: CPU -> EL1 */
		return;               /* unreachable */
	}

	/* ---- HOT RELOAD: receive a code/data blob over the network into memory
	 * and make it executable, WITHOUT a reset/U-Boot/YMODEM round-trip.
	 * `rx <addr> <len>`: read <len> bytes from the console stream into <addr>,
	 * clean D-cache to PoC + invalidate I-cache to PoU over the range (so the
	 * CPU fetches the fresh code, not stale I-cache), and report a checksum so
	 * the host can retry on the lossy link. Then `c <addr>` runs it — JS-style
	 * hot reload: compile a tiny routine, push it, call it, ~2s total. */
	if (streq(cmd, "rx")) {
		unsigned long addr = 0, len = 0, o, got = 0, stall = 0;
		volatile uint8_t *dst;
		uint32_t sum = 0;
		if (nt < 3 || !parse_hex(tok[1], &addr) || !parse_hex(tok[2], &len)) {
			cputs("usage: rx <addr> <len>\r\n");
			return;
		}
		if (len > 0x200000UL) {   /* sanity cap: 2 MB, reject fat-finger lengths */
			cputs("rx: len too big (>2MB)\r\n");
			return;
		}
		cputs("rx: streaming now (hex)\r\n");
		console_flush();
		dst = (volatile uint8_t *)addr;
		/* Wire format is HEX (2 ASCII nibbles per byte): the EMAC console is
		 * text-oriented and truncates a payload at the first NUL, so raw
		 * binary (which contains 0x00) gets cut short. Hex has no NULs. */
		{
			int have_hi = 0; unsigned hi = 0;
			while (got < len) {
				int c, v;
				console_poll();
				wdt_pet();
				c = console_getc();
				if (c < 0) {
					/* REPL-side protection: short no-data timeout (~1-2 s) so a
					 * lost/short stream or bad length NEVER wedges the resident
					 * loop — it aborts and returns to the prompt. Paired with
					 * the 2 MB len cap above. */
					if (++stall > 2000000UL) { cputs("rx timeout\r\n"); break; }
					continue;
				}
				stall = 0;
				if (c >= '0' && c <= '9') v = c - '0';
				else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
				else continue;   /* skip whitespace/pad/non-hex */
				if (!have_hi) { hi = (unsigned)v; have_hi = 1; }
				else {
					uint8_t byte = (uint8_t)((hi << 4) | (unsigned)v);
					dst[got++] = byte;
					sum += byte;
					have_hi = 0;
				}
			}
		}
		for (o = 0; o < got; o += 64)
			__asm__ volatile("dc cvac, %0" :: "r"(addr + o) : "memory");
		__asm__ volatile("dsb ish");
		for (o = 0; o < got; o += 64)
			__asm__ volatile("ic ivau, %0" :: "r"(addr + o) : "memory");
		__asm__ volatile("dsb ish; isb");
		cputs("rx done got=");
		print_hex32((uint32_t)got);
		cputs(" sum=");
		print_hex32(sum);
		cputs("\r\n");
		return;
	}

	/* ---- help ---- */
	if (streq(cmd, "h") || streq(cmd, "?")) {
		cmd_help();
		return;
	}

	/* unknown */
	cputs("?\r\n");
}

/* ------------------------------------------------------------------ *
 * The resident loop.
 * ------------------------------------------------------------------ */

#define LINE_MAX 128

void repl_run(void)
{
	static char line[LINE_MAX];
	int len = 0;
	uint32_t hud_tick = 0;   /* throttle for the on-screen HUD refresh */

	/* Grace window (loop iterations) that the link may stay down before we
	 * let the WDT recover us — rides out brief RGMII autoneg flaps without
	 * rebooting on every glitch. */
	static const int LINK_GRACE = 2000000;
	int link_grace = LINK_GRACE;

	cputs("mk> ");
	console_flush();

	for (;;) {
		int c;

		/* Watchdog policy (the whole point of the resident design): pet the
		 * ~16 s WDT ONLY while the control channel is alive and no reboot was
		 * requested. So a `reset` command OR a sustained link loss STOPS the
		 * pet and the WDT returns us to U-Boot — remote reset with no button,
		 * and automatic recovery if the network dies. An always-pet loop would
		 * be un-resettable by software (the flaw that forced a physical reset). */
		if (g_reboot) {
			cputs("rebooting -> U-Boot (clean USB disconnect + ~2s WDT)\r\n");
			console_flush();
			/* Clean reboot: drop the USB pull-up so the host sees a proper
			 * disconnect, then a ~2s watchdog — avoids the zombie-gadget the
			 * old 0.5s reset caused (host couldn't re-enumerate in time). */
			reboot_clean();     /* noreturn */
		}
		if (console_link_up())
			link_grace = LINK_GRACE;      /* healthy: refill the grace */
		else if (--link_grace <= 0)
			for (;;) { }        /* link gone too long; let the WDT reset us */

		wdt_pet();
		console_poll();

		/* Refresh the on-screen HUD a few times/sec (throttled by loop count;
		 * hud_update only overwrites value fields, no flicker). */
		if ((++hud_tick & 0x3FFFFu) == 0u)
			hud_update((const struct el2_frame *)0);

		c = console_getc();
		if (c < 0)
			continue;   /* nothing received; keep spinning */

		if (c == '\r' || c == '\n') {
			newline();               /* echo end-of-line */
			line[len] = '\0';
			exec_line(line);         /* never allowed to crash the loop */
			len = 0;
			cputs("mk> ");
			console_flush();
			continue;
		}

		/* crude line editing: backspace / delete */
		if (c == 0x08 || c == 0x7f) {
			if (len > 0) {
				len--;
				cputs("\b \b");
				console_flush();
			}
			continue;
		}

		/* buffer + echo printable input; silently drop on overflow */
		if (len < LINE_MAX - 1) {
			line[len++] = (char)c;
			console_putc(c);
			console_flush();
		}
	}
}
