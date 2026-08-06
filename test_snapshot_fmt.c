/* SPDX-License-Identifier: BSD-2-Clause */

/* test_snapshot_fmt.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the *pure* parts of the guest snapshot/restore feature
 * (ROADMAP D1): snapshot.c/snapshot.h's on-store header ABI and its CRC32,
 * and snapshot_net.c/snapshot_net.h's wire framing, chunk geometry, bitmap
 * helpers and RX validation/reassembly path.
 *
 * ============================================================================
 * WHY THIS FILE EXISTS AT ALL — THE FEATURE IS CURRENTLY UNMEASURABLE
 * ============================================================================
 * snapshot.o and snapshot_net.o link into exactly one target, `dbg`
 * (Makefile:175-180 DBG_OBJS). Of the whole feature, exactly ONE function is
 * ever called at runtime: snapshot_net_rx_frame(), from emac.c:1401's
 * ethertype-0x88B8 dispatch (declared weak at emac.c:216-220 so the other
 * targets still link). snapshot_save(), snapshot_restore(),
 * snapshot_present(), snapshot_net_send() and snapshot_net_recv() have NO
 * caller anywhere in the tree — docs/snapshot-integration.md:36-77 specifies
 * dbgmon `snap`/`rest`/`snapi` commands, and `grep -n '"snap' dbgmon.c`
 * returns nothing: they were never added. So today the only executable
 * evidence about this ~900 lines of code is snapshot_net.py's `selftest`,
 * which covers a header round trip, three bitmap bits, one CRC32 check value
 * and one chunk-geometry invariant. This file raises that floor.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "snapshot.c"` WITH STUBS
 * ============================================================================
 * snapshot.h, snapshot.c, snapshot_net.h and snapshot_net.c were read in full
 * before writing this file, and the same house rule as
 * test_stage2_tables.c/test_vconsole_uart.c applies for the same reason:
 * neither .c is includable on x86_64, and making them so would mean editing
 * read-only reference sources.
 *
 *   snapshot.c is almost entirely raw AArch64 system-register and cache
 *   instructions that gas rejects on x86_64:
 *     - RD()/WR()/WR_ISB() "mrs"/"msr ..., isb"                (lines 34-37)
 *     - snap_wr32()'s "dc civac, %0\n\tdsb sy"                 (line 45)
 *     - sysregs_capture()  29x MRS of EL1/EL2 registers        (lines 56-98)
 *     - sysregs_reload()   29x MSR + ISB                       (lines 108-166)
 *     - restore_maintenance() "tlbi vmalle1is/alle1is; ic ialluis" (184-192)
 *     - dram_copy()'s "dc cvac"/"dsb sy" + wdt_pet()           (lines 262-268)
 *     - RD("cntpct_el0")                                        (lines 312/400)
 *   and the two public entry points are *defined* in terms of absolute
 *   physical addresses: snap_hdr() (line 277-280) casts the literal
 *   SNAP_STORE_BASE == 0x80000000 to a pointer and dereferences it. There is
 *   no seam to inject a test store through — not a stubbable dependency, a
 *   hardcoded address. Making it injectable means editing snapshot.c.
 *
 *   snapshot_net.c is closer to portable but still has
 *     - store_cache_clean()'s "dc civac"/"dsb sy"              (lines 186-192)
 *     - store_zero()'s "dc cvac"/"dsb sy" + wdt_pet()          (lines 215-228)
 *     - crc32_region()'s wdt_pet()                             (line 176)
 *     - snapnet_send_one()/collect_until_idle()'s emac_send_frame()/emac_poll()
 *       and netcon_send()/netcon_recv() externs                (lines 21-24, 261-266)
 *   and, again, snapshot_net_rx_frame() writes straight to
 *   `SNAP_STORE_BASE + off` (line 314) — a 1 GiB region at a fixed PA that a
 *   hosted test can neither allocate nor address.
 *
 * So: every function below is a hand transcription with an exact source
 * line-number citation, and the only deliberate deviations are the two
 * documented shims described under "TEST SHIMS" below.
 *
 * ============================================================================
 * WHAT THIS FILE PROVES
 * ============================================================================
 *  1. The two INDEPENDENT CRC32 implementations in the feature agree with
 *     each other and with the standard IEEE/zlib check value. snapshot.c:210
 *     has a bit-at-a-time crc32_update_word() folded into the save-time DRAM
 *     copy; snapshot_net.c:140-161 has a separate table-driven
 *     crc32_init()/crc32_calc(). snapshot_net.c:130-135 *asserts in a comment*
 *     that they are "mathematically identical ... so the final recheck against
 *     the store header's crc32 field agrees bit-for-bit" — and that claim is
 *     load-bearing: snapshot_net_recv() step 6 (snapshot_net.c:486) refuses
 *     to restore unless the table-driven CRC over the received image equals
 *     the bit-at-a-time CRC that snapshot_save() wrote into the header. If
 *     they ever diverged, EVERY snapshot transfer would fail with -4 (or,
 *     worse, a real corruption would slip through). NOTHING in the tree
 *     compares them today. These tests do.
 *  2. `struct snapshot_hdr`'s byte layout, field by field, via compile-time
 *     _Static_assert on offsetof/sizeof. This is a genuine ABI: the header is
 *     written to a fixed physical address (snapshot.c:277-280) and read back
 *     by a restore that may be a DIFFERENT BUILD of the hypervisor after a
 *     warm reset (snapshot.h:165-168 explicitly designs for that), and by
 *     snapshot_net.py on the host. Nothing versions the struct except
 *     snapshot_hdr.version, which is only bumped by hand.
 *  3. snapshot_net_rx_frame()'s full validation ladder and its bitmap
 *     bookkeeping — the single best host-testable piece of the whole feature,
 *     and the ONLY part of it that actually runs on the board today.
 *  4. The chunk geometry / manifest arithmetic that BOTH ends independently
 *     recompute from seq alone (snapshot_net.h:150-157 deliberately puts
 *     neither `off` nor `total` on the wire), plus the bit_test/bit_set
 *     helpers at the four indices where an off-by-one would hide (bit 0,
 *     bit 7, bit 8, and the very last valid bit).
 *  5. Three real robustness holes found while reading, each PINNED at its
 *     current (unfixed) behaviour rather than fixed — see "OPEN ISSUE" below
 *     and the individual tests. snapshot.c/snapshot_net.c are NOT modified by
 *     this file.
 *
 * ============================================================================
 * WHAT THIS FILE EXPLICITLY DOES *NOT* PROVE
 * ============================================================================
 *  - Nothing about snapshot_save()/snapshot_restore() as executed. The sysreg
 *    capture/reload sets (snapshot.c:56-166), the TLB/I-cache maintenance
 *    (:182-193), the CNTVOFF_EL2 timer re-base (:400-402) and the in-place
 *    trap-frame rewrite (:416) are pure MSR/MRS/TLBI sequences with no
 *    return value and no observable effect on x86_64. Their correctness is
 *    an architectural argument, not a testable one, and whether a restored
 *    FreeBSD actually resumes is an END-TO-END property only the board can
 *    answer (snapshot.c:16-18 says exactly this).
 *  - Nothing about the transport as a transport: no frame ever goes near
 *    emac_send_frame()/emac_poll(), no netcon handshake, no packet loss, no
 *    retransmission round, no timing. snapshot_net_send()/-_recv() are not
 *    mirrored at all — they are ~140 lines of loop driving four external
 *    functions, and a mirror of them would be a mirror of my own stubs.
 *  - Nothing about the 1 GiB store. TOTAL_CHUNKS is 767,006 and the store is
 *    0x40010000 bytes; allocating it in a hosted unit test would be absurd.
 *    The RX tests therefore model ONLY the addressing arithmetic (which uses
 *    the REAL constants) plus a small 8 KiB window that a chosen chunk's
 *    write lands in — see "TEST SHIMS". A write that the real code would
 *    place outside that window is counted, not performed. So these tests
 *    prove the *arithmetic and the accept/reject decision*, NOT that a 1 GiB
 *    reassembly ever completes.
 *  - Nothing about cache coherency, DMA, or what a warm WDT reset actually
 *    preserves. The `dc civac` in the RX path is counted, not executed.
 *  - The RX-path duplicate/idle-drop tests exercise a single-threaded
 *    sequence; on the board snapshot_net_rx_frame() runs from emac_poll() on
 *    CPU1 concurrently with s_rx_active being flipped by the recv() driver
 *    (snapshot_net.c:425/427). This file cannot say anything about that race.
 *
 * ============================================================================
 * TEST SHIMS (the only two deliberate deviations from the source)
 * ============================================================================
 *  SHIM 1 — destination redirect. snapshot_net.c:314 computes
 *    `dst = (uint8_t *)(SNAP_STORE_BASE + off)`. Here that single line becomes
 *    a call to t_dst(off, plen), which returns a pointer into an 8 KiB
 *    in-test window when the chunk falls inside it and a discard sink
 *    (counted in t_writes_outside_window) otherwise. Every byte of arithmetic
 *    that FEEDS the address — off = seq*SNAPNET_CHUNK, remain, expect_len —
 *    is verbatim and uses the real constants; only the final base changes.
 *  SHIM 2 — cache maintenance. store_cache_clean() (snapshot_net.c:186-192)
 *    is a `dc civac` loop. Here it only records (addr, size, call count) so
 *    tests can assert it is invoked for the right byte range. It performs no
 *    maintenance, so nothing about coherency is being tested.
 *  Everything else — the 16-byte header decode (rd16/rd32), the five-step
 *  validation ladder, the CRC check, the byte copy, the bit_test/bit_set
 *  bookkeeping and the s_rx_count bump — is verbatim.
 *  NOTE the manifest bitmaps are NOT shimmed: SNAPNET_MANIFEST_BYTES is only
 *  95,876 bytes, so the tests use the REAL, full-size bitmap.
 *
 * ============================================================================
 * OPEN ISSUES PINNED (not fixed) BY THIS FILE
 * ============================================================================
 *  (a) snapshot_present() (snapshot.c:282-287) validates magic/valid/version
 *      and NOTHING ELSE, and snapshot_restore() (snapshot.c:375) then copies
 *      `h->dram_size` bytes — the size the STORE (or, via snapshot_net_recv,
 *      the WIRE) supplied — rather than the compile-time SNAP_DRAM_SIZE.
 *      test_open_issue_present_accepts_absurd_geometry() shows a header with
 *      an absurd dram_size is still "present", and computes exactly how far
 *      past the store the resulting copy runs.
 *  (b) snapshot_hdr.crc32 covers the DRAM image ONLY, never the header
 *      itself, so a garbled header passes every integrity check the feature
 *      has. test_open_issue_header_is_not_crc_covered() pins that.
 *  (c) The snapshotted "guest DRAM" range CONTAINS the hypervisor's own
 *      image and stack. test_open_issue_snapshot_range_contains_hv_image()
 *      pins the containment arithmetically. See that test for why it matters.
 *
 * Build: gcc -Wall -Wextra -O2 -o test_snapshot_fmt test_snapshot_fmt.c &&
 *        ./test_snapshot_fmt
 * (NOT wired into `make test`: the Makefile is off-limits to this change. To
 *  wire it in, add `test_snapshot_fmt` to the `test:` prerequisite list and
 *  its `./test_snapshot_fmt` run line at Makefile:81-91, plus the two-line
 *  build rule pattern used at Makefile:93-120.)
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ==================================================================== *
 * Constants mirrored from snapshot.h:72-100 / :188.
 * ==================================================================== */
