/* SPDX-License-Identifier: BSD-2-Clause */

/* zstage.c — implementation. See zstage.h for the full rationale: why the
 * image lands in low DRAM instead of straight in Zephyr's slice, and why the
 * copy MUST happen on CPU0 before smp_init()/kload_enter().
 *
 * Freestanding: <stdint.h> only; memcpy declared locally with the exact
 * signature libmin.c provides, the same convention kload.c and zload2.c
 * already use (no shared "libmin.h" exists in this tree).
 *
 * Built for the board (breadcrumbs, cache maintenance) but written so that
 * zstage_span() — the only part with any real logic — compiles and runs
 * unchanged in a hosted unit test. Everything board-specific is behind
 * ZSTAGE_HOSTED_TEST.
 */
#include <stdint.h>
#include "zstage.h"

#ifndef ZSTAGE_HOSTED_TEST
#include "zguest_cpu3.h"
#include "hv_addrmap.h"

extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int c, unsigned long n);
#else
#include <string.h>
#endif

/* ------------------------------------------------------------------ *
 * Minimal local ELF64/AArch64 definitions. Duplicated from zload2.c rather
 * than shared, for the same reason zload2.c duplicates kload.c's: see
 * zload2.h's header comment. The duplication is one-directional and inert —
 * this file only ever READS these fields, it never places anything.
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
} zstage_elf64_ehdr_t;

typedef struct {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} zstage_elf64_phdr_t;

#define ZSTAGE_PT_LOAD     1u
#define ZSTAGE_ELFCLASS64  2u
#define ZSTAGE_ELFDATA2LSB 1u
#define ZSTAGE_EM_AARCH64  183u
#define ZSTAGE_ET_EXEC     2u
#define ZSTAGE_ET_DYN      3u

/* Same bound zload2.c enforces (ZLOAD2_MAX_PHDR). Kept a separate constant
 * on purpose: this file rejecting an image zload2 would have accepted (or
 * vice versa) is a bug worth being able to see, not a coincidence worth
 * hiding behind a shared #define. */
#define ZSTAGE_MAX_PHDR 8u

