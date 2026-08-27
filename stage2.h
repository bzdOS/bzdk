/* SPDX-License-Identifier: BSD-2-Clause */

/* stage2.h — ARMv8-A EL2 stage-2 (IPA -> PA) translation for the bzdOS
 * microkernel-turned-hypervisor on the Allwinner A64 (Cortex-A53, GICv2).
 *
 * FIRST MILESTONE: an IDENTITY stage-2 map (IPA == PA) covering the low
 * SoC MMIO window and DRAM, so an EL1 guest (see guest.c/guest.h) sees
 * real pass-through memory and devices once HCR_EL2.VM is turned on. This
 * is intentionally NOT isolation — it exists so guest_enter()/guest_config()
 * can flip on stage-2 without changing what the guest observes, and so a
 * later milestone can replace individual block entries with something that
 * traps/remaps, one region at a time.
 *
 * We do NOT touch stage-1 anywhere (neither our own EL2 stage-1, which stays
 * exactly as U-Boot's flat mapping left it, nor a future guest EL1 stage-1).
 * Only VTCR_EL2, VTTBR_EL2 and HCR_EL2.VM are stage-2 hypervisor state, and
 * those are all this file writes.
 *
 * ------------------------------------------------------------------------
 * Table topology (see stage2.c for the full derivation):
 *   - 4 KiB translation granule (TG0 = 0b00).
 *   - 40-bit IPA space (T0SZ = 24, i.e. 64 - 40).
 *   - Starting level 1 (VTCR_EL2.SL0 = 1), with the architecturally
 *     REQUIRED concatenation of 2 level-1 tables (2 = 2^(25 - T0SZ), the
 *     standard stage-2 concatenation formula for start-level 1 / 4 KiB
 *     granule — a single level-1 table only covers 512 GiB = 2^39 bytes,
 *     and 40-bit IPA needs 2^40 = 1 TiB, hence exactly 2 tables).
 *   - Level-1 index 1..N (IPA 0x40000000 upward, DRAM) is still a plain
 *     1 GiB BLOCK descriptor, Normal WB cacheable — unchanged.
 *   - Level-1 index 0 (IPA 0x00000000-0x3FFFFFFF, SoC MMIO) is NO LONGER a
 *     single 1 GiB block. It is now a TABLE descriptor pointing at a
 *     level-2 table (512 x 2 MiB entries) so we can trap just the UART:
 *       - every level-2 entry is a plain 2 MiB Device-nGnRE BLOCK,
 *         identity-mapped, EXCEPT the one entry (index 14, IPA
 *         0x01C00000-0x01DFFFFF) that contains UART0_BASE.
 *       - that one level-2 entry is itself a TABLE descriptor pointing at
 *         a level-3 table (512 x 4 KiB page entries), where every page is
 *         a plain identity Device-nGnRE PAGE descriptor EXCEPT the single
 *         page at UART0_BASE (index 40 within that table), which is left
 *         entirely INVALID (all zero, bits[1:0]=00).
 *     Net effect: every MMIO byte that used to be identity-mapped still
 *     is, at 2 MiB or 4 KiB granularity as needed — only the 4 KiB UART0
 *     page is unmapped, so a guest access there takes a stage-2
 *     translation fault (DFSC in ESR_EL2.ISS) that el2_trap hands to
 *     vconsole_handle_fault() (see vconsole.c/vconsole.h).
 *   - Only the first concatenated table is populated; the second (which
 *     covers IPA 512 GiB..1 TiB, entirely unused) stays all-zero/invalid.
 *     The architecture requires it to exist and be contiguous with table 0
 *     purely because of how VTCR_EL2.T0SZ/SL0 encode the walk — it does not
 *     need any valid entries.
 * ------------------------------------------------------------------------
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only. Breadcrumb
 * writes use the same cache-coherent (dc civac + dsb sy) pattern as every
 * other lane in this tree so a post-reset `bc 0x50000c00` still shows the
 * last state even with the D-cache on.
 */
#ifndef BZDOS_STAGE2_H
#define BZDOS_STAGE2_H

#include <stdint.h>
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

