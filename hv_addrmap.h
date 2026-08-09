/* SPDX-License-Identifier: BSD-2-Clause */

/* hv_addrmap.h — single authoritative map of the fixed DRAM "hv-scratch"
 * window (DTB-reserved hv-scratch@0x50000000), the cross-core-coherent,
 * warm-reset-survivable storage the hypervisor uses for breadcrumbs, lock
 * words, and small test buffers.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * These addresses used to be hard-coded literals scattered across a dozen .c
 * files, each with a local comment claiming its neighbours. On 2026-07-24 that
 * caught up with us: VBLK_USED_LOCK_PA was placed at 0x50020200, which already
 * belonged to emmc_bio.c's EBIO failure-diagnostics window — whose word[0] is
 * the read-failure counter. The first eMMC read error stamped a nonzero value
 * into the lock word, wedging the used-ring lock permanently "held" and
 * reopening the exact CPU0-vs-CPU2 race the lock was added to close, precisely
 * when I/O errors start. A comment ("clear of every breadcrumb slot") can be
 * wrong and nobody notices; a _Static_assert cannot. This header turns the
 * densest, most collision-prone sub-block (the 0x50020000 I/O-storage page)
 * into compile-time-checked non-overlapping allocations.
 *
 * FULL hv-scratch MAP (0x50000000 page-block; addresses documented here are
 * still owned by their subsystems' own files — only the 0x50020000 I/O block
 * below is centralised so far; migrating the rest is future work):
 *   0x50000000..0x50000eff  early boot / GIC / misc breadcrumbs (GICT 0x800…)
 *   0x50000f00..0x50000f1f  vconsole ring HEADER only ("UART" magic, counters,
 *                           and from layout v2 the buffer's base+size)
 *   0x50040000..0x5004ffff  vconsole 64 KiB capture BUFFER (moved here
 *                           2026-08-01; while it followed the header it ran to
 *                           0x50010f0f and buried the vgic/vgic-self/gtrace/
 *                           first-fault/single-step/HDMI windows listed below —
 *                           a deliberate temporary trade whose reason expired)
 *   0x50011000..0x50011fff  netcon / snapshot_net staging
 *   0x50012000..            flightrec "FLTR" ring
 *   0x50001c00              vgic breadcrumb window ("VGIC")
 *   0x50001d00              vgic self-test guest ("VGST")
 *   0x50006000              software-BMC block
 *   0x50020000..0x50020fff  virtio-blk / eMMC / SD I/O storage — MAPPED BELOW
 *   0x50060000..0x5006ffff  snapshot/restore metadata header — MAPPED BELOW
 *
 * If you need a new fixed word, add it to the block below (or a new documented
 * lane) so the _Static_assert chain proves it doesn't alias anything. Never
 * hand-pick a literal in a .c file again.
 */
#ifndef HV_ADDRMAP_H
#define HV_ADDRMAP_H

#include <stdint.h>

/* ---- 0x50020000 I/O-storage block (virtio-blk / eMMC / SD) --------------
 * Every region below is [BASE, BASE+SIZE); the assert chain at the bottom
 * proves they are strictly ordered and therefore never overlap. Sizes are
 * generous reservations, not the currently-used extent, so a lane can grow a
 * few words without a reshuffle. */

/* virtio-blk vblk_bc() breadcrumbs (vblk_emmc.c: idx 0..~26 used). */
#define HVMAP_VBLK_BC        0x50020000UL
#define HVMAP_VBLK_BC_SIZE   0x100UL

/* eMMC controller lock word (vblk_emmc.c: CPU0 sync vs CPU1 debug-core). */
#define HVMAP_EMMC_LOCK      0x50020100UL
#define HVMAP_EMMC_LOCK_SIZE 0x4UL

/* eMMC failure-diagnostics breadcrumbs (emmc_bio.c: 8 words, idx 0..7). */
#define HVMAP_EBIO_BC        0x50020200UL
#define HVMAP_EBIO_BC_SIZE   0x20UL

/* eMMC high-speed probe test buffer (emmc_bio.c: one 512-byte sector). */
#define HVMAP_EMMC_HS_TESTBUF      0x50020300UL
#define HVMAP_EMMC_HS_TESTBUF_SIZE 0x200UL

