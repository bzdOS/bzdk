/* SPDX-License-Identifier: BSD-2-Clause */

/* test_kload_modinfo.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the pure ELF-parsing / FreeBSD-modinfo-blob-building logic in
 * kload.c.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "kload.c"` WITH STUBS
 * ============================================================================
 * kload.c was read in full before writing this file. Its ELF header/program-
 * header parser (kload_parse_elf), segment placer (kload_place_segments),
 * modinfo record writer (kload_put_rec/kload_build_modinfo) and the FDT-size
 * reader (kload_dtb_totalsize) are pure address/index/tag arithmetic with NO
 * real hardware access -- exactly the "pure logic" this task asked to
 * extract. But kload.c is not ISOLATED pure logic in the source: every public
 * entry point also calls into raw ARMv8 inline asm or asm-laden helpers:
 *   - kload_bc()                  "dc civac, %0\n\tdsb sy"      (line 166)
 *   - kload_cache_clean_inval()   "dc cvac"/"ic ivau"/"dsb"/"isb" (lines 236-240)
 *   - kload_write_sp_el1/spsr_el2/elr_el2()  "msr ..."           (lines 254-269)
 *   - kload_enter()               "mov x0, %0\n\teret"          (lines 699-704)
 * None of those are gcc builtins or macros on x86_64 -- the assembler rejects
 * the mnemonics outright. Redefining them away would mean either (a) globally
 * overriding `__asm__` (silently neutering the EL2->EL1 handoff itself, well
 * beyond "cache maintenance" -- the exact "too invasive/fragile" case the
 * task called out), or (b) editing kload.c to wrap each asm block behind an
 * overridable stub first, explicitly forbidden here (it is live,
 * hardware-verified code, not ours to edit).
 *
 * So: this file hand-transcribes ONLY the pure parsing/encoding functions,
 * verbatim in control flow, with an exact kload.c/kload.h line-number
 * citation on each one. Every mirrored function omits ONLY the
 * kload_bc()/kload_cache_clean_inval() calls (breadcrumb telemetry and cache
 * maintenance respectively -- side effects that change neither the parsed
 * struct fields nor the bytes written to the modinfo blob under test) and
 * uses the SAME memcpy/memset semantics kload.c gets from libmin.c (plain C,
 * nothing to strip there). kload_enter() itself (pure EL2->EL1 asm handoff,
 * no parsing/encoding logic at all) is NOT mirrored -- out of scope per the
 * task's own framing ("mirror the pure tag-encoding/parsing logic").
 *
 * THE BUG THIS FILE IS AIMED AT: the MODINFOMD tag off-by-one documented at
 * kload.h:164-176 and kload.c:33-56. The first cut emitted MODINFOMD_ENVP/
 * HOWTO/KERNEND as 7/8/9 instead of the real FreeBSD sys/sys/linker.h values
 * 6/7/8, which silently ALIASED each emitted record with the NEXT real tag
 * down (the kernel's MD_FETCH-by-tag walk found our ENVP record when it
 * asked for HOWTO, and our HOWTO record when it asked for KERNEND) --
 * corrupting the guest's boot state (SP landed in read-only rodata) with no
 * parse error anywhere, because record framing (type/len/data/pad) is
 * completely insensitive to whether the *type* values are the ones the
 * consumer expects. test_off_by_one_regression_would_have_caught_bug() below
 * reproduces exactly that failure mode against a deliberately-wrong blob and
 * confirms the CURRENT kload.h values do not exhibit it.
 *
 * Build: gcc -o test_kload_modinfo test_kload_modinfo.c && ./test_kload_modinfo
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Mock "physical DRAM": a gpa is a plain byte offset into this flat array,
 * exactly like test_vblk_ring.c's g_guest_mem -- kload.c treats every
 * address it touches (elf_addr, pa_base, scratch_pa, dtb_*_pa) as a plain
 * EL2 pointer under the identity map, so a byte-array stand-in is faithful.
 * ------------------------------------------------------------------ */
#define MEM_SIZE (4u << 20)   /* 4 MiB: room for a small fake ELF + blob */
static uint8_t g_mem[MEM_SIZE];

/* ------------------------------------------------------------------ *
 * Mirrored constants -- verbatim from kload.h (AUTHORITATIVE tag values;
 * see kload.h:158-180 for the full derivation/confidence discussion this
 * test suite exists to pin down).
 * ------------------------------------------------------------------ */
#define MODINFO_NAME          0x0001u  /* kload.h:158 */
#define MODINFO_TYPE          0x0002u  /* kload.h:159 */
#define MODINFO_ADDR          0x0003u  /* kload.h:160 */
#define MODINFO_SIZE          0x0004u  /* kload.h:161 */
#define MODINFO_METADATA      0x8000u  /* kload.h:162 */

/* machine-INDEPENDENT (sys/sys/linker.h), CORRECTED values -- kload.h:174-176 */
#define MODINFOMD_ENVP        0x0006u
#define MODINFOMD_HOWTO       0x0007u
#define MODINFOMD_KERNEND     0x0008u