/* ------------------------------------------------------------------ *
 * Identity-map region definitions. Both bases/sizes are 1 GiB block
 * aligned (required — we only ever emit 1 GiB level-1 block descriptors
 * in this milestone).
 *
 *   MMIO region:  [0x00000000, 0x00000000 + STAGE2_MMIO_SIZE)
 *                 covers GIC (0x01c81000), UART, EMAC (0x01c30000),
 *                 CCU (0x01c20000) and everything else in the A64's
 *                 0x01000000..0x02000000 MMIO cluster — mapped as a
 *                 single 1 GiB Device-nGnRE block for simplicity/safety
 *                 (mapping "0 up to DRAM base" as device, per the brief).
 *
 *   DRAM region:  [STAGE2_DRAM_BASE, STAGE2_DRAM_BASE + STAGE2_DRAM_SIZE)
 *                 mapped as Normal, Inner-Shareable, Write-Back cacheable,
 *                 executable (XN=0) RW.
 *
 * STAGE2_DRAM_SIZE is a #define specifically so the integrator can widen
 * it later (e.g. to the board's full RAM size) by bumping one constant and
 * relinking — stage2_init() fills however many contiguous 1 GiB blocks
 * that implies, starting right after the MMIO entry.
 * ------------------------------------------------------------------ */
#define STAGE2_MMIO_BASE   0x00000000UL
#define STAGE2_MMIO_SIZE   0x40000000UL   /* 1 GiB: covers 0..0x40000000 */

#define STAGE2_DRAM_BASE   0x40000000UL

/* GUEST_DRAM_2G takes the invitation in the comment above: the board has 2 GiB
 * of DRAM at [0x40000000, 0xC0000000) -- 0xC0000000 is the top, confirmed live
 * by a dram_copy() that faulted with FAR exactly there -- and by default the
 * guest only gets the low half, because snapshot.c claims the high GiB as its
 * verbatim mirror (SNAP_DRAM_STORE, exactly 1 GiB, hv_addrmap.h). That is a
 * real trade, not an oversight, and with GUEST_DRAM_2G the trade is taken the
 * other way: the guest gets all 2 GiB and snapshot/restore is not linked.
 *
 * Why anyone would want that: the board is the project's own build host, and
 * measured on 2026-08-27 a single Mesa NIR generator peaks at 648 MB against
 * ~850 MB of usermem -- so Mesa could not be built at all, at any -j, and
 * `ninja -j3` merely failed faster. See
 * [[guest-memory-limits-build-parallelism]].
 *
 * MUTUALLY EXCLUSIVE WITH THE `dual` BUILD, enforced at compile time in
 * stage2_zephyr.h rather than left to a comment: Zephyr's guest slice is
 * 0xBE000000-0xC0000000, i.e. INSIDE the high GiB this hands to FreeBSD.
 *
 * Whoever flips this must also widen the DTB's /memory node -- gen_config.py
 * does it from the same board-config.xml switch, for the same reason it now
 * removes a stale cpu@N node: a build flag whose DTB half was not applied is
 * exactly the half-configured state that has cost this project board time. */
#if defined(GUEST_DRAM_2G) && GUEST_DRAM_2G
/* The FULL 2 GiB, deliberately -- and deliberately NOT the same number the
 * guest is told about. Two constraints pull in opposite directions and the
 * only clean answer is to let them:
 *
 * 1. This map is built from whole 1 GiB level-1 blocks. stage2_init() computes
 *    `nblocks = STAGE2_DRAM_SIZE / STAGE2_BLOCK_SIZE`, so anything that is not
 *    a multiple of 1 GiB is silently TRUNCATED. Setting 0x78000000 here gave
 *    nblocks == 1 and left everything above 0x80000000 unmapped -- measured, as
 *    a guest write to IPA 0xB7FF1000 taking a level-1 translation fault with
 *    stage2_l1[0][2] still zero. The header comment above ("fills however many
 *    contiguous 1 GiB blocks that implies") was exactly right and exactly the
 *    trap.
 * 2. The guest must not touch the top ~113 MiB. U-Boot's own bdinfo on this
 *    board reports DRAM as [0x40000000-0xbfffffff] but reserves
 *    [0xb8f18770-0xbfffffff] no-overwrite, with its MMU translation tables at
 *    TLB addr = 0xbfff0000 and its relocated self at 0xbdf44000. FreeBSD
 *    allocates downward from the top of the memory it is told it has, so
 *    handing it 0xC0000000 hands it U-Boot's page tables. That attempt left the
 *    board unreachable.
 *
 * So: stage-2 maps 2 GiB in whole blocks (satisfying 1), while the DTB's
 * /memory node stops at 0xB8000000 (satisfying 2) -- see board-config.xml's
 * dtb-memory-size, which gen_config.py applies. Mapping more than the guest is
 * told about costs nothing: the guest never generates those addresses, and if
 * it ever did, a mapped page is a safer failure than a fault storm.
 *
 * The alternative -- teaching stage2_init() to split a partial trailing block
 * into an L2 table -- is real work on the boot path for no benefit here, and is
 * not done.
 *
 * One correction while here, since it is what sent me looking in the wrong
 * place: hv_addrmap.h says 0xC0000000 is "the last byte of real, installed
 * DRAM (confirmed live)", citing a dram_copy() fault. That fault was under
 * QEMU. The board's real usable ceiling for a guest was established for the
 * first time by the bdinfo read above. */