#define SNAP_DRAM_BASE   0x40000000UL            /* snapshot.h:72  */
#define SNAP_DRAM_SIZE   0x40000000UL            /* snapshot.h:73  */
#define SNAP_STORE_BASE  0x80000000UL            /* snapshot.h:95  */
#define SNAP_META_SIZE   0x00010000UL            /* snapshot.h:96  */
#define SNAP_DRAM_STORE  (SNAP_STORE_BASE + SNAP_META_SIZE)  /* snapshot.h:97 */
#define SNAP_MAGIC       0x534E5031u             /* snapshot.h:100 ("SNP1") */
#define SNAPSHOT_VERSION 1u                      /* snapshot.h:188 */

/* snapshot.h:104-108 — enum snapshot_store_kind. */
#define SNAP_STORE_DRAM  0
#define SNAP_STORE_EMMC  1
#define SNAP_STORE_EMAC  2

/* snapshot.c:208 */
#define CRC32_INIT 0xFFFFFFFFu

/* ==================================================================== *
 * Constants mirrored from snapshot_net.h:111-169 and snapshot_net.c:44.
 * Kept as the same macro EXPRESSIONS as the source (not hand-computed
 * literals) so this mirror derives them the same way the board does; the
 * _Static_asserts further down pin the resulting numbers.
 * ==================================================================== */
#define SNAPNET_ETHERTYPE      0x88B8u                            /* .h:111 */
#define SNAPNET_CHUNK          1400u                              /* .h:119 */
#define SNAPNET_STORE_LEN      (SNAP_META_SIZE + SNAP_DRAM_SIZE)  /* .h:126 */
#define SNAPNET_TOTAL_CHUNKS   ((SNAPNET_STORE_LEN + SNAPNET_CHUNK - 1u) / SNAPNET_CHUNK) /* .h:134 */
#define SNAPNET_MANIFEST_BYTES ((SNAPNET_TOTAL_CHUNKS + 7u) / 8u)  /* .h:135 */
#define SNAPNET_MAX_ROUNDS     5                                   /* .h:142 */
#define SNAPNET_MISSING_CAP    8192u                               /* .h:143 */
#define SNAPNET_HDR_LEN        16u                                 /* .h:168 */
#define SNAPNET_MAGIC          0x42504E53u                         /* .h:169 ("SNPB" LE) */
#define SNAPNET_SENTINEL       0xFFFFFFFFu                         /* snapshot_net.c:44 */

/* ==================================================================== *
 * struct el2_frame — VERBATIM from exceptions.h:22-30. The offsets in the
 * comments there are a hard contract with el2_common's stores in
 * exceptions.S, and snapshot_hdr embeds this struct by value
 * (snapshot.h:181), so the whole store header's layout depends on it.
 * ==================================================================== */
struct el2_frame {
	uint64_t x[31];      /* 0x000: x0..x30 (ends at 0x0f8)      */
	uint64_t kind;       /* 0x0f8: EL2_KIND_* + group<<2        */
	uint64_t elr;        /* 0x100: ELR_EL2  (faulting/return PC) */
	uint64_t spsr;       /* 0x108: SPSR_EL2                      */
	uint64_t esr;        /* 0x110: ESR_EL2  (syndrome)          */
	uint64_t far;        /* 0x118: FAR_EL2  (fault address)     */
	uint64_t sp_at_entry;/* 0x120                               */
};

/* ==================================================================== *
 * struct snapshot_sysregs — VERBATIM from snapshot.h:118-161, field order
 * preserved exactly (snapshot.h:112-113 calls this "the on-disk/store
 * order; keep it stable").
 * ==================================================================== */
struct snapshot_sysregs {
	/* --- MMU / translation --- */
	uint64_t sctlr_el1;
	uint64_t ttbr0_el1;
	uint64_t ttbr1_el1;
	uint64_t tcr_el1;
	uint64_t mair_el1;
	uint64_t amair_el1;
	uint64_t contextidr_el1;

	/* --- Exception / vector state --- */
	uint64_t vbar_el1;
	uint64_t esr_el1;
	uint64_t far_el1;
	uint64_t elr_el1;
	uint64_t spsr_el1;
	uint64_t sp_el0;
	uint64_t sp_el1;

	/* --- Thread / TLS pointers --- */
	uint64_t tpidr_el0;
	uint64_t tpidrro_el0;
	uint64_t tpidr_el1;

	/* --- EL0 access control / feature enables --- */
	uint64_t cpacr_el1;
	uint64_t mdscr_el1;
	uint64_t pmcr_el0;

	/* --- Generic timer (SKEW-SENSITIVE) --- */
	uint64_t cntkctl_el1;
	uint64_t cntp_ctl_el0;
	uint64_t cntp_cval_el0;
	uint64_t cntv_ctl_el0;
	uint64_t cntv_cval_el0;

	/* --- EL2-side per-guest translation (stage-2 + trap routing) --- */
	uint64_t vttbr_el2;
	uint64_t vtcr_el2;
	uint64_t hcr_el2;
	uint64_t cntvoff_el2;
};

/* ==================================================================== *
 * struct snapshot_hdr — VERBATIM from snapshot.h:170-186.
 * ==================================================================== */
struct snapshot_hdr {
	uint32_t magic;        /* SNAP_MAGIC ("SNP1")                          */
	uint32_t version;      /* struct layout version                        */
	uint32_t valid;        /* 0 while writing, 1 once fully committed      */
	uint32_t store_kind;   /* enum snapshot_store_kind used                */

	uint64_t dram_base;
	uint64_t dram_size;
	uint64_t dram_store;
	uint64_t taken_cntpct;

	struct el2_frame        frame;
	struct snapshot_sysregs sysregs;

	uint32_t crc32;
	uint32_t reserved;
};

/* -------------------------------------------------------------------- *
 * ON-STORE ABI, pinned at COMPILE time. Item 2 of "WHAT THIS FILE PROVES".
 *
 * These are _Static_asserts rather than runtime assert()s on purpose: a
 * layout change must fail the BUILD, not a test run, because the layout is
 * what a different build of the hypervisor reads back out of DRAM after a
 * warm reset (snapshot.h:165-168) and what snapshot_net.py's chunk 0 carries
 * over the wire. Every number below was derived from the field list above,
 * not copied from a run.
 * -------------------------------------------------------------------- */
/* struct el2_frame (exceptions.h:22-30) — the offsets its own comments
 * promise, which exceptions.S's el2_common stores to by hand. */
_Static_assert(offsetof(struct el2_frame, x)           == 0x000, "el2_frame.x");
_Static_assert(sizeof(((struct el2_frame *)0)->x)      == 31 * 8, "el2_frame.x size");
_Static_assert(offsetof(struct el2_frame, kind)        == 0x0f8, "el2_frame.kind");
_Static_assert(offsetof(struct el2_frame, elr)         == 0x100, "el2_frame.elr");
_Static_assert(offsetof(struct el2_frame, spsr)        == 0x108, "el2_frame.spsr");
_Static_assert(offsetof(struct el2_frame, esr)         == 0x110, "el2_frame.esr");
_Static_assert(offsetof(struct el2_frame, far)         == 0x118, "el2_frame.far");
_Static_assert(offsetof(struct el2_frame, sp_at_entry) == 0x120, "el2_frame.sp_at_entry");
_Static_assert(sizeof(struct el2_frame)                == 0x128, "sizeof el2_frame");