/* SD-card driver breadcrumbs (sd_bio.c: idx 0..~9). */
#define HVMAP_SD_BC          0x50020500UL
#define HVMAP_SD_BC_SIZE     0x28UL

/* CPU2 async-I/O offload breadcrumbs (vblk_async.c: "VBA1", idx 0..2). */
#define HVMAP_ASYNC_BC       0x50020600UL
#define HVMAP_ASYNC_BC_SIZE  0xCUL

/* Used-ring publish lock word (vblk_emmc.c: CPU0 sync vs CPU2 async). MOVED
 * here from 0x50020200 — see the header comment above for the alias bug. */
#define HVMAP_USED_LOCK      0x50020700UL
#define HVMAP_USED_LOCK_SIZE 0x4UL

/* End of the reserved I/O-storage block (one 4 KiB page). */
#define HVMAP_IO_BLOCK_END   0x50021000UL

/* ---- 0x50021000 dbgtools lane (CPU1 heartbeat / build-id / entry-hold) --
 * Added 2026-07-26 after a live RSP-debugging session where CPU1/gdbstub
 * appeared "wedged" (EMAC frames sent, zero replies, tcpdump confirmed TX)
 * with no way to tell "CPU1 spinning but not answering THIS channel" from
 * "CPU1 actually stopped", and separately where hvdbg.py's wdt_reset()
 * resolved reboot_clean()'s address via `nm` against the microkernel-dbg.elf
 * *file on disk* while the board was still running an OLDER build — the
 * call silently jumped to garbage. See dbgtools.h for the full design.
 *
 * Deliberately placed HERE — immediately after the already-centralized I/O
 * block and well before vnet_emac's window (VNET_BC_BASE 0x50030000) —
 * rather than in the nominal "0x50000000..0x50000eff early boot / misc"
 * region this header's own map comment (above) still calls ad-hoc-owned.
 * A full-tree grep (`grep -rohE "0x50[0-9a-f]{6}"`) done while adding this
 * lane found that region is WORSE than the map comment admits: it already
 * has real, undocumented aliasing —
 *   - ring.c's RING_BC_BASE and hwbp.c's HWBP_BC_BASE both claim 0x50000600
 *     (hwbp.c's own "distinct from" list doesn't even mention RING);
 *   - backtrace.c's BTR1 (0x50000700) documents its own alias with alloc.c's
 *     ALLC_BC_BASE, and gic_timer.c's GICT (0x50000800) overlaps BTR1's tail
 *     (BTR1 runs to 0x50000820);
 *   - main_dbg.c/main_fbsd.c/main_gdb.c's shared "DBG1"/"FBS1"/"GDB1" bracket
 *     window, backtrace.c's separate "BTS1" window, and vconsole.c's host->
 *     guest RX ring (VC_RX_HEAD/TAIL/buf) all pack into the same
 *     0x50000e00-0x50000eff slot.
 * None of that is this change's bug to fix (out of scope — see the task that
 * added this lane), but it is reason enough not to add a fourth or fifth
 * contender into an already-contested block. 0x50021000 is confirmed clear
 * by the same grep (nothing else in the tree references it or anything
 * between it and 0x50030000).
 *
 * Word layout (uint32_t unless noted), all cache-coherent-store (dc civac +
 * dsb) writes, matching the tree's universal breadcrumb convention:
 *   [0] (+0x00) magic HVMAP_DBGTOOLS_MAGIC ("DBGT") — also the cold/warm
 *       reset detector: dbgtools_init() treats "magic already present" as
 *       proof this is a WARM reset (DRAM retained our own marker) and
 *       leaves HOLD/RELEASE untouched; otherwise (magic absent — a true
 *       cold TFTP boot, or first boot ever) it zeros both. See dbgtools.c.
 *   [1] (+0x04) HEARTBEAT — bumped UNCONDITIONALLY every iteration of
 *       smp_secondary_main()'s CPU1 debug-core for(;;) loop (smp.c),
 *       regardless of which branch (dbgmon/gdbstub/idle) that iteration
 *       takes. Answerable via the raw ETHERTYPE_DBGRAW EMAC peek (dbgtools.c
 *       / emac.c) which is serviced directly inside emac_poll() — i.e.
 *       independent of whether dbgmon_service() or gdbstub_poll() is the
 *       active branch. See dbgtools.h for the one case this does not cover.
 *   [2] (+0x08) HOLD — host-armed "pause before guest entry" gate, checked
 *       by main_dbg.c/main_gdb.c immediately before kload_enter(). Fixed
 *       address (not a linked symbol) so a host tool can set it without
 *       nm-resolving a possibly-stale ELF.
 *   [3] (+0x0c) RELEASE — set nonzero to let a held CPU0 proceed into
 *       kload_enter(). See main_dbg.c's comment for the exact loop and
 *       dbgtools.c's for the cold-vs-warm-reset caveat: this mechanism only
 *       works across a WARM wdt_reset()-style reload (DRAM survives), NOT a
 *       fresh cold TFTP load (nothing runs early enough to have armed it).
 *   [4..11] (+0x10..0x2f) BUILD_ID — 32-byte zero-padded ASCII string,
 *       written once at HV startup from -DBZDOS_BUILD_ID (Makefile: `git
 *       rev-parse --short=12 HEAD` [+ a trailing '+' if the tree is dirty],
 *       falling back to a UTC date-time stamp when git is unavailable).
 */