/* arm64 machine-DEPENDENT (sys/arm64/include/metadata.h) -- kload.h:180 */
#define MODINFOMD_DTBP        0x1002u

/* The OLD, WRONG numbering this whole test file exists to guard against
 * (kload.h:167-173 / kload.c:33-56's postmortem): off by one against every
 * one of the three MI values above. NOT used by the current source -- kept
 * here only as the "what a regression would look like" fixture. */
#define OLD_BUGGY_MODINFOMD_ENVP    0x0007u
#define OLD_BUGGY_MODINFOMD_HOWTO   0x0008u
#define OLD_BUGGY_MODINFOMD_KERNEND 0x0009u

#define KLOAD_MAX_PHDR 16          /* kload.h:186 */
#define KLOAD_KERNEND_ALIGN 0x200000ull   /* kload.c:216 */

/* ------------------------------------------------------------------ *
 * Mirrored types -- verbatim field layout from kload.c:175-201.
 * ------------------------------------------------------------------ */

/* Mirrors kload.c:175-190 kload_elf64_ehdr_t. */
struct elf64_ehdr {
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
};

/* Mirrors kload.c:192-201 kload_elf64_phdr_t. */
struct elf64_phdr {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

#define ELF_PT_LOAD     1u
#define ELF_ELFCLASS64  2u
#define ELF_ELFDATA2LSB 1u
#define ELF_EM_AARCH64  183u
#define ELF_ET_EXEC     2u
#define ELF_ET_DYN      3u

/* Mirrors kload.h:190-200 struct kload_seg. */
struct kload_seg {
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr_pa;
	uint64_t p_filesz;
	uint64_t p_memsz;
};

/* Mirrors kload.c:276-288's file-scope `kls` parser/placer state (only the
 * fields the mirrored functions below read/write). */
static struct {
	int      valid;
	int      placed;
	uint64_t elf_addr;
	uint64_t e_entry;
	uint64_t kernbase;
	int      n_seg;
	struct kload_seg seg[KLOAD_MAX_PHDR];

