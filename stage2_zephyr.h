/* SPDX-License-Identifier: BSD-2-Clause */

/* stage2_zephyr.h — a SECOND, disjoint ARMv8-A EL2 stage-2 (IPA -> PA)
 * translation table set, for the Zephyr guest running concurrently on CPU3
 * (dual-guest milestone). Linked ONLY into the `dual` Makefile target.
 *
 * WHY A SEPARATE TABLE SET, NOT A PARAMETERIZED stage2.c
 * --------------------------------------------------------
 * stage2.c/stage2.h are explicitly frozen this pass (owned by a parallel
 * isolation milestone — see main_zephyr.c's own header comment, which
 * already documents this exact freeze). More fundamentally, VTCR_EL2 and
 * VTTBR_EL2 are PER-PE BANKED system registers on ARMv8-A: each physical
 * core has its OWN independent copy. stage2.c programs CPU0's copy (the
 * FreeBSD guest's stage-2); this file programs CPU3's copy, from a
 * COMPLETELY SEPARATE table in DRAM. The two never share a register, a
 * table, or a symbol — this is exactly what the architecture's per-PE
 * banking is FOR, and is what lets two guests with two different physical-
 * memory views run genuinely concurrently on different cores with no
 * VMID-multiplexing tricks needed.
 *
 * THE ISOLATION BOUNDARY THIS BUILDS
 * -------------------------------------
 * Zephyr's private memory is the TOP 32 MiB of the high GiB DRAM window
 * (0x80000000-0xBFFFFFFF) — already guest-invisible/HV-scratch-adjacent
 * territory, and entirely disjoint from FreeBSD's whole low-DRAM gigabyte
 * (stage2.h's STAGE2_DRAM_BASE=0x40000000, STAGE2_DRAM_SIZE=0x40000000,
 * i.e. 0x40000000-0x7FFFFFFF). This table set maps:
 *
 *   IPA 0x00000000-0x3FFFFFFF (all MMIO)         -> ENTIRELY INVALID
 *   IPA 0x40000000-0x7FFFFFFF (FreeBSD's DRAM)    -> ENTIRELY INVALID
 *   IPA 0x80000000-0xBDFFFFFF (unused high GiB)   -> INVALID
 *   IPA 0xBE000000-0xBFFFFFFF (Zephyr's 32 MiB)   -> valid Normal-WB identity
 *
 * Zephyr gets ZERO real MMIO passthrough in this design (see
 * mmio_absorb.h/.c): every access below IPA 0x40000000 stage-2-faults, and
 * the CPU3 el2_exc.c routing (see that file's header) hands it to
 * mmio_absorb_fault() instead of any real device model.
 *
 * FUTURE RECONCILIATION NOTE (documentation only — do NOT act on this here)
 * ---------------------------------------------------------------------------
 * snapshot.h's SNAP_STORE_BASE/SNAP_DRAM_SIZE claim the ENTIRE high GiB
 * (0x80000000-0xC0000000) as a 1:1 mirror-on-restore scratch window (see
 * snapshot.h's own header + hv_addrmap.h's HVMAP_SNAP_HDR_* comment) — this
 * file's 32 MiB slice (0xBE000000-0xC0000000) sits INSIDE that same window.
 * The two features (dual-guest Zephyr + snapshot/restore) are therefore
 * MUTUALLY EXCLUSIVE as shipped today (see the `dual` Makefile target,
 * which deliberately excludes snapshot.o/snapshot_net.o). A future pass
 * that wants BOTH simultaneously would need to either (a) shrink
 * SNAP_DRAM_SIZE by 32 MiB (e.g. to 0x3E000000, ending the snapshot mirror
 * at 0xBE000000 instead of 0xC0000000) and add that same 32 MiB to
 * snapshot.c's dram_copy_excluding() exclusion list (same treatment
 * HVSCR_BASE already gets — see snapshot.h's "EXCLUSION WINDOWS"), or
 * (b) shrink stage2.h's own STAGE2_DRAM_SIZE and relocate Zephyr's slice
 * somewhere else in the map entirely. Neither is implemented here.
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only.
 */
#ifndef BZDOS_STAGE2_ZEPHYR_H
#define BZDOS_STAGE2_ZEPHYR_H

#include <stdint.h>