#define HVMAP_DBGTOOLS_BASE         0x50021000UL
#define HVMAP_DBGTOOLS_MAGIC        0x44424754UL   /* "DBGT" */
#define HVMAP_DBGTOOLS_HEARTBEAT    (HVMAP_DBGTOOLS_BASE + 0x04UL)
#define HVMAP_DBGTOOLS_HOLD         (HVMAP_DBGTOOLS_BASE + 0x08UL)
#define HVMAP_DBGTOOLS_RELEASE      (HVMAP_DBGTOOLS_BASE + 0x0cUL)
#define HVMAP_DBGTOOLS_BUILDID      (HVMAP_DBGTOOLS_BASE + 0x10UL)
#define HVMAP_DBGTOOLS_BUILDID_SIZE 0x20UL           /* 32 bytes */
#define HVMAP_DBGTOOLS_SIZE         0x40UL            /* generous, room to grow */
#define HVMAP_DBGTOOLS_END          (HVMAP_DBGTOOLS_BASE + HVMAP_DBGTOOLS_SIZE)

/* ---- Compile-time non-overlap proof (address-ordered chain) ------------- */
_Static_assert(HVMAP_VBLK_BC + HVMAP_VBLK_BC_SIZE <= HVMAP_EMMC_LOCK,
               "vblk breadcrumbs overlap the eMMC lock word");
_Static_assert(HVMAP_EMMC_LOCK + HVMAP_EMMC_LOCK_SIZE <= HVMAP_EBIO_BC,
               "eMMC lock overlaps the EBIO breadcrumbs");
_Static_assert(HVMAP_EBIO_BC + HVMAP_EBIO_BC_SIZE <= HVMAP_EMMC_HS_TESTBUF,
               "EBIO breadcrumbs overlap the HS test buffer");
_Static_assert(HVMAP_EMMC_HS_TESTBUF + HVMAP_EMMC_HS_TESTBUF_SIZE <= HVMAP_SD_BC,
               "HS test buffer overlaps the SD breadcrumbs");
_Static_assert(HVMAP_SD_BC + HVMAP_SD_BC_SIZE <= HVMAP_ASYNC_BC,
               "SD breadcrumbs overlap the async breadcrumbs");
_Static_assert(HVMAP_ASYNC_BC + HVMAP_ASYNC_BC_SIZE <= HVMAP_USED_LOCK,
               "async breadcrumbs overlap the used-ring lock word");
_Static_assert(HVMAP_USED_LOCK + HVMAP_USED_LOCK_SIZE <= HVMAP_IO_BLOCK_END,
               "used-ring lock overruns the I/O-storage block");
_Static_assert(HVMAP_IO_BLOCK_END <= HVMAP_DBGTOOLS_BASE,
               "dbgtools lane overlaps the I/O-storage block");
_Static_assert(HVMAP_DBGTOOLS_END <= 0x50030000UL,
               "dbgtools lane overruns vnet_emac's window (VNET_BC_BASE)");

