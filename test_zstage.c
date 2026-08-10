/* SPDX-License-Identifier: BSD-2-Clause */

/* test_zstage.c — hosted (x86_64, plain gcc, no cross-compiler) unit tests for
 * zstage_span(), the bulk loader's span computation (zstage.c).
 *
 * ============================================================================
 * WHY THIS ONE #includes THE REAL SOURCE INSTEAD OF MIRRORING IT
 * ============================================================================
 * test_zload2_parsing.c and test_kload_modinfo.c both hand-transcribe the
 * functions they test, and they explain why: the functions they need are
 * tangled up with raw ARMv8 inline asm ("dc cvac"/"ic ivau") that plain
 * x86_64 gcc rejects, so there is no way to compile the real file on the host.
 * That is a real constraint, but it has a real cost — a mirror can silently
 * drift from the source it claims to test, and then the test passes while the
 * shipped code is wrong.
 *
 * zstage.c was written specifically to avoid that: everything board-specific
 * (breadcrumbs, cache maintenance, the memcpy into physical DRAM) is behind
 * ZSTAGE_HOSTED_TEST, leaving zstage_span() as genuinely isolated pure logic.
 * So this test compiles THE ACTUAL SOURCE, and drift is impossible by
 * construction rather than by discipline. Where a future test can be built
 * this way, it should be.
 *
 * WHAT THIS FILE IS AIMED AT: zstage_span() decides how many bytes get copied
 * out of a window whose contents are, in the ordinary "nothing was staged"
 * case, uninitialised DRAM garbage. Two properties matter most and are the
 * reason this test exists:
 *
 *   1. It must REJECT garbage without reading past `avail`. On the board that
 *      window is real DRAM inside the FreeBSD guest's gigabyte, and a header
 *      whose e_phoff says "4 GiB in" must be refused, not followed. Getting
 *      this wrong faults EL2 itself during early boot, before any console
 *      exists to say so.
 *   2. It must NOT include section headers or debug sections in the span.
 *      A Zephyr ELF's .debug_* sections routinely dwarf its loadable content;
 *      copying to e_shoff instead of to the last PT_LOAD would move megabytes
 *      of bytes zload2_parse_and_place() never reads (it walks PT_LOAD only —
 *      see zload2.c) and could overflow the 16 MiB destination window for an
 *      image whose real content is a few hundred KiB. Test 3 pins this down.
 *
 * Build: gcc -o test_zstage test_zstage.c && ./test_zstage
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define ZSTAGE_HOSTED_TEST 1
#include "zstage.c"

static int g_fail;

#define CHECK(cond, ...) do {                                           \
	if (!(cond)) {                                                  \
		printf("  FAIL %s:%d: ", __func__, __LINE__);           \
		printf(__VA_ARGS__);                                    \
		printf("\n");                                           \
		g_fail++;                                               \
	}                                                               \
} while (0)

/* ------------------------------------------------------------------ *
 * Fixture builder: a flat byte buffer standing in for the raw file staged at
 * ZSTAGE_LOW_PA. zstage_span() takes a plain `const void *`, so a malloc'd
 * buffer is a faithful stand-in with no address arithmetic to get wrong.
 * ------------------------------------------------------------------ */
#define FIX_SIZE (1u << 20)   /* 1 MiB — larger than any fixture below */

typedef struct {
	uint8_t *buf;
	uint64_t avail;
} fixture_t;

static fixture_t
fix_new(uint64_t avail)
{
	fixture_t f;
	f.buf = calloc(1, FIX_SIZE);
	if (f.buf == NULL) {
		printf("calloc failed\n");
		exit(1);
	}
	f.avail = avail;
	return f;
}

static void
fix_free(fixture_t *f)
{
	free(f->buf);
	f->buf = NULL;
}