/* struct snapshot_sysregs — 29 uint64_t, no padding, order per snapshot.h. */
_Static_assert(offsetof(struct snapshot_sysregs, sctlr_el1)      ==   0, "sr.sctlr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, ttbr0_el1)      ==   8, "sr.ttbr0_el1");
_Static_assert(offsetof(struct snapshot_sysregs, ttbr1_el1)      ==  16, "sr.ttbr1_el1");
_Static_assert(offsetof(struct snapshot_sysregs, tcr_el1)        ==  24, "sr.tcr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, mair_el1)       ==  32, "sr.mair_el1");
_Static_assert(offsetof(struct snapshot_sysregs, amair_el1)      ==  40, "sr.amair_el1");
_Static_assert(offsetof(struct snapshot_sysregs, contextidr_el1) ==  48, "sr.contextidr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, vbar_el1)       ==  56, "sr.vbar_el1");
_Static_assert(offsetof(struct snapshot_sysregs, esr_el1)        ==  64, "sr.esr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, far_el1)        ==  72, "sr.far_el1");
_Static_assert(offsetof(struct snapshot_sysregs, elr_el1)        ==  80, "sr.elr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, spsr_el1)       ==  88, "sr.spsr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, sp_el0)         ==  96, "sr.sp_el0");
_Static_assert(offsetof(struct snapshot_sysregs, sp_el1)         == 104, "sr.sp_el1");
_Static_assert(offsetof(struct snapshot_sysregs, tpidr_el0)      == 112, "sr.tpidr_el0");
_Static_assert(offsetof(struct snapshot_sysregs, tpidrro_el0)    == 120, "sr.tpidrro_el0");
_Static_assert(offsetof(struct snapshot_sysregs, tpidr_el1)      == 128, "sr.tpidr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, cpacr_el1)      == 136, "sr.cpacr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, mdscr_el1)      == 144, "sr.mdscr_el1");
_Static_assert(offsetof(struct snapshot_sysregs, pmcr_el0)       == 152, "sr.pmcr_el0");
_Static_assert(offsetof(struct snapshot_sysregs, cntkctl_el1)    == 160, "sr.cntkctl_el1");
_Static_assert(offsetof(struct snapshot_sysregs, cntp_ctl_el0)   == 168, "sr.cntp_ctl_el0");
_Static_assert(offsetof(struct snapshot_sysregs, cntp_cval_el0)  == 176, "sr.cntp_cval_el0");
_Static_assert(offsetof(struct snapshot_sysregs, cntv_ctl_el0)   == 184, "sr.cntv_ctl_el0");
_Static_assert(offsetof(struct snapshot_sysregs, cntv_cval_el0)  == 192, "sr.cntv_cval_el0");
_Static_assert(offsetof(struct snapshot_sysregs, vttbr_el2)      == 200, "sr.vttbr_el2");
_Static_assert(offsetof(struct snapshot_sysregs, vtcr_el2)       == 208, "sr.vtcr_el2");
_Static_assert(offsetof(struct snapshot_sysregs, hcr_el2)        == 216, "sr.hcr_el2");
_Static_assert(offsetof(struct snapshot_sysregs, cntvoff_el2)    == 224, "sr.cntvoff_el2");
_Static_assert(sizeof(struct snapshot_sysregs)                   == 232, "sizeof sysregs (29 x u64)");

/* struct snapshot_hdr — the record at SNAP_STORE_BASE. */
_Static_assert(offsetof(struct snapshot_hdr, magic)        ==   0, "hdr.magic");
_Static_assert(offsetof(struct snapshot_hdr, version)      ==   4, "hdr.version");
_Static_assert(offsetof(struct snapshot_hdr, valid)        ==   8, "hdr.valid");
_Static_assert(offsetof(struct snapshot_hdr, store_kind)   ==  12, "hdr.store_kind");
_Static_assert(offsetof(struct snapshot_hdr, dram_base)    ==  16, "hdr.dram_base");
_Static_assert(offsetof(struct snapshot_hdr, dram_size)    ==  24, "hdr.dram_size");
_Static_assert(offsetof(struct snapshot_hdr, dram_store)   ==  32, "hdr.dram_store");
_Static_assert(offsetof(struct snapshot_hdr, taken_cntpct) ==  40, "hdr.taken_cntpct");
_Static_assert(offsetof(struct snapshot_hdr, frame)        ==  48, "hdr.frame");
_Static_assert(offsetof(struct snapshot_hdr, sysregs)      == 344, "hdr.sysregs (48+0x128)");
_Static_assert(offsetof(struct snapshot_hdr, crc32)        == 576, "hdr.crc32 (344+232)");
_Static_assert(offsetof(struct snapshot_hdr, reserved)     == 580, "hdr.reserved");
_Static_assert(sizeof(struct snapshot_hdr)                 == 584, "sizeof snapshot_hdr");

/* The header must fit inside the 64 KiB metadata reservation, otherwise
 * snapshot_save() would scribble into the DRAM copy region that starts at
 * SNAP_DRAM_STORE (snapshot.h:88-89). Huge margin today (584 of 65536), but
 * an unpinned invariant is an invariant waiting to break. */
_Static_assert(sizeof(struct snapshot_hdr) <= SNAP_META_SIZE,
               "snapshot_hdr must fit in SNAP_META_SIZE");

/* Chunk geometry (snapshot_net.h:126-135), pinned as literals. These exact
 * numbers appear in snapshot_net.h's own comment ("767,006 chunks / ~93.6
 * KiB of bitmap") and are hardcoded a second time in snapshot_net.py:76-77;
 * snapshot_net.py's extended selftest now cross-checks the C side
 * mechanically, and these asserts pin the C side itself. */
_Static_assert(SNAPNET_STORE_LEN      == 0x40010000UL, "STORE_LEN");
_Static_assert(SNAPNET_STORE_LEN      == 1073807360UL, "STORE_LEN decimal");
_Static_assert(SNAPNET_TOTAL_CHUNKS   == 767006UL,     "TOTAL_CHUNKS");
_Static_assert(SNAPNET_MANIFEST_BYTES == 95876UL,      "MANIFEST_BYTES");
/* The last chunk is SHORT: 360 bytes, not SNAPNET_CHUNK. Both ends must
 * derive that from seq alone (snapshot_net.h:152-157). */
_Static_assert(SNAPNET_STORE_LEN - (SNAPNET_TOTAL_CHUNKS - 1u) * SNAPNET_CHUNK == 360UL,
               "last chunk is 360 bytes");
/* Every chunk offset is 8-byte aligned, which is what lets chunk_is_zero()
 * (snapshot_net.c:198-209) do a uint64_t scan — its comment claims this. */
_Static_assert(SNAPNET_CHUNK % 8u == 0u, "CHUNK must be 8-byte aligned");
_Static_assert(SNAP_STORE_BASE % 8u == 0u, "STORE_BASE must be 8-byte aligned");
/* 14 (Ethernet) + 16 (snapnet hdr) + 1400 = 1430 <= 1500 MTU, per
 * snapshot_net.h:116-118. */
_Static_assert(14u + SNAPNET_HDR_LEN + SNAPNET_CHUNK <= 1500u, "frame must clear 1500 MTU");

/* ==================================================================== *
 * CRC32 IMPLEMENTATION A — VERBATIM mirror of snapshot.c:210-220
 * (bit-at-a-time, one 64-bit word at a time, used on the SAVE path only,
 * folded into dram_copy(); snapshot.c:246-271).
 * ==================================================================== */
static uint32_t crc32_update_word(uint32_t crc, uint64_t word)
{
	int k, b;

	for (k = 0; k < 8; k++) {
		crc ^= (uint32_t)(word >> (8 * k)) & 0xffu;
		for (b = 0; b < 8; b++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
	}
	return crc;
}

/* How snapshot.c ACTUALLY drives implementation A end to end: init
 * CRC32_INIT, one crc32_update_word() per 64-bit word read out of the source
 * region (snapshot.c:254-258), final XOR 0xFFFFFFFF at snapshot.c:334.
 *
 * The 8 bytes are assembled here little-endian-first, which is exactly what
 * the AArch64 `ldr x, [s]` at snapshot.c:255 yields on this (little-endian)
 * CPU: `word >> (8*k)` then extracts the byte living at src_pa + i*8 + k.
 * So implementation A's byte order == memory order — and ONLY because the
 * core is little-endian. test_crc32_agreement_is_little_endian_dependent()
 * below makes that dependency explicit rather than incidental.
 *
 * Requires len % 8 == 0, which snapshot.c always satisfies: it only ever
 * calls dram_copy() with SNAP_DRAM_SIZE / h->dram_size. */
static uint32_t crc32_wordwise(const uint8_t *p, size_t len)
{
	uint32_t crc = CRC32_INIT;
	size_t i;

	assert(len % 8u == 0u);
	for (i = 0; i < len; i += 8) {
		uint64_t w = 0;
		int b;
		for (b = 0; b < 8; b++)
			w |= (uint64_t)p[i + b] << (8 * b);
		crc = crc32_update_word(crc, w);
	}
	return crc ^ 0xFFFFFFFFu;
}

/* Same as crc32_wordwise but assembling each word BIG-endian-first. Used by
 * exactly one test, to demonstrate that A-vs-B agreement is a property of
 * the little-endian word load and not a coincidence of the polynomial. Not a
 * mirror of anything in the source. */
static uint32_t crc32_wordwise_be(const uint8_t *p, size_t len)
{
	uint32_t crc = CRC32_INIT;
	size_t i;

	assert(len % 8u == 0u);
	for (i = 0; i < len; i += 8) {
		uint64_t w = 0;
		int b;
		for (b = 0; b < 8; b++)
			w = (w << 8) | (uint64_t)p[i + b];
		crc = crc32_update_word(crc, w);
	}
	return crc ^ 0xFFFFFFFFu;
}

/* BYTE-GRANULAR RESTATEMENT of implementation A's inner loop (the two lines
 * at snapshot.c:215-217), so implementation A can be compared against
 * implementation B at lengths that are NOT multiples of 8 — including the
 * standard 9-byte "123456789" check vector. This is NOT a mirror of any
 * function in the tree; it is the same arithmetic with the outer 8-byte
 * unrolling removed. test_crc32_bitwise_restatement_is_faithful() proves the
 * restatement equals crc32_wordwise() on multiple-of-8 buffers before any
 * other test relies on it. */
static uint32_t crc32_bitwise_bytes(const uint8_t *p, size_t len)
{
	uint32_t crc = CRC32_INIT;
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= (uint32_t)p[i] & 0xffu;
		for (b = 0; b < 8; b++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
	}
	return crc ^ 0xFFFFFFFFu;
}

/* ==================================================================== *
 * CRC32 IMPLEMENTATION B — VERBATIM mirror of snapshot_net.c:137-161
 * (table-driven; used on the RX validation path for every chunk, and for
 * the whole-image recheck at snapshot_net.c:486 via crc32_region(), which is
 * the same loop with a wdt_pet() in it).
 * ==================================================================== */
static uint32_t crc_table[256];
static int      crc_table_ready;

static void crc32_init(void)
{
	uint32_t i;
	for (i = 0; i < 256u; i++) {
		uint32_t c = i;
		int k;
		for (k = 0; k < 8; k++)
			c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
	crc_table_ready = 1;
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFu, i;
	if (!crc_table_ready)
		crc32_init();
	for (i = 0; i < len; i++)
		crc = crc_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

/* ==================================================================== *
 * Little-endian byte accessors — VERBATIM from snapshot_net.c:98-116.
 * ==================================================================== */
static inline uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff); }

static inline void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* ==================================================================== *
 * Bitmap helpers — VERBATIM from snapshot_net.c:121-125.
 * ==================================================================== */
static inline int bit_test(const uint8_t *bm, uint32_t idx)
{ return (bm[idx >> 3] >> (idx & 7u)) & 1u; }

static inline void bit_set(uint8_t *bm, uint32_t idx)
{ bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u)); }

/* ==================================================================== *
 * chunk_is_zero() — VERBATIM from snapshot_net.c:198-209. Drives the
 * zero-skip decision on the send path; snapshot_net.py's build_manifest()
 * is the host-side twin of this predicate.
 * ==================================================================== */
static int chunk_is_zero(const uint8_t *p, uint32_t n)
{
	const uint64_t *w = (const uint64_t *)(uintptr_t)p;
	uint32_t nwords = n / 8u, i;
	for (i = 0; i < nwords; i++)
		if (w[i] != 0)
			return 0;
	for (i = nwords * 8u; i < n; i++)
		if (p[i] != 0)
			return 0;
	return 1;
}

/* ==================================================================== *
 * snapshot_present() — VERBATIM mirror of snapshot.c:282-287, with the
 * fixed-PA snap_hdr() (snapshot.c:277-280, which dereferences the literal
 * 0x80000000) replaced by a caller-supplied pointer. That is the ONLY
 * change; the three checks are the source's three checks.
 * ==================================================================== */
static int snapshot_present_at(const struct snapshot_hdr *h)
{
	return (h->magic == SNAP_MAGIC && h->valid == 1u &&
	        h->version == SNAPSHOT_VERSION);
}

/* ==================================================================== *
 * RX PATH under test — mirror of snapshot_net.c:275-323, plus the module
 * state it touches (snapshot_net.c:73-90) and the SHIMS described in the
 * file banner.
 * ==================================================================== */

/* snapshot_net.c:73-75 / :84-90 — the real, full-size bitmap (95,876 B) and
 * the real counters. NOT shimmed. */
static uint8_t           s_received[SNAPNET_MANIFEST_BYTES];
static volatile int      s_rx_active;
static volatile uint32_t s_rx_count;

/* SHIM 1 (see banner): an 8 KiB window standing in for the 1 GiB store, plus
 * a discard sink for chunks that fall outside it. t_win_base is the store
 * BYTE OFFSET that t_win[0] models. */
#define TEST_WIN_BYTES 8192u
static uint64_t t_win_base;
static uint8_t  t_win[TEST_WIN_BYTES];
static uint8_t  t_sink[SNAPNET_CHUNK];
static uint32_t t_writes_outside_window;

static uint8_t *t_dst(uint64_t off, uint32_t plen)
{
	if (off >= t_win_base && (off - t_win_base) + plen <= TEST_WIN_BYTES)
		return &t_win[off - t_win_base];
	t_writes_outside_window++;
	return t_sink;
}

/* SHIM 2 (see banner): store_cache_clean() (snapshot_net.c:186-192) records
 * instead of executing `dc civac`. */
static uint32_t  t_clean_calls;
static uintptr_t t_clean_addr;
static uint32_t  t_clean_size;

static void store_cache_clean_stub(uintptr_t addr, uint32_t size)
{
	t_clean_calls++;
	t_clean_addr = addr;
	t_clean_size = size;
}

/* VERBATIM mirror of snapshot_net.c:275-323. Deviations, both flagged
 * inline: the destination base (SHIM 1) and the cache clean (SHIM 2). The
 * five-check validation ladder, its ORDER, the CRC check and the
 * bit_test/bit_set/s_rx_count bookkeeping are unchanged. */
static void snapshot_net_rx_frame(const uint8_t *payload, uint16_t len)
{
	uint32_t magic, seq, crc, i;
	uint16_t plen, expect_len;
	uint64_t off;
	uint32_t remain;
	const uint8_t *data;
	uint8_t *dst;

	if (len < SNAPNET_HDR_LEN)
		return;
	magic = rd32(payload + 0);
	if (magic != SNAPNET_MAGIC)
		return;
	if (!s_rx_active)
		return;

	seq  = rd32(payload + 4);
	plen = rd16(payload + 8);
	crc  = rd32(payload + 12);

	if ((uint32_t)plen > (uint32_t)len - SNAPNET_HDR_LEN)
		return;                          /* corrupt/truncated: drop */
	if (seq >= SNAPNET_TOTAL_CHUNKS)
		return;                          /* bogus chunk index: drop */

	off    = (uint64_t)seq * SNAPNET_CHUNK;
	remain = (uint32_t)(SNAPNET_STORE_LEN - off);
	expect_len = (remain < SNAPNET_CHUNK) ? (uint16_t)remain : (uint16_t)SNAPNET_CHUNK;
	if (plen != expect_len)
		return;                          /* wrong length for this index: drop */

	data = payload + SNAPNET_HDR_LEN;
	if (crc32_calc(data, plen) != crc)
		return;                          /* corrupt payload: drop */

	dst = t_dst(off, plen);              /* SHIM 1 for snapshot_net.c:314 */
	for (i = 0; i < plen; i++)
		dst[i] = data[i];
	store_cache_clean_stub((uintptr_t)dst, plen);   /* SHIM 2 for :317 */

	if (!bit_test(s_received, seq)) {
		bit_set(s_received, seq);
		s_rx_count++;
	}
}

/* ==================================================================== *
 * Test helpers (not mirrors of anything).
 * ==================================================================== */

/* Reset every piece of RX state so each test starts from one clean, known
 * point with one call. */
static void rx_reset(uint64_t win_base)
{
	memset(s_received, 0, sizeof(s_received));
	s_rx_count = 0;
	s_rx_active = 1;
	t_win_base = win_base;
	memset(t_win, 0, sizeof(t_win));
	memset(t_sink, 0, sizeof(t_sink));
	t_writes_outside_window = 0;
	t_clean_calls = 0;
	t_clean_addr = 0;
	t_clean_size = 0;
}

/* Build a wire frame exactly as snapnet_send_one() (snapshot_net.c:241-267)
 * does: 16-byte LE header via wr32/wr16, then the payload; CRC over the
 * payload only. `buf` must hold SNAPNET_HDR_LEN + plen bytes. Returns the
 * total length. Payload bytes are a cheap deterministic pattern. */
static uint16_t build_frame(uint8_t *buf, uint32_t magic, uint32_t seq,
                           uint16_t plen, int good_crc, uint8_t seed)
{
	uint32_t crc;
	uint16_t i;

	for (i = 0; i < plen; i++)
		buf[SNAPNET_HDR_LEN + i] = (uint8_t)(seed + i);
	crc = crc32_calc(buf + SNAPNET_HDR_LEN, plen);
	if (!good_crc)
		crc ^= 0xFFFFFFFFu;   /* any wrong value; ^~0 is never a fixed point */

	wr32(buf + 0, magic);
	wr32(buf + 4, seq);
	wr16(buf + 8, plen);
	wr16(buf + 10, 0);
	wr32(buf + 12, crc);
	return (uint16_t)(SNAPNET_HDR_LEN + plen);
}

/* The expected payload length for chunk `seq`, computed the way BOTH ends
 * are required to (snapshot_net.h:152-157 — neither off nor total is on the
 * wire). Deliberately a separate restatement from the mirrored RX path so a
 * transcription slip in one shows up as a test failure, not a shared bug. */
static uint16_t expect_len_for(uint32_t seq)
{
	uint64_t off = (uint64_t)seq * SNAPNET_CHUNK;
	uint64_t remain = SNAPNET_STORE_LEN - off;
	return (remain < SNAPNET_CHUNK) ? (uint16_t)remain : (uint16_t)SNAPNET_CHUNK;
}

/* ==================================================================== *
 * TESTS — 1. the two CRC32 implementations must agree
 * ==================================================================== */

/* Before anything is compared through crc32_bitwise_bytes(), prove that the
 * byte-granular restatement of snapshot.c:215-217 really is the same
 * function as the verbatim 8-bytes-at-a-time crc32_update_word() driven the
 * way snapshot.c drives it. Without this, an A-vs-B "agreement" test could
 * be comparing B against a bug in my own restatement. */
static void test_crc32_bitwise_restatement_is_faithful(void)
{
	static const size_t lens[] = { 8, 16, 24, 64, 256, 1400, 4096 };
	uint8_t buf[4096];
	size_t li, i;

	for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
		size_t n = lens[li];
		for (i = 0; i < n; i++)
			buf[i] = (uint8_t)(i * 37u + 11u);
		assert(crc32_wordwise(buf, n) == crc32_bitwise_bytes(buf, n));
	}
	/* Same for the two degenerate contents the DRAM image is mostly made
	 * of in practice. */
	memset(buf, 0x00, sizeof(buf));
	assert(crc32_wordwise(buf, sizeof(buf)) == crc32_bitwise_bytes(buf, sizeof(buf)));
	memset(buf, 0xFF, sizeof(buf));
	assert(crc32_wordwise(buf, sizeof(buf)) == crc32_bitwise_bytes(buf, sizeof(buf)));
}