/* ---- LOW-BLOCK non-overlap proof (added 2026-08-01) --------------------
 * The chain above only ever covered 0x50020000.., which is why it did not
 * catch the failure that motivated this: vconsole's capture buffer was
 * defined as RING_BASE + HDR_SIZE and later grown 3 KiB -> 64 KiB, so it
 * silently swallowed SIX windows below 0x50020000 (vgic, vgic-self-test,
 * gtrace, the first-fault latch, the single-step ring, HDMI). Read live,
 * those windows held console text instead of their magics — i.e. six
 * subsystems could not report anything, and project memory holds a RETRACTED
 * theory built on readings from one of them.
 *
 * Two things made it invisible for months: the buffer's address was DERIVED
 * from its neighbour (grow the header and it marches into whoever is next),
 * and a comment claiming "vconsole ends 0x50001b10" stayed true only for the
 * 3 KiB version. Comments cannot be checked; these can.
 *
 * These mirror addresses owned by other headers (vconsole.h, vgic.c,
 * gtrace.h, el2_exc.c, hud.c). Mirroring is the same compromise the rest of
 * this file already makes and is noted in bzd_board.py too: if one of those
 * moves, update it here. That is strictly better than the previous state,
 * where nothing checked them at all. */
#define HVMAP_LOW_VCONSOLE_HDR   0x50000f00UL
#define HVMAP_LOW_VCONSOLE_HDR_SZ 0x20UL      /* header only, v2: 8 words */
#define HVMAP_LOW_VGIC_BC        0x50001c00UL
#define HVMAP_LOW_VGST_BC        0x50001d00UL
#define HVMAP_LOW_GTRACE         0x50002000UL
#define HVMAP_LOW_GTRACE_END     0x50002210UL
#define HVMAP_LOW_FFL1           0x50002400UL
#define HVMAP_LOW_FFL1_END       0x50002530UL
#define HVMAP_LOW_SST1           0x50002800UL
#define HVMAP_LOW_HDMI_BC        0x50003000UL
/* vconsole capture BUFFER — an explicit absolute base since 2026-08-01,
 * deliberately NOT derived from the header, and placed above every other
 * window so growing it can never reach a neighbour again. */
#define HVMAP_LOW_VCONSOLE_BUF   0x50040000UL
#define HVMAP_LOW_VCONSOLE_BUF_SZ 0x10000UL

_Static_assert(HVMAP_LOW_VCONSOLE_HDR + HVMAP_LOW_VCONSOLE_HDR_SZ
               <= HVMAP_LOW_VGIC_BC,
               "vconsole ring header overlaps the vgic breadcrumb window");
_Static_assert(HVMAP_LOW_VGIC_BC + 0x100UL <= HVMAP_LOW_VGST_BC,
               "vgic breadcrumbs overlap the vgic self-test window");
_Static_assert(HVMAP_LOW_VGST_BC + 0x100UL <= HVMAP_LOW_GTRACE,
               "vgic self-test window overlaps the gtrace ring");
_Static_assert(HVMAP_LOW_GTRACE_END <= HVMAP_LOW_FFL1,
               "gtrace ring overlaps the first-fault latch");
_Static_assert(HVMAP_LOW_FFL1_END <= HVMAP_LOW_SST1,
               "first-fault latch overlaps the single-step ring");
_Static_assert(HVMAP_LOW_SST1 + 0x400UL <= HVMAP_LOW_HDMI_BC,
               "single-step ring overlaps the HDMI breadcrumb window");
/* THE ONE THAT WOULD HAVE CAUGHT IT: the 64 KiB capture buffer must start
 * above every window above, not run through them. */
_Static_assert(HVMAP_LOW_VCONSOLE_BUF >= HVMAP_LOW_HDMI_BC + 0x1000UL,
               "vconsole capture buffer starts inside the low breadcrumb block");
_Static_assert(HVMAP_LOW_VCONSOLE_BUF >= HVMAP_DBGTOOLS_END,
               "vconsole capture buffer collides with the dbgtools lane");
_Static_assert(HVMAP_LOW_VCONSOLE_BUF + HVMAP_LOW_VCONSOLE_BUF_SZ
               <= 0x50200000UL,
               "vconsole capture buffer runs past the DTB hv-scratch reserve");

