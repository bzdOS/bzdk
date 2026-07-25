/* SPDX-License-Identifier: BSD-2-Clause */

/* kload.h — FreeBSD/arm64 kernel loader + boot handoff for the bzdOS EL2
 * hypervisor (Allwinner A64, Cortex-A53).
 *
 * Takes a real FreeBSD aarch64 kernel ELF already resident in DRAM (staged
 * there by U-Boot/the integration lane's own load path — this file does NOT
 * fetch it), parses its PT_LOAD program headers, copies each segment to a
 * caller-chosen contiguous physical load base, builds the tagged "modinfo"
 * metadata blob the FreeBSD arm64 locore.S/initarm() boot protocol expects,
 * and drops the CPU to EL1 at the kernel's physical entry point. Mirrors
 * guest.c's EL2->EL1 handoff (SPSR_EL2/ELR_EL2/SP_EL1/eret) but with the
 * FreeBSD-specific x0 = modinfo-blob-PA convention instead of a bare
 * hypercall test payload, and with SCTLR_EL1.M left at 0 (MMU off) exactly
 * as the FreeBSD loader hands off — the kernel builds its own page tables.
 *
 * ------------------------------------------------------------------------
 * The FreeBSD arm64 boot protocol, as reconstructed from FreeBSD source
 * (sys/arm64/arm64/locore.S _start, sys/arm64/arm64/machdep.c initarm(),
 * sys/sys/linker.h struct preload record layout, sys/boot/efi loader
 * metadata.c and sys/boot/arm64 self_reloc/x0 handling):
 *
 *   Entry state expected by _start (locore.S):
 *     - Entered at EL1 (the loader/ubldr itself typically drops from EL2 to
 *       EL1 before jumping to the kernel when there is no separate
 *       hypervisor underneath it; here WE are the EL2 hypervisor and are
 *       ourselves doing that EL2->EL1 drop on the loader's behalf).
 *     - MMU and D-cache OFF (SCTLR_EL1.{M,C} = 0). locore.S runs
 *       position-independently at whatever physical address the segments
 *       were placed at, computes its own kernbase<->physical delta, and
 *       builds the kernel's own initial page tables before ever turning the
 *       MMU on itself.
 *     - x0 = address of the "modinfo" / preloaded-modules metadata blob
 *       built by the loader. THIS is the FreeBSD convention — it is NOT the
 *       Linux/EFI-stub convention of x0 = DTB pointer. FreeBSD's initarm()
 *       instead calls preload_search_by_type("elf kernel", ...) and walks
 *       MODINFOMD_DTBP out of that same blob to find the DTB. locore.S's
 *       create_pagetables accepts x0 as EITHER a physical or a virtual
 *       (>= KERNBASE) pointer; we hand it the blob's KVA on purpose, because
 *       a virtual modulep makes create_pagetables extend the early TTBR1 map
 *       to phys(modulep)+~6 MiB and so cover the blob, whereas a physical
 *       modulep is taken as a raw DTB and left in unmapped high memory.
 *     - x1..x3: unused/ignored by locore.S in the standard boot path (the
 *       loader convention only defines x0; historically x1 sometimes carried
 *       a magic number on some 32-bit ports, arm64 does not use it).
 *
 *   The modinfo blob itself (struct-free, from sys/sys/linker.h): a flat
 *   sequence of variable-length records:
 *
 *       uint32_t type;   // MODINFO_* or MODINFOMD_* (below)
 *       uint32_t len;    // length of the following data, in bytes
 *       uint8_t  data[len];
 *       // then padded so the NEXT record's `type` field starts at an
 *       // offset aligned to sizeof(u_long) == 8 on arm64 (roundup2(len,8))
 *
 *   terminated by a single (type=0, len=0) record. This exactly matches
 *   sys/boot/common/module.c:MOD_METADATA()/loader's build path (it emits
 *   type/len/data/pad per record) and kern/link_elf.c / kern/linker.c's
 *   preload_search_by_type()/preload_search_info() consumer side (identical
 *   record walk, identical roundup2(..., sizeof(u_long)) stride).
 *
 *   Tag values (from sys/sys/linker.h):
 *     MODINFO_END        0x0000  (implicit: len==0 terminator, not a real tag)
 *     MODINFO_NAME       0x0001  string, NUL-terminated module name
 *     MODINFO_TYPE       0x0002  string, NUL-terminated, "elf kernel" for us
 *     MODINFO_ADDR       0x0003  vm_offset_t/u_long: module's LOAD address
 *                                (for us: physical load base, since we run
 *                                the kernel with its own MMU off at that PA)
 *     MODINFO_SIZE       0x0004  u_long: module's size in bytes
 *     MODINFO_EMPTY      0x0005  (retired placeholder, unused)
 *     MODINFO_ARGS       0x0006  string: module load arguments (unused here)
 *     MODINFO_METADATA   0x8000  OR'd into a MODINFOMD_* value to mark it as
 *                                metadata-about-a-module rather than a plain
 *                                MODINFO_* tag; i.e. every tag below is
 *                                emitted as (MODINFO_METADATA | MODINFOMD_x).
 *   MODINFOMD_* sub-tags, OR'd with MODINFO_METADATA. There are TWO ranges:
 *   the machine-INDEPENDENT block (sys/sys/linker.h) and the arm64
 *   machine-DEPENDENT block (sys/arm64/include/metadata.h), which lives at
 *   0x1000+ specifically so MD tags never collide with the MI enum. The
 *   values below are now AUTHORITATIVE, corrected after a live hypervisor
 *   debugger decoded our first (wrong) blob in the running guest and the
 *   coordinator read the exact enum out of the target's linker.h: subtype
 *   0x0004 is ESYM and subtype 0x0007 is ENVP. Those two fixed points pin
 *   the whole MI ordering (0x0005 DYNAMIC, 0x0006 MB2HDR sit between them),
 *   hence HOWTO=0x0008 and KERNEND=0x0009 — NOT the 5/6/7 an older
 *   MB2HDR-less header would give:
 *
 *     -- machine-independent (sys/sys/linker.h) --
 *     MODINFOMD_AOUTEXEC 0x0001  a.out exec header
 *     MODINFOMD_ELFHDR   0x0002  copy of the ELF header
 *     MODINFOMD_SSYM     0x0003  start of symbols
 *     MODINFOMD_ESYM     0x0004  end of symbols   <-- what we WRONGLY used
 *                                for DTBP in the first cut; the kernel read
 *                                our DTB pointer as "end of symbols".
 *     MODINFOMD_DYNAMIC  0x0005  _DYNAMIC pointer
 *     MODINFOMD_MB2HDR   0x0006  multiboot2 header info
 *     MODINFOMD_ENVP     0x0007  char*: kernel environment ("n=v\0...\0\0")
 *                                <-- what we WRONGLY used for KERNEND in the
 *                                first cut; the kernel read our kernend as
 *                                envp -> garbage early setup.
 *     MODINFOMD_HOWTO    0x0008  int: boot howto flags (RB_* bitmask)
 *     MODINFOMD_KERNEND  0x0009  vm_offset_t: VIRTUAL end of all loaded
 *                                material. initarm() uses this as the VA
 *                                cursor its bootstrap allocator starts from
 *                                (thread0's initial kstack included), so it
 *                                MUST be a KVA, MUST be page/2MB rounded,
 *                                and MUST cover everything placed (kernel +
 *                                DTB + this blob), not just the kernel.
 *     -- arm64 machine-dependent (sys/arm64/include/metadata.h), 0x1000+ --
 *     MODINFOMD_EFI_MAP  0x1001  EFI memory map (we omit; not on this board)
 *     MODINFOMD_DTBP     0x1002  pointer to the DTB. THIS is the real DTBP
 *                                tag (NOT MI 0x0004). initarm() dereferences
 *                                it directly via OF_init((void *)dtbp) while
 *                                only locore's early KVA map is live (before
 *                                pmap_bootstrap / the DMAP exists), so it
 *                                must be an address that early map covers:
 *                                we relocate the DTB just above the kernel
 *                                image and emit DTBP as its KVA (kernbase +
 *                                (dtb_dst - pa_base)). A raw high physical
 *                                DTB pointer is unmapped there and faults.
 *     MODINFOMD_EFI_FB   0x1003  EFI framebuffer info (we omit)
 *     MODINFOMD_MODULEP  0x1004  physical modulep base (we omit; single
 *                                kernel, no separately-loaded modules)
 *
 *   CONFIDENCE NOTE (post-fix): record framing (type/len/data/pad-to-8) and
 *   MODINFO_NAME/TYPE/ADDR/SIZE (1/2/3/4) are HIGH confidence (stable since
 *   FreeBSD 9). The MODINFOMD values above are now HIGH confidence for the
 *   target kernel: DTBP=0x1002 and KERNEND=0x0009 are locked by the two
 *   enum data points the debugger/coordinator verified against the actual
 *   linker.h (ESYM=0x0004, ENVP=0x0007). The one remaining judgment call is
 *   whether KERNEND/ENVP are consumed as virtual or physical: initarm()
 *   dereferences them while only locore's early KVA map is live, so we emit
 *   them as KVAs (kernbase + (pa - pa_base)) — and, having disassembled
 *   create_pagetables/initarm on this exact kernel, DTBP and the modulep
 *   (x0) are now emitted as KVAs for the SAME reason: the early map does not
 *   reach the high physical scratch the DTB/blob previously used, so the DTB
 *   is relocated just above the kernel image and both are handed virtually.
 *   x0 = the modinfo blob pointer is HIGH confidence (the guest reached its
 *   own C code / set VBAR from this blob, proving locore walked it); it is
 *   now that blob's KVA so create_pagetables sizes the early map to cover
 *   it.
 * ------------------------------------------------------------------------
 *
 * Freestanding: <stdint.h> only, no libc beyond libmin.c's memcpy/memset
 * (already linked into this tree), -mgeneral-regs-only.
 */