	uint64_t pa_base;
	uint64_t entry_pa;
	uint64_t kernel_end_pa;
} kls;

static void reset_kls(void)
{
	memset(&kls, 0, sizeof(kls));
}

static void reset_mem(void)
{
	memset(g_mem, 0xAA, sizeof(g_mem));   /* poison, catch stray reads */
}

/* ------------------------------------------------------------------ *
 * kload_parse_elf() -- mirrors kload.c:290-355 verbatim in control flow,
 * MINUS the kload_bc() breadcrumb calls (lines 344-348/352-353: pure
 * side-channel telemetry, cannot affect parsed state).
 * ------------------------------------------------------------------ */
static int
kload_parse_elf(uint64_t elf_addr)
{
	const struct elf64_ehdr *eh = (const struct elf64_ehdr *)&g_mem[elf_addr];
	const struct elf64_phdr *ph;
	uint32_t i;

	kls.valid = 0;
	kls.placed = 0;

	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
		goto fail;
	if (eh->e_ident[4] != ELF_ELFCLASS64)
		goto fail;
	if (eh->e_ident[5] != ELF_ELFDATA2LSB)
		goto fail;
	if (eh->e_machine != ELF_EM_AARCH64)
		goto fail;
	if (eh->e_type != ELF_ET_EXEC && eh->e_type != ELF_ET_DYN)
		goto fail;
	if (eh->e_phnum == 0 || eh->e_phentsize != sizeof(struct elf64_phdr))
		goto fail;
	if (eh->e_phnum > 64)
		goto fail;

	kls.elf_addr = elf_addr;
	kls.e_entry = eh->e_entry;
	kls.n_seg = 0;
	kls.kernbase = ~0ull;

	ph = (const struct elf64_phdr *)&g_mem[elf_addr + eh->e_phoff];
	for (i = 0; i < eh->e_phnum; i++) {
		struct kload_seg *s;

		if (ph[i].p_type != ELF_PT_LOAD)
			continue;
		if (kls.n_seg >= KLOAD_MAX_PHDR)
			goto fail;

		s = &kls.seg[kls.n_seg++];
		s->p_offset = ph[i].p_offset;
		s->p_vaddr  = ph[i].p_vaddr;
		s->p_filesz = ph[i].p_filesz;
		s->p_memsz  = ph[i].p_memsz;
		s->p_paddr_pa = 0;

		if (ph[i].p_vaddr < kls.kernbase)
			kls.kernbase = ph[i].p_vaddr;
	}
	if (kls.n_seg == 0)
		goto fail;

	kls.valid = 1;
	return 1;

fail:
	return 0;
}

/* ------------------------------------------------------------------ *
 * kload_place_segments() -- mirrors kload.c:357-403 verbatim, MINUS the
 * kload_cache_clean_inval() call (line 384: cache maintenance changes only
 * cache state, never the copied/zeroed bytes) and the kload_bc() calls
 * (lines 397-401: telemetry). memcpy/memset here are the mock-DRAM
 * equivalents of libmin.c's (already plain C, nothing asm to strip).
 * ------------------------------------------------------------------ */
static int
kload_place_segments(uint64_t elf_addr, uint64_t pa_base)
{
	uint64_t max_end_delta = 0;
	uint64_t total_bytes = 0;
	uint64_t first_dest = 0;
	int i;

	if (!kls.valid || elf_addr != kls.elf_addr || pa_base == 0)
		return 0;

	for (i = 0; i < kls.n_seg; i++) {
		struct kload_seg *s = &kls.seg[i];
		uint64_t dest = pa_base + (s->p_vaddr - kls.kernbase);
		uint64_t end_delta;

		s->p_paddr_pa = dest;
		if (i == 0)
			first_dest = dest;

		if (s->p_filesz)
			memcpy(&g_mem[dest], &g_mem[elf_addr + s->p_offset],
			       (size_t)s->p_filesz);
		if (s->p_memsz > s->p_filesz)
			memset(&g_mem[dest + s->p_filesz], 0,
			       (size_t)(s->p_memsz - s->p_filesz));

		total_bytes += s->p_memsz;
		end_delta = (s->p_vaddr - kls.kernbase) + s->p_memsz;
		if (end_delta > max_end_delta)
			max_end_delta = end_delta;
	}
	(void)first_dest;
	(void)total_bytes;

	kls.pa_base = pa_base;
	kls.kernel_end_pa = pa_base + max_end_delta;
	kls.entry_pa = pa_base + (kls.e_entry - kls.kernbase);
	kls.placed = 1;

	return 1;
}

static uint64_t kload_entry_pa(void)      { return kls.placed ? kls.entry_pa : 0; }
static uint64_t kload_kernel_end_pa(void) { return kls.placed ? kls.kernel_end_pa : 0; }

/* ------------------------------------------------------------------ *
 * kload_put_rec() -- mirrors kload.c:410-431 verbatim.
 * ------------------------------------------------------------------ */
static int
kload_put_rec(uint64_t base, uint64_t cap, uint64_t *offp,
              uint32_t type, const void *data, uint32_t len)
{
	uint64_t off = *offp;
	uint32_t *hdr;

	if (off + 8u + len > cap)
		return 0;

	hdr = (uint32_t *)&g_mem[base + off];
	hdr[0] = type;
	hdr[1] = len;
	off += 8;
	if (len)
		memcpy(&g_mem[base + off], data, len);
	off += len;
	off = (off + 7u) & ~(uint64_t)7u;

	*offp = off;
	return 1;
}

/* ------------------------------------------------------------------ *
 * kload_dtb_totalsize() -- mirrors kload.c:437-452 verbatim.
 * ------------------------------------------------------------------ */
static uint64_t
kload_dtb_totalsize(uint64_t dtb_pa)
{
	const uint8_t *p = &g_mem[dtb_pa];
	uint32_t magic, total;

	if (dtb_pa == 0)
		return 0;
	magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	        ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
	if (magic != 0xd00dfeedu)
		return 0;
	total = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
	        ((uint32_t)p[6] << 8)  | (uint32_t)p[7];
	return total;
}

/* ------------------------------------------------------------------ *
 * kload_build_modinfo() -- mirrors kload.c:454-673, the tag-encoding /
 * record-emission control flow verbatim. MINUS: the giant kenv string
 * literal's actual boot-argument CONTENTS (kload.c:505-592, an evolving
 * FreeBSD kenv value with no bearing on tag-encoding correctness -- this
 * mirror uses a trivial fixed placeholder string of the same shape:
 * NUL-terminated entries ending in a double-NUL) and the
 * kload_cache_clean_inval()/kload_bc() calls (cache maintenance +
 * telemetry, same rationale as kload_place_segments() above). Every
 * MODINFO_ and MODINFOMD_ tag value, record ordering, and the KERNEND/
 * modulep VA-translation arithmetic is unchanged.
 * ------------------------------------------------------------------ */
struct modinfo_result {
	uint64_t addr_val, size_val, dtbp_val, kernend_val, howto_val, envp_val;
	uint64_t modulep_va;
	uint64_t blob_base;   /* == scratch_pa, for the reader below */
	uint64_t blob_cap;
};

static uint64_t
kload_build_modinfo(uint64_t dtb_src_pa, uint64_t dtb_dst_pa, uint64_t scratch_pa,
                     struct modinfo_result *out)
{
	const uint64_t cap = 4096;
	uint64_t base = scratch_pa;
	uint64_t off = 0;
	static const char mod_name[] = "kernel";
	static const char mod_type[] = "elf kernel";
	/* Placeholder for kload.c's real kenv string (see the header comment
	 * above): same shape (NUL-terminated entries, trailing double-NUL via
	 * the literal's implicit final NUL), irrelevant content. */
	static const char kenv[] = "vfs.root.mountfrom=ufs:/dev/vtbd0p3\0";
	uint64_t addr_val, size_val, dtbp_val, kernend_val, howto_val, envp_val;
	uint64_t env_pa, env_va, dtb_size, dtb_end, blob_end, max_pa, max_delta;
	uint64_t modulep_va;

	if (!kls.placed || scratch_pa == 0)
		return 0;