/* ---- Snapshot/restore (ROADMAP D1) metadata header ----------------------
 * Added 2026-08 fixing a real overrun found by actually running the feature
 * for the first time (snapshot-qemu exercise): snapshot.h's high-GiB DRAM
 * store window ([0x80000000,0xC0000000), SNAP_DRAM_SIZE == exactly 1 GiB,
 * matching the guest's own stage-2 window byte for byte) has ZERO slack for
 * a header on top of a full 1:1 mirror. The original layout placed the 64
 * KiB struct snapshot_hdr at the FRONT of that window and started the mirror
 * SNAP_META_SIZE bytes later — which pushed the mirror's END the same amount
 * past 0xC0000000, the last byte of real, installed DRAM (confirmed live:
 * dram_copy() faulted with FAR=0xC0000000, exactly the top of QEMU's
 * `-m 2048` backed RAM, which mirrors the real board's 2 GiB total 1:1).
 *
 * FIX: the header moves out of the high GiB entirely, into this lane, so the
 * high GiB becomes a pure, full-SNAP_DRAM_SIZE mirror with zero header
 * overhead (SNAP_DRAM_STORE == SNAP_STORE_BASE now — see snapshot.h). Placed
 * here (just past the already-centralised 0x50020000 I/O block and the
 * dbgtools lane, well below the 0x50100000 non-cacheable EMAC-DMA-scratch
 * boundary — see el2_ncmap.c's idx0..255/idx256..511 split comment, which is
 * why this needs to be a Normal-WB address, not Normal-NC) rather than
 * packed any tighter against HVMAP_LOW_VCONSOLE_BUF, to leave that buffer
 * (grep'd, not assumed, per this file's own house rule) unambiguous room to
 * grow without another audit. HVMAP_SNAP_HDR_SIZE must stay >= snapshot.h's
 * SNAP_META_SIZE (checked in snapshot.h, which includes this file).
 *
 * FOLLOW-UP FIX (2026-08, same investigation): moving the header IN here
 * exposed that the header's own storage (this lane) sits INSIDE
 * [SNAP_DRAM_BASE, SNAP_DRAM_BASE+SNAP_DRAM_SIZE) — the very range
 * snapshot_save()/snapshot_restore() sweep byte-for-byte — same as the whole
 * 2 MiB hv-scratch block (HVSCR_BASE, stage2.c) it lives inside. A blind
 * sweep would read this lane as if it were guest data (harmless) and, on
 * restore, WRITE stale snapshot-time bytes back over it and every other
 * hv-scratch window (breadcrumbs, rings, BMC block) while the hypervisor is
 * actively using them — self-corruption of the running HV, not a
 * guest-visible bug, and not hypothetical: the analogous window for the
 * HV's own image produced a live, reproduced fault (see snapshot.h's
 * "EXCLUSION WINDOWS"). Fixed the same way: snapshot.c's
 * dram_copy_excluding() treats [HVSCR_BASE, HVSCR_BASE+0x200000) — which
 * contains this whole lane — as an exclusion window and never touches it. */
#define HVMAP_SNAP_HDR_BASE   0x50060000UL
#define HVMAP_SNAP_HDR_SIZE   0x00010000UL   /* == snapshot.h SNAP_META_SIZE */
#define HVMAP_SNAP_HDR_END    (HVMAP_SNAP_HDR_BASE + HVMAP_SNAP_HDR_SIZE)

_Static_assert(HVMAP_SNAP_HDR_BASE >= HVMAP_LOW_VCONSOLE_BUF + HVMAP_LOW_VCONSOLE_BUF_SZ,
               "snapshot header lane overlaps the vconsole capture buffer");
_Static_assert(HVMAP_SNAP_HDR_END <= 0x50100000UL,
               "snapshot header lane runs into emac.c's non-cacheable DMA "
               "scratch window (SCRATCH_BASE 0x50100000) -- see el2_ncmap.c");
_Static_assert(HVMAP_SNAP_HDR_END <= 0x50200000UL,
               "snapshot header lane runs past the DTB hv-scratch reserve");

/* ---- Dual-guest (Zephyr on CPU3, `dual` target only) lanes --------------
 * Added for the Zephyr-on-CPU3 concurrent-guest milestone: zguest_cpu3.c,
 * zload2.c, stage2_zephyr.c and vconsole.c's new second channel each need a
 * small fixed DRAM breadcrumb/ring lane, same convention as every other
 * window in this file. Placed right after the snapshot header lane
 * (HVMAP_SNAP_HDR_END == 0x50070000) and well below el2_ncmap.c's
 * non-cacheable DMA scratch boundary (0x50100000) — a full-tree grep for
 * `0x5007[0-9a-f]{4}` / `0x5008[0-9a-f]{4}` found nothing else there. These
 * lanes are only ever written/read by objects linked into the `dual`
 * target (zguest_cpu3.o/zload2.o/stage2_zephyr.o/mmio_absorb.o) plus
 * vconsole.c's new chan1 path (always compiled, but chan1 is only ever
 * exercised on CPU3, which no existing target ever brings up) — inert for
 * every other target. */