#define STAGE2_DRAM_SIZE   0x80000000UL   /* 2 GiB mapped (0x40000000..0xC0000000) */
#else
#define STAGE2_DRAM_SIZE   0x40000000UL   /* 1 GiB by default (0x40000000..0x80000000) */
#endif

/* A64 UART0 — the physical console FreeBSD's DTB points the kernel at
 * (chosen/stdout-path = "serial0:115200n8", serial0 = /soc/serial@1c28000,
 * "reg = <0x1c28000 0x400>" in /opt/bzdos/build/bananapi-min.dtb — this
 * confirms, rather than contradicts, the base below). 8250/16550-compatible,
 * one 4 KiB page holds all the registers vconsole.c needs (THR/LSR/etc, see
 * vconsole.h).
 *
 * This exact page is carved out of the level-1 MMIO identity block (see the
 * table-topology comment above and stage2.c) so guest accesses fault to EL2
 * instead of reaching real hardware we aren't wired to (our host link is the
 * USB gadget, not the UART pins) — vconsole.c emulates the UART from there.
 * Every other MMIO page (GIC, EMAC, CCU, UART1, ...) stays identity-mapped
 * exactly as before. */
#define UART0_BASE   SOC_A64_UART0_BASE
#define UART0_SIZE   0x1000UL      /* 4 KiB page carved out of stage-2 */

/* ------------------------------------------------------------------ *
 * FIRST-FAULT PROBE — catch the guest's ORIGINAL EL1 fault.
 *
 * The FreeBSD guest dies in a recursive-exception storm so early that the
 * ORIGINAL first fault's ELR_EL1/ESR_EL1/FAR_EL1 are immediately masked:
 * the guest re-enters its own EL1 vector table (VBAR_EL1) over and over,
 * each re-entry overwriting those banked registers with a NEW fault before
 * anything can read them. locore runs with PSTATE.D=1 so HW breakpoints /
 * watchpoints / software single-step never fire either.
 *
 * The probe makes the guest's EL1 vector PAGE fault at stage-2 on the very
 * first vector fetch: when the guest takes its first EL1 exception the
 * hardware sets ELR_EL1/ESR_EL1/FAR_EL1/SPSR_EL1 to the ORIGINAL fault and
 * branches to VBAR_EL1. If the stage-2 translation of that page is invalid,
 * the fetch takes a stage-2 INSTRUCTION ABORT to EL2 (ESR_EL2 EC=0x20)
 * BEFORE the storm overwrites the EL1 fault registers — firstfault.c then
 * reads the still-pristine EL1 fault state = the real first fault.
 *
 * GUEST_VECTOR_IPA is VBAR_EL1's physical page: the FreeBSD kernel is loaded
 * at phys 0x46000000 and its EL1 vector table (VBAR_EL1 KVA
 * 0xffff000000927000) is phys 0x46927000. One 4 KiB page covers all 16
 * architectural vector entries (0x927000..0x9277ff). Since this sits inside
 * DRAM (the 1 GiB level-1 block at IPA index 1), the probe splits that block
 * into a level-2 (2 MiB) table and, for the 2 MiB block that holds the page,
 * a level-3 (4 KiB) table — every page identity Normal-WB executable EXCEPT
 * GUEST_VECTOR_IPA, left invalid. Modelled exactly on the UART0 L2/L3 split.
 * ------------------------------------------------------------------ */
#define GUEST_VECTOR_IPA   0x46927000UL
#define GUEST_VECTOR_SIZE  0x1000UL      /* 4 KiB page: all 16 EL1 vectors */

