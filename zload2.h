/* SPDX-License-Identifier: BSD-2-Clause */

/* zload2.h — small, disjoint ELF64/AArch64 parser+placer for the Zephyr
 * guest running concurrently on CPU3 (dual-guest milestone). Linked ONLY
 * into the `dual` Makefile target — every other target neither compiles
 * nor links this file.
 *
 * WHY A SEPARATE FILE, NOT A PARAMETERIZED kload.c
 * -------------------------------------------------
 * kload.c/kload.h are the hardware-verified FreeBSD kernel loader: real,
 * board-booted, and NOT ours to touch for this milestone (too much fan-out
 * into every existing boot path — see the task that added this file). This
 * project's own precedent for exactly this situation is kload.c itself,
 * which already duplicates guest.c's small EL2/EL1 register-write asm
 * helpers in its own translation unit rather than sharing them via a
 * header, specifically so kload.c stays a self-contained, disjoint
 * addition. zload2.c does the same relative to kload.c: its own local ELF
 * struct definitions, its own local cache-maintenance loop, its own local
 * parser/placer state — zero shared symbols with kload.c/kload.h.
 *
 * WHY THIS IS SIMPLER THAN kload.c's GENERAL CASE
 * -------------------------------------------------
 * main_zephyr.c's header comment (read in full before writing this file)
 * already establishes that Zephyr's own ELF is a plain EXEC with a SINGLE
 * PT_LOAD segment whose p_vaddr IS the intended physical load address (no
 * KVA/PA split the way FreeBSD's kernbase-vs-pa_base relocation needs) —
 * i.e. the general "relocate every segment relative to the lowest p_vaddr"
 * placement kload.c implements for FreeBSD DEGENERATES to a plain identity
 * copy for Zephyr when pa_base equals that lowest p_vaddr. This file keeps
 * the same general relocation formula (so it still behaves correctly if a
 * future Zephyr image is linked at some OTHER base address and needs to be
 * relocated into the dual-guest window, e.g. 0xBE000000 — see
 * stage2_zephyr.h) but drops everything kload.c needs beyond that: no
 * modinfo blob, no DTB relocation, no symtab location, no MODINFOMD tags.
 *
 * SAFETY: unlike kload.c (which places FreeBSD anywhere in its own 1 GiB
 * DRAM gigabyte with no independent bound), zload2_parse_and_place() also
 * enforces a hard placement-size ceiling (ZLOAD2_MAX_IMAGE_SIZE) so a
 * malformed or oversized ELF is rejected at PLACEMENT time, before stage-2
 * (stage2_zephyr.c) is ever asked to trust it. This is defense in depth,
 * not a substitute for stage-2 isolation: stage2_zephyr.c's own hardware
 * isolation self-check (stage2_zephyr_isolation_selfcheck()) is what
 * actually proves the guest cannot escape its 32 MiB slice at runtime.
 *
 * Freestanding: <stdint.h> only, no libc dependency beyond the same
 * memcpy/memset libmin.c already provides (declared locally, exactly as
 * kload.c does, since no shared "libmin.h" exists in this tree — see
 * kload.c's own header comment for the same note).
 */
#ifndef BZDOS_ZLOAD2_H
#define BZDOS_ZLOAD2_H

#include <stdint.h>

/* Hard ceiling on the TOTAL span (highest placed byte - pa_base) this
 * loader will ever place, independent of and duplicated from (never
 * #include'd from) stage2_zephyr.h's ZSTAGE2_DRAM_SIZE — see this file's
 * header comment above for why duplication, not sharing, is deliberate
 * here. Kept in sync BY VALUE (32 MiB): if ZSTAGE2_DRAM_SIZE ever changes,
 * update this constant to match so a "successfully placed" image is always
 * one stage2_zephyr.c can actually map. A mismatch here is safe in only one
 * direction (this being SMALLER than the real stage-2 window just leaves
 * headroom unused); this being LARGER would let zload2 accept a placement
 * stage-2 can't fully cover, so keep them equal, not just "close enough". */
#define ZLOAD2_MAX_IMAGE_SIZE 0x02000000UL   /* 32 MiB, == ZSTAGE2_DRAM_SIZE */

/* Parse the ELF64/AArch64 image at `elf_addr` (validate header: ELF magic,
 * ELFCLASS64, ELFDATA2LSB, EM_AARCH64, ET_EXEC/ET_DYN, sane e_phnum) and, if
 * valid, copy every PT_LOAD segment to its resolved physical destination:
 *
 *     zbase = lowest p_vaddr across every PT_LOAD segment (an ELF-internal
 *             quantity, NOT necessarily related to pa_base a priori)
 *     dest  = pa_base + (p_vaddr - zbase)
 *
 * which is an plain identity copy (dest == p_vaddr) in the common case
 * where the image is already linked with zbase == pa_base (true of the
 * zephyr-guest bpi_m64_hv board today, p_vaddr == 0x51000000 — see
 * main_zephyr.c's header comment) and a genuine relocation otherwise.
 *
 * Every byte actually written is memcpy'd (p_filesz) then zero-filled out
 * to p_memsz, then cache-maintained (dc cvac + ic ivau, same idiom as
 * kload_cache_clean_inval()), exactly mirroring kload_place_segments()'s
 * per-segment sequence.
 *
 * SAFETY CHECK (not in kload.c's general case, needed here because this
 * loader's whole reason to exist is feeding an isolated 32 MiB slice):
 * rejects (returns 0, places NOTHING) if the image has zero PT_LOAD
 * segments, more than a small sane cap, or if any resolved destination
 * range [dest, dest+p_memsz) would fall outside
 * [pa_base, pa_base + ZLOAD2_MAX_IMAGE_SIZE). The check runs over every
 * segment BEFORE any memcpy happens, so a rejected image is placed
 * atomically-nothing, never partially copied.
 *
 * On success, returns 1 and stores the physical entry address
 * (pa_base + (e_entry - zbase)) into *entry_pa_out (if non-NULL). On
 * failure, returns 0 and leaves *entry_pa_out untouched.
 */
int zload2_parse_and_place(uint64_t elf_addr, uint64_t pa_base,
                            uint64_t *entry_pa_out);

#endif /* BZDOS_ZLOAD2_H */