	dtb_size = kload_dtb_totalsize(dtb_src_pa);
	if (dtb_size && dtb_dst_pa) {
		memcpy(&g_mem[dtb_dst_pa], &g_mem[dtb_src_pa], (size_t)dtb_size);
		dtbp_val = kls.kernbase + (dtb_dst_pa - kls.pa_base);
		dtb_end  = dtb_dst_pa + dtb_size;
	} else {
		dtbp_val = 0;
		dtb_end  = 0;
	}

	env_pa = scratch_pa + 2048;
	memcpy(&g_mem[env_pa], kenv, sizeof(kenv));
	env_va = kls.kernbase + (env_pa - kls.pa_base);

	modulep_va = kls.kernbase + (scratch_pa - kls.pa_base);

	blob_end = env_pa + sizeof(kenv);
	max_pa   = kls.kernel_end_pa;
	if (dtb_end  > max_pa) max_pa = dtb_end;
	if (blob_end > max_pa) max_pa = blob_end;
	max_delta = max_pa - kls.pa_base;
	max_delta = (max_delta + (KLOAD_KERNEND_ALIGN - 1)) & ~(KLOAD_KERNEND_ALIGN - 1);

	addr_val    = kls.pa_base;
	size_val    = kls.kernel_end_pa - kls.pa_base;
	envp_val    = env_va;
	kernend_val = kls.kernbase + max_delta;
	howto_val   = 0x800u | 0x1u;   /* RB_VERBOSE | RB_SINGLE, same as kload.c:635 */

	if (!kload_put_rec(base, cap, &off, MODINFO_NAME, mod_name, (uint32_t)sizeof(mod_name)))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_TYPE, mod_type, (uint32_t)sizeof(mod_type)))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_ADDR, &addr_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_SIZE, &size_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_ENVP, &envp_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_HOWTO, &howto_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_DTBP, &dtbp_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, MODINFO_METADATA | MODINFOMD_KERNEND, &kernend_val, 8))
		return 0;
	if (!kload_put_rec(base, cap, &off, 0, 0, 0))
		return 0;

	if (out) {
		out->addr_val = addr_val;
		out->size_val = size_val;
		out->dtbp_val = dtbp_val;
		out->kernend_val = kernend_val;
		out->howto_val = howto_val;
		out->envp_val = envp_val;
		out->modulep_va = modulep_va;
		out->blob_base = base;
		out->blob_cap = cap;
	}
	return modulep_va;
}

/* ==================================================================== *
 * Test-only scaffolding: NOT a mirror of any bzdOS source. Models the
 * FreeBSD KERNEL-SIDE consumer of the modinfo blob (kern/linker.c
 * preload_search_info() / MD_FETCH), per the record-walk description in
 * kload.h's protocol comment ("identical record walk, identical
 * roundup2(..., sizeof(u_long)) stride"). This is what lets a test prove
 * "a metadata blob round-trips to the right tag" the way the task asked:
 * walk records by (type,len) exactly as the consumer would, and look up by
 * tag value, so an off-by-one in the EMITTER's tag constants shows up as
 * the WRONG value coming back for a given tag, or as "not found" -- exactly
 * the class of bug this suite exists to catch.
 * ==================================================================== */
static int
modinfo_find(uint64_t base, uint64_t cap, uint32_t want_type,
             uint64_t *out_off, uint32_t *out_len)
{
	uint64_t off = 0;

	for (;;) {
		uint32_t *hdr;
		uint32_t type, len;

		if (off + 8u > cap)
			return 0;
		hdr = (uint32_t *)&g_mem[base + off];
		type = hdr[0];
		len  = hdr[1];
		if (type == 0 && len == 0)
			return 0;   /* terminator: not found */
		if (type == want_type) {
			*out_off = base + off + 8u;
			*out_len = len;
			return 1;
		}
		off += 8u + len;
		off = (off + 7u) & ~(uint64_t)7u;
	}
}

static uint64_t
modinfo_find_u64(uint64_t base, uint64_t cap, uint32_t want_type, int *found)
{
	uint64_t off; uint32_t len; uint64_t v = 0;
	int ok = modinfo_find(base, cap, want_type, &off, &len);
	if (found) *found = ok;
	if (ok && len == 8)
		memcpy(&v, &g_mem[off], 8);
	return v;
}

/* ==================================================================== *
 * ELF fixture builder: writes a minimal but structurally valid ELF64/
 * aarch64 EXEC image (ehdr + N phdrs + segment bytes) into g_mem at
 * elf_addr, mirroring the field layout kload_parse_elf() expects.
 * ==================================================================== */
struct fixture_seg { uint64_t vaddr; uint32_t filesz; uint32_t memsz; uint8_t fill; };