/* PROBE TOGGLE. Neither of these is called by the normal boot path — a probe
 * build (e.g. main_dbg.c) calls stage2_unmap_guest_vector() AFTER
 * stage2_init()/stage2_enable() to arm the trap; firstfault_handle() calls
 * stage2_map_guest_vector() once it has latched the original fault, so the
 * guest can proceed into its own EL1 handler instead of looping in EL2.
 * A build that never calls stage2_unmap_guest_vector() boots identically to
 * before (the vector page stays part of the plain DRAM identity map). */
void stage2_unmap_guest_vector(void);
void stage2_map_guest_vector(void);

/* IPA address used by the self-check: must land inside the DRAM region
 * above, AND (since A1's HVIMG_L2_IDX/HVSCR_L2_IDX exclusions and the
 * first-fault VEC_L2_IDX split all live in the same 1 GiB DRAM block, see
 * stage2.c) on a PLAIN, un-excluded, un-split 2 MiB entry within it — not
 * 0x42000000 (that IS the hv-image exclusion's base address, now
 * deliberately unmapped). 0x60000000 is 512 MiB into the 1 GiB DRAM span,
 * clear of hv-image (0x42000000-0x421fffff), the guest vector-page split
 * (0x46800000-0x469fffff) and hv-scratch (0x50000000-0x501fffff). */
#define STAGE2_SELFTEST_IPA 0x60000000UL

/* Program VTCR_EL2 + build/fill the stage-2 identity tables + program
 * VTTBR_EL2 (VMID 0). Does NOT touch HCR_EL2.VM — stage-2 translation is
 * fully configured but still logically "off" until stage2_enable() runs.
 * Safe to call more than once (idempotent: tables are rebuilt from
 * scratch, registers rewritten). Writes breadcrumb words 0,1,2,4,5,6 (see
 * stage2.c for the exact layout) — including the self-check of word 6.
 */
void stage2_init(void);

/* Turn stage-2 translation on: HCR_EL2.VM = 1 via read-modify-write
 * (every other HCR_EL2 bit, in particular RW and IMO, is preserved
 * untouched — this must NEVER undo guest.c's RW or gic_timer.c's IMO).
 * Followed by `tlbi vmalls12e1; dsb ish; isb` to flush any stale
 * stage-1+stage-2 combined translations before anything relies on the
 * new mapping. Must be called AFTER stage2_init() has programmed
 * VTCR_EL2/VTTBR_EL2 — turning VM on before the tables/VTCR exist is
 * undefined. Writes breadcrumb word 3 (HCR_EL2 readback).
 */
/* Program THIS PE's banked VTCR_EL2/VTTBR_EL2 against the already-built global
 * tables. stage2_init() calls it for the boot core; a secondary guest vCPU
 * needs it because those two registers are per-PE while the tables are not. */
void stage2_program_this_pe(void);

/* Bring a secondary guest vCPU's stage-2 regime up on the calling core:
 * program this PE's registers, then enable. Builds no tables -- one guest, one
 * set of descriptors, one VMID. See vcpu2.c. */
void stage2_arm_secondary(void);

void stage2_enable(void);

/* Turn stage-2 translation back off: HCR_EL2.VM = 0 (read-modify-write,
 * same preservation rule as stage2_enable), then the same
 * tlbi/dsb/isb flush. Clean fallback path — does not free or alter the
 * tables themselves, so a subsequent stage2_enable() (without re-running
 * stage2_init()) resumes the identical mapping.
 */
void stage2_disable(void);

/* One-call bring-up + verification: stage2_init(); stage2_enable(); then
 * re-reads VTCR_EL2 and HCR_EL2 to confirm the programmed bits actually
 * stuck in hardware (not just that we wrote them), storing both back into
 * the breadcrumb. This cannot fully prove stage-2 translation works (that
 * needs a running EL1 guest actually touching memory through it), but it
 * does prove: the tables were built, the self-check on our own top-level
 * table's DRAM entry is internally consistent (IPA 0x42000000 -> PA
 * 0x42000000), and every system register write took.
 */
void stage2_selftest(void);

