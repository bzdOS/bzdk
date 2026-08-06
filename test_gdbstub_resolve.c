/* SPDX-License-Identifier: BSD-2-Clause */

/* test_gdbstub_resolve.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the pure address-resolution logic in gdbstub.c's kload_va_to_pa()
 * and resolve() -- the functions at the heart of the SW-breakpoint fix
 * documented in project memory `gdbstub-breakpoint-resolve-broken.md`.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "gdbstub.c"` WITH STUBS
 * ============================================================================
 * gdbstub.c was re-read in full (current, POST-FIX version) before writing
 * this file. kload_va_to_pa()/resolve() (gdbstub.c:188-218) are pure
 * address/range arithmetic with no hardware access at all -- exactly the
 * class of logic this project's test suite already extracts (see
 * test_kload_modinfo.c's header comment for the established house rule).
 * But gdbstub.c as a WHOLE is not includable on plain x86_64 gcc: it's full
 * of raw ARMv8 inline asm the assembler flat-out rejects on this target --
 * e.g. isync_patch() (gdbstub.c:146-155) uses "dc cvau"/"ic ivau"/"dsb ish"/
 * "isb", set_debug_bit() uses "mrs %0, mdscr_el1"/"msr mdscr_el1, %0", and
 * the file pulls in exceptions.h/kload.h which assume a freestanding ARM64
 * environment. Redefining `__asm__` away would silently neuter real cache/
 * debug-register maintenance well beyond the scope of "the resolve() logic",
 * so instead: this file hand-transcribes ONLY kload_va_to_pa() and resolve()
 * verbatim in control flow, with an exact gdbstub.c line-number citation on
 * each, and mocks the three `extern` accessors (kload_kernbase()/
 * kload_pa_base()/kload_kernel_end_va(), real declarations pull from kload.c,
 * NOT touched here per this task's constraints) as plain file-scope uint64_t
 * variables the test sets directly -- exactly what the task description asks
 * for ("just use plain file-scope variables the test can set directly").
 *
 * THE BUG THIS FILE IS AIMED AT: the pre-fix resolve() (see gdbstub.c:157-187's
 * block comment and the gdbstub-breakpoint-resolve-broken memory) ran a live
 * `AT S1E1R` stage-1 translate-instruction on whichever core called it. Since
 * bp_insert()/dispatch() only ever run on CPU1 (the debug-service core, which
 * never hosts the FreeBSD guest and has no valid banked TTBR0/TTBR1_EL1), that
 * translate instruction returned garbage, and the OLD fallback
 * ("if the walk fails, treat the VA as a raw PA") then dereferenced a bogus
 * physical address built from a guest KERNEL VIRTUAL address
 * (0xffff000000632080-shaped) -- confirmed live via a same-EL translation
 * fault inside bp_insert() itself (ESR 0x96000144, FAR == the exact VA
 * requested). The FIX replaces the live walk with pure static arithmetic
 * (pa_base + (va - kernbase), recorded once at kernel-placement time, valid
 * from any core) and, critically, REMOVES the dangerous "unknown -> flat PA"
 * fallback for high KVAs: an address that looks like a guest kernel VA
 * (>= GDB_HIGH_KVA_LINE) but falls outside the loaded image's own
 * [kernbase, kernel_end_va) range must now return FAILURE (0), not a bogus PA.
 * test_resolve_high_kva_outside_kernel_image_fails_cleanly() below reproduces
 * exactly that address shape and asserts resolve() refuses it -- proving this
 * mirror is the FIXED version, not the old buggy one.
 *
 * Build: gcc -o test_gdbstub_resolve test_gdbstub_resolve.c && ./test_gdbstub_resolve
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Mock accessors -- stand-ins for kload.c's kload_kernbase()/
 * kload_pa_base()/kload_kernel_end_va() (real `extern` declarations at
 * gdbstub.c's top-of-file kload.h include; not reproduced here since kload.h
 * is off-limits for this task). Plain file-scope variables the test sets
 * directly, per the task's own guidance.
 * ------------------------------------------------------------------ */
static uint64_t g_kernbase;
static uint64_t g_kernel_end_va;
static uint64_t g_pa_base;

static uint64_t kload_kernbase(void)      { return g_kernbase; }
static uint64_t kload_kernel_end_va(void) { return g_kernel_end_va; }
static uint64_t kload_pa_base(void)       { return g_pa_base; }

