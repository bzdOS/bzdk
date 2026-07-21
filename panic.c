/* panic.c — persistent panic log implementation. See panic.h. */
#include <stdint.h>
#include "panic.h"
#include "exceptions.h"

/* Backtrace walker (backtrace.c). Defensive: never faults on a bad fp. */
extern int backtrace_walk(uint64_t pc, uint64_t fp, uint64_t lr,
                          uint64_t *out, int max);

/* Network console (emac.c) — always linked in the dbg/repl builds that use
 * this. We print the post-mortem over the wire so a sniffer sees it. */
extern void emac_puts(const char *s);
extern void emac_flush(void);

/* Optional local console hook (main_*.c define console_putc = emac_putc etc.).
 * Weak so panic.o links even in a build that has no separate console. */
__attribute__((weak)) void console_putc(int c) { (void)c; }
__attribute__((weak)) void console_flush(void) { }

/* Optional HUD sink: the HUD lane (hud.c) can define this to paint the last
 * panic on-screen. Weak no-op default so builds without a HUD still link. */
__attribute__((weak)) void hud_show_panic(const volatile struct panic_record *p)
{
	(void)p;
}

/* ------------------------------------------------------------------ */

static volatile struct panic_record *const PR =
	(volatile struct panic_record *)PANIC_BASE;

/* Cached this-boot sequence, set by panic_dump_if_present(). .bss is zeroed
 * every boot, so 0 means "dump-if-present has not run yet". */
static uint32_t this_boot_seq;

/* Cache-clean [base, base+len) so the record survives a warm reset with the
 * D-cache on (same dc civac + dsb sy as the exception breadcrumb). */
static void pr_clean(void)
{
	uintptr_t p   = PANIC_BASE;
	uintptr_t end = PANIC_BASE + sizeof(struct panic_record);

	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void pr_copy_reason(const char *reason)
{
	uint32_t i = 0;

	if (reason)
		for (; i < PANIC_REASON_MAX - 1u && reason[i]; i++)
			PR->reason[i] = reason[i];
	for (; i < PANIC_REASON_MAX; i++)
		PR->reason[i] = 0;
}

void panic_record(struct el2_frame *frame, const char *reason)
{
	uint64_t bt[PANIC_BT_MAX];
	int n, i;

	if (!frame)
		return;

	/* Stamp with whatever boot_seq dump-if-present established. If a panic
	 * somehow beats dump-if-present, fall back to the stored/zero value. */
	PR->panic_seq = this_boot_seq ? this_boot_seq : PR->boot_seq;

	PR->esr  = frame->esr;
	PR->elr  = frame->elr;
	PR->far  = frame->far;
	PR->sp   = frame->sp_at_entry;
	PR->spsr = frame->spsr;
	PR->kind = frame->kind;

	for (i = 0; i < 31; i++)
		PR->x[i] = frame->x[i];

	pr_copy_reason(reason);

	/* Compact backtrace: pc=ELR, fp=x29, lr=x30. backtrace_walk is the
	 * defensive AAPCS64 chain walker — it validates every fp before use. */
	n = backtrace_walk(frame->elr, frame->x[29], frame->x[30],
	                   bt, (int)PANIC_BT_MAX);
	if (n < 0)
		n = 0;
	if (n > (int)PANIC_BT_MAX)
		n = (int)PANIC_BT_MAX;
	for (i = 0; i < n; i++)
		PR->bt[i] = bt[i];
	for (; i < (int)PANIC_BT_MAX; i++)
		PR->bt[i] = 0;
	PR->bt_count = (uint32_t)n;

	/* Publish last: magic + valid make the record readable only once fully
	 * written. */
	PR->magic = PANIC_MAGIC;
	PR->valid = 1u;

	pr_clean();
}

int panic_present(void)
{
	return (PR->magic == PANIC_MAGIC && PR->valid == 1u) ? 1 : 0;
}

const volatile struct panic_record *panic_last(void)
{
	return PR;
}

/* ---- printing helpers (network + console) ---- */

/* Mirror a NUL-terminated string to BOTH the local console and the network. */
static void pline(const char *s)
{
	const char *p = s;

	while (*p)
		console_putc(*p++);
	emac_puts(s);
}

static void phex(uint64_t v, int nibbles)
{
	static const char hx[] = "0123456789abcdef";
	char buf[19];
	int i, j = 0;

	buf[j++] = '0';
	buf[j++] = 'x';
	for (i = nibbles - 1; i >= 0; i--)
		buf[j++] = hx[(v >> (i * 4)) & 0xf];
	buf[j] = 0;
	pline(buf);
}

void panic_dump_if_present(void)
{
	int i;

	if (this_boot_seq)
		return;                    /* already ran this boot */

	if (PR->magic != PANIC_MAGIC) {
		/* Cold boot / uninitialized (or stale garbage): initialize the
		 * region so subsequent boots have a monotonic counter. */
		PR->magic    = PANIC_MAGIC;
		PR->boot_seq = 1u;
		PR->valid    = 0u;
		PR->panic_seq = 0u;
		this_boot_seq = 1u;
		pr_clean();
		return;
	}

	/* Warm/subsequent boot: advance the persistent boot counter. */
	PR->boot_seq += 1u;
	this_boot_seq = PR->boot_seq;

	/* A valid record whose panic_seq is from a strictly EARLIER boot means we
	 * died last time. (panic_seq == this_boot_seq would be an in-this-boot
	 * record, which dump does not consume.) */
	if (PR->valid == 1u && PR->panic_seq < this_boot_seq) {
		pline("\r\n==== bzdOS PANIC from previous boot ====\r\n");
		pline("reason: ");
		pline((const char *)PR->reason);
		pline("\r\n");
		pline("boot#="); phex(PR->panic_seq, 8); pline("\r\n");
		pline("ESR ="); phex(PR->esr, 16); pline("\r\n");
		pline("ELR ="); phex(PR->elr, 16); pline("\r\n");
		pline("FAR ="); phex(PR->far, 16); pline("\r\n");
		pline("SP  ="); phex(PR->sp, 16);  pline("\r\n");
		pline("SPSR="); phex(PR->spsr, 16);pline("\r\n");
		pline("KIND="); phex(PR->kind, 8); pline("\r\n");
		for (i = 0; i < 31; i++) {
			pline("x"); phex((uint64_t)i, 2); pline("=");
			phex(PR->x[i], 16); pline("\r\n");
		}
		pline("backtrace:\r\n");
		for (i = 0; i < (int)PR->bt_count && i < (int)PANIC_BT_MAX; i++) {
			pline("  "); phex(PR->bt[i], 16); pline("\r\n");
		}
		pline("==== end panic ====\r\n");
		emac_flush();
		console_flush();

		hud_show_panic(PR);

		/* Consume: keep the record readable via panic_last() but mark it so
		 * we don't re-dump on the next boot. */
		PR->valid = 0u;
		pr_clean();
	}
}