#ifndef BZDOS_KLOAD_H
#define BZDOS_KLOAD_H

#include <stdint.h>

/* ------------------------------------------------------------------ *
 * FreeBSD loader modinfo tag values (AUTHORITATIVE — see the big protocol
 * comment above for the full derivation + confidence). MODINFO_* and the
 * METADATA marker are from sys/sys/linker.h; the MODINFOMD_* below are the
 * subset we emit, with their source header noted. Corrected after a live
 * hypervisor debugger proved the first cut's DTBP/KERNEND subtypes wrong
 * (they aliased ESYM/ENVP).
 * ------------------------------------------------------------------ */
#define MODINFO_NAME          0x0001u  /* sys/sys/linker.h: module name (string) */
#define MODINFO_TYPE          0x0002u  /* sys/sys/linker.h: module type (string) */
#define MODINFO_ADDR          0x0003u  /* sys/sys/linker.h: module load address */
#define MODINFO_SIZE          0x0004u  /* sys/sys/linker.h: module size */
#define MODINFO_METADATA      0x8000u  /* sys/sys/linker.h: marks a MODINFOMD_* record */

/* machine-INDEPENDENT metadata (sys/sys/linker.h, verified 2026-07-14 against
 * the exact matching source: freebsd-src commit 9263fb9, releng/15.1 RC2/RC3,
 * the branch/commit this board's actual kernel binary is built from). The
 * previous ENVP=7/HOWTO=8/KERNEND=9 numbering here was off-by-one against the
 * real header (which has ENVP=6/HOWTO=7/KERNEND=8) — that mismatch made
 * freebsd_parse_boot_param() read OUR emitted ENVP record as HOWTO, and OUR
 * emitted HOWTO record as KERNEND (MD_FETCH matches by MODINFO_METADATA|tag,
 * so each record silently aliased the next real field down). This is the
 * true root cause of the "HOWTO value came back as lastaddr" symptom found
 * via onebp live tracing — not a compiled-code quirk, a plain tag bug. */