/* Combined-translation coherency proof. Performs `AT S12E1R` (stage-1 +
 * stage-2 EL1-read translation) of `va` and stores the resulting PAR_EL1
 * into stage2 breadcrumb words 7 (low32) + 8 (high32) at 0x50000c1c /
 * 0x50000c20. PAR_EL1.F (bit0) == 0 means the combined walk succeeded; the
 * output PA is in bits[47:12] and the memory attributes (should be 0xFF =
 * Normal Inner+Outer WB for DRAM) in bits[63:56]. Must be called AFTER
 * stage2_enable(). Uses the CURRENT EL1 stage-1 regime: if called before
 * the guest kernel turns on its own MMU (SCTLR_EL1.M==0), stage-1 is flat
 * so this isolates and proves the stage-2 DRAM mapping; if called from a
 * trap while the guest MMU is on, it proves the full combined walk the
 * guest's hardware table-walker sees. Safe/side-effect-free besides the
 * breadcrumb write.
 *
 * Integrator: add `stage2_at_check(0x42000000);` (a known DRAM IPA) right
 * after `stage2_enable();` in main_fbsd.c to record the proof each boot. */
void stage2_at_check(uint64_t va);

/* A1 isolation self-check: HARDWARE proof (via `AT S12E1W`) that the guest's
 * EL1 translation regime cannot reach either HV DRAM window (hv-image /
 * hv-scratch). Call ONCE on the boot path AFTER stage2_enable() +
 * stage2_unmap_guest_vector(), before the guest runs. Returns 1 if the
 * boundary holds (both HV windows fault, a control DRAM address still
 * translates), 0 if any HV window is reachable (a hole in the partition).
 * Records the result in the STG2 breadcrumb window slots [14..18] — see the
 * function's own comment in stage2.c for the exact layout. */
int stage2_isolation_selfcheck(void);

/* W^X self-check (ROADMAP v1 gate: "W^X на гостевых маппингах" — the isolation
 * bullet's last unchecked half). Exhaustively walks EVERY valid leaf in the
 * concatenated stage-2 tables (both level-1 tables, following every TABLE
 * descriptor down through level-2 and, where present, level-3) and counts how
 * many are simultaneously writable (S2AP write bit set) and executable
 * (XN==0). Unlike stage2_isolation_selfcheck(), which probes three known
 * addresses, this is a GENERAL walk that needs no per-region knowledge and
 * stays correct as the table topology changes.
 *
 * HONESTLY: call this a measurement, not a gate. It currently reports FAIL
 * (violations > 0) on every real boot, because guest DRAM genuinely is mapped
 * RW+executable everywhere today (S2AP is hardcoded RW by every descriptor
 * helper in this file; DRAM's XN=0 is what lets the guest's own code
 * execute) — see stage2.c's own comment above stage2_wx_selfcheck() for why
 * that is a real, currently-unclosed finding rather than an oversight, and
 * why a real fix is a separate feature (kload.c segment-aware permissions),
 * not a bug in this function. What IS fully closed and permanently enforced:
 * no MMIO/device leaf is ever executable — this function proves that by the
 * same general walk, not a hardcoded exemption, so a future regression there
 * (e.g. a new device window added without XN=1) moves the violation count
 * and is caught.
 *
 * Records violations / first-violation IPA / pass into the STG2 breadcrumb
 * window (indices 19-22 — see the comment above the function in stage2.c for
 * the exact layout). Like every other self-check in this file, does NOT halt
 * or alter the boot on failure — it is diagnostic only, read via
 * `bc 0x50000c00`. Returns 1 iff zero violations were found (currently always
 * 0 on this build), 0 otherwise. Call AFTER stage2_init()/stage2_enable() so
 * the tables it walks are the real, final ones. */
int stage2_wx_selfcheck(void);