/* Both implementations must produce the standard IEEE/zlib check value for
 * "123456789". snapshot_net.py:368 already checks this for zlib.crc32 on the
 * host side; nothing checked either C implementation. Note the vector is 9
 * bytes, so implementation A is exercised through the restatement proven
 * faithful by the test above. */
static void test_crc32_standard_check_value(void)
{
	static const uint8_t v[9] = { '1','2','3','4','5','6','7','8','9' };

	assert(crc32_calc(v, 9) == 0xCBF43926u);            /* implementation B */
	assert(crc32_bitwise_bytes(v, 9) == 0xCBF43926u);   /* implementation A */

	/* And the empty-input identity both must satisfy (init ^ final xor). */
	assert(crc32_calc(v, 0) == 0u);
	assert(crc32_bitwise_bytes(v, 0) == 0u);
}

/* THE test this file exists for: the save path computes the header's crc32
 * with implementation A (snapshot.c:331-334), and snapshot_net_recv() step 6
 * (snapshot_net.c:486) refuses to restore unless implementation B over the
 * received image returns the SAME value. snapshot_net.c:130-135 asserts they
 * are identical in a comment. Verify it over the shapes that actually occur:
 * a full 1400-byte chunk, the short 360-byte last chunk, all-zero, all-0xFF,
 * and several non-multiple-of-4 lengths. */
static void test_crc32_two_implementations_agree(void)
{
	static const size_t lens[] = {
		1, 2, 3, 5, 7, 9, 15, 17, 63, 65, 127, 255, 359, 360, 361,
		1399, 1400, 1401, 2048, 4093, 4096,
	};
	uint8_t buf[4096];
	size_t li, i;

	for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
		size_t n = lens[li];

		/* pseudo-random-ish */
		for (i = 0; i < n; i++)
			buf[i] = (uint8_t)((i * 131u + 7u) ^ (i >> 3));
		assert(crc32_bitwise_bytes(buf, n) == crc32_calc(buf, (uint32_t)n));

		/* all zero — the overwhelmingly common case in a 1 GiB image, and
		 * the one zero-skip depends on */
		memset(buf, 0x00, n);
		assert(crc32_bitwise_bytes(buf, n) == crc32_calc(buf, (uint32_t)n));

		/* all 0xFF — the other degenerate pattern, and the one most likely
		 * to expose a sign/shift error */
		memset(buf, 0xFF, n);
		assert(crc32_bitwise_bytes(buf, n) == crc32_calc(buf, (uint32_t)n));
	}

	/* And on the exact 8-byte-word path snapshot.c really runs (no
	 * restatement in the loop at all), for a multiple-of-8 length. */
	for (i = 0; i < 2048; i++)
		buf[i] = (uint8_t)(i ^ 0x5A);
	assert(crc32_wordwise(buf, 2048) == crc32_calc(buf, 2048));
	memset(buf, 0, 2048);
	assert(crc32_wordwise(buf, 2048) == crc32_calc(buf, 2048));
	memset(buf, 0xFF, 2048);
	assert(crc32_wordwise(buf, 2048) == crc32_calc(buf, 2048));
}

/* The A==B agreement above holds because the AArch64 64-bit load in
 * snapshot.c:255 is LITTLE-endian, so crc32_update_word()'s `word >> (8*k)`
 * walks bytes in ascending ADDRESS order. Make that dependency explicit: if
 * the same words were assembled big-endian-first, A and B would disagree.
 * This is not a bug today (the A53 runs LE, SCTLR_EL2.EE=0), it is a pinned
 * assumption — the day anything runs this store through a BE path, the
 * comment at snapshot_net.c:130-135 stops being true. */
static void test_crc32_agreement_is_little_endian_dependent(void)
{
	uint8_t buf[64];
	size_t i;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)(i + 1);

	assert(crc32_wordwise(buf, sizeof(buf)) == crc32_calc(buf, sizeof(buf)));
	assert(crc32_wordwise_be(buf, sizeof(buf)) != crc32_calc(buf, sizeof(buf)));
}

/* ==================================================================== *
 * TESTS — 2. header ABI (runtime half; the layout itself is _Static_assert'd)
 * ==================================================================== */

/* The magic/version constants, and the store-kind enum values
 * snapshot_save() writes (snapshot.c:303-305). SNAP_MAGIC is documented as
 * "SNP1" (snapshot.h:99-100); check the bytes really spell that, because the
 * whole point of a magic is that a human staring at a `bc 0x80000000` dump
 * recognises it. */
static void test_header_magic_and_version_constants(void)
{
	uint8_t b[4];

	assert(SNAP_MAGIC == 0x534E5031u);
	wr32(b, SNAP_MAGIC);          /* little-endian, as the store holds it */
	assert(b[0] == '1' && b[1] == 'P' && b[2] == 'N' && b[3] == 'S');
	/* i.e. a byte dump reads "1PNS"; the mnemonic "SNP1" is the big-endian
	 * reading of the constant, NOT the byte order in memory. Worth pinning
	 * so nobody "fixes" one side to match the other. */

	assert(SNAPSHOT_VERSION == 1u);
	assert(SNAP_STORE_DRAM == 0 && SNAP_STORE_EMMC == 1 && SNAP_STORE_EMAC == 2);

	/* Geometry constants the header echoes back (snapshot.c:309-311). */
	assert(SNAP_DRAM_BASE == 0x40000000UL);
	assert(SNAP_DRAM_SIZE == 0x40000000UL);
	assert(SNAP_STORE_BASE == 0x80000000UL);
	assert(SNAP_DRAM_STORE == 0x80010000UL);
	/* The store must be able to hold header + a full DRAM image, and must
	 * start exactly where the guest's stage-2 identity range ends
	 * (snapshot.h:78-83). */
	assert(SNAP_DRAM_BASE + SNAP_DRAM_SIZE == SNAP_STORE_BASE);
}

/* A committed header, byte-for-byte, is what a *different build* reads back.
 * Round-trip it through a raw byte buffer at the offsets the
 * _Static_asserts pin, to prove the field order in the struct really is the
 * field order on the store (i.e. no compiler-inserted padding shifted
 * anything relative to the offsets a host-side decoder would use). */