#define MODINFOMD_ENVP        0x0006u  /* char*  kernel environment (KVA) */
#define MODINFOMD_HOWTO       0x0007u  /* int    boot howto flags */
#define MODINFOMD_KERNEND     0x0008u  /* vm_offset_t VIRTUAL end of loaded material */

/* arm64 machine-DEPENDENT metadata (sys/arm64/include/metadata.h), 0x1000+
 * range so it never collides with the MI enum above. */
#define MODINFOMD_DTBP        0x1002u  /* vm_paddr_t PHYSICAL pointer to the DTB */

/* Upper bound on PT_LOAD segments we track. The real FreeBSD arm64 GENERIC
 * kernel has on the order of 2-4 (text+rodata combined, data, bss may be
 * its own PT_LOAD or folded into data's p_memsz > p_filesz tail) — 16 is
 * generous headroom, not a tight fit. */
#define KLOAD_MAX_PHDR 16

/* One recorded PT_LOAD segment, in both its ELF-file terms and its
 * resolved physical destination (filled in by kload_place_segments()). */
struct kload_seg {
	uint64_t p_offset;   /* byte offset of segment data within the ELF file */
	uint64_t p_vaddr;    /* linked virtual address (FreeBSD KVA, e.g. under
	                      * 0xffff000000000000 + kernbase) */
	uint64_t p_paddr_pa; /* RESOLVED physical destination = pa_base +
	                      * (p_vaddr - kernbase); NOT the raw ELF p_paddr
	                      * field, which on this kernel equals p_vaddr and
	                      * is therefore not itself a physical address */
	uint64_t p_filesz;
	uint64_t p_memsz;
};

