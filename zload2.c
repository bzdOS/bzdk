/* SPDX-License-Identifier: BSD-2-Clause */

/* zload2.c — implementation. See zload2.h for the full API contract and the
 * rationale for why this is a separate, disjoint file rather than a
 * parameterized kload.c.
 *
 * Freestanding: <stdint.h> only; memcpy/memset declared locally with the
 * exact signatures libmin.c provides, same convention kload.c already
 * uses (no shared "libmin.h" header exists in this tree).
 */
#include <stdint.h>
#include "zload2.h"
#include "hv_addrmap.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);

/* ------------------------------------------------------------------ *
 * Breadcrumb window: HVMAP_ZLOAD2_BC (0x50071000, "ZLD2") — see
 * hv_addrmap.h for why this lane is safe (dual-guest-only, distinct from
 * every other window in the tree).
 *   [0] magic       0x5A4C4432 ("ZLD2")
 *   [1] elf_valid   1 if zload2_parse_and_place()'s header check passed
 *   [2] n_seg       number of PT_LOAD segments recorded
 *   [3] zbase_lo    lowest p_vaddr across every PT_LOAD segment, low 32
 *   [4] pa_base_lo  pa_base as given to zload2_parse_and_place(), low 32
 *   [5] entry_pa_lo resolved physical entry address, low 32
 *   [6] bytes_copied total bytes memcpy'd across every PT_LOAD segment
 *   [7] fail_reason 0 = none; 1 = bad ELF header; 2 = too many/zero
 *                    PT_LOAD segments; 3 = a segment falls outside the
 *                    [pa_base, pa_base+ZLOAD2_MAX_IMAGE_SIZE) safety window
 * ------------------------------------------------------------------ */
#define ZLD2_MAGIC 0x5A4C4432u   /* "ZLD2" */

enum {
	ZBC_MAGIC = 0,
	ZBC_ELF_VALID,
	ZBC_N_SEG,
	ZBC_ZBASE_LO,
	ZBC_PA_BASE_LO,
	ZBC_ENTRY_PA_LO,
	ZBC_BYTES_COPIED,
	ZBC_FAIL_REASON,
};