static void test_header_byte_image_round_trip(void)
{
	struct snapshot_hdr h;
	uint8_t img[sizeof(struct snapshot_hdr)];
	int i;

	memset(&h, 0, sizeof(h));
	h.magic = SNAP_MAGIC;
	h.version = SNAPSHOT_VERSION;
	h.valid = 1u;
	h.store_kind = SNAP_STORE_DRAM;
	h.dram_base = SNAP_DRAM_BASE;
	h.dram_size = SNAP_DRAM_SIZE;
	h.dram_store = SNAP_DRAM_STORE;
	h.taken_cntpct = 0x0123456789ABCDEFull;
	for (i = 0; i < 31; i++)
		h.frame.x[i] = 0x1000ull + (uint64_t)i;
	h.frame.elr = 0xFFFF0000DEADBEEFull;
	h.frame.spsr = 0x3C5ull;
	h.sysregs.sctlr_el1 = 0x30D0180Dull;
	h.sysregs.cntvoff_el2 = 0xFFFFFFFFFFFF0000ull;
	h.crc32 = 0xCBF43926u;

	memcpy(img, &h, sizeof(img));

	/* Decode straight out of the byte image at the pinned offsets. */
	assert(rd32(img + 0) == SNAP_MAGIC);
	assert(rd32(img + 4) == SNAPSHOT_VERSION);
	assert(rd32(img + 8) == 1u);
	assert(rd32(img + 12) == (uint32_t)SNAP_STORE_DRAM);
	assert(rd32(img + 16) == 0x40000000u && rd32(img + 20) == 0u);      /* dram_base */
	assert(rd32(img + 24) == 0x40000000u && rd32(img + 28) == 0u);      /* dram_size */
	assert(rd32(img + 32) == 0x80010000u && rd32(img + 36) == 0u);      /* dram_store */
	assert(rd32(img + 40) == 0x89ABCDEFu && rd32(img + 44) == 0x01234567u);
	/* frame.x[0] at 48, x[1] at 56, ... x[30] at 48+240=288 */
	assert(rd32(img + 48) == 0x1000u);
	assert(rd32(img + 48 + 30 * 8) == 0x101Eu);
	/* frame.elr at 48+0x100 = 304 */
	assert(rd32(img + 48 + 0x100) == 0xDEADBEEFu);
	assert(rd32(img + 48 + 0x104) == 0xFFFF0000u);
	/* sysregs.sctlr_el1 at 344, sysregs.cntvoff_el2 at 344+224 = 568 */
	assert(rd32(img + 344) == 0x30D0180Du);
	assert(rd32(img + 568) == 0xFFFF0000u);
	assert(rd32(img + 576) == 0xCBF43926u);   /* crc32 */
	assert(rd32(img + 580) == 0u);            /* reserved */

	/* And back the other way: a byte image decodes into the same struct. */
	{
		struct snapshot_hdr h2;
		memcpy(&h2, img, sizeof(h2));
		assert(memcmp(&h, &h2, sizeof(h)) == 0);
	}
}

/* ==================================================================== *
 * TESTS — 3. snapshot_net_rx_frame() validation + reassembly
 * ==================================================================== */

/* A well-formed frame for chunk 0 is accepted: the payload lands at the
 * right store offset, the bitmap bit is set, the counter is bumped, and the
 * cache clean is issued for exactly the bytes written. */
static void test_rx_accepts_valid_frame(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t n;
	uint16_t i;

	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 0, SNAPNET_CHUNK, 1, 0x11);
	snapshot_net_rx_frame(f, n);

	assert(s_rx_count == 1);
	assert(bit_test(s_received, 0));
	assert(t_writes_outside_window == 0);
	assert(t_clean_calls == 1);
	assert(t_clean_size == SNAPNET_CHUNK);
	assert(t_clean_addr == (uintptr_t)&t_win[0]);
	for (i = 0; i < SNAPNET_CHUNK; i++)
		assert(t_win[i] == (uint8_t)(0x11 + i));
	/* nothing spilled past the chunk */
	assert(t_win[SNAPNET_CHUNK] == 0);
}

/* Chunk 3 must land at 3*1400 = 4200, derived from seq alone. This is the
 * one arithmetic snapshot_net.h:152-157 deliberately refuses to put on the
 * wire, so a divergence between the two ends is silent data corruption
 * rather than a protocol error. */
static void test_rx_offset_is_seq_times_chunk(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t n, i;

	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 3, SNAPNET_CHUNK, 1, 0x40);
	snapshot_net_rx_frame(f, n);

	assert(s_rx_count == 1 && bit_test(s_received, 3));
	assert(t_clean_addr == (uintptr_t)&t_win[3 * SNAPNET_CHUNK]);
	for (i = 0; i < SNAPNET_CHUNK; i++)
		assert(t_win[3 * SNAPNET_CHUNK + i] == (uint8_t)(0x40 + i));
	/* chunks 0..2 untouched */
	for (i = 0; i < 3 * SNAPNET_CHUNK; i++)
		assert(t_win[i] == 0);
}

/* len < SNAPNET_HDR_LEN is rejected before anything is even parsed
 * (snapshot_net.c:284-285) — and it must be, because the very next line
 * would compute `len - SNAPNET_HDR_LEN` in unsigned arithmetic and wrap. */
static void test_rx_rejects_short_frame(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t l;

	build_frame(f, SNAPNET_MAGIC, 0, SNAPNET_CHUNK, 1, 0x22);
	for (l = 0; l < SNAPNET_HDR_LEN; l++) {
		rx_reset(0);
		snapshot_net_rx_frame(f, l);
		assert(s_rx_count == 0);
		assert(!bit_test(s_received, 0));
		assert(t_clean_calls == 0);
	}
	/* Exactly SNAPNET_HDR_LEN is NOT short — it is a header with a zero
	 * payload, which then fails the plen==expect_len check instead. Pin the
	 * boundary so a `<=` slip is caught. */
	rx_reset(0);
	build_frame(f, SNAPNET_MAGIC, 0, 0, 1, 0x22);
	snapshot_net_rx_frame(f, SNAPNET_HDR_LEN);
	assert(s_rx_count == 0);   /* rejected, but by the LENGTH check, not this one */
}

static void test_rx_rejects_bad_magic(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t n;

	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC ^ 1u, 0, SNAPNET_CHUNK, 1, 0x33);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0 && !bit_test(s_received, 0) && t_clean_calls == 0);

	/* A zero magic (e.g. a runt/padded frame) is rejected too. */
	rx_reset(0);
	n = build_frame(f, 0u, 0, SNAPNET_CHUNK, 1, 0x33);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0);

	/* The magic really is the LE byte spelling snapshot_net.h:169 claims. */
	{
		uint8_t b[4];
		wr32(b, SNAPNET_MAGIC);
		assert(b[0] == 'S' && b[1] == 'N' && b[2] == 'P' && b[3] == 'B');
	}
}

/* snapshot_net.c:289-290 drops everything unless a recv() window is open.
 * NOTE THE ORDER, which this test pins deliberately: the magic check comes
 * BEFORE the s_rx_active check, so an idle board still parses and compares
 * the magic of every 0x88B8 frame it sees. That is 4 loads and a compare per
 * stray frame — cheap, but it means the idle path is not a pure early-out.
 * If the order were ever swapped, the bad-magic test above would still pass
 * while this one would too; only their conjunction pins the sequence. */
static void test_rx_drops_everything_while_idle(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t n;

	rx_reset(0);
	s_rx_active = 0;
	n = build_frame(f, SNAPNET_MAGIC, 0, SNAPNET_CHUNK, 1, 0x44);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0);
	assert(!bit_test(s_received, 0));
	assert(t_clean_calls == 0);
	assert(t_win[0] == 0);

	/* Re-arming makes the very same frame acceptable — proving the drop was
	 * the arming state and nothing else about the frame. */
	s_rx_active = 1;
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 1 && bit_test(s_received, 0));
}

/* seq must be < SNAPNET_TOTAL_CHUNKS (snapshot_net.c:298-299). Without this
 * the store write at :314 would be at an arbitrary offset past the store. */
static void test_rx_rejects_out_of_range_seq(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t n;
	static const uint32_t bad[] = {
		(uint32_t)SNAPNET_TOTAL_CHUNKS,
		(uint32_t)SNAPNET_TOTAL_CHUNKS + 1u,
		0x7FFFFFFFu,
		0xFFFFFFFFu,
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		rx_reset(0);
		n = build_frame(f, SNAPNET_MAGIC, bad[i], SNAPNET_CHUNK, 1, 0x55);
		snapshot_net_rx_frame(f, n);
		assert(s_rx_count == 0);
		assert(t_clean_calls == 0);
		assert(t_writes_outside_window == 0);   /* never even attempted */
	}

	/* The last VALID index is accepted (with its own short length) — pins
	 * the >= boundary rather than just "big values fail". */
	{
		uint32_t last = (uint32_t)SNAPNET_TOTAL_CHUNKS - 1u;
		uint64_t off = (uint64_t)last * SNAPNET_CHUNK;
		rx_reset(off);
		n = build_frame(f, SNAPNET_MAGIC, last, expect_len_for(last), 1, 0x66);
		snapshot_net_rx_frame(f, n);
		assert(s_rx_count == 1 && bit_test(s_received, last));
	}
}

/* plen must equal the length THIS seq implies (snapshot_net.c:301-305). The
 * interesting case is the LAST chunk: 360 bytes, not 1400. A sender that
 * blindly sends 1400 for every chunk is rejected there and nowhere else, so
 * the bug would only ever show up on the final frame of a 1 GiB transfer. */
static void test_rx_rejects_wrong_length_for_seq(void)
{
	static uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK + 8];
	uint16_t n;
	uint32_t last = (uint32_t)SNAPNET_TOTAL_CHUNKS - 1u;

	/* A normal (non-last) chunk with a short payload: rejected. */
	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 1, SNAPNET_CHUNK - 1u, 1, 0x77);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0 && t_clean_calls == 0);

	/* ...and with a zero-length payload: rejected. */
	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 1, 0, 1, 0x77);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0);

	/* THE LAST CHUNK: expects exactly 360. */
	assert(expect_len_for(last) == 360u);
	assert(expect_len_for(last - 1u) == SNAPNET_CHUNK);

	/* Full-size payload for the last chunk: rejected. */
	rx_reset((uint64_t)last * SNAPNET_CHUNK);
	n = build_frame(f, SNAPNET_MAGIC, last, SNAPNET_CHUNK, 1, 0x88);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0 && t_clean_calls == 0);

	/* 360 bytes for the last chunk: accepted, and only 360 bytes written. */
	rx_reset((uint64_t)last * SNAPNET_CHUNK);
	n = build_frame(f, SNAPNET_MAGIC, last, 360u, 1, 0x99);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 1 && bit_test(s_received, last));
	assert(t_clean_size == 360u);
	assert(t_win[359] == (uint8_t)(0x99 + 359));
	assert(t_win[360] == 0);

	/* 360 bytes offered for a NON-last chunk: rejected. */
	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 0, 360u, 1, 0x9A);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0);
}

/* plen may not exceed the bytes actually present (snapshot_net.c:296-297) —
 * a truncated frame. Note the check is `>`, not `!=`, so TRAILING bytes are
 * tolerated: a frame carrying more bytes than plen is still accepted. That
 * is deliberate and necessary, because Ethernet pads any payload under 46
 * bytes; snapshot_net.py's _eth_frame() does exactly that padding. It cannot
 * bite in practice here (the smallest legal snapnet payload is the 360-byte
 * last chunk, so 16+360=376 is already above the pad floor), which this test
 * also records. */