/* Parse & validate the ELF64/aarch64 header at elf_addr (kernel image
 * already resident in DRAM at that address, e.g. staged there by U-Boot).
 * Checks the ELF magic, ELFCLASS64, ELFDATA2LSB, EM_AARCH64, and a sane
 * e_phnum, then records e_entry (the raw KVA entry point, NOT yet
 * relocated to a physical address — see kload_entry_pa()) and every
 * PT_LOAD program header's p_offset/p_vaddr/p_filesz/p_memsz (p_paddr is
 * read but NOT trusted as physical — see struct kload_seg above). Also
 * computes kernbase = the lowest p_vaddr across all PT_LOAD entries.
 *
 * Returns 1 if the header + program headers look valid and at least one
 * PT_LOAD was found, 0 otherwise (bad magic/class/machine, e_phnum ==0,
 * more PT_LOAD entries than KLOAD_MAX_PHDR, or any PT_LOAD with
 * p_vaddr < kernbase-so-far AND a suspicious layout). On a 0 return no
 * other kload_* call should be trusted (state is left in whatever
 * partially-parsed condition it reached — callers should treat 0 as fatal
 * for this boot attempt).
 */
int kload_parse_elf(uint64_t elf_addr);

/* Copy every PT_LOAD segment recorded by kload_parse_elf() from
 * elf_addr+p_offset to its resolved physical destination
 * pa_base + (p_vaddr - kernbase), zero the .bss tail (p_memsz > p_filesz)
 * of each, and cache-maintain the copied physical range: clean D-cache to
 * PoC ("dc cvac" + "dsb ish") then invalidate I-cache to PoU ("ic ivau" +
 * "dsb ish; isb") over every 64-byte line touched, exactly the idiom
 * already used by repl.c's hot-reload ("rx"/"c" commands) and fb.c's
 * flush path in this tree — so the kernel's own D-cache-off/I-cache-on (or
 * off, per SCTLR_EL1 RES1 reset state) fetch sees the freshly-copied bytes
 * rather than something stale from before the copy.
 *
 * pa_base is caller-supplied (the integration lane owns physical memory
 * layout/reservation) and becomes the physical address of the LOWEST
 * p_vaddr segment; every other segment lands at pa_base plus its offset
 * from kernbase, preserving the kernel's internal relative layout exactly
 * as linked.
 *
 * Must be called after a successful kload_parse_elf() on the SAME elf_addr.
 * Returns 1 on success, 0 if called before a valid parse or if pa_base is 0.
 * Tracks kernel_end_pa = pa_base + (max(p_vaddr+p_memsz) - kernbase), the
 * physical end of the placed kernel image, retrievable via
 * kload_kernel_end_pa(). Also computes and records entry_pa = pa_base +
 * (e_entry - kernbase), retrievable via kload_entry_pa() — the physical
 * address locore.S actually starts executing at (e_entry itself is a KVA,
 * not something you can `eret` to with the MMU off).
 */
int kload_place_segments(uint64_t elf_addr, uint64_t pa_base);