/* ------------------------------------------------------------------ *
 * DYNAMIC W^X enforcement for guest DRAM (opt-in, OFF by default).
 *
 * See docs/wx-enforcement.md for the full design, the five questions it was
 * asked to answer with evidence, and why the outcome is "implement it, but
 * disabled" rather than either a silent claim of victory or a flat refusal.
 * One-line summary: stage2_wx_selfcheck() above measures that guest DRAM is
 * RW+executable everywhere (510 leaves, every boot) and explains why a
 * STATIC boot-time split cannot close that without either leaving kldload's
 * pages unenforced or breaking kldload outright. This is the DYNAMIC
 * alternative a static split cannot be: guest DRAM defaults to
 * writable-but-execute-never; the FIRST instruction fetch from a page takes
 * a stage-2 permission fault, which flips that ONE 4 KiB page to
 * read-execute (and revokes write on it); a LATER write to a page currently
 * in that state flips it back. Needs no guest cooperation and no hypercall.
 *
 * STAGE2_WX_DYNAMIC now defaults to 1 (ON), because the one question that kept
 * it off has been answered ON HARDWARE (2026-08-20). The doubt was never the
 * flip logic — that was proven board-free in test_stage2_tables.c — it was
 * whether the bounded L3 pool below survives a REAL guest's kldload
 * fragmentation, which cannot be answered without the board.
 *
 * Measured, with the full workload this guest actually runs (boot to
 * multiuser, kld_list loading drm.ko + lima.ko + bzfb.ko, then limabench's
 * 2421-draw GL workload and 12052 zero-copy presented frames), read from
 * HVMAP_WXDYN_BC:
 *
 *     pool_used      = 27 of 64      (2.4x headroom)
 *     pool_exhausted = 0
 *     flip_count     = 4045          (the mechanism is doing real work, not idling)
 *
 * The guest booted, rendered and presented normally throughout, so W+X on
 * guest DRAM — the last open item of the v1 isolation gate — is no longer the
 * standing state. Exhaustion still fails OPEN (degrades that one 2 MiB block to
 * W+X rather than hanging the guest), so a workload heavier than the measured
 * one loses enforcement on a block instead of breaking. Build with
 * `-DSTAGE2_WX_DYNAMIC=0` for the old behaviour. */
#ifndef STAGE2_WX_DYNAMIC
#define STAGE2_WX_DYNAMIC 1
#endif

#if STAGE2_WX_DYNAMIC
#include "exceptions.h"   /* struct el2_frame */

/* Number of on-demand L3 (4 KiB-granularity) tables this mechanism may ever
 * create in one boot. Each is created ONCE — the first time ANY page in its
 * 2 MiB block is executed — and never freed for the life of the boot (see
 * docs/wx-enforcement.md Q2/Q4 for why "never freed" is the deliberately
 * simple, bounded-risk choice here, and what happens when the pool is
 * exhausted: fail OPEN, not closed — see stage2.c's stage2_wx_flip()).
 * Sized against the hv-image's hard 2 MiB ceiling (link.ld's ASSERT):
 * 64 * 4 KiB = 256 KiB, well inside the ~1.36 MiB of slack this image had
 * free as of the commit that added this (see the doc for the exact
 * `size microkernel-dbg.elf` numbers). This is NOT a validated capacity for
 * any real kldload workload — just a conservative starting point that
 * leaves headroom for everything else in this tree. The hardware experiment
 * has now been run: 27 of these 64 were used by the real workload (see
 * STAGE2_WX_DYNAMIC above), so 64 stands with 2.4x headroom. */
#ifndef STAGE2_WX_POOL_TABLES
#define STAGE2_WX_POOL_TABLES 64u
#endif

/* el2_trap dispatch entry (see el2_exc.c's guest-sync-trap chain, alongside
 * firstfault_handle()/hwbp_handle()/vconsole_handle_fault()/etc. — same
 * calling convention as all of them). Called on every lower-EL instruction
 * OR data abort (EC 0x20/0x21/0x24/0x25) when STAGE2_WX_DYNAMIC is nonzero;
 * reads ESR_EL2 (from `frame`) and HPFAR_EL2 (itself, same pattern every
 * sibling handler already uses) and returns 1 iff this was a guest-DRAM
 * stage-2 PERMISSION fault it fully handled (el2_trap should then return
 * without recording it as a generic fault, exactly like vblk_mmio_fault()'s
 * contract) — this INCLUDES the pool-exhaustion fallback (still returns 1:
 * see stage2.c's stage2_wx_flip() for why fail-OPEN, not closed, is the
 * only safe choice there). Returns 0 for anything else this is not
 * (translation fault, non-DRAM IPA, one of the two HV windows, an EC this
 * mechanism doesn't own) — el2_trap handles those exactly as it does today.
 * Never advances ELR itself (matches the house rule for every guest-fault
 * handler): fixing the permission and re-executing the SAME instruction is
 * the entire point. */
int stage2_wx_fault(struct el2_frame *frame);
#endif /* STAGE2_WX_DYNAMIC */

#endif /* BZDOS_STAGE2_H */