/* A well-formed ELF64/AArch64 EXEC header, ready for the caller to adjust. */
static zstage_elf64_ehdr_t *
fix_ehdr(fixture_t *f, uint16_t phnum, uint64_t phoff)
{
	zstage_elf64_ehdr_t *eh = (zstage_elf64_ehdr_t *)f->buf;

	eh->e_ident[0] = 0x7F;
	eh->e_ident[1] = 'E';
	eh->e_ident[2] = 'L';
	eh->e_ident[3] = 'F';
	eh->e_ident[4] = ZSTAGE_ELFCLASS64;
	eh->e_ident[5] = ZSTAGE_ELFDATA2LSB;
	eh->e_type = ZSTAGE_ET_EXEC;
	eh->e_machine = ZSTAGE_EM_AARCH64;
	eh->e_version = 1;
	eh->e_entry = 0xBE000000ull;
	eh->e_phoff = phoff;
	eh->e_ehsize = sizeof(*eh);
	eh->e_phentsize = sizeof(zstage_elf64_phdr_t);
	eh->e_phnum = phnum;
	return eh;
}

static zstage_elf64_phdr_t *
fix_phdr(fixture_t *f, uint64_t phoff, unsigned idx)
{
	return (zstage_elf64_phdr_t *)(f->buf + phoff +
				       idx * sizeof(zstage_elf64_phdr_t));
}

/* ------------------------------------------------------------------ *
 * 1. The ordinary case: one PT_LOAD segment. Span is exactly its end, which
 *    is what the real zg3_trivial_payload.elf looks like (ELF header + one
 *    phdr at 0x40, one PT_LOAD at file offset 0x10000, 0x18 bytes) — the
 *    image the dual-guest hardware test actually staged by hand.
 * ------------------------------------------------------------------ */