/* Build the FreeBSD-loader-style modinfo metadata blob (see the big
 * protocol comment above) at scratch_pa, a spare physical DRAM region the
 * caller owns/reserves (must not overlap the placed kernel segments or the
 * DTB). Emits, in order: MODINFO_NAME ("kernel"), MODINFO_TYPE
 * ("elf kernel"), MODINFO_ADDR (= pa_base from the last successful
 * kload_place_segments() call), MODINFO_SIZE (= kernel_end_pa - pa_base),
 * MODINFOMD_ENVP (KVA of an empty "\0\0" env placed inside this scratch
 * region — a valid empty environment, which FreeBSD prefers over a NULL),
 * MODINFOMD_HOWTO (= 0, normal multiuser boot), MODINFOMD_DTBP (KVA of the
 * relocated DTB — see below), MODINFOMD_KERNEND, then the type=0/len=0
 * terminator record. Cache-cleans the written range with the same
 * dc cvac/ic ivau idiom as kload_place_segments().
 *
 * DTB RELOCATION + REACHABILITY. The kernel dereferences both modulep (in
 * parse_boot_param) and the DTBP value (in OF_init) while ONLY locore.S's
 * early page tables are live — and those map, for TTBR1, just
 * [pa_base, phys(modulep)+~6 MiB) and, for TTBR0, one 2 MiB block at
 * kernbase (verified by disassembling create_pagetables). So neither the
 * blob nor the DTB may sit at the ~47 MiB-high scratch the U-Boot path
 * used. This routine therefore:
 *   - copies the DTB from dtb_src_pa (where U-Boot/TFTP left it) to
 *     dtb_dst_pa (which the caller must place just above the kernel image,
 *     inside the window, and BELOW scratch_pa) and emits DTBP as the KVA
 *     kernbase + (dtb_dst_pa - pa_base); DTB size is read from the FDT's
 *     own header (magic 0xd00dfeed @+0, big-endian totalsize @+4) — if the
 *     magic is absent nothing is copied and DTBP is emitted as 0 so the
 *     kernel simply skips OF_init instead of faulting;
 *   - RETURNS the modulep the caller must hand the kernel as a KVA
 *     (kernbase + (scratch_pa - pa_base)), NOT scratch_pa itself: a virtual
 *     modulep is what steers create_pagetables into extending the TTBR1 map
 *     to phys(modulep)+~6 MiB so the blob is reachable.
 *
 * MODINFOMD_KERNEND is emitted as a VIRTUAL address (KVA) covering
 * EVERYTHING placed, rounded up to a 2 MiB boundary:
 *     kernend_va = kernbase + roundup2(max_pa - pa_base, 2MiB)
 * where max_pa = max(kernel_end_pa, dtb_dst_pa + dtb_totalsize,
 * scratch_pa + blob_len). This is the fix for the observed in-rodata SP:
 * initarm() uses kernend as the VA cursor its bootstrap allocator (thread0's
 * kstack included) starts from, so it must be a KVA past all loaded
 * material — see the KERNEND rationale in the big protocol comment/kload.c.
 *
 * Must be called after a successful kload_place_segments() (needs pa_base/
 * kernel_end_pa/kernbase from it). Returns the modulep KVA to hand the
 * kernel (pass it straight to kload_enter() as modinfo_pa), or 0 if called
 * out of order.
 */
uint64_t kload_build_modinfo(uint64_t dtb_src_pa, uint64_t dtb_dst_pa, uint64_t scratch_pa);

/* Physical entry point resolved by the last successful
 * kload_place_segments() call: pa_base + (e_entry - kernbase). This is
 * the value to pass as `entry` to kload_enter() — NOT the raw ELF e_entry
 * (a KVA under 0xffff000000000000, meaningless as a physical `eret`
 * target with the MMU off). Returns 0 if no successful placement has run.
 */
uint64_t kload_entry_pa(void);

/* Physical end address of the placed kernel image: pa_base +
 * (max(p_vaddr+p_memsz) - kernbase). Returns 0 if no successful placement
 * has run. Useful for the integration lane to pick a non-overlapping
 * scratch_pa/dtb_pa above the kernel. */
uint64_t kload_kernel_end_pa(void);

/* Drop the CPU from EL2 to EL1 at physical address `entry` (see
 * kload_entry_pa()), with x0 = modinfo_pa (the FreeBSD loader's x0
 * convention — see the protocol comment above) and SP_EL1 = sp.
 * Mirrors guest.c's guest_enter(): SPSR_EL2 = EL1h (M[3:0]=0b0101) with
 * D/A masked, I/F unmasked (cosmetic here — see guest.c's rationale, same
 * reasoning applies: HCR_EL2.IMO, if set, keeps steering physical IRQs to
 * EL2 regardless of EL1's own mask bits); ELR_EL2 = entry; SP_EL1 = sp;
 * then `eret`. Deliberately does NOT touch SCTLR_EL1 — FreeBSD's locore.S
 * expects to find the MMU/caches exactly as U-Boot/EL2 left EL1's system
 * registers (i.e. whatever guest_config()-equivalent state the integration
 * lane has established; this file does not call guest_config() itself so
 * the integration lane stays in full control of when/whether SCTLR_EL1 is
 * touched — the locore.S contract only requires SCTLR_EL1.M == 0, which is
 * the documented reset/guest_config() state throughout this tree already).
 * Never returns (eret transfers control away for good, barring a future
 * trap back to EL2 exactly like the guest.c preemption path).
 */