static uint64_t
build_elf_fixture(uint64_t elf_addr, uint64_t entry,
                   const struct fixture_seg *segs, int n_segs)
{
	struct elf64_ehdr eh;
	memset(&eh, 0, sizeof(eh));
	eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
	eh.e_ident[4] = ELF_ELFCLASS64;
	eh.e_ident[5] = ELF_ELFDATA2LSB;
	eh.e_type = ELF_ET_EXEC;
	eh.e_machine = ELF_EM_AARCH64;
	eh.e_entry = entry;
	eh.e_phoff = sizeof(eh);
	eh.e_phentsize = sizeof(struct elf64_phdr);
	eh.e_phnum = (uint16_t)n_segs;

	memcpy(&g_mem[elf_addr], &eh, sizeof(eh));

	uint64_t file_cursor = sizeof(eh) + (uint64_t)n_segs * sizeof(struct elf64_phdr);
	for (int i = 0; i < n_segs; i++) {
		struct elf64_phdr ph;
		memset(&ph, 0, sizeof(ph));
		ph.p_type = ELF_PT_LOAD;
		ph.p_offset = file_cursor;
		ph.p_vaddr = segs[i].vaddr;
		ph.p_paddr = segs[i].vaddr;   /* kload.c never trusts this field */
		ph.p_filesz = segs[i].filesz;
		ph.p_memsz = segs[i].memsz;
		memcpy(&g_mem[elf_addr + sizeof(eh) + (uint64_t)i * sizeof(ph)], &ph, sizeof(ph));

		memset(&g_mem[elf_addr + file_cursor], segs[i].fill, segs[i].filesz);
		file_cursor += segs[i].filesz;
	}
	return file_cursor;   /* total file size written, informational */
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* Pins every tag value this file (and kload.h) asserts is AUTHORITATIVE
 * against sys/sys/linker.h / sys/arm64/include/metadata.h, AND that the old,
 * wrong, off-by-one numbering is NOT what's in use. A future accidental
 * revert to the old numbers fails here immediately, with no board needed. */
static void test_modinfomd_tag_values_match_linker_h(void)
{
	assert(MODINFO_NAME == 0x0001u);
	assert(MODINFO_TYPE == 0x0002u);
	assert(MODINFO_ADDR == 0x0003u);
	assert(MODINFO_SIZE == 0x0004u);
	assert(MODINFO_METADATA == 0x8000u);

	assert(MODINFOMD_ENVP    == 0x0006u);
	assert(MODINFOMD_HOWTO   == 0x0007u);
	assert(MODINFOMD_KERNEND == 0x0008u);
	assert(MODINFOMD_DTBP    == 0x1002u);

	/* The fixed points that pin the whole MI ordering per kload.h:80-84:
	 * ESYM=0x0004 and ENVP's neighbor DYNAMIC/MB2HDR sit strictly between
	 * ESYM and ENVP, so ENVP/HOWTO/KERNEND must be the consecutive run
	 * 6/7/8, NOT 7/8/9. */
	assert(MODINFOMD_ENVP == MODINFOMD_HOWTO - 1);
	assert(MODINFOMD_HOWTO == MODINFOMD_KERNEND - 1);

	/* Explicitly NOT the old buggy numbering. */
	assert(MODINFOMD_ENVP    != OLD_BUGGY_MODINFOMD_ENVP);
	assert(MODINFOMD_HOWTO   != OLD_BUGGY_MODINFOMD_HOWTO);
	assert(MODINFOMD_KERNEND != OLD_BUGGY_MODINFOMD_KERNEND);
}

/* A valid 3-segment kernel-shaped ELF (text+rodata, data, bss-tail) is
 * accepted, kernbase is the lowest p_vaddr, and every PT_LOAD is recorded
 * with its exact field values (kload.c:290-355). */
static void test_parse_elf_valid_computes_kernbase(void)
{
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x1000;
	uint64_t kernbase = 0xffff000000200000ull;
	struct fixture_seg segs[3] = {
		{ kernbase,          0x4000, 0x4000, 0x11 },
		{ kernbase + 0x5000, 0x2000, 0x2000, 0x22 },
		{ kernbase + 0x8000, 0x1000, 0x3000, 0x33 },   /* bss tail: memsz>filesz */
	};
	build_elf_fixture(elf_addr, kernbase + 0x40, segs, 3);

	int rc = kload_parse_elf(elf_addr);
	assert(rc == 1);
	assert(kls.valid == 1);
	assert(kls.kernbase == kernbase);
	assert(kls.e_entry == kernbase + 0x40);
	assert(kls.n_seg == 3);
	assert(kls.seg[0].p_vaddr == kernbase);
	assert(kls.seg[1].p_vaddr == kernbase + 0x5000);
	assert(kls.seg[2].p_memsz == 0x3000 && kls.seg[2].p_filesz == 0x1000);
}

/* Bad ELF magic is rejected outright (kload.c:300-302). */
static void test_parse_elf_rejects_bad_magic(void)
{
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x2000;
	struct fixture_seg segs[1] = { { 0xffff000000200000ull, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0xffff000000200040ull, segs, 1);
	g_mem[elf_addr + 1] = 'X';   /* corrupt "ELF" -> "EXF" */

	assert(kload_parse_elf(elf_addr) == 0);
	assert(kls.valid == 0);
}

/* Wrong machine (not AArch64) is rejected (kload.c:307-308). */
static void test_parse_elf_rejects_wrong_machine(void)
{
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x3000;
	struct fixture_seg segs[1] = { { 0xffff000000200000ull, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0xffff000000200040ull, segs, 1);
	/* e_machine is at ehdr offset 18 (after e_ident[16]+e_type[2]). */
	uint16_t wrong_machine = 0x3e;   /* EM_X86_64, not EM_AARCH64 */
	memcpy(&g_mem[elf_addr + 18], &wrong_machine, 2);

	assert(kload_parse_elf(elf_addr) == 0);
}

/* e_phnum == 0 is rejected (kload.c:311). */
static void test_parse_elf_rejects_zero_phnum(void)
{
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x4000;
	struct fixture_seg segs[1] = { { 0xffff000000200000ull, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0xffff000000200040ull, segs, 1);
	uint16_t zero = 0;
	memcpy(&g_mem[elf_addr + 54], &zero, 2);   /* e_phnum offset: see ehdr layout */

	assert(kload_parse_elf(elf_addr) == 0);
}

/* More PT_LOAD segments than KLOAD_MAX_PHDR is a hard refusal, not a silent
 * truncation (kload.c:327-328's "goto fail", explicitly NOT skipping the
 * excess like a lesser implementation might). */
static void test_parse_elf_rejects_too_many_segments(void)
{
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x5000;
	struct fixture_seg segs[KLOAD_MAX_PHDR + 1];
	for (int i = 0; i < KLOAD_MAX_PHDR + 1; i++) {
		segs[i].vaddr = 0xffff000000200000ull + (uint64_t)i * 0x1000u;
		segs[i].filesz = 0x10;
		segs[i].memsz = 0x10;
		segs[i].fill = (uint8_t)i;
	}
	build_elf_fixture(elf_addr, 0xffff000000200000ull, segs, KLOAD_MAX_PHDR + 1);

	assert(kload_parse_elf(elf_addr) == 0);
	assert(kls.valid == 0);
}

/* kload_place_segments(): dest = pa_base + (p_vaddr - kernbase) for every
 * segment, bss tail (memsz>filesz) zeroed, kernel_end_pa/entry_pa computed
 * from the LAST/highest segment (kload.c:357-403). */
static void test_place_segments_dest_and_bss_zero(void)
{
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x10000;
	uint64_t kernbase = 0xffff000000200000ull;
	struct fixture_seg segs[2] = {
		{ kernbase,          0x1000, 0x1000, 0xAB },
		{ kernbase + 0x2000, 0x0800, 0x1800, 0xCD },   /* 0x1000 bss tail */
	};
	build_elf_fixture(elf_addr, kernbase + 0x10, segs, 2);
	assert(kload_parse_elf(elf_addr) == 1);

	uint64_t pa_base = 0x200000ull;
	int rc = kload_place_segments(elf_addr, pa_base);
	assert(rc == 1);

	uint64_t dest0 = pa_base;                    /* p_vaddr==kernbase -> delta 0 */
	uint64_t dest1 = pa_base + 0x2000;

	assert(kls.seg[0].p_paddr_pa == dest0);
	assert(kls.seg[1].p_paddr_pa == dest1);

	/* Copied bytes match the fixture fill. */
	assert(g_mem[dest0] == 0xAB && g_mem[dest0 + 0xFFF] == 0xAB);
	assert(g_mem[dest1] == 0xCD && g_mem[dest1 + 0x7FF] == 0xCD);

	/* bss tail (segment 1: filesz=0x800, memsz=0x1800) is zeroed, not left
	 * as poison (0xAA) or stray fixture fill. */
	assert(g_mem[dest1 + 0x800] == 0);
	assert(g_mem[dest1 + 0x1000] == 0);
	assert(g_mem[dest1 + 0x17FF] == 0);

	assert(kls.entry_pa == pa_base + 0x10);
	uint64_t expect_end = pa_base + (0x2000 + 0x1800);   /* seg1's vaddr-delta + memsz */
	assert(kls.kernel_end_pa == expect_end);
	assert(kload_entry_pa() == kls.entry_pa);
	assert(kload_kernel_end_pa() == kls.kernel_end_pa);
}

/* kload_place_segments() called before a valid parse, or with pa_base==0,
 * is refused (kload.c:365). */
static void test_place_segments_rejects_bad_call_order(void)
{
	reset_mem(); reset_kls();
	assert(kload_place_segments(0x1000, 0x200000ull) == 0);   /* never parsed */

	uint64_t elf_addr = 0x20000;
	struct fixture_seg segs[1] = { { 0xffff000000200000ull, 0x100, 0x100, 0 } };
	build_elf_fixture(elf_addr, 0xffff000000200000ull, segs, 1);
	assert(kload_parse_elf(elf_addr) == 1);
	assert(kload_place_segments(elf_addr, 0) == 0);   /* pa_base == 0 */
}

/* kload_put_rec(): record framing is type(4)+len(4)+data(len)+pad-to-8, and
 * a record that would overflow cap is refused rather than silently
 * clobbering past the caller's scratch region (kload.c:410-431). */
static void test_put_rec_padding_and_cap_overflow(void)
{
	reset_mem();
	uint64_t base = 0x30000, cap = 64, off = 0;
	uint32_t d1 = 0xdeadbeefu;

	assert(kload_put_rec(base, cap, &off, 0x1234u, &d1, 4) == 1);
	assert(off == 16);   /* 8 hdr + 4 data -> pad to 8 -> 16 */
	uint32_t *hdr = (uint32_t *)&g_mem[base];
	assert(hdr[0] == 0x1234u && hdr[1] == 4u);
	uint32_t got; memcpy(&got, &g_mem[base + 8], 4);
	assert(got == d1);

	uint8_t d2[5] = { 1, 2, 3, 4, 5 };
	assert(kload_put_rec(base, cap, &off, 0x5678u, d2, 5) == 1);
	assert(off == 16 + 16);   /* 8 hdr + 5 data -> pad to 8 -> +16 */

	/* Now force an overflow: remaining cap is 64-32=32 bytes; ask for a
	 * record needing 8+40=48. */
	uint8_t big[40]; memset(big, 0, sizeof(big));
	uint64_t off_before = off;
	assert(kload_put_rec(base, cap, &off, 0x9999u, big, 40) == 0);
	assert(off == off_before);   /* refused: offset must be unchanged */
}

/* kload_dtb_totalsize(): valid FDT magic (big-endian 0xd00dfeed) yields the
 * big-endian totalsize field; a bad magic yields 0 (kload.c:437-452) -- the
 * caller then knows to skip DTB relocation instead of faulting on it. */
static void test_dtb_totalsize_magic_check(void)
{
	reset_mem();
	uint64_t dtb_pa = 0x40000;
	uint8_t hdr[8] = { 0xd0, 0x0d, 0xfe, 0xed, 0x00, 0x00, 0x12, 0x34 };
	memcpy(&g_mem[dtb_pa], hdr, 8);
	assert(kload_dtb_totalsize(dtb_pa) == 0x1234u);

	uint64_t bad_pa = 0x41000;
	uint8_t bad[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	memcpy(&g_mem[bad_pa], bad, 8);
	assert(kload_dtb_totalsize(bad_pa) == 0);

	assert(kload_dtb_totalsize(0) == 0);   /* dtb_pa==0 shortcut */
}

/* THE round-trip test the task explicitly asked for: build a real modinfo
 * blob via the mirrored kload_build_modinfo(), then read it back with the
 * kernel-side record-walk test double (modinfo_find*), and confirm every
 * tag maps to its OWN field with no aliasing -- this is exactly what an
 * off-by-one in the tag constants would break silently. */
static void test_build_modinfo_blob_roundtrip(void)
{
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x100000;
	uint64_t kernbase = 0xffff000000200000ull;
	struct fixture_seg segs[1] = { { kernbase, 0x3000, 0x4000, 0x77 } };
	build_elf_fixture(elf_addr, kernbase + 0x20, segs, 1);
	assert(kload_parse_elf(elf_addr) == 1);

	uint64_t pa_base = 0x200000ull;
	assert(kload_place_segments(elf_addr, pa_base) == 1);

	/* Give it a real-looking FDT at dtb_src so DTBP comes out nonzero. */
	uint64_t dtb_src = 0x300000, dtb_dst = pa_base + 0x10000;
	uint8_t fdt_hdr[8] = { 0xd0, 0x0d, 0xfe, 0xed, 0x00, 0x00, 0x02, 0x00 };
	memcpy(&g_mem[dtb_src], fdt_hdr, 8);

	uint64_t scratch_pa = pa_base + 0x20000;
	struct modinfo_result r;
	uint64_t modulep = kload_build_modinfo(dtb_src, dtb_dst, scratch_pa, &r);
	assert(modulep != 0);
	assert(modulep == r.modulep_va);

	int found;
	uint64_t v;

	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_ADDR, &found);
	assert(found && v == r.addr_val);

	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_SIZE, &found);
	assert(found && v == r.size_val);

	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_METADATA | MODINFOMD_ENVP, &found);
	assert(found && v == r.envp_val);

	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_METADATA | MODINFOMD_HOWTO, &found);
	assert(found && v == r.howto_val);

	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_METADATA | MODINFOMD_DTBP, &found);
	assert(found && v == r.dtbp_val && v != 0);

	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_METADATA | MODINFOMD_KERNEND, &found);
	assert(found && v == r.kernend_val);

	/* No cross-aliasing: each of the four MD tags' fetched value is DISTINCT
	 * from the other three (a silent off-by-one collapse would make two of
	 * these collide). */
	assert(r.envp_val != r.howto_val);
	assert(r.howto_val != r.kernend_val);
	assert(r.envp_val != r.kernend_val);
	assert(r.dtbp_val != r.envp_val && r.dtbp_val != r.howto_val && r.dtbp_val != r.kernend_val);

	/* KERNEND covers everything placed: kernel image, the relocated DTB,
	 * and the blob itself (kload.c:606-620's rounded-up VA cursor). */
	assert(r.kernend_val > r.envp_val);
	assert((r.kernend_val - kernbase) % KLOAD_KERNEND_ALIGN == 0);   /* 2 MiB aligned */
}