static void test_rx_rejects_truncated_payload_but_allows_trailing(void)
{
	static uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK + 64];
	uint16_t i;

	/* Truncated: header claims 1400 but only 1000 payload bytes follow. */
	rx_reset(0);
	build_frame(f, SNAPNET_MAGIC, 0, SNAPNET_CHUNK, 1, 0xA0);
	snapshot_net_rx_frame(f, (uint16_t)(SNAPNET_HDR_LEN + 1000u));
	assert(s_rx_count == 0 && t_clean_calls == 0);

	/* Trailing bytes: header claims 1400 and 1400+40 follow — accepted,
	 * and exactly 1400 bytes are written. */
	rx_reset(0);
	build_frame(f, SNAPNET_MAGIC, 0, SNAPNET_CHUNK, 1, 0xA0);
	for (i = 0; i < 40; i++)
		f[SNAPNET_HDR_LEN + SNAPNET_CHUNK + i] = 0xEE;
	snapshot_net_rx_frame(f, (uint16_t)(SNAPNET_HDR_LEN + SNAPNET_CHUNK + 40u));
	assert(s_rx_count == 1 && bit_test(s_received, 0));
	assert(t_clean_size == SNAPNET_CHUNK);
	assert(t_win[SNAPNET_CHUNK] == 0);       /* trailing 0xEE not copied */

	/* The Ethernet pad floor cannot affect any real snapnet frame. */
	assert(SNAPNET_HDR_LEN + 360u >= 46u);
}

/* A payload whose CRC does not match is dropped (snapshot_net.c:308-312) and
 * leaves NOTHING behind — the chunk then reappears as "missing" in the next
 * manifest-diff round, which is the whole reliability story. */
static void test_rx_rejects_bad_payload_crc(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t n;

	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 2, SNAPNET_CHUNK, /*good_crc=*/0, 0xB0);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0);
	assert(!bit_test(s_received, 2));
	assert(t_clean_calls == 0);
	assert(t_win[2 * SNAPNET_CHUNK] == 0);

	/* A single flipped payload bit (with the ORIGINAL crc still in the
	 * header) is also caught — the realistic corruption, not a wrong CRC
	 * field. */
	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 2, SNAPNET_CHUNK, 1, 0xB0);
	f[SNAPNET_HDR_LEN + 700] ^= 0x01u;
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 0 && !bit_test(s_received, 2));
}

/* Duplicates: a resend of an already-received chunk is accepted and RE-WRITTEN
 * (idempotent, since the payload is identical), the cache clean happens again,
 * but s_rx_count is NOT bumped a second time (snapshot_net.c:319-322). That
 * matters because s_rx_count is the ONLY "did anything change" signal
 * snapnet_collect_until_idle() (:328-342) has: if duplicates bumped it, a
 * host stuck in a resend loop would hold the collection window open forever. */
static void test_rx_duplicate_is_idempotent_and_does_not_bump_count(void)
{
	uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint16_t n, i;

	rx_reset(0);
	n = build_frame(f, SNAPNET_MAGIC, 1, SNAPNET_CHUNK, 1, 0xC0);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 1 && t_clean_calls == 1);

	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 1);            /* NOT 2 */
	assert(bit_test(s_received, 1));    /* still set */
	assert(t_clean_calls == 2);         /* but the write+clean DID repeat */

	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 1);
	assert(t_clean_calls == 3);

	for (i = 0; i < SNAPNET_CHUNK; i++)
		assert(t_win[SNAPNET_CHUNK + i] == (uint8_t)(0xC0 + i));
}

/* Every rejection path must leave the bitmap, the counter and the
 * destination byte-identical to before. Run them all in sequence against a
 * single state and assert nothing moved — the property the manifest-diff
 * round depends on (a dropped chunk MUST still read as missing). */
static void test_rx_rejections_leave_state_untouched(void)
{
	static uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK + 8];
	uint8_t bm_before[SNAPNET_MANIFEST_BYTES];
	uint8_t win_before[TEST_WIN_BYTES];
	uint16_t n;

	rx_reset(0);

	/* Seed one legitimately-received chunk so "untouched" is a non-trivial
	 * state, not just all-zero. */
	n = build_frame(f, SNAPNET_MAGIC, 0, SNAPNET_CHUNK, 1, 0xD0);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 1);

	memcpy(bm_before, s_received, sizeof(bm_before));
	memcpy(win_before, t_win, sizeof(win_before));
	t_clean_calls = 0;

	/* (1) short */
	snapshot_net_rx_frame(f, 4);
	/* (2) bad magic */
	n = build_frame(f, SNAPNET_MAGIC + 1u, 1, SNAPNET_CHUNK, 1, 0xD1);
	snapshot_net_rx_frame(f, n);
	/* (3) seq out of range */
	n = build_frame(f, SNAPNET_MAGIC, (uint32_t)SNAPNET_TOTAL_CHUNKS, SNAPNET_CHUNK, 1, 0xD2);
	snapshot_net_rx_frame(f, n);
	/* (4) wrong length for seq */
	n = build_frame(f, SNAPNET_MAGIC, 1, SNAPNET_CHUNK - 4u, 1, 0xD3);
	snapshot_net_rx_frame(f, n);
	/* (5) truncated */
	build_frame(f, SNAPNET_MAGIC, 1, SNAPNET_CHUNK, 1, 0xD4);
	snapshot_net_rx_frame(f, (uint16_t)(SNAPNET_HDR_LEN + 17u));
	/* (6) bad crc */
	n = build_frame(f, SNAPNET_MAGIC, 1, SNAPNET_CHUNK, 0, 0xD5);
	snapshot_net_rx_frame(f, n);

	assert(s_rx_count == 1);
	assert(t_clean_calls == 0);
	assert(memcmp(bm_before, s_received, sizeof(bm_before)) == 0);
	assert(memcmp(win_before, t_win, sizeof(win_before)) == 0);
	assert(!bit_test(s_received, 1));
}

/* A small multi-chunk reassembly inside the 8 KiB window: five consecutive
 * chunks arriving OUT OF ORDER, one of them twice and one of them corrupt,
 * must reconstruct contiguously with exactly the corrupt one still missing.
 * This is the reassembly invariant scaled down; it says nothing about the
 * real 767,006-chunk pass. */