/* Zephyr's private DRAM slice: top 32 MiB of the high GiB. MUST match
 * zload2.h's ZLOAD2_MAX_IMAGE_SIZE by value (see that header's comment) and
 * MUST stay 2 MiB aligned (this file only ever emits 2 MiB L2 block
 * descriptors for it). */
#define ZSTAGE2_DRAM_BASE   0xBE000000UL
#define ZSTAGE2_DRAM_SIZE   0x02000000UL   /* 32 MiB: 0xBE000000..0xC0000000 */

/* Program VTCR_EL2 (same field values as stage2.c's stage2_init() — T0SZ=24,
 * SL0=1 with 2 concatenated level-1 tables, IRGN0/ORGN0=WB RW-Alloc, SH0=
 * Inner Shareable, TG0=4 KiB granule, PS read at runtime from
 * ID_AA64MMFR0_EL1 — see stage2.c's big VTCR_EL2 comment for the full
 * per-field rationale, mirrored here verbatim for correctness) + build/fill
 * a SEPARATE, disjoint table set (zstage2_l1[]/zstage2_l2_high[], not
 * stage2.c's stage2_l1[]) + program VTTBR_EL2 to point at THIS table's base
 * (VMID 0 — see this file's header comment for why sharing VMID 0 with
 * CPU0's stage2.c is safe: VTTBR_EL2 is banked per PE, so this write only
 * ever affects the CALLING core's own register/TLB, never CPU0's).
 *
 * MUST be called on CPU3 itself (VTCR_EL2/VTTBR_EL2 are per-PE banked;
 * calling this from any other core would program THAT core's stage-2, not
 * CPU3's). Does NOT touch HCR_EL2.VM — stage2_zephyr_enable() is a separate
 * explicit step, same two-phase contract as stage2_init()/stage2_enable().
 * Safe to call more than once (idempotent).
 */
void stage2_zephyr_init(void);

/* Turn CPU3's stage-2 translation on: HCR_EL2.VM = 1 via read-modify-write
 * (every other HCR_EL2 bit — in particular RW, set by guest_config(), and
 * IMO if ever set on this core — is preserved untouched), followed by
 * `tlbi vmalls12e1; dsb ish; isb` (CPU-local, not broadcast — correct here
 * since each core's stage-2 TLB entries are private to it). Must be called
 * AFTER stage2_zephyr_init() has programmed VTCR_EL2/VTTBR_EL2.
 */
void stage2_zephyr_enable(void);

/* Hardware isolation self-check, via `AT S12E1W` — same technique as
 * stage2.c's stage2_isolation_selfcheck(), against THIS core's own stage-2
 * regime. Proves, in hardware (the CPU3's own table-walker, not a software
 * re-read of the descriptors), that:
 *   - an MMIO address (IPA 0x00000000)                  FAULTS
 *   - an address inside FreeBSD's low-DRAM gigabyte      FAULTS
 *   - an address in the high GiB but OUTSIDE Zephyr's own
 *     32 MiB slice (IPA 0x80000000, same 1 GiB L1 block
 *     as the slice, proving the L2 split genuinely limits
 *     to just the 16 valid entries, not the whole block)  FAULTS
 *   - an address INSIDE Zephyr's own slice (ZSTAGE2_DRAM_BASE) TRANSLATES
 *
 * Returns 1 if every one of those four checks matches the wanted outcome
 * (the isolation boundary holds), 0 otherwise (a hole in the partition —
 * MUST NOT be treated as a warning; see zguest_cpu3.c's caller, which halts
 * rather than entering the guest on a 0 return). Must be called AFTER
 * stage2_zephyr_enable(), on CPU3, before the guest ever runs.
 */
int stage2_zephyr_isolation_selfcheck(void);


/* Zephyr's slice (0xBE000000-0xC0000000) lives inside the high GiB that
 * GUEST_DRAM_2G hands entirely to the FreeBSD guest, so the two cannot coexist.
 * A build error, not a comment: the same posture vcpu3.h takes with
 * bzdos_cpu3_owner, and for the same reason -- silent overlap of two guests'
 * DRAM is the one mistake this file exists to prevent. */
#if defined(GUEST_DRAM_2G) && GUEST_DRAM_2G
#error "GUEST_DRAM_2G gives the FreeBSD guest the high GiB that Zephyr's slice sits in -- the dual build cannot be built with it"
#endif

#endif /* BZDOS_STAGE2_ZEPHYR_H */