static inline void
zld2_bc(int i, uint32_t v)
{
	volatile uint32_t *p =
		(volatile uint32_t *)(HVMAP_ZLOAD2_BC + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Minimal local ELF64/AArch64 definitions — deliberately NOT shared with
 * kload.c's identical-looking copies (see zload2.h's header comment for
 * why duplication is the deliberate choice here, matching kload.c's own
 * precedent of duplicating guest.c's asm helpers rather than sharing them).
 * ------------------------------------------------------------------ */
typedef struct {
	uint8_t  e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} zload2_elf64_ehdr_t;

typedef struct {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} zload2_elf64_phdr_t;

#define ZLOAD2_PT_LOAD     1u
#define ZLOAD2_ELFCLASS64  2u
#define ZLOAD2_ELFDATA2LSB 1u
#define ZLOAD2_EM_AARCH64  183u
#define ZLOAD2_ET_EXEC     2u
#define ZLOAD2_ET_DYN      3u

/* Sane upper bound on PT_LOAD segments -- Zephyr images are expected to
 * have exactly one (see main_zephyr.c's header comment), but a handful of
 * headroom costs nothing and mirrors kload.c's KLOAD_MAX_PHDR role without
 * sharing its value. */
#define ZLOAD2_MAX_PHDR 8u

/* ------------------------------------------------------------------ *
 * Cache maintenance: clean D-cache to PoC ("dc cvac") + invalidate I-cache
 * to PoU ("ic ivau") over every 64-byte line in [addr, addr+len). Same
 * idiom as kload_cache_clean_inval() (which is `static` in kload.c, hence
 * this local duplicate rather than a cross-file call — see kload.c's own
 * comment for why this exact loop is already duplicated more than once in
 * this tree, e.g. repl.c's hot-reload path and fb.c's flush path). */
#define ZLOAD2_CACHE_LINE 64ul

static void
zload2_cache_clean_inval(uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~(ZLOAD2_CACHE_LINE - 1);
	uint64_t end = addr + len;
	uint64_t o;

	for (o = start; o < end; o += ZLOAD2_CACHE_LINE)
		__asm__ volatile("dc cvac, %0" :: "r"(o) : "memory");
	__asm__ volatile("dsb ish");
	for (o = start; o < end; o += ZLOAD2_CACHE_LINE)
		__asm__ volatile("ic ivau, %0" :: "r"(o) : "memory");
	__asm__ volatile("dsb ish\n\tisb");
}

/* ------------------------------------------------------------------ *
 * Parser/placer state. One in-flight image at a time, same "parse then
 * place" two-step model as kload.c's kls -- but a completely separate
 * static instance, memory-disjoint from it (no shared symbols). ------- */
struct zload2_seg {
	uint64_t p_vaddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_offset;
};

static struct {
	int      valid;        /* zload2_parse_and_place()'s header check passed */
	uint64_t elf_addr;      /* ELF image base, as last parsed */
	uint64_t e_entry;       /* raw ELF entry point */
	uint64_t zbase;         /* lowest p_vaddr across every PT_LOAD segment */
	int      n_seg;
	struct zload2_seg seg[ZLOAD2_MAX_PHDR];

	uint64_t pa_base;       /* physical placement base, as last placed */
	uint64_t entry_pa;      /* pa_base + (e_entry - zbase) */
} zls;

/* Validate the ELF header + walk PT_LOAD program headers into zls.seg[].
 * Returns 1 and leaves zls.valid==1 on success; 0 (zls.valid==0) on any
 * validation failure. Pure parsing -- no memory is copied here. */
static int
zload2_parse_elf(uint64_t elf_addr)
{
	const zload2_elf64_ehdr_t *eh = (const zload2_elf64_ehdr_t *)elf_addr;
	const zload2_elf64_phdr_t *ph;
	uint32_t i;

	zls.valid = 0;

	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		return 0;
	if (eh->e_ident[4] != ZLOAD2_ELFCLASS64)
		return 0;
	if (eh->e_ident[5] != ZLOAD2_ELFDATA2LSB)
		return 0;
	if (eh->e_machine != ZLOAD2_EM_AARCH64)
		return 0;
	if (eh->e_type != ZLOAD2_ET_EXEC && eh->e_type != ZLOAD2_ET_DYN)
		return 0;
	if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(zload2_elf64_phdr_t))
		return 0;
	if (eh->e_phnum > 64u)   /* sanity cap on TOTAL headers, mirrors kload.c */
		return 0;

	zls.elf_addr = elf_addr;
	zls.e_entry = eh->e_entry;
	zls.n_seg = 0;
	zls.zbase = ~0ull;

	ph = (const zload2_elf64_phdr_t *)(elf_addr + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		struct zload2_seg *s;

		if (ph[i].p_type != ZLOAD2_PT_LOAD)
			continue;
		if (zls.n_seg >= (int)ZLOAD2_MAX_PHDR)
			return 0;   /* more PT_LOAD than we track -> refuse, never truncate */

		s = &zls.seg[zls.n_seg++];
		s->p_vaddr  = ph[i].p_vaddr;
		s->p_filesz = ph[i].p_filesz;
		s->p_memsz  = ph[i].p_memsz;
		s->p_offset = ph[i].p_offset;

		if (ph[i].p_vaddr < zls.zbase)
			zls.zbase = ph[i].p_vaddr;
	}
	if (zls.n_seg == 0)
		return 0;

	zls.valid = 1;
	return 1;
}

int
zload2_parse_and_place(uint64_t elf_addr, uint64_t pa_base,
                        uint64_t *entry_pa_out)
{
	uint64_t total_bytes = 0;
	int i;

	zld2_bc(ZBC_MAGIC, ZLD2_MAGIC);
	zld2_bc(ZBC_FAIL_REASON, 0u);

	if (!zload2_parse_elf(elf_addr) || pa_base == 0) {
		zld2_bc(ZBC_ELF_VALID, 0u);
		zld2_bc(ZBC_FAIL_REASON, 1u);   /* bad ELF header (or pa_base==0) */
		return 0;
	}
	zld2_bc(ZBC_ELF_VALID, 1u);
	zld2_bc(ZBC_N_SEG, (uint32_t)zls.n_seg);
	zld2_bc(ZBC_ZBASE_LO, (uint32_t)zls.zbase);
	zld2_bc(ZBC_PA_BASE_LO, (uint32_t)pa_base);

	if (zls.n_seg <= 0 || zls.n_seg > (int)ZLOAD2_MAX_PHDR) {
		zld2_bc(ZBC_FAIL_REASON, 2u);   /* zero/too-many PT_LOAD segments */
		return 0;
	}

	/* SAFETY PASS: validate every segment's resolved destination fits
	 * inside [pa_base, pa_base + ZLOAD2_MAX_IMAGE_SIZE) BEFORE copying
	 * anything -- a rejected image is placed atomically-nothing. This is
	 * the check kload.c's general case does not need (FreeBSD's DRAM
	 * gigabyte has no independent per-guest ceiling) but this loader does,
	 * since its whole purpose is feeding an isolated 32 MiB slice (see
	 * zload2.h's header comment). */
	for (i = 0; i < zls.n_seg; i++) {
		struct zload2_seg *s = &zls.seg[i];
		uint64_t dest = pa_base + (s->p_vaddr - zls.zbase);
		uint64_t dest_end = dest + s->p_memsz;

		if (dest < pa_base || dest_end < dest ||
		    dest_end > pa_base + ZLOAD2_MAX_IMAGE_SIZE) {
			zld2_bc(ZBC_FAIL_REASON, 3u);   /* outside the safety window */
			return 0;
		}
	}

	/* PLACEMENT PASS: every segment already validated above -- copy +
	 * zero-fill + cache-maintain, same per-segment sequence as
	 * kload_place_segments(). */
	for (i = 0; i < zls.n_seg; i++) {
		struct zload2_seg *s = &zls.seg[i];
		uint64_t dest = pa_base + (s->p_vaddr - zls.zbase);

		if (s->p_filesz)
			memcpy((void *)dest, (const void *)(elf_addr + s->p_offset),
			       (unsigned long)s->p_filesz);
		if (s->p_memsz > s->p_filesz)
			memset((void *)(dest + s->p_filesz), 0,
			       (unsigned long)(s->p_memsz - s->p_filesz));
		if (s->p_memsz)
			zload2_cache_clean_inval(dest, s->p_memsz);

		total_bytes += s->p_memsz;
	}

	zls.pa_base = pa_base;
	zls.entry_pa = pa_base + (zls.e_entry - zls.zbase);

	zld2_bc(ZBC_ENTRY_PA_LO, (uint32_t)zls.entry_pa);
	zld2_bc(ZBC_BYTES_COPIED, (uint32_t)total_bytes);

	if (entry_pa_out)
		*entry_pa_out = zls.entry_pa;
	return 1;
}