static void test_rx_out_of_order_reassembly_window(void)
{
	static uint8_t f[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	static const uint32_t order[] = { 3, 0, 4, 1, 3, 2 };
	uint16_t n;
	size_t k;
	uint32_t seq, i;

	rx_reset(0);
	/* 5 chunks * 1400 = 7000 <= TEST_WIN_BYTES */
	assert(5u * SNAPNET_CHUNK <= TEST_WIN_BYTES);

	for (k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
		seq = order[k];
		/* chunk 2 arrives corrupt */
		n = build_frame(f, SNAPNET_MAGIC, seq, SNAPNET_CHUNK,
		                seq == 2u ? 0 : 1, (uint8_t)(0x10 * (seq + 1)));
		snapshot_net_rx_frame(f, n);
	}

	/* 0,1,3,4 present (3 sent twice, counted once); 2 missing. */
	assert(s_rx_count == 4);
	assert(bit_test(s_received, 0) && bit_test(s_received, 1));
	assert(!bit_test(s_received, 2));
	assert(bit_test(s_received, 3) && bit_test(s_received, 4));
	assert(t_writes_outside_window == 0);

	for (seq = 0; seq < 5u; seq++) {
		for (i = 0; i < SNAPNET_CHUNK; i++) {
			uint8_t got = t_win[seq * SNAPNET_CHUNK + i];
			if (seq == 2u)
				assert(got == 0);   /* pre-zeroed, still zero */
			else
				assert(got == (uint8_t)(0x10 * (seq + 1) + i));
		}
	}

	/* The resend of chunk 2, now intact, completes the window. */
	n = build_frame(f, SNAPNET_MAGIC, 2, SNAPNET_CHUNK, 1, 0x30);
	snapshot_net_rx_frame(f, n);
	assert(s_rx_count == 5 && bit_test(s_received, 2));
	for (i = 0; i < SNAPNET_CHUNK; i++)
		assert(t_win[2 * SNAPNET_CHUNK + i] == (uint8_t)(0x30 + i));
}

/* ==================================================================== *
 * TESTS — 4. chunk geometry / manifest arithmetic / bitmap helpers
 * ==================================================================== */

/* The whole store is covered exactly once: chunk lengths sum to STORE_LEN,
 * with only the final chunk short. Checked at the boundaries rather than by
 * iterating 767,006 times. */
static void test_chunk_geometry(void)
{
	uint32_t last = (uint32_t)SNAPNET_TOTAL_CHUNKS - 1u;

	assert(SNAPNET_CHUNK == 1400u);
	assert(SNAPNET_STORE_LEN == 0x40010000UL);
	assert(SNAPNET_TOTAL_CHUNKS == 767006u);
	assert(SNAPNET_MANIFEST_BYTES == 95876u);

	/* First and a middle chunk are full-size. */
	assert(expect_len_for(0) == SNAPNET_CHUNK);
	assert(expect_len_for(1) == SNAPNET_CHUNK);
	assert(expect_len_for(last / 2u) == SNAPNET_CHUNK);
	assert(expect_len_for(last - 1u) == SNAPNET_CHUNK);

	/* The last is 360. */
	assert(expect_len_for(last) == 360u);

	/* Exact tiling: (n-1) full chunks + the short tail == STORE_LEN, and
	 * the last chunk ends EXACTLY at STORE_LEN (the invariant
	 * snapshot_net.py:370-371 already checks on the host side). */
	assert((uint64_t)last * SNAPNET_CHUNK + 360u == SNAPNET_STORE_LEN);
	assert((uint64_t)last * SNAPNET_CHUNK + expect_len_for(last) == SNAPNET_STORE_LEN);

	/* One fewer chunk would NOT cover the store: pins the ceiling divide. */
	assert((uint64_t)SNAPNET_TOTAL_CHUNKS * SNAPNET_CHUNK >= SNAPNET_STORE_LEN);
	assert((uint64_t)(SNAPNET_TOTAL_CHUNKS - 1u) * SNAPNET_CHUNK < SNAPNET_STORE_LEN);

	/* Header + DRAM image geometry the store length is built from. */
	assert(SNAPNET_STORE_LEN == SNAP_META_SIZE + SNAP_DRAM_SIZE);
	/* Chunk 0 therefore carries the header (all 584 bytes of it, plus the
	 * first 816 bytes of metadata padding) — which is why chunk 0 can never
	 * legitimately be zero-skipped on a committed store. */
	assert(sizeof(struct snapshot_hdr) <= SNAPNET_CHUNK);
}

/* The manifest bitmap sizing, and bit_test/bit_set at exactly the indices
 * where an off-by-one hides: 0, 7 (last bit of byte 0), 8 (first bit of byte
 * 1), and TOTAL_CHUNKS-1 (the last valid bit, which must be inside the
 * allocation). Also records the two PADDING bits the ceiling divide creates. */
static void test_manifest_bitmap_helpers(void)
{
	static uint8_t bm[SNAPNET_MANIFEST_BYTES];
	uint32_t last = (uint32_t)SNAPNET_TOTAL_CHUNKS - 1u;

	memset(bm, 0, sizeof(bm));

	/* LSB-first within each byte (snapshot_net.c:121-125), which is also
	 * what snapshot_net.py:102-107 does — a mismatch here would silently
	 * transpose the manifest. */
	bit_set(bm, 0);
	assert(bm[0] == 0x01u);
	assert(bit_test(bm, 0) && !bit_test(bm, 1) && !bit_test(bm, 7));

	bit_set(bm, 7);
	assert(bm[0] == 0x81u);
	assert(bit_test(bm, 7) && !bit_test(bm, 8));

	bit_set(bm, 8);
	assert(bm[0] == 0x81u && bm[1] == 0x01u);
	assert(bit_test(bm, 8));

	/* The very last valid chunk index must be addressable inside the
	 * allocation: byte 95875, bit 5. */
	assert((last >> 3) == 95875u);
	assert((last & 7u) == 5u);
	assert((last >> 3) < SNAPNET_MANIFEST_BYTES);
	bit_set(bm, last);
	assert(bit_test(bm, last));
	assert(bm[SNAPNET_MANIFEST_BYTES - 1u] == 0x20u);

	/* The ceiling divide leaves 2 unused high bits in the final byte
	 * (95876*8 = 767008 vs 767006 chunks). Nothing in snapshot_net.c reads
	 * them (every loop is bounded by SNAPNET_TOTAL_CHUNKS: :361, :443,
	 * :475), so they are harmless — but they are also never cleared or
	 * validated, so a manifest arriving over netcon with those bits set is
	 * accepted verbatim. Pinned here as a known, benign slack. */
	assert(SNAPNET_MANIFEST_BYTES * 8u == SNAPNET_TOTAL_CHUNKS + 2u);
	assert(!bit_test(bm, (uint32_t)SNAPNET_TOTAL_CHUNKS));
	assert(!bit_test(bm, (uint32_t)SNAPNET_TOTAL_CHUNKS + 1u));

	/* Independence: setting one bit never disturbs a neighbour. */
	memset(bm, 0, sizeof(bm));
	bit_set(bm, 100);
	assert(bit_test(bm, 100) && !bit_test(bm, 99) && !bit_test(bm, 101));
}

/* The missing-list control message the two ends exchange
 * (snapshot_net.c:443-462 / snapshot_net.py:238-246): exactly
 * MISSING_CAP*4 bytes, real entries first, the rest SNAPNET_SENTINEL, and
 * the reader stops at the first sentinel (snapshot_net.c:398-399). Modelled
 * over the real cap, which is only 32 KiB. */
static void test_missing_list_message_shape(void)
{
	static uint8_t ctrl[SNAPNET_MISSING_CAP * 4u];
	static const uint32_t missing[] = { 0, 1, 7, 8, 4200, 767005 };
	uint32_t count = (uint32_t)(sizeof(missing) / sizeof(missing[0]));
	uint32_t i, seen;

	assert(SNAPNET_MISSING_CAP == 8192u);
	assert(sizeof(ctrl) == 32768u);
	assert(SNAPNET_SENTINEL == 0xFFFFFFFFu);
	/* The sentinel must not collide with any legal chunk index. */
	assert(SNAPNET_SENTINEL >= SNAPNET_TOTAL_CHUNKS);
	/* One control message must be able to carry the whole manifest's worth
	 * of misses in at most MAX_ROUNDS rounds... which it CANNOT: 5*8192 =
	 * 40,960 < 767,006. Pinned as a documented bound, not a bug: a transfer
	 * losing more than 40,960 chunks fails with -3 by design
	 * (snapshot_net.c:477). */
	assert((uint64_t)SNAPNET_MAX_ROUNDS * SNAPNET_MISSING_CAP < SNAPNET_TOTAL_CHUNKS);

	for (i = 0; i < count; i++)
		wr32(ctrl + i * 4u, missing[i]);
	for (i = count; i < SNAPNET_MISSING_CAP; i++)
		wr32(ctrl + i * 4u, SNAPNET_SENTINEL);

	/* Reader side (snapshot_net.c:396-404): stop at the first sentinel. */
	seen = 0;
	for (i = 0; i < SNAPNET_MISSING_CAP; i++) {
		uint32_t mseq = rd32(ctrl + i * 4u);
		if (mseq == SNAPNET_SENTINEL)
			break;
		assert(mseq == missing[seen]);
		assert(mseq < SNAPNET_TOTAL_CHUNKS);
		seen++;
	}
	assert(seen == count);
	/* And that everything after the stop really is padding. */
	for (i = count; i < SNAPNET_MISSING_CAP; i++)
		assert(rd32(ctrl + i * 4u) == SNAPNET_SENTINEL);
}

/* chunk_is_zero() (snapshot_net.c:198-209) is the zero-skip predicate; a
 * false positive silently drops real data (the receiver's pre-zeroed store
 * keeps the chunk at zero and the manifest never asks for it). Exercised at
 * both the word-loop and byte-tail boundaries, including the 360-byte last
 * chunk (360 % 8 == 0, so its tail loop is empty — pinned) and a
 * deliberately non-multiple-of-8 length so the tail loop is not dead. */
static void test_chunk_is_zero_predicate(void)
{
	static uint8_t buf[SNAPNET_CHUNK];
	uint32_t i;

	memset(buf, 0, sizeof(buf));
	assert(chunk_is_zero(buf, SNAPNET_CHUNK));
	assert(chunk_is_zero(buf, 360u));
	assert(chunk_is_zero(buf, 0u));

	/* A single non-zero byte anywhere must be detected — first, last, and
	 * every byte position within one word. */
	for (i = 0; i < 8u; i++) {
		memset(buf, 0, sizeof(buf));
		buf[i] = 1u;
		assert(!chunk_is_zero(buf, SNAPNET_CHUNK));
	}
	memset(buf, 0, sizeof(buf));
	buf[SNAPNET_CHUNK - 1u] = 0x80u;
	assert(!chunk_is_zero(buf, SNAPNET_CHUNK));
	memset(buf, 0, sizeof(buf));
	buf[359] = 0x01u;
	assert(!chunk_is_zero(buf, 360u));

	/* The last chunk length is a multiple of 8, so the byte tail loop never
	 * runs for any real chunk. Exercise it anyway at a length that does. */
	assert(360u % 8u == 0u);
	assert(SNAPNET_CHUNK % 8u == 0u);
	memset(buf, 0, sizeof(buf));
	buf[12] = 0xFFu;
	assert(!chunk_is_zero(buf, 13u));    /* found by the tail loop */
	assert(chunk_is_zero(buf, 12u));     /* just short of it */
}

/* ==================================================================== *
 * TESTS — 5. OPEN ISSUES: measured and pinned, deliberately NOT fixed
 * ==================================================================== */

/* OPEN ISSUE (a) — snapshot_present() trusts the store's own geometry.
 *
 * snapshot_present() (snapshot.c:282-287) checks magic, valid and version.
 * It does NOT compare h->dram_base / h->dram_size / h->dram_store against
 * the compile-time SNAP_DRAM_BASE / SNAP_DRAM_SIZE / SNAP_DRAM_STORE, even
 * though snapshot_save() (snapshot.c:309-311) is the only thing that ever
 * writes them and always writes exactly those constants. snapshot_restore()
 * then copies h->dram_size bytes (snapshot.c:375), so a stale, torn or
 * hostile header steers the length of a 1 GiB memcpy.
 *
 * The exposure is real, not theoretical, because snapshot_net_recv()
 * reconstructs the header FROM THE WIRE (chunk 0) before calling
 * snapshot_restore() (snapshot_net.c:491) — and it is worse there:
 * snapshot_net.c:486 reads crc32_region(h->dram_store, h->dram_size), i.e.
 * a wire-supplied BASE as well as a wire-supplied LENGTH.
 *
 * This test does not fix any of that. It pins today's behaviour and computes
 * exactly how far past the store the copy runs. */
static void test_open_issue_present_accepts_absurd_geometry(void)
{
	struct snapshot_hdr h;
	uint64_t src_end, dst_end, store_end;

	memset(&h, 0, sizeof(h));
	h.magic = SNAP_MAGIC;
	h.version = SNAPSHOT_VERSION;
	h.valid = 1u;
	/* Deliberately absurd/hostile geometry. */
	h.dram_base  = 0;
	h.dram_size  = 0xFFFFFFFFFFFFFFFFull;
	h.dram_store = 0;

	/* snapshot_present() says yes anyway. THIS IS THE ISSUE. */
	assert(snapshot_present_at(&h) == 1);

	/* A merely-slightly-wrong size is accepted too, and that is the case a
	 * torn header actually produces. Compute the overrun snapshot.c:375
	 * would then perform: dram_copy(SNAP_DRAM_BASE, SNAP_DRAM_STORE,
	 * h->dram_size, NULL). */
	h.dram_size = SNAP_DRAM_SIZE + 0x1000UL;
	assert(snapshot_present_at(&h) == 1);

	store_end = SNAP_STORE_BASE + SNAP_META_SIZE + SNAP_DRAM_SIZE;
	src_end   = SNAP_DRAM_STORE + h.dram_size;
	dst_end   = SNAP_DRAM_BASE  + h.dram_size;

	/* The SOURCE read runs 0x1000 bytes past the end of the store. */
	assert(src_end > store_end);
	assert(src_end - store_end == 0x1000UL);

	/* Worse, the DESTINATION write runs past the end of guest DRAM — and
	 * guest DRAM ends exactly AT SNAP_STORE_BASE (snapshot.h:78-83), so the
	 * overrun lands on the store header itself, destroying the very record
	 * being restored from, mid-restore. */
	assert(dst_end > SNAP_STORE_BASE);
	assert(SNAP_DRAM_BASE + SNAP_DRAM_SIZE == SNAP_STORE_BASE);
	assert(dst_end - SNAP_STORE_BASE == 0x1000UL);

	/* For contrast: the check that is missing. If snapshot_present() (or
	 * snapshot_restore()) compared against the compile-time constants, both
	 * headers above would be rejected. */
	assert(!(h.dram_base == SNAP_DRAM_BASE &&
	         h.dram_size == SNAP_DRAM_SIZE &&
	         h.dram_store == SNAP_DRAM_STORE));

	/* A header written by the real snapshot_save() does satisfy it, so the
	 * missing check would cost nothing on the happy path. */
	h.dram_base = SNAP_DRAM_BASE;
	h.dram_size = SNAP_DRAM_SIZE;
	h.dram_store = SNAP_DRAM_STORE;
	assert(snapshot_present_at(&h) == 1);
	assert(h.dram_base == SNAP_DRAM_BASE && h.dram_size == SNAP_DRAM_SIZE &&
	       h.dram_store == SNAP_DRAM_STORE);

	/* The three checks that ARE performed do work — pin them so the issue
	 * above is understood as "incomplete", not "absent". */
	h.magic = SNAP_MAGIC + 1u;  assert(snapshot_present_at(&h) == 0);
	h.magic = SNAP_MAGIC;
	h.valid = 0u;               assert(snapshot_present_at(&h) == 0);
	h.valid = 2u;               assert(snapshot_present_at(&h) == 0);  /* == 1, not != 0 */
	h.valid = 1u;
	h.version = SNAPSHOT_VERSION + 1u; assert(snapshot_present_at(&h) == 0);
}

/* OPEN ISSUE (b) — snapshot_hdr.crc32 does not cover the header.
 *
 * snapshot.c:331-334 computes the CRC over the DRAM image only, folded into
 * dram_copy()'s source pass. Nothing anywhere computes or checks a CRC over
 * the header. snapshot_net_recv()'s "belt-and-suspenders" recheck
 * (snapshot_net.c:486) compares a CRC of the DRAM image against
 * h->crc32 — so it validates the image using a number that lives in the
 * unvalidated part of the record. A garbled header therefore passes every
 * integrity check the feature has: the frame, the sysregs, the timer base
 * and the geometry are all unprotected.
 *
 * Pinned by showing that mutating header fields does not change the computed
 * CRC, and that a CRC which DID cover the header would have caught them. */
static void test_open_issue_header_is_not_crc_covered(void)
{
	struct snapshot_hdr h;
	uint8_t image[2048];             /* stands in for the DRAM image */
	uint32_t crc_before, crc_after;
	uint32_t hdr_crc_before, hdr_crc_after;
	size_t i;

	for (i = 0; i < sizeof(image); i++)
		image[i] = (uint8_t)(i * 17u + 3u);

	memset(&h, 0, sizeof(h));
	h.magic = SNAP_MAGIC;
	h.version = SNAPSHOT_VERSION;
	h.valid = 1u;
	h.dram_base = SNAP_DRAM_BASE;
	h.dram_size = SNAP_DRAM_SIZE;
	h.dram_store = SNAP_DRAM_STORE;
	h.taken_cntpct = 0x1111222233334444ull;
	h.frame.elr = 0xFFFF000041000000ull;
	h.sysregs.sctlr_el1 = 0x30D0180Dull;

	/* This is exactly what snapshot.c:331-334 stores: a CRC of the image,
	 * nothing else. */
	h.crc32 = crc32_wordwise(image, sizeof(image));
	crc_before = h.crc32;

	/* A CRC that DID cover the header, for contrast (not in the source). */
	hdr_crc_before = crc32_calc((const uint8_t *)&h, (uint32_t)offsetof(struct snapshot_hdr, crc32));

	/* Now garble the header in four separate, individually-catastrophic
	 * ways — the guest PC, the MMU control register, the timer base and the
	 * DRAM geometry. */
	h.frame.elr        = 0xDEADBEEFDEADBEEFull;   /* guest resumes at garbage */
	h.sysregs.sctlr_el1 = 0;                       /* guest MMU/caches off    */
	h.taken_cntpct     = 0;                        /* timer re-base explodes  */
	h.dram_size        = SNAP_DRAM_SIZE * 2u;      /* see open issue (a)      */

	/* The stored CRC is unchanged, and recomputing it the way
	 * snapshot_net.c:486 does still matches: the corruption is INVISIBLE. */
	crc_after = crc32_wordwise(image, sizeof(image));
	assert(crc_after == crc_before);
	assert(h.crc32 == crc_after);
	assert(crc32_calc(image, sizeof(image)) == h.crc32);   /* the real recheck */

	/* snapshot_present() is equally blind to all of it. */
	assert(snapshot_present_at(&h) == 1);

	/* A header-covering CRC would have caught every one of those edits. */
	hdr_crc_after = crc32_calc((const uint8_t *)&h, (uint32_t)offsetof(struct snapshot_hdr, crc32));
	assert(hdr_crc_after != hdr_crc_before);
}

/* OPEN ISSUE (b'), same family — the coherent-store convention the header
 * comment promises is only applied to the first four fields.
 *
 * snapshot.h:165-168 states that "the whole header is written with the
 * dc-civac + dsb-sy coherent-store convention so a warm WDT reset (DRAM
 * survives) does not lose it and a host `bc`/`d` dump of 0x80000000 shows a
 * valid record". In snapshot.c, snap_wr32() (the dc-civac writer, :42-46) is
 * used for exactly four fields — valid/magic/version/store_kind (:302-305)
 * and valid again (:341). dram_base..taken_cntpct (:309-312), frame (:316),
 * sysregs (:320) and crc32 (:334) are PLAIN stores followed only by `dsb sy`
 * (:322, :340), which orders writes but does not clean a write-back D-cache.
 *
 * `dc civac` acts on the whole 64-byte line, so bytes 0..63 are cleaned as a
 * side effect of the valid/magic writes. Everything from offset 64 up is
 * not. Pin that boundary arithmetically — it is the mechanical part of the
 * claim; whether it actually matters on the board (it should, for the
 * warm-reset and host-dump cases the comment names) is a hardware question
 * this test cannot settle. */
static void test_open_issue_only_first_cacheline_is_explicitly_cleaned(void)
{
	const size_t line = 64;

	/* The four snap_wr32()'d fields all live in line 0. */
	assert(offsetof(struct snapshot_hdr, magic)      + 4 <= line);
	assert(offsetof(struct snapshot_hdr, version)    + 4 <= line);
	assert(offsetof(struct snapshot_hdr, valid)      + 4 <= line);
	assert(offsetof(struct snapshot_hdr, store_kind) + 4 <= line);

	/* So do the four plain-stored 64-bit metadata fields — cleaned only by
	 * luck, because they share line 0. */
	assert(offsetof(struct snapshot_hdr, taken_cntpct) + 8 <= line);

	/* frame straddles the boundary: x[0] and x[1] are inside line 0, x[2]
	 * onwards are not. */
	assert(offsetof(struct snapshot_hdr, frame) == 48);
	assert(offsetof(struct snapshot_hdr, frame) + 16 == line);

	/* And these are entirely outside any dc-civac'd line: the guest PC and
	 * PSTATE, the whole sysreg set, and the integrity CRC itself. */
	assert(offsetof(struct snapshot_hdr, frame) + offsetof(struct el2_frame, elr) >= line);
	assert(offsetof(struct snapshot_hdr, frame) + offsetof(struct el2_frame, spsr) >= line);
	assert(offsetof(struct snapshot_hdr, sysregs) >= line);
	assert(offsetof(struct snapshot_hdr, crc32) >= line);

	/* How much of the header is left uncleaned. */
	assert(sizeof(struct snapshot_hdr) - line == 520);
}

/* OPEN ISSUE (c) — the snapshotted "guest DRAM" range contains the
 * hypervisor itself.
 *
 * SNAP_DRAM_BASE..+SNAP_DRAM_SIZE is 0x40000000..0x80000000 (snapshot.h:72-73,
 * == stage2.h:87-88's STAGE2_DRAM_BASE/SIZE). link.ld:12 places the
 * hypervisor image at ORIGIN = 0x42000000, LENGTH = 8M, with a build-time
 * ASSERT (link.ld, tail) that text+rodata+data+bss stays inside the 2 MiB
 * stage-2-protected window at 0x42000000 — and the HV's own run-time stack is
 * smp_stacks[0], a .bss array (see start.S:30-39), i.e. inside that window.
 *
 * Consequences, none of which snapshot.h's "WHAT IS (AND IS NOT) CAPTURED"
 * inventory (snapshot.h:26-50) mentions:
 *   - snapshot_save() copies the hypervisor's own .text/.data/.bss/stack into
 *     the store (harmless in itself: it is only a read).
 *   - snapshot_restore() (snapshot.c:375) writes 1 GiB back over that range
 *     *while executing out of it*, overwriting its own code, data and live
 *     call stack with the save-time bytes. .text/.rodata would be identical
 *     for an identical build, but .bss/.data/stack are not: the dram_copy()
 *     frame's own return address is in the range being overwritten.
 *   - The HV's fixed breadcrumb/ring windows at 0x5000xxxx (vconsole ring,
 *     flightrec, stage-2 AT results, ...) are in the range too, so a restore
 *     also rolls those back.
 *
 * This test only pins the containment arithmetic — that is the mechanical,
 * host-checkable part. Whether a restore actually survives it is a board
 * question, and the answer is very likely "no". */
static void test_open_issue_snapshot_range_contains_hv_image(void)
{
	/* Mirrored from link.ld:12/17 and its tail ASSERT. */
	const uint64_t hv_base = 0x42000000UL;
	const uint64_t hv_region_len = 8UL * 1024UL * 1024UL;   /* link.ld LENGTH */
	const uint64_t hv_protected_len = 0x200000UL;           /* link.ld ASSERT */
	/* One of the HV's fixed DRAM breadcrumb windows (main_qemu.c:33 cites
	 * 0x50000c1c; vconsole's ring is at 0x50000f10 per ORIENTATION.md). */
	const uint64_t hv_bc = 0x50000000UL;

	const uint64_t snap_lo = SNAP_DRAM_BASE;
	const uint64_t snap_hi = SNAP_DRAM_BASE + SNAP_DRAM_SIZE;

	/* The HV image — including the 2 MiB window holding its stack — is
	 * wholly inside the region snapshot_save() copies and
	 * snapshot_restore() overwrites. */
	assert(hv_base >= snap_lo);
	assert(hv_base + hv_protected_len <= snap_hi);
	assert(hv_base + hv_region_len <= snap_hi);

	/* So are the HV's breadcrumb windows. */
	assert(hv_bc >= snap_lo && hv_bc < snap_hi);

	/* And the store itself is safely OUTSIDE that range — the one part the
	 * design did get right (snapshot.h:78-83). */
	assert(SNAP_STORE_BASE >= snap_hi);

	/* Offset of the HV image within the store's DRAM copy, i.e. where these
	 * bytes actually land — useful for anyone inspecting a pulled image. */
	assert(SNAP_DRAM_STORE + (hv_base - SNAP_DRAM_BASE) == 0x82010000UL);
}

/* ==================================================================== *
 * main()
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	/* 1. the two CRC32 implementations */
	{ "crc32_bitwise_restatement_is_faithful",      test_crc32_bitwise_restatement_is_faithful },
	{ "crc32_standard_check_value",                 test_crc32_standard_check_value },
	{ "crc32_two_implementations_agree",            test_crc32_two_implementations_agree },
	{ "crc32_agreement_is_little_endian_dependent", test_crc32_agreement_is_little_endian_dependent },
	/* 2. header ABI */
	{ "header_magic_and_version_constants",         test_header_magic_and_version_constants },
	{ "header_byte_image_round_trip",               test_header_byte_image_round_trip },
	/* 3. snapshot_net_rx_frame() */
	{ "rx_accepts_valid_frame",                     test_rx_accepts_valid_frame },
	{ "rx_offset_is_seq_times_chunk",               test_rx_offset_is_seq_times_chunk },
	{ "rx_rejects_short_frame",                     test_rx_rejects_short_frame },
	{ "rx_rejects_bad_magic",                       test_rx_rejects_bad_magic },
	{ "rx_drops_everything_while_idle",             test_rx_drops_everything_while_idle },
	{ "rx_rejects_out_of_range_seq",                test_rx_rejects_out_of_range_seq },
	{ "rx_rejects_wrong_length_for_seq",            test_rx_rejects_wrong_length_for_seq },
	{ "rx_rejects_truncated_payload_but_allows_trailing",
	                                                test_rx_rejects_truncated_payload_but_allows_trailing },
	{ "rx_rejects_bad_payload_crc",                 test_rx_rejects_bad_payload_crc },
	{ "rx_duplicate_is_idempotent_and_does_not_bump_count",
	                                                test_rx_duplicate_is_idempotent_and_does_not_bump_count },
	{ "rx_rejections_leave_state_untouched",        test_rx_rejections_leave_state_untouched },
	{ "rx_out_of_order_reassembly_window",          test_rx_out_of_order_reassembly_window },
	/* 4. geometry / manifest */
	{ "chunk_geometry",                             test_chunk_geometry },
	{ "manifest_bitmap_helpers",                    test_manifest_bitmap_helpers },
	{ "missing_list_message_shape",                 test_missing_list_message_shape },
	{ "chunk_is_zero_predicate",                    test_chunk_is_zero_predicate },
	/* 5. open issues, pinned not fixed */
	{ "open_issue_present_accepts_absurd_geometry", test_open_issue_present_accepts_absurd_geometry },
	{ "open_issue_header_is_not_crc_covered",       test_open_issue_header_is_not_crc_covered },
	{ "open_issue_only_first_cacheline_is_explicitly_cleaned",
	                                                test_open_issue_only_first_cacheline_is_explicitly_cleaned },
	{ "open_issue_snapshot_range_contains_hv_image", test_open_issue_snapshot_range_contains_hv_image },
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

	printf("---- snapshot format/framing tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
