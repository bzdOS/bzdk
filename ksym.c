/* SPDX-License-Identifier: BSD-2-Clause */

/* ksym.c — bounded, on-board kernel-symbol resolver. See ksym.h for the full
 * rationale/contract. */
#include <stdint.h>
#include "ksym.h"
#include "kload.h"

/* Elf64_Sym — defined locally (not shared via a header) rather than pulling
 * in a system <elf.h>, matching kload.c's own "freestanding, define what you
 * need" local ELF struct style (kload_elf64_ehdr_t/kload_elf64_phdr_t). */
typedef struct {
	uint32_t st_name;
	uint8_t  st_info;
	uint8_t  st_other;
	uint16_t st_shndx;
	uint64_t st_value;
	uint64_t st_size;
} ksym_elf64_sym_t;

#define KSYM_STT_FUNC        2u
#define KSYM_SHN_UNDEF       0u
#define KSYM_ST_TYPE(info)   ((uint32_t)(info) & 0xfu)

/* Hard cap on symbol-table entries scanned per resolve call, independent of
 * whatever count kload_symtab_info() reports — defensive against a corrupt
 * section header implying an implausible sh_size, exactly the same
 * "bounded regardless of what the caller/data claims" posture backtrace.c's
 * hop cap and coredump.c's byte caps already use elsewhere in this tree. A
 * real FreeBSD GENERIC kernel's .symtab runs a few tens of thousands of
 * entries at most; 262144 is comfortably above that without being
 * "unbounded" in the pathological-input case. */
#define KSYM_MAX_SCAN 262144u

int
ksym_resolve(uint64_t kernel_va, char *name_out, int name_max, uint64_t *offset_out)
{
	uint64_t sym_addr, str_addr, str_size;
	uint32_t sym_count, scan_n, i;
	uint64_t kbase, kend;
	const ksym_elf64_sym_t *best = 0;
	uint64_t best_value = 0;

	if (!name_out || name_max <= 0 || !offset_out)
		return 0;
	name_out[0] = 0;

	if (!kload_symtab_info(&sym_addr, &sym_count, &str_addr, &str_size))
		return 0;

	/* Cheap reject: is this VA even inside the loaded kernel's own image at
	 * all? Skips the scan entirely for HV-own addresses, pre-KVA-mapping
	 * guest addresses, or garbage — exactly the check kload.h's
	 * kload_kernbase()/kload_kernel_end_va() exist to make possible without
	 * a resolver having to recompute anything kload.c already knows. */
	kbase = kload_kernbase();
	kend  = kload_kernel_end_va();
	if (kbase == 0 || kend == 0 || kernel_va < kbase || kernel_va >= kend)
		return 0;

	scan_n = sym_count;
	if (scan_n > KSYM_MAX_SCAN)
		scan_n = KSYM_MAX_SCAN;

	/* Nearest STT_FUNC symbol AT OR BELOW kernel_va: track the entry with
	 * the largest st_value that does not exceed kernel_va. Plain linear
	 * scan — see ksym.h's header comment for why this is the right choice
	 * (unsorted symtab, no allocator to sort it, fault/incident path only,
	 * not a hot loop). */
	for (i = 0; i < scan_n; i++) {
		const ksym_elf64_sym_t *s = (const ksym_elf64_sym_t *)
			(sym_addr + (uint64_t)i * (uint64_t)sizeof(ksym_elf64_sym_t));

		if (KSYM_ST_TYPE(s->st_info) != KSYM_STT_FUNC)
			continue;
		if (s->st_shndx == KSYM_SHN_UNDEF)
			continue;
		if (s->st_value > kernel_va)
			continue;
		if (!best || s->st_value > best_value) {
			best = s;
			best_value = s->st_value;
		}
	}

	if (!best)
		return 0;

	/* Copy the name, bounded by BOTH the caller's buffer and the string
	 * table's own recorded size — a corrupt/truncated st_name never reads
	 * past str_addr+str_size, and the copy always stops within
	 * name_max-1 bytes leaving room for the NUL. */
	{
		uint64_t noff = best->st_name;
		int j = 0;

		if (noff < str_size) {
			const char *cp = (const char *)(str_addr + noff);

			while (j < name_max - 1 && (noff + (uint64_t)j) < str_size && cp[j] != 0) {
				name_out[j] = cp[j];
				j++;
			}
		}
		name_out[j] = 0;
	}

	*offset_out = kernel_va - best_value;
	return 1;
}
