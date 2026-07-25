/* SPDX-License-Identifier: BSD-2-Clause */

/* ksym.h — ROADMAP B3 crash-forensics: bounded, on-board kernel-symbol
 * resolver for the bzdOS EL2 hypervisor.
 *
 * WHY THIS EXISTS
 * ----------------
 * backtrace.c's frame-pointer walker already turns a live (pc, fp, lr) into a
 * bounded list of raw guest return addresses — genuinely useful, but an
 * operator watching a live incident over the network sees only hex numbers
 * until they separately fetch the exact kernel.debug ELF and run addr2line by
 * hand. This module closes that gap FOR THE LIVE/IMMEDIATE case (the coredump
 * path in coredump.c already does the real, complete job for POST-MORTEM kgdb
 * against the host's own kernel.debug — that design is unchanged and correct;
 * this is additive, not a replacement).
 *
 * kload.c already parses the exact same FreeBSD kernel ELF image resident in
 * guest DRAM (that's its whole job — PT_LOAD placement + the modinfo blob);
 * ROADMAP B3 only asked it to ALSO locate (not read) that same ELF's
 * .symtab/.strtab section pair via the section headers it wasn't previously
 * consulting (see kload.h's kload_symtab_info()/kload_kernbase()/
 * kload_kernel_end_va()). This module is the consumer: given a kernel VA
 * (e.g. one entry of backtrace_walk()'s out[]), it does a bounded linear scan
 * of the STT_FUNC symbols kload.c located and returns the nearest one at or
 * below that address, plus the byte offset into it — "symbol+0xoffset",
 * exactly what an operator wants to read next to the raw hex address.
 *
 * Freestanding: <stdint.h> + kload.h only. No libc, no allocation — every
 * loop below is bounded (KSYM_MAX_SCAN caps the symbol-table walk even if a
 * corrupt section header somehow implied an enormous count; the name copy is
 * bounded by both the caller's buffer and the string table's own size).
 *
 * Linear, not binary, search: a FreeBSD kernel's ELF .symtab is NOT sorted by
 * st_value (it's typically in whatever order the linker emitted /
 * discovered them), so a binary search would need an extra sort pass this
 * tree has no allocator/scratch buffer to perform. A single bounded linear
 * pass per resolve call is the correct match for "don't over-engineer" here:
 * this only ever runs on a fault/incident path, not a hot loop.
 */
#ifndef BZDOS_KSYM_H
#define BZDOS_KSYM_H

#include <stdint.h>

/* Resolve `kernel_va` to the nearest STT_FUNC symbol at-or-below it in the
 * kernel ELF kload.c parsed (see kload_symtab_info()), writing the (bounded,
 * NUL-terminated) symbol name into name_out[0..name_max) and the byte offset
 * from that symbol's st_value into *offset_out.
 *
 * Returns 1 on a match, 0 if: no kernel has been kload_parse_elf()'d yet, the
 * kernel ELF has no .symtab (stripped), kernel_va falls outside the loaded
 * kernel image's own [kernbase, kernel_end_va) range entirely (a cheap
 * reject — e.g. an EL2-hypervisor-own address, or a guest address from
 * before the kernel's MMU/KVA mapping was up), or no STT_FUNC symbol's
 * st_value is <= kernel_va. On a 0 return, name_out[0] is set to '\0' and
 * *offset_out is left untouched — callers must check the return value, not
 * infer success from the buffer.
 *
 * name_max must be >= 1. A name longer than name_max-1 bytes is silently
 * truncated (still NUL-terminated) — this is a diagnostic label, not an
 * identifier callers act on programmatically. */
int ksym_resolve(uint64_t kernel_va, char *name_out, int name_max, uint64_t *offset_out);

#endif /* BZDOS_KSYM_H */