#define HVMAP_ZGUEST3_BC        0x50070000UL   /* zguest_cpu3.c breadcrumbs ("ZG3\0") */
#define HVMAP_ZGUEST3_BC_SIZE   0x40UL

#define HVMAP_ZLOAD2_BC         0x50071000UL   /* zload2.c breadcrumbs ("ZLD2") */
#define HVMAP_ZLOAD2_BC_SIZE    0x80UL

#define HVMAP_STAGE2Z_BC        0x50072000UL   /* stage2_zephyr.c breadcrumbs ("STGZ") */
#define HVMAP_STAGE2Z_BC_SIZE   0x80UL

#define HVMAP_MMIOABS_BC        0x50073000UL   /* mmio_absorb.c breadcrumbs ("MABS") */
#define HVMAP_MMIOABS_BC_SIZE   0x20UL

/* vconsole.c channel-1 (Zephyr/CPU3) ring — smaller than channel 0's 64 KiB
 * capture buffer (see vconsole.h): Zephyr's console output at v1 is a short
 * banner + heartbeat lines, not a full verbose OS boot log. Header mirrors
 * VCONSOLE_HDR_WORDS' 8-word shape (see vconsole.h); buffer is a separate,
 * explicit absolute base (same "never derive the buffer from the header"
 * discipline as HVMAP_LOW_VCONSOLE_BUF, for the same reason: growing the
 * header must never be able to march the buffer into a neighbour). */
#define HVMAP_VCONSOLE_CHAN1_HDR       0x50074000UL
#define HVMAP_VCONSOLE_CHAN1_HDR_SIZE  0x20UL      /* 8 words, same as chan0 */
#define HVMAP_VCONSOLE_CHAN1_BUF       0x50075000UL
#define HVMAP_VCONSOLE_CHAN1_BUF_SIZE  0x4000UL    /* 16 KiB */
#define HVMAP_VCONSOLE_CHAN1_END \
	(HVMAP_VCONSOLE_CHAN1_BUF + HVMAP_VCONSOLE_CHAN1_BUF_SIZE)

_Static_assert(HVMAP_ZGUEST3_BC >= HVMAP_SNAP_HDR_END,
               "zguest_cpu3 breadcrumb lane overlaps the snapshot header lane");
_Static_assert(HVMAP_ZGUEST3_BC + HVMAP_ZGUEST3_BC_SIZE <= HVMAP_ZLOAD2_BC,
               "zguest_cpu3 breadcrumbs overlap the zload2 breadcrumbs");
_Static_assert(HVMAP_ZLOAD2_BC + HVMAP_ZLOAD2_BC_SIZE <= HVMAP_STAGE2Z_BC,
               "zload2 breadcrumbs overlap the stage2_zephyr breadcrumbs");
_Static_assert(HVMAP_STAGE2Z_BC + HVMAP_STAGE2Z_BC_SIZE <= HVMAP_MMIOABS_BC,
               "stage2_zephyr breadcrumbs overlap the mmio_absorb breadcrumbs");
_Static_assert(HVMAP_MMIOABS_BC + HVMAP_MMIOABS_BC_SIZE <= HVMAP_VCONSOLE_CHAN1_HDR,
               "mmio_absorb breadcrumbs overlap the vconsole chan1 header");
_Static_assert(HVMAP_VCONSOLE_CHAN1_HDR + HVMAP_VCONSOLE_CHAN1_HDR_SIZE
               <= HVMAP_VCONSOLE_CHAN1_BUF,
               "vconsole chan1 header overlaps the vconsole chan1 buffer");
_Static_assert(HVMAP_VCONSOLE_CHAN1_END <= 0x50100000UL,
               "dual-guest lanes run into el2_ncmap.c's non-cacheable DMA "
               "scratch window (SCRATCH_BASE 0x50100000)");

#endif /* HV_ADDRMAP_H */
