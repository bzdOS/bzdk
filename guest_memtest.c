/* SPDX-License-Identifier: BSD-2-Clause
 *
 * guest_memtest.c -- a guest-side self-test for the ONE question that keeps
 * getting asked of this hypervisor's memory management: "is stage-2 eating my
 * mapping?"
 *
 * WHY THIS EXISTS.  On 2026-08-26 python3.12 was segfaulting on the guest, and
 * `ktrace` showed SEGV_MAPERR arriving immediately after an anonymous
 * `mmap()` that had itself SUCCEEDED.  That reads like a smoking gun aimed
 * straight at us: this hypervisor maps guest DRAM writable-but-execute-never
 * and promotes pages to executable on demand from a fixed pool of L3 tables
 * (`STAGE2_WX_DYNAMIC`), so "the kernel granted a mapping and the first touch
 * faulted" is exactly the shape a bug in that mechanism would take.
 *
 * It was not us.  The real fault was ~70 python312 files silently corrupt at
 * their correct length -- stale wreckage from the since-fixed era when
 * `rd_cntpct()` could run backwards.  The lesson worth keeping is the cheaper
 * one: **ktrace names the last syscall before a SIGSEGV, not its cause.**  A
 * userland process faults between syscalls, so adjacency in a trace proves
 * nothing at all about which subsystem is guilty.
 *
 * Hence this file.  It is deliberately far smaller than CPython, it needs no
 * package database and no network, and it exercises the promotion path
 * directly rather than by implication.  Run it FIRST the next time a guest
 * userland crash looks like it might be ours; if every step passes, stage-2 is
 * not the story and the hunt belongs in the guest.
 *
 * Step 6 is the one that actually matters, and the reason the other five are
 * not sufficient: writing bytes to a page and then making that same page
 * executable and CALLING it is the real JIT/dlopen shape, and the only shape
 * that forces a W^X promotion.  Steps 1-5 would pass even if promotion were
 * completely broken.
 *
 * Build and run on the guest (root is read-only by design; use /tmp, which is
 * tmpfs):
 *     scp guest_memtest.c root@<guest>:/tmp/
 *     ssh root@<guest> 'cd /tmp && cc -O0 -o memtest guest_memtest.c && ./memtest'
 * Exit status 0 means every step passed.  Each step flushes stdout *before*
 * the risky operation, so if it dies the last line printed names the step that
 * faulted -- which is the whole point of an escalating test rather than one
 * big one.
 */
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *try_map(size_t len, int prot, const char *what)
{
	void *p = mmap(0, len, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
	printf("%-28s len=%#zx prot=%#x -> %p\n", what, len, prot, p);
	fflush(stdout);
	return p == MAP_FAILED ? NULL : p;
}

/* Write a distinct byte per page and read every one of them back.  Touching
 * one page proves less than it looks: a promotion pool that is exhausted or
 * mis-indexed typically fails on a later page, not the first. */
static int touch_all(volatile char *p, size_t len)
{
	size_t i;

	printf("  writing every page ...\n");
	fflush(stdout);
	for (i = 0; i < len; i += 4096)
		p[i] = (char)(i >> 12);

	printf("  reading back ...\n");
	fflush(stdout);
	for (i = 0; i < len; i += 4096)
		if (p[i] != (char)(i >> 12)) {
			printf("  MISMATCH at %#zx: got %#x want %#x\n", i,
			       p[i] & 0xff, (int)(char)(i >> 12) & 0xff);
			fflush(stdout);
			return 1;
		}
	printf("  OK\n");
	fflush(stdout);
	return 0;
}

int main(void)
{
	/* 0x100000 is the size python was dying on, kept so this test covers
	 * the original report exactly; the others bracket it. */
	size_t sizes[] = { 0x1000, 0x100000, 0x400000, 0x1000000 };
	volatile char *p;
	int i, bad = 0;

	for (i = 0; i < 4; i++) {
		p = try_map(sizes[i], PROT_READ | PROT_WRITE, "anon RW");
		if (!p) {
			printf("  mmap REFUSED (an error return, not a fault)\n");
			bad++;
			continue;
		}
		bad += touch_all(p, sizes[i]);
		munmap((void *)p, sizes[i]);
	}

	p = try_map(0x100000, PROT_READ | PROT_WRITE | PROT_EXEC, "anon RWX");
	if (!p) {
		printf("  mmap REFUSED\n");
		bad++;
	} else {
		bad += touch_all(p, 0x100000);
		munmap((void *)p, 0x100000);
	}

	/* The step that exercises W^X promotion for real. */
	p = try_map(0x1000, PROT_READ | PROT_WRITE, "anon RW->RX, then call");
	if (!p) {
		printf("  mmap REFUSED\n");
		bad++;
	} else {
		/* AArch64: mov w0, #42 ; ret */
		unsigned int code[] = { 0x52800540u, 0xd65f03c0u };
		int (*fn)(void);
		int got;

		memcpy((void *)p, code, sizeof code);
		printf("  code written, mprotect R|X ...\n");
		fflush(stdout);
		if (mprotect((void *)p, 0x1000, PROT_READ | PROT_EXEC) != 0) {
			printf("  mprotect FAILED\n");
			bad++;
		} else {
			__builtin___clear_cache((char *)p, (char *)p + 0x1000);
			fn = (int (*)(void))p;
			printf("  calling it ...\n");
			fflush(stdout);
			got = fn();
			printf("  returned %d (want 42)%s\n", got,
			       got == 42 ? "" : "  <-- WRONG");
			fflush(stdout);
			if (got != 42)
				bad++;
		}
		munmap((void *)p, 0x1000);
	}

	printf("%s bad=%d\n", bad ? "FAIL" : "PASS", bad);
	return bad ? 1 : 0;
}