void kload_enter(uint64_t entry, uint64_t modinfo_pa, uint64_t sp) __attribute__((noreturn));

/* ------------------------------------------------------------------ *
 * ROADMAP B3 (crash forensics) — on-board kernel symbol table access.
 *
 * kload_parse_elf() already walks this same in-DRAM ELF's PROGRAM headers
 * (PT_LOAD, for placement) — it now ALSO makes one bounded pass over the
 * ELF's SECTION headers (e_shoff/e_shnum, previously parsed into the header
 * struct but unused) purely to LOCATE, not read, a symbol table: the first
 * section with sh_type==SHT_SYMTAB, and — via THAT section's sh_link field,
 * never by matching section-name strings against .shstrtab, which is one
 * fewer thing that can be stripped/renamed/absent — its paired sh_type==
 * SHT_STRTAB string table. This is best-effort and never fatal: a stripped
 * kernel (no .symtab) simply leaves kload_symtab_info() returning 0 forever;
 * kload_parse_elf()'s core placement contract is completely unaffected
 * either way (this scan runs after kernbase is already established and
 * cannot itself fail the overall parse).
 *
 * The functions below hand a symbol-resolver (ksym.c) exactly the same
 * already-known quantities kload.c computed for its OWN placement math
 * (kernbase, the ELF's own base address) — reused, not recomputed, per the
 * B3 task brief. Symbol st_value fields in a FreeBSD kernel ELF are KVAs in
 * the SAME space as every PT_LOAD's p_vaddr, so a caller comparing a live
 * guest KVA (e.g. a backtrace return address) against st_value needs no
 * VA<->PA delta at all; kload_kernbase()/kload_kernel_end_va() exist only so
 * a resolver can cheaply reject an address that isn't even in the loaded
 * kernel image's own VA range before it bothers scanning the symbol table.
 */

/* Returns 1 and fills every output if the last successful kload_parse_elf()
 * located a valid SHT_SYMTAB + paired SHT_STRTAB section pair; returns 0 (no
 * outputs touched) if no parse has run yet or the kernel ELF has no symbol
 * table (e.g. stripped). sym_addr/str_addr are absolute addresses (elf_addr
 * + sh_offset) directly dereferenceable by EL2 — same DRAM-resident-ELF
 * assumption kload_parse_elf()/kload_place_segments() already make, NOT
 * offsets the caller must add elf_addr to again. sym_count is the number of
 * Elf64_Sym entries (sh_size / sh_entsize, sh_entsize already validated by
 * kload_parse_elf() to be exactly sizeof(Elf64_Sym) == 24, so no ambiguity
 * about entry stride reaches the caller). str_size bounds every st_name
 * lookup into the string table. */
int kload_symtab_info(uint64_t *sym_addr, uint32_t *sym_count,
                       uint64_t *str_addr, uint64_t *str_size);

/* kernbase: the lowest PT_LOAD p_vaddr, i.e. the low end of the loaded
 * kernel's own KVA range (same value kload_place_segments() uses for its
 * dest_pa = pa_base + (p_vaddr - kernbase) math). Returns 0 if no successful
 * kload_parse_elf() has run. */
uint64_t kload_kernbase(void);

/* High end of the loaded kernel's own KVA range: max(p_vaddr + p_memsz)
 * across every PT_LOAD segment, computed directly from the ELF during
 * kload_parse_elf() — this is a pure VA quantity and, unlike
 * kload_kernel_end_pa(), needs no pa_base/placement step to exist, so it is
 * available immediately after a successful parse. Returns 0 if no
 * successful kload_parse_elf() has run. */
uint64_t kload_kernel_end_va(void);

#endif /* BZDOS_KLOAD_H */