int
zstage_span(const void *elf, uint64_t avail, uint64_t *span_out)
{
	const zstage_elf64_ehdr_t *eh = (const zstage_elf64_ehdr_t *)elf;
	uint64_t phdr_end;
	uint64_t span;
	unsigned n_load = 0;
	unsigned i;

	if (span_out == 0)
		return 0;

	/* Every field below is read only after the window is known to be large
	 * enough to contain it — a completely unstaged window is ordinary DRAM
	 * garbage, and walking off the end of it would fault EL2 itself. */
	if (avail < sizeof(*eh))
		return 0;

	if (eh->e_ident[0] != 0x7Fu || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		return 0;
	if (eh->e_ident[4] != ZSTAGE_ELFCLASS64)
		return 0;
	if (eh->e_ident[5] != ZSTAGE_ELFDATA2LSB)
		return 0;
	if (eh->e_machine != ZSTAGE_EM_AARCH64)
		return 0;
	if (eh->e_type != ZSTAGE_ET_EXEC && eh->e_type != ZSTAGE_ET_DYN)
		return 0;
	if (eh->e_phentsize != sizeof(zstage_elf64_phdr_t))
		return 0;
	if (eh->e_phnum == 0 || eh->e_phnum > ZSTAGE_MAX_PHDR)
		return 0;

	/* Overflow-safe: e_phoff is bounded by avail first, so the multiply and
	 * add below cannot wrap a 64-bit value for any avail this HV could ever
	 * be given. */
	if (eh->e_phoff > avail)
		return 0;
	phdr_end = eh->e_phoff +
		   (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
	if (phdr_end > avail)
		return 0;

	span = phdr_end;

	for (i = 0; i < eh->e_phnum; i++) {
		const zstage_elf64_phdr_t *ph =
			(const zstage_elf64_phdr_t *)((const uint8_t *)elf +
						      eh->e_phoff +
						      (uint64_t)i *
						      eh->e_phentsize);
		uint64_t seg_end;

		if (ph->p_type != ZSTAGE_PT_LOAD)
			continue;
		n_load++;

		/* A zero-filesz PT_LOAD (pure .bss) contributes nothing to copy
		 * but is still a legitimate loadable segment, so it counts
		 * towards n_load. */
		if (ph->p_filesz == 0)
			continue;

		if (ph->p_offset > avail)
			return 0;
		seg_end = ph->p_offset + ph->p_filesz;
		if (seg_end > avail)
			return 0;
		if (seg_end > span)
			span = seg_end;
	}

	if (n_load == 0)
		return 0;

	*span_out = span;
	return 1;
}

#ifndef ZSTAGE_HOSTED_TEST

/* ------------------------------------------------------------------ *
 * Breadcrumb window: HVMAP_ZSTAGE_BC (0x5007A000, "ZSTG") — see
 * hv_addrmap.h.
 *   [0] magic        0x5A535447 ("ZSTG")
 *   [1] state        1 = entered, 2 = no valid image staged (ordinary, not an
 *                    error — see zstage.h), 3 = image too large for the
 *                    destination window (refused, nothing copied),
 *                    4 = copied OK
 *   [2] span_lo      bytes zstage_span() asked for, low 32 bits
 *   [3] copied_lo    bytes actually memcpy'd, low 32 bits
 *   [4] dest_pa_lo   destination physical address, low 32 bits
 *   [5] invalidated  bytes zeroed at dest_pa because this boot staged no
 *                    usable image (see ZSTAGE_INVAL_BYTES below); 0 whenever
 *                    an image WAS copied
 * Published as REAL zeros on entry before anything else runs, so "all zeros"
 * unambiguously means "this code never ran" rather than "it ran and found
 * nothing" — the distinction this project has been burned by before.
 * ------------------------------------------------------------------ */
#define ZSTAGE_MAGIC 0x5A535447u   /* "ZSTG" */

enum {
	ZSTG_MAGIC_IDX = 0,
	ZSTG_STATE_IDX,
	ZSTG_SPAN_IDX,
	ZSTG_COPIED_IDX,
	ZSTG_DEST_PA_IDX,
	ZSTG_INVALIDATED_IDX,
	ZSTG_NWORDS,
};

static inline void
zstg_bc(int i, uint32_t v)
{
	volatile uint32_t *p =
		(volatile uint32_t *)(HVMAP_ZSTAGE_BC + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Clean the copied image out to the point of coherency. Both windows are
 * ordinary Normal-WB DRAM and SMPEN cache coherency means CPU3 would see the
 * bytes anyway; this is belt-and-braces for the same reason zload2.c does it
 * on the placement side, and cheap relative to the copy itself. Deliberately
 * NOT an I-cache invalidate: the bytes copied here are a raw ELF file that is
 * never executed in place — zload2_parse_and_place() copies the actual
 * segments out of it, and it does its own ic ivau on those. */
#define ZSTAGE_CACHE_LINE 64ul

static void
zstage_cache_clean(uint64_t addr, uint64_t len)
{
	uint64_t start = addr & ~(ZSTAGE_CACHE_LINE - 1);
	uint64_t end = addr + len;
	uint64_t o;

	for (o = start; o < end; o += ZSTAGE_CACHE_LINE)
		__asm__ volatile("dc cvac, %0" :: "r"(o) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* How much of the destination to zero when this boot staged no usable image.
 * Only the ELF header (64 bytes) and the program-header table are ever read by
 * zload2_parse_and_place() before it decides an image is valid, and this file's
 * own ZSTAGE_MAX_PHDR bounds that table at 8 entries of 56 bytes; 4 KiB is
 * comfortably more than either and still a trivial memset. */
#define ZSTAGE_INVAL_BYTES 0x1000ul

/* ==========================================================================
 * WHY A FAILED/ABSENT STAGE MUST ACTIVELY WIPE THE DESTINATION
 * ==========================================================================
 * Found on real hardware, 2026-08-10, and it is a genuine trap rather than a
 * theoretical one. A `halt -p` on the guest goes PSCI SYSTEM_OFF -> the HV's
 * own clean WARM reset, and a warm reset PRESERVES DRAM. So the previous
 * boot's copied image is still sitting at dest_pa, byte for byte, when the
 * next boot starts.
 *
 * Observed consequence, live: an image was deliberately staged that
 * zstage_span() correctly rejected (breadcrumb state 2, span 0, copied 0) --
 * and `zboot` then booted a guest anyway, reaching state 3 ("entered guest"),
 * because zload2_parse_and_place() found the PREVIOUS boot's perfectly valid
 * Zephyr image at that address. Nothing anywhere reported a problem.
 *
 * That is exactly the class of confusion this project has repeatedly paid for:
 * an operator whose TFTP silently failed, or who forgot --zguest, would boot
 * the OLD image and have every visible indicator agree that all was well. The
 * breadcrumb said "no valid image staged" and the guest ran regardless, so the
 * two readings contradicted each other and the reassuring one won.
 *
 * So: if this boot did not put an image there, nothing bootable may remain
 * there. zload2 then rejects the wiped header, `zboot` reports 0xBAD1, and the
 * honest answer is the only available one.
 *
 * This cannot destroy a hand-staged image: zguest_stage_copyin() runs during
 * HV init on CPU0, long before EMAC/dbgmon exists to write anything by hand
 * (which is how the pre-bulk-loader milestone staged its payload).
 * ========================================================================== */
static void
zstage_invalidate_dest(uint64_t dest_pa, uint64_t dest_limit)
{
	uint64_t n = ZSTAGE_INVAL_BYTES;

	if (n > dest_limit)
		n = dest_limit;

	memset((void *)dest_pa, 0, (unsigned long)n);
	zstage_cache_clean(dest_pa, n);
	zstg_bc(ZSTG_INVALIDATED_IDX, (uint32_t)n);
}

uint64_t
zstage_copy_to(uint64_t dest_pa, uint64_t dest_limit)
{
	uint64_t span = 0;
	int i;

	for (i = 0; i < ZSTG_NWORDS; i++)
		zstg_bc(i, 0);
	zstg_bc(ZSTG_MAGIC_IDX, ZSTAGE_MAGIC);
	zstg_bc(ZSTG_DEST_PA_IDX, (uint32_t)dest_pa);
	zstg_bc(ZSTG_STATE_IDX, 1u);

	if (!zstage_span((const void *)ZSTAGE_LOW_PA, ZSTAGE_LOW_SIZE, &span)) {
		zstg_bc(ZSTG_STATE_IDX, 2u);
		zstage_invalidate_dest(dest_pa, dest_limit);
		return 0;
	}
	zstg_bc(ZSTG_SPAN_IDX, (uint32_t)span);

	if (span > dest_limit) {
		zstg_bc(ZSTG_STATE_IDX, 3u);
		zstage_invalidate_dest(dest_pa, dest_limit);
		return 0;
	}

	memcpy((void *)dest_pa, (const void *)ZSTAGE_LOW_PA,
	       (unsigned long)span);
	zstage_cache_clean(dest_pa, span);

	zstg_bc(ZSTG_COPIED_IDX, (uint32_t)span);
	zstg_bc(ZSTG_STATE_IDX, 4u);
	return span;
}

void
zguest_stage_copyin(void)
{
	(void)zstage_copy_to(ZG3_ELF_STAGE_PA, ZG3_ELF_STAGE_SIZE);
}

#endif /* !ZSTAGE_HOSTED_TEST */