static void
t1_single_segment(void)
{
	fixture_t f = fix_new(FIX_SIZE);
	uint64_t span = 0;
	zstage_elf64_phdr_t *ph;

	fix_ehdr(&f, 1, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x10000;
	ph->p_filesz = 0x18;
	ph->p_memsz = 0x18;

	CHECK(zstage_span(f.buf, f.avail, &span) == 1, "valid ELF rejected");
	CHECK(span == 0x10018, "span %llu, want 0x10018",
	      (unsigned long long)span);
	fix_free(&f);
}

/* ------------------------------------------------------------------ *
 * 2. Multiple PT_LOADs, deliberately NOT in file-offset order — the span is
 *    the maximum end, not the last segment's end. A loop that just took the
 *    final segment would pass test 1 and fail here.
 * ------------------------------------------------------------------ */
static void
t2_multi_segment_out_of_order(void)
{
	fixture_t f = fix_new(FIX_SIZE);
	uint64_t span = 0;
	zstage_elf64_phdr_t *ph;

	fix_ehdr(&f, 3, 0x40);

	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x8000;
	ph->p_filesz = 0x100;

	ph = fix_phdr(&f, 0x40, 1);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x20000;    /* the furthest one, in the MIDDLE */
	ph->p_filesz = 0x400;

	ph = fix_phdr(&f, 0x40, 2);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x10000;
	ph->p_filesz = 0x200;

	CHECK(zstage_span(f.buf, f.avail, &span) == 1, "valid ELF rejected");
	CHECK(span == 0x20400, "span 0x%llx, want 0x20400",
	      (unsigned long long)span);
	fix_free(&f);
}

/* ------------------------------------------------------------------ *
 * 3. THE PROPERTY THIS FILE EXISTS FOR: section headers and debug sections
 *    sit far past the loadable content, and must not be copied. A real Zephyr
 *    ELF is shaped exactly like this — tiny .text/.rodata, enormous .debug_*.
 * ------------------------------------------------------------------ */
static void
t3_section_headers_excluded(void)
{
	fixture_t f = fix_new(FIX_SIZE);
	uint64_t span = 0;
	zstage_elf64_ehdr_t *eh;
	zstage_elf64_phdr_t *ph;

	eh = fix_ehdr(&f, 1, 0x40);
	eh->e_shoff = 0x80000;     /* 512 KiB in: debug sections + shdrs */
	eh->e_shentsize = 64;
	eh->e_shnum = 40;

	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x1000;
	ph->p_filesz = 0x2000;     /* real loadable content: 8 KiB */

	CHECK(zstage_span(f.buf, f.avail, &span) == 1, "valid ELF rejected");
	CHECK(span == 0x3000,
	      "span 0x%llx, want 0x3000 — section headers must NOT be copied",
	      (unsigned long long)span);
	fix_free(&f);
}

/* ------------------------------------------------------------------ *
 * 4. A pure-.bss PT_LOAD (p_filesz == 0) is still a loadable segment, so the
 *    image is valid; it just contributes no file bytes. Span falls back to the
 *    end of the program-header table.
 * ------------------------------------------------------------------ */
static void
t4_bss_only_segment(void)
{
	fixture_t f = fix_new(FIX_SIZE);
	uint64_t span = 0;
	zstage_elf64_phdr_t *ph;

	fix_ehdr(&f, 1, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x1000;
	ph->p_filesz = 0;
	ph->p_memsz = 0x4000;

	CHECK(zstage_span(f.buf, f.avail, &span) == 1,
	      "bss-only PT_LOAD rejected");
	CHECK(span == 0x40 + sizeof(zstage_elf64_phdr_t),
	      "span 0x%llx, want end-of-phdr-table",
	      (unsigned long long)span);
	fix_free(&f);
}

/* ------------------------------------------------------------------ *
 * 5. Exact-boundary acceptance: a segment ending precisely at `avail` is in
 *    bounds. Off-by-one in the wrong direction rejects a perfectly good image.
 * ------------------------------------------------------------------ */
static void
t5_exact_boundary_accepted(void)
{
	fixture_t f = fix_new(0x11000);
	uint64_t span = 0;
	zstage_elf64_phdr_t *ph;

	fix_ehdr(&f, 1, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x10000;
	ph->p_filesz = 0x1000;     /* ends exactly at avail */

	CHECK(zstage_span(f.buf, f.avail, &span) == 1,
	      "segment ending exactly at avail rejected");
	CHECK(span == 0x11000, "span 0x%llx, want 0x11000",
	      (unsigned long long)span);
	fix_free(&f);
}

/* ------------------------------------------------------------------ *
 * 6-13. Rejection cases. Each must return 0 AND leave *span_out untouched —
 *       a caller that ignores the return value must not be handed a plausible
 *       byte count. The sentinel checks that explicitly.
 * ------------------------------------------------------------------ */
#define SENTINEL 0xDEADBEEFull

static void
reject_case(const char *name, fixture_t *f)
{
	uint64_t span = SENTINEL;

	if (zstage_span(f->buf, f->avail, &span) != 0)
		CHECK(0, "%s: accepted, should have been rejected", name);
	else
		CHECK(span == SENTINEL,
		      "%s: rejected but clobbered *span_out (0x%llx)",
		      name, (unsigned long long)span);
}

static void
t6_rejections(void)
{
	fixture_t f;
	zstage_elf64_ehdr_t *eh;
	zstage_elf64_phdr_t *ph;

	/* 6a. All-zero window: the ordinary "nothing was staged" case on a
	 *     freshly-reset board. Must be rejected, quietly. */
	f = fix_new(FIX_SIZE);
	reject_case("all-zero window", &f);
	fix_free(&f);

	/* 6b. Window smaller than an ELF header. */
	f = fix_new(sizeof(zstage_elf64_ehdr_t) - 1);
	fix_ehdr(&f, 1, 0x40);
	reject_case("window smaller than ehdr", &f);
	fix_free(&f);

	/* 6c. Bad magic (one byte off — the realistic corruption). */
	f = fix_new(FIX_SIZE);
	eh = fix_ehdr(&f, 1, 0x40);
	eh->e_ident[3] = 'G';
	reject_case("bad ELF magic", &f);
	fix_free(&f);

	/* 6d. 32-bit ELF. */
	f = fix_new(FIX_SIZE);
	eh = fix_ehdr(&f, 1, 0x40);
	eh->e_ident[4] = 1;   /* ELFCLASS32 */
	reject_case("ELFCLASS32", &f);
	fix_free(&f);

	/* 6e. Big-endian. */
	f = fix_new(FIX_SIZE);
	eh = fix_ehdr(&f, 1, 0x40);
	eh->e_ident[5] = 2;   /* ELFDATA2MSB */
	reject_case("big-endian", &f);
	fix_free(&f);

	/* 6f. Wrong machine — an x86_64 image staged by mistake. */
	f = fix_new(FIX_SIZE);
	eh = fix_ehdr(&f, 1, 0x40);
	eh->e_machine = 62;   /* EM_X86_64 */
	reject_case("wrong e_machine", &f);
	fix_free(&f);

	/* 6g. Relocatable object rather than an executable. */
	f = fix_new(FIX_SIZE);
	eh = fix_ehdr(&f, 1, 0x40);
	eh->e_type = 1;       /* ET_REL */
	reject_case("ET_REL", &f);
	fix_free(&f);

	/* 6h. Mismatched e_phentsize — walking the table with the wrong stride
	 *     would read garbage as segment offsets. */
	f = fix_new(FIX_SIZE);
	eh = fix_ehdr(&f, 1, 0x40);
	eh->e_phentsize = 32;
	reject_case("bad e_phentsize", &f);
	fix_free(&f);

	/* 6i. No program headers at all. */
	f = fix_new(FIX_SIZE);
	fix_ehdr(&f, 0, 0x40);
	reject_case("e_phnum == 0", &f);
	fix_free(&f);

	/* 6j. Absurd e_phnum — past the sanity bound. */
	f = fix_new(FIX_SIZE);
	fix_ehdr(&f, ZSTAGE_MAX_PHDR + 1, 0x40);
	reject_case("e_phnum over bound", &f);
	fix_free(&f);

	/* 6k. e_phoff points outside the window. THE fault-EL2 case. */
	f = fix_new(0x1000);
	fix_ehdr(&f, 1, 0x100000000ull);
	reject_case("e_phoff outside window", &f);
	fix_free(&f);

	/* 6l. Program-header table starts inside but runs off the end. */
	f = fix_new(0x50);
	fix_ehdr(&f, 4, 0x40);
	reject_case("phdr table runs past avail", &f);
	fix_free(&f);

	/* 6m. Segment content runs off the end by one byte. */
	f = fix_new(0x11000);
	fix_ehdr(&f, 1, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x10000;
	ph->p_filesz = 0x1001;
	reject_case("segment one byte past avail", &f);
	fix_free(&f);

	/* 6n. Segment offset itself outside the window. */
	f = fix_new(0x1000);
	fix_ehdr(&f, 1, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x100000000ull;
	ph->p_filesz = 0x10;
	reject_case("p_offset outside window", &f);
	fix_free(&f);

	/* 6o. Program headers present, but none of them loadable. Nothing to
	 *     copy means nothing to boot — reject rather than copy a header. */
	f = fix_new(FIX_SIZE);
	fix_ehdr(&f, 2, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = 4;   /* PT_NOTE */
	ph->p_offset = 0x1000;
	ph->p_filesz = 0x20;
	ph = fix_phdr(&f, 0x40, 1);
	ph->p_type = 6;   /* PT_PHDR */
	ph->p_offset = 0x40;
	ph->p_filesz = 0x38;
	reject_case("no PT_LOAD segments", &f);
	fix_free(&f);

	/* 6p. A NULL span_out must be refused rather than dereferenced. */
	f = fix_new(FIX_SIZE);
	fix_ehdr(&f, 1, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x1000;
	ph->p_filesz = 0x10;
	CHECK(zstage_span(f.buf, f.avail, NULL) == 0,
	      "NULL span_out accepted");
	fix_free(&f);
}

/* ------------------------------------------------------------------ *
 * 7. A PT_LOAD mixed in with non-loadable headers is still found. Zephyr's
 *    real ELF has PT_LOAD alongside PT_NOTE/PT_PHDR/PT_GNU_STACK, so "skip
 *    the non-loadable ones and keep going" is the actual shape encountered,
 *    not a synthetic edge case.
 * ------------------------------------------------------------------ */
static void
t7_mixed_segment_types(void)
{
	fixture_t f = fix_new(FIX_SIZE);
	uint64_t span = 0;
	zstage_elf64_phdr_t *ph;

	fix_ehdr(&f, 3, 0x40);

	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = 6;            /* PT_PHDR */
	ph->p_offset = 0x40;
	ph->p_filesz = 0xA8;

	ph = fix_phdr(&f, 0x40, 1);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x1000;
	ph->p_filesz = 0x3000;

	ph = fix_phdr(&f, 0x40, 2);
	ph->p_type = 0x6474e551;   /* PT_GNU_STACK, offset 0 */
	ph->p_offset = 0;
	ph->p_filesz = 0;

	CHECK(zstage_span(f.buf, f.avail, &span) == 1, "valid ELF rejected");
	CHECK(span == 0x4000, "span 0x%llx, want 0x4000",
	      (unsigned long long)span);
	fix_free(&f);
}

/* ------------------------------------------------------------------ *
 * 8. Sanity on the real numbers this feeds: the span of a plausible Zephyr
 *    image must fit the 16 MiB destination window, and an image that would
 *    NOT fit is what zstage_copy_to() refuses. zstage_copy_to() itself is
 *    board-only (breadcrumbs + physical memcpy), so what is checked here is
 *    the arithmetic it gates on, against the real ZG3_ELF_STAGE_SIZE value.
 * ------------------------------------------------------------------ */
static void
t8_destination_window_arithmetic(void)
{
	const uint64_t dest_limit = 0x01000000ull;   /* ZG3_ELF_STAGE_SIZE */
	fixture_t f;
	uint64_t span = 0;
	zstage_elf64_phdr_t *ph;

	/* A 512 KiB image — comfortably inside the window. */
	f = fix_new(FIX_SIZE);
	fix_ehdr(&f, 1, 0x40);
	ph = fix_phdr(&f, 0x40, 0);
	ph->p_type = ZSTAGE_PT_LOAD;
	ph->p_offset = 0x1000;
	ph->p_filesz = 0x7F000;
	CHECK(zstage_span(f.buf, f.avail, &span) == 1, "valid ELF rejected");
	CHECK(span <= dest_limit,
	      "a 512 KiB image must fit the 16 MiB destination window");
	fix_free(&f);

	/* And the guard itself: ZSTAGE_LOW_SIZE (32 MiB) is deliberately LARGER
	 * than the destination window (16 MiB), so an image can legitimately
	 * pass zstage_span() and still be refused by zstage_copy_to(). That is
	 * a real, reachable state, not a can't-happen — which is why the
	 * refusal exists and gets its own breadcrumb (state 3). */
	CHECK(ZSTAGE_LOW_SIZE > dest_limit,
	      "landing window must exceed the destination window for the "
	      "too-large refusal to be reachable");
}

int
main(void)
{
	printf("test_zstage: zstage_span() — bulk-loader span computation\n");

	t1_single_segment();
	t2_multi_segment_out_of_order();
	t3_section_headers_excluded();
	t4_bss_only_segment();
	t5_exact_boundary_accepted();
	t6_rejections();
	t7_mixed_segment_types();
	t8_destination_window_arithmetic();

	if (g_fail != 0) {
		printf("test_zstage: %d FAILED\n", g_fail);
		return 1;
	}
	printf("test_zstage: all checks passed\n");
	return 0;
}