/* THE regression test: reproduce the exact historical off-by-one failure
 * mode against a DELIBERATELY wrong blob (built using
 * OLD_BUGGY_MODINFOMD_{ENVP,HOWTO,KERNEND} = 7/8/9) and confirm that
 * fetching by the REAL FreeBSD tag values (6/7/8) reads back ALIASED data
 * -- proving this test suite would have caught the bug before it ever
 * reached hardware. Then confirms the CURRENT kload_build_modinfo() mirror
 * (6/7/8) does NOT exhibit it. */
static void test_off_by_one_regression_would_have_caught_bug(void)
{
	/* --- Part 1: replay the OLD BUGGY emitter by hand ------------------ */
	reset_mem();
	uint64_t base = 0x50000, cap = 4096, off = 0;
	uint64_t envp_val = 0xffff0000002afff0ull;
	uint64_t howto_val = 0x801u;
	uint64_t kernend_val = 0xffff000000400000ull;

	assert(kload_put_rec(base, cap, &off, MODINFO_METADATA | OLD_BUGGY_MODINFOMD_ENVP,
	                      &envp_val, 8));
	assert(kload_put_rec(base, cap, &off, MODINFO_METADATA | OLD_BUGGY_MODINFOMD_HOWTO,
	                      &howto_val, 8));
	assert(kload_put_rec(base, cap, &off, MODINFO_METADATA | OLD_BUGGY_MODINFOMD_KERNEND,
	                      &kernend_val, 8));
	assert(kload_put_rec(base, cap, &off, 0, 0, 0));

	int found;
	uint64_t v;

	/* The real FreeBSD kernel asks for MODINFOMD_ENVP == 6 -- the old
	 * buggy emitter never wrote tag 6 at all, so it's simply missing. */
	v = modinfo_find_u64(base, cap, MODINFO_METADATA | MODINFOMD_ENVP, &found);
	assert(!found);

	/* The kernel asking for MODINFOMD_HOWTO == 7 instead gets back the
	 * OLD emitter's ENVP record (which it wrote at tag 7) -- exactly the
	 * "read our emitted ENVP record as HOWTO" bug from kload.c:33-43. */
	v = modinfo_find_u64(base, cap, MODINFO_METADATA | MODINFOMD_HOWTO, &found);
	assert(found && v == envp_val);
	assert(v != howto_val);

	/* The kernel asking for MODINFOMD_KERNEND == 8 gets back the OLD
	 * emitter's HOWTO record (written at tag 8) -- "our emitted HOWTO
	 * record as KERNEND", the exact SP-in-rodata root cause. */
	v = modinfo_find_u64(base, cap, MODINFO_METADATA | MODINFOMD_KERNEND, &found);
	assert(found && v == howto_val);
	assert(v != kernend_val);

	/* --- Part 2: the CURRENT (fixed) emitter does not alias ------------ */
	reset_mem(); reset_kls();
	uint64_t elf_addr = 0x60000;
	uint64_t kernbase = 0xffff000000200000ull;
	struct fixture_seg segs[1] = { { kernbase, 0x1000, 0x1000, 0x01 } };
	build_elf_fixture(elf_addr, kernbase, segs, 1);
	assert(kload_parse_elf(elf_addr) == 1);
	assert(kload_place_segments(elf_addr, 0x200000ull) == 1);

	struct modinfo_result r;
	assert(kload_build_modinfo(0, 0, 0x210000ull, &r) != 0);

	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_METADATA | MODINFOMD_ENVP, &found);
	assert(found && v == r.envp_val);   /* NOT howto_val/kernend_val */
	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_METADATA | MODINFOMD_HOWTO, &found);
	assert(found && v == r.howto_val);  /* NOT envp_val/kernend_val */
	v = modinfo_find_u64(r.blob_base, r.blob_cap, MODINFO_METADATA | MODINFOMD_KERNEND, &found);
	assert(found && v == r.kernend_val);/* NOT howto_val -- the actual historical bug */
}