static void reset_state(void)
{
	g_kernbase = 0;
	g_kernel_end_va = 0;
	g_pa_base = 0;
}

/* ------------------------------------------------------------------ *
 * kload_va_to_pa() -- mirrors gdbstub.c:188-200 verbatim.
 * ------------------------------------------------------------------ */
static int
kload_va_to_pa(uint64_t va, unsigned long *pa)
{
	uint64_t kb  = kload_kernbase();
	uint64_t end = kload_kernel_end_va();
	uint64_t pb  = kload_pa_base();

	if (!kb || !end || !pb)
		return 0;                 /* no kernel placed this boot yet */
	if (va < kb || va >= end)
		return 0;                 /* outside the loaded image's KVA range */
	*pa = (unsigned long)(pb + (va - kb));
	return 1;
}

/* Mirrors gdbstub.c:202. */
#define GDB_HIGH_KVA_LINE 0xffff000000000000ull

/* ------------------------------------------------------------------ *
 * resolve() -- mirrors gdbstub.c:209-218 verbatim. THIS is the fixed
 * function: note there is NO fallback branch that turns an unresolvable
 * high KVA into a raw PA -- that dangerous path (the actual historical bug)
 * is simply absent, exactly like the real, current source.
 * ------------------------------------------------------------------ */