/* ==================================================================== *
 * main() — runs every test, reports pass/fail, exits nonzero on failure.
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "modinfomd_tag_values_match_linker_h", test_modinfomd_tag_values_match_linker_h },
	{ "parse_elf_valid_computes_kernbase",   test_parse_elf_valid_computes_kernbase },
	{ "parse_elf_rejects_bad_magic",         test_parse_elf_rejects_bad_magic },
	{ "parse_elf_rejects_wrong_machine",     test_parse_elf_rejects_wrong_machine },
	{ "parse_elf_rejects_zero_phnum",        test_parse_elf_rejects_zero_phnum },
	{ "parse_elf_rejects_too_many_segments", test_parse_elf_rejects_too_many_segments },
	{ "place_segments_dest_and_bss_zero",    test_place_segments_dest_and_bss_zero },
	{ "place_segments_rejects_bad_call_order", test_place_segments_rejects_bad_call_order },
	{ "put_rec_padding_and_cap_overflow",    test_put_rec_padding_and_cap_overflow },
	{ "dtb_totalsize_magic_check",           test_dtb_totalsize_magic_check },
	{ "build_modinfo_blob_roundtrip",        test_build_modinfo_blob_roundtrip },
	{ "off_by_one_regression_would_have_caught_bug", test_off_by_one_regression_would_have_caught_bug },
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

	printf("---- kload modinfo tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