static int
resolve(uint64_t addr, unsigned long *pa)
{
	if (kload_va_to_pa(addr, pa))
		return 1;
	if (addr < GDB_HIGH_KVA_LINE) {
		*pa = (unsigned long)addr;   /* flat PA — not a guest KVA at all */
		return 1;
	}
	return 0;                        /* unresolvable high KVA */
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* A VA inside [kernbase, kernel_end_va) resolves via static arithmetic to
 * pa_base + (va - kernbase) -- the common "break at a kernel symbol" case
 * (gdbstub.c:173-178's block comment, case 1). */
static void test_resolve_kernel_image_va_maps_to_pa_base_offset(void)
{
	reset_state();
	g_kernbase      = 0xffff000000200000ull;
	g_kernel_end_va = 0xffff000000600000ull;
	g_pa_base       = 0x46000000ull;

	unsigned long pa = 0xdeadbeef;
	uint64_t va = g_kernbase + 0x1234;
	assert(resolve(va, &pa) == 1);
	assert(pa == (unsigned long)(g_pa_base + 0x1234));

	/* Left boundary is inclusive. */
	pa = 0xdeadbeef;
	assert(resolve(g_kernbase, &pa) == 1);
	assert(pa == (unsigned long)g_pa_base);

	/* Right boundary is EXCLUSIVE: kernel_end_va itself is outside the image
	 * and (being a high KVA) must fail, not silently resolve. */
	pa = 0xdeadbeef;
	assert(resolve(g_kernel_end_va, &pa) == 0);
}

/* A VA below the canonical high-KVA line (GDB_HIGH_KVA_LINE,
 * gdbstub.c:202) resolves as a flat PA with no kernel-image check at all --
 * this is how gdbstub reads its own EL2 structures / low guest-physical
 * memory (gdbstub.c:179-183's block comment, case 2). This must hold even
 * when NO kernel has been placed yet (all-zero kload state), since it's
 * checked independently of kload_va_to_pa(). */
static void test_resolve_low_va_is_flat_pa_regardless_of_kernel_state(void)
{
	reset_state();   /* kernbase/pa_base/kernel_end_va all zero */

	unsigned long pa = 0;
	uint64_t low_va = 0x50000800ull;   /* e.g. a GICT breadcrumb PA */
	assert(resolve(low_va, &pa) == 1);
	assert(pa == (unsigned long)low_va);

	/* Also true once a kernel IS placed, as long as the address is still
	 * below the high-KVA line (kload_va_to_pa() itself would reject it via
	 * the va < kb check, but resolve()'s low-VA branch is checked first in
	 * neither case does it matter here since kb is a high KVA). */
	g_kernbase      = 0xffff000000200000ull;
	g_kernel_end_va = 0xffff000000600000ull;
	g_pa_base       = 0x46000000ull;
	pa = 0;
	assert(resolve(low_va, &pa) == 1);
	assert(pa == (unsigned long)low_va);
}

/* THE core regression test: a VA that IS a high KVA (>= GDB_HIGH_KVA_LINE)
 * but falls OUTSIDE the loaded kernel's [kernbase, kernel_end_va) range must
 * return failure (0) -- this is exactly the historical crash case (a guest
 * dynamic-KVA/direct-map address that isn't part of the static kernel image;
 * see the file header's live-fault evidence). Proves this mirror does NOT
 * carry the old buggy "unknown -> treat as flat PA" fallback: if it did,
 * this assert would observe resolve() returning 1 with a bogus PA equal to
 * the raw (huge) VA, matching the old dangerous behavior instead of the
 * fixed one. */
static void test_resolve_high_kva_outside_kernel_image_fails_cleanly(void)
{
	reset_state();
	g_kernbase      = 0xffff000000200000ull;
	g_kernel_end_va = 0xffff000000600000ull;
	g_pa_base       = 0x46000000ull;

	/* Exactly the shape of the live-crash address from the memory writeup:
	 * a high KVA, but far outside [kernbase, kernel_end_va). */
	uint64_t bad_va = 0xffff000000632080ull;
	assert(bad_va >= GDB_HIGH_KVA_LINE);
	assert(bad_va >= g_kernel_end_va);   /* outside the image, past its end */

	unsigned long pa = 0x12345;   /* poison -- must stay unmodified on failure */
	int rc = resolve(bad_va, &pa);
	assert(rc == 0);
	assert(pa == 0x12345);   /* resolve() must not have touched *pa */

	/* Also check a high KVA BELOW kernbase (e.g. the direct map, or a
	 * dynamic KVA region below the kernel's own load address) -- same
	 * "outside the image" failure, different side of the range. */
	uint64_t below_va = 0xffff000000100000ull;
	assert(below_va >= GDB_HIGH_KVA_LINE);
	assert(below_va < g_kernbase);
	pa = 0x54321;
	assert(resolve(below_va, &pa) == 0);
	assert(pa == 0x54321);
}

/* Edge case: kernbase/pa_base/kernel_end_va all zero (no kernel placed this
 * boot yet, e.g. gdbstub attached before kload_place_segments() ever ran).
 * kload_va_to_pa()'s `if (!kb || !end || !pb) return 0;` guard
 * (gdbstub.c:194-195) must reject cleanly -- no divide-by-zero (there is no
 * division in this arithmetic, but a naive implementation might still
 * underflow `va - kb` with kb==0 and treat every address as "in range"), and
 * no wraparound. A high KVA must still fail via resolve()'s "unresolvable"
 * path; a low VA still succeeds via the flat-PA branch (case 2, independent
 * of kernel-placement state) -- covered by the previous test. */
static void test_resolve_zero_kload_state_fails_for_high_kva(void)
{
	reset_state();   /* everything zero: kernel not yet placed */

	unsigned long pa = 0xaaaaaaaaul;
	/* Without the !kb guard, a naive `va >= kb` (kb==0) would be trivially
	 * true for any va, and `va < end` (end==0) would be trivially false for
	 * any nonzero va -- i.e. the range check alone would incorrectly ACCEPT
	 * every high VA as "in range" once kb==0. The explicit zero-guard is
	 * what prevents that. */
	uint64_t va = 0xffff000012345678ull;
	assert(kload_va_to_pa(va, &pa) == 0);
	assert(resolve(va, &pa) == 0);
	assert(pa == 0xaaaaaaaaul);   /* untouched */

	/* Partial state (only kernbase set, pa_base/end still zero) must also
	 * fail -- guards against a "some but not all of the kload globals were
	 * initialized" boot-ordering bug. */
	g_kernbase = 0xffff000000200000ull;
	pa = 0xbbbbbbbbul;
	assert(kload_va_to_pa(g_kernbase, &pa) == 0);
	assert(pa == 0xbbbbbbbbul);
}

/* ==================================================================== *
 * main() — runs every test, reports pass/fail, exits nonzero on failure.
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "resolve_kernel_image_va_maps_to_pa_base_offset",
	  test_resolve_kernel_image_va_maps_to_pa_base_offset },
	{ "resolve_low_va_is_flat_pa_regardless_of_kernel_state",
	  test_resolve_low_va_is_flat_pa_regardless_of_kernel_state },
	{ "resolve_high_kva_outside_kernel_image_fails_cleanly",
	  test_resolve_high_kva_outside_kernel_image_fails_cleanly },
	{ "resolve_zero_kload_state_fails_for_high_kva",
	  test_resolve_zero_kload_state_fails_for_high_kva },
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

	printf("---- gdbstub resolve tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
