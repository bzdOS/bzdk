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

/* eMMC failure-diagnostics breadcrumbs (emmc_bio.c).
 *
 * SIZE WAS WRONG (0x20 = 8 words) while emmc_bio.c's own EBIO_BC_NWORDS is 13
 * and it writes as high as ebio_bc(11). The writes landed in the unused padding
 * up to HS_TESTBUF at 0x50020300, so nothing was ever corrupted -- but the
 * _Static_assert chain below was protecting a size the code does not honour, so
 * moving HS_TESTBUF down to 0x50020220 would have passed the assert and let
 * slots [8..12] silently eat it. Sized to 16 words now, and emmc_bio.c carries a
 * _Static_assert tying EBIO_BC_NWORDS to this constant so the two cannot drift
 * apart again. The asserts here prove windows do not overlap; only that one
 * proves a window is big enough for its own writer. */
#define HVMAP_EBIO_BC        0x50020200UL
#define HVMAP_EBIO_BC_SIZE   0x80UL

/* eMMC high-speed probe test buffer (emmc_bio.c: one 512-byte sector). */
#define HVMAP_EMMC_HS_TESTBUF      0x50020300UL
#define HVMAP_EMMC_HS_TESTBUF_SIZE 0x200UL

/* SD-card driver breadcrumbs (sd_bio.c: idx 0..12, widened 2026-08-25 for
 * the CSD-capacity-parse fields -- was 0x28 (idx 0..9); 0x50020528..
 * 0x505ff was free (HVMAP_ASYNC_BC starts at 0x50020600), so this is a
 * pure widen, no relocation needed. */
#define HVMAP_SD_BC          0x50020500UL
#define HVMAP_SD_BC_SIZE     0x40UL

/* CPU2 async-I/O offload breadcrumbs (vblk_async.c: "VBA1", idx 0..2). */
#define HVMAP_ASYNC_BC       0x50020600UL
#define HVMAP_ASYNC_BC_SIZE  0xCUL

/* Used-ring publish lock word (vblk_emmc.c: CPU0 sync vs CPU2 async). MOVED
 * here from 0x50020200 — see the header comment above for the alias bug. */
#define HVMAP_USED_LOCK      0x50020700UL
#define HVMAP_USED_LOCK_SIZE 0x4UL

/* SD-card single-sector scratch buffer, for dbgmon's `sd read/write` probe
 * (sd_bio.c has no caller in any build -- see dbgmon.c's cmd_sd()). Placed at
 * 0x50020800 rather than reusing HVMAP_EMMC_HS_TESTBUF: that buffer belongs to
 * emmc_bio.c's high-speed probe, and this file's whole reason for existing is
 * that two purposes sharing one window is how the 2026-07-24 alias bug
 * happened. 0x50020704..0x50020fff was free (HVMAP_USED_LOCK is 4 bytes at
 * 0x50020700), and 0x800 keeps it aligned and clear of it. */
#define HVMAP_SD_TESTBUF     0x50020800UL
#define HVMAP_SD_TESTBUF_SIZE 0x200UL    /* one 512-byte sector */

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
_Static_assert(HVMAP_USED_LOCK + HVMAP_USED_LOCK_SIZE <= HVMAP_SD_TESTBUF,
               "used-ring lock overlaps the SD scratch buffer");
_Static_assert(HVMAP_SD_TESTBUF + HVMAP_SD_TESTBUF_SIZE <= HVMAP_IO_BLOCK_END,
               "SD scratch buffer overruns the I/O-storage block");
_Static_assert(HVMAP_SD_BC + HVMAP_SD_BC_SIZE <= HVMAP_SD_TESTBUF,
               "SD breadcrumbs overlap the SD scratch buffer");
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
/* FIXED (found while adding zero-copy-scanout support, see
 * docs/zero-copy-scanout.md): this used to mirror 0x50003000, hdmi.c's
 * ORIGINAL breadcrumb base. hdmi.h relocated it to 0x50011800 on 2026-07-25
 * ("Relocated 2026-07-25 from 0x50003000" — that address sits inside the
 * vconsole capture ring and was being clobbered by guest console bytes).
 * This mirror was never updated to match, so it spent months silently
 * fencing a dead address instead of the real one — a live instance of the
 * exact failure mode this file's own header comment warns about ("if one of
 * those moves, update it here"). hud.c had the same bug independently (its
 * own local HDMI_BC_BASE copy) — fixed there too. */
#define HVMAP_LOW_HDMI_BC        0x50011800UL
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

/* zstage.c breadcrumbs ("ZSTG") — the CPU0-side copy-in that moves a raw
 * guest ELF from the low-DRAM TFTP landing window into Zephyr's own private
 * slice before the first guest ever runs (see zstage.h).
 *
 * Base picked with a deliberate one-page gap above HVMAP_VCONSOLE_CHAN1_END
 * (0x50079000) rather than butting straight against it: this file's own
 * history is that a buffer grown without slack marched over SIX neighbouring
 * windows. The gap costs nothing and the next window after this one gets the
 * same courtesy. */
#define HVMAP_ZSTAGE_BC         0x5007A000UL
#define HVMAP_ZSTAGE_BC_SIZE    0x40UL

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
_Static_assert(HVMAP_VCONSOLE_CHAN1_END <= HVMAP_ZSTAGE_BC,
               "vconsole chan1 buffer overlaps the zstage breadcrumbs");
_Static_assert(HVMAP_ZSTAGE_BC + HVMAP_ZSTAGE_BC_SIZE <= 0x50100000UL,
               "dual-guest lanes run into el2_ncmap.c's non-cacheable DMA "
               "scratch window (SCRATCH_BASE 0x50100000)");

/* ---- vconsole POSTMORTEM carry-over lane (added 2026-08-11) -------------
 * A verbatim copy of the PREVIOUS run's channel-0 console capture ring, made
 * by vconsole_init() before it resets the live ring, so a crash's console text
 * survives the reload that is the only way to get the debug channel back.
 *
 * WHY: the live ring (HVMAP_LOW_VCONSOLE_BUF, 64 KiB) is written by the guest
 * and reset on every HV start. When a fault takes the whole board down — not
 * just the guest — the sequence to regain the debug channel is TFTP-reload the
 * hypervisor, which runs vconsole_init(), which zeroes the ring, and then boots
 * a FreeBSD whose own output refills it. By the time a human can ask "what did
 * it print?", the answer has been overwritten twice over. That is exactly what
 * happened on 2026-08-11 chasing the Mali/lima whole-board crash: three
 * different capture attempts (post-reload `con read`, live `--follow` on the TX
 * tee, a raw EMAC sweep of the whole 64 KiB ring) all came back with the
 * CURRENT boot's text, and the crash was never read at all.
 *
 * The bytes were in DRAM the whole time — every ring store is `dc civac`'d to
 * PoC precisely so it survives a reset (see vconsole.h) — so nothing needed to
 * be captured differently. Only the metadata was being thrown away, and then
 * the bytes themselves. Copying them aside first is the whole fix.
 *
 * LINEARISED ON PURPOSE: offset 0 of the buffer is the OLDEST surviving byte
 * and `len` runs forward to the newest, so a reader needs no modular
 * arithmetic. Getting that arithmetic wrong on the live ring is what made
 * `bmc con read` dump the boot banner for months (vconsole.h has the account);
 * a postmortem reader is exactly the caller least able to afford re-deriving
 * it, since it is used when something has already gone wrong.
 *
 * ONE GENERATION DEEP. Each HV start overwrites the lane, so the carry-over
 * holds the run immediately before this one and nothing older. `gen` (word[3])
 * counts captures and survives them, which is how a reader tells "this is the
 * crash I am looking for" from "I reloaded twice and lost it". Read it before
 * the second reload.
 *
 * Placed at 0x50080000: a page of slack above HVMAP_ZSTAGE_BC's end
 * (0x5007A040) per this file's own hygiene rule, and 0x6f000 clear of
 * el2_ncmap.c's non-cacheable boundary at 0x50100000, which this lane must
 * stay below — it has to be Normal-WB for the `dc civac` stores to mean
 * anything.
 *
 * Word layout (uint32_t, at HVMAP_VCPM_HDR):
 *   [0] (+0x00) magic HVMAP_VCPM_MAGIC ("VCPM"), written LAST so a reader can
 *       never see a half-filled lane as valid
 *   [1] (+0x04) len         bytes valid in the buffer, 0..HVMAP_VCPM_BUF_SIZE
 *   [2] (+0x08) prev_total  the previous run's unclamped total_bytes — i.e.
 *       how much the guest printed in total, so `prev_total - len` is exactly
 *       how much was lost to the ring's wrap and is NOT in this copy
 *   [3] (+0x0c) gen         captures performed, ever (survives captures)
 *   [4] (+0x10) buf_base    self-describing, same discipline as vconsole's own
 *   [5] (+0x14) buf_size
 *   [6] (+0x18) prev_faults the previous run's vconsole fault_count
 *   [7] (+0x1c) reserved, 0
 */
#define HVMAP_VCPM_HDR        0x50080000UL
#define HVMAP_VCPM_HDR_SIZE   0x20UL           /* 8 words, same shape as vconsole */
#define HVMAP_VCPM_MAGIC      0x5643504DUL     /* "VCPM" */
#define HVMAP_VCPM_BUF        0x50081000UL     /* explicit, NOT derived from HDR */
#define HVMAP_VCPM_BUF_SIZE   0x10000UL        /* must be >= VCONSOLE_BUF_SIZE  */
#define HVMAP_VCPM_END        (HVMAP_VCPM_BUF + HVMAP_VCPM_BUF_SIZE)

_Static_assert(HVMAP_VCPM_HDR >= HVMAP_ZSTAGE_BC + HVMAP_ZSTAGE_BC_SIZE,
               "postmortem lane overlaps the zstage breadcrumbs");
_Static_assert(HVMAP_VCPM_HDR + HVMAP_VCPM_HDR_SIZE <= HVMAP_VCPM_BUF,
               "postmortem header overlaps its own buffer");
_Static_assert(HVMAP_VCPM_END <= 0x50100000UL,
               "postmortem lane runs into el2_ncmap.c's non-cacheable DMA "
               "scratch window (SCRATCH_BASE 0x50100000) -- it must stay "
               "Normal-WB for the dc civac stores to reach DRAM");
/* The lane must be able to hold the ENTIRE live ring, or a carry-over would
 * silently truncate the newest bytes -- the ones that matter most. The
 * VCONSOLE_BUF_SIZE side of this is asserted in vconsole.c, which is the only
 * file that sees both headers. */
_Static_assert(HVMAP_VCPM_BUF_SIZE >= HVMAP_LOW_VCONSOLE_BUF_SZ,
               "postmortem buffer is smaller than the vconsole ring it copies");

/* ---- hv-fb double-buffer geometry (zero-copy scanout, HV_HDMI builds) ---
 * See docs/zero-copy-scanout.md for the full design. hv-fb itself is NOT a
 * hv-scratch lane (it is its own DTB reserved-memory node, hv-fb@4d000000,
 * excluded from the guest's stage-2 map by stage2.c's `#ifdef HV_HDMI`
 * HVFB_BASE/HVFB_SIZE carve-out — see that file) but its two buffers are
 * fixed windows exactly like every other allocation in this file, so they
 * get the same _Static_assert treatment rather than a hand-picked offset
 * some future edit silently outgrows.
 *
 * HVMAP_FB_BASE/HVMAP_FB_WINDOW_SIZE are LITERAL MIRRORS of hdmi.h's
 * HDMI_FB_BASE and stage2.c's HVFB_SIZE (same "mirroring" compromise this
 * file already makes for the LOW-BLOCK section above — hv_addrmap.h has no
 * #include on hdmi.h/stage2.h and this keeps it that way). If either moves,
 * update all three.
 *
 * HVMAP_FB_BUF_W/H mirror hdmi.h's HDMI_MODE_HACTIVE/HDMI_MODE_VACTIVE — the
 * geometry hdmi_init() actually runs (720p; the 1080p PHY divider path never
 * locked on real hardware, see hdmi.h's own comment) and the ONLY geometry
 * fb_init() is ever called with (hud.c:282, hdmi.c:1047 — both call
 * fb_init(hdmi_fb(), hdmi_width(), hdmi_height(), hdmi_stride()), i.e. these
 * same numbers via accessors, never a literal 1920x1080 anywhere at runtime).
 * If the mode ever changes back to 1080p, HVMAP_FB_BUF_W/H must change with
 * it or the asserts below catch the mismatch at compile time instead of
 * silently laying buffer 1 over the wrong bytes. */
#define HVMAP_FB_BASE            0x4D000000UL   /* mirrors hdmi.h HDMI_FB_BASE /
                                                  * stage2.c HVFB_BASE          */
#define HVMAP_FB_WINDOW_SIZE     0x00800000UL   /* mirrors stage2.c HVFB_SIZE and
                                                  * the hv-fb@4d000000 DTB node's
                                                  * `reg` size (8 MiB) */

#define HVMAP_FB_BUF_W           1280UL         /* mirrors hdmi.h HDMI_MODE_HACTIVE */
#define HVMAP_FB_BUF_H           720UL          /* mirrors hdmi.h HDMI_MODE_VACTIVE */
#define HVMAP_FB_BUF_BPP         4UL             /* XRGB8888, hdmi.h/fb.h        */
#define HVMAP_FB_BUF_STRIDE      (HVMAP_FB_BUF_W * HVMAP_FB_BUF_BPP)    /* 5120 B/line, no padding */
#define HVMAP_FB_BUF_SIZE        (HVMAP_FB_BUF_STRIDE * HVMAP_FB_BUF_H) /* 3,686,400 B == 0x384000 */

/* Buffer 0 is BY CONSTRUCTION the same address hdmi.h's HDMI_FB_BASE /
 * stage2.c's HVFB_BASE already use — this is what keeps the existing
 * single-buffer behaviour (hud.c, hdmi_demo()) working completely unchanged
 * by default: nobody has to touch hdmi_fb()/HDMI_FB_BASE for buffer 0 to be
 * correct. Buffer 1 is the new back buffer, immediately after it. */
#define HVMAP_FB_BUF0_BASE       (HVMAP_FB_BASE + 0UL)
#define HVMAP_FB_BUF1_BASE       (HVMAP_FB_BASE + HVMAP_FB_BUF_SIZE)

_Static_assert(HVMAP_FB_BUF_SIZE % 0x1000UL == 0UL,
               "hv-fb buffer size is not 4 KiB page-aligned — pick a scanout "
               "base alignment before changing HVMAP_FB_BUF_W/H");
_Static_assert(HVMAP_FB_BUF0_BASE + HVMAP_FB_BUF_SIZE <= HVMAP_FB_BUF1_BASE,
               "hv-fb buffer 0 and buffer 1 overlap");
_Static_assert(HVMAP_FB_BUF1_BASE + HVMAP_FB_BUF_SIZE
               <= HVMAP_FB_BASE + HVMAP_FB_WINDOW_SIZE,
               "double-buffered hv-fb layout overflows the 8 MiB hv-fb DTB "
               "reservation -- see docs/zero-copy-scanout.md's fit arithmetic");

/* ---- scanout.c doorbell breadcrumb ("SCAN") ------------------------------
 * scanout.c emulates a tiny guest-facing MMIO device at SCANOUT_MMIO_BASE
 * (scanout.h, 0x0A002000 — inside the SAME already-stage-2-trapped 2 MiB
 * block vblk_emmc.h/vnet_emac.h already use, so no stage2.c change was
 * needed for that device; see scanout.h's own comment). This is the
 * SEPARATE, EL2-only breadcrumb mirror of that device's state, readable via
 * the existing `bc`/`md.l` debug convention without needing a guest at all
 * (see docs/zero-copy-scanout.md's verification section). Placed a full
 * page above HVMAP_VCPM_END per this file's own hygiene rule (a buffer
 * grown without slack has marched into a neighbour before — see the VCPM
 * section above), well below el2_ncmap.c's non-cacheable DMA scratch
 * boundary (0x50100000) so the coherent-store (`dc civac`) pattern applies. */
#define HVMAP_SCANOUT_BC         0x50092000UL
#define HVMAP_SCANOUT_BC_SIZE    0x40UL

_Static_assert(HVMAP_SCANOUT_BC >= HVMAP_VCPM_END,
               "scanout breadcrumb lane overlaps the vconsole postmortem lane");
_Static_assert(HVMAP_SCANOUT_BC + HVMAP_SCANOUT_BC_SIZE <= 0x50100000UL,
               "scanout breadcrumb lane runs into el2_ncmap.c's non-cacheable "
               "DMA scratch window (SCRATCH_BASE 0x50100000)");

/* ---- Dynamic W^X breadcrumb ("WXD1") -------------------------------------
 * stage2.c's opt-in dynamic W^X mechanism (STAGE2_WX_DYNAMIC, off by
 * default — see stage2.h and docs/wx-enforcement.md). Diagnostic only, same
 * discipline as every other lane here: never read back by any code path,
 * never gates behavior. Placed a full page above HVMAP_SCANOUT_BC's end
 * (0x50092040) per this file's own hygiene rule (a buffer grown without
 * slack has marched into a neighbour before — see the VCPM section above);
 * confirmed clear by grepping the whole tree for `0x5009[0-9a-f]{4}` /
 * `0x500a[0-9a-f]{4}` before picking it — only HVMAP_SCANOUT_BC itself
 * matched. Well below el2_ncmap.c's non-cacheable DMA scratch boundary
 * (0x50100000), so the coherent-store (`dc civac`) pattern applies.
 *
 * Word layout (uint32_t):
 *   [0] magic HVMAP_WXDYN_MAGIC ("WXD1")
 *   [1] pool_used        on-demand L3 tables handed out so far (see
 *       STAGE2_WX_POOL_TABLES, stage2.h)
 *   [2] pool_exhausted   distinct 2 MiB blocks that fell back to plain
 *       RW+X because the pool was already full when they first executed
 *       (fail OPEN, not closed -- see stage2.c's stage2_wx_flip())
 *   [3] flip_count       total W<->X flips performed, either direction
 *   [4] last_ipa         low 32 bits of the most recent flip's IPA */
#define HVMAP_WXDYN_BC        0x50093000UL
#define HVMAP_WXDYN_BC_SIZE   0x20UL
#define HVMAP_WXDYN_MAGIC     0x57584431UL   /* "WXD1" */

_Static_assert(HVMAP_WXDYN_BC >= HVMAP_SCANOUT_BC + HVMAP_SCANOUT_BC_SIZE,
               "dynamic W^X breadcrumb lane overlaps the scanout breadcrumb lane");

/* ---- GICD trap-and-police breadcrumb ("VGCD") ----------------------------
 * vgicd.c's counters for the trapped GIC distributor. Placed one full 4 KiB
 * step above HVMAP_WXDYN_BC, the spacing every lane in this region uses, and
 * with slack per this file's own hygiene rule.
 *
 * The interesting word is [4]: a nonzero value means one guest wrote
 * GICD_ITARGETSR trying to aim an interrupt at a core it does not own, and the
 * affinity field was masked down to what it is entitled to. On a single-guest
 * build that should stay 0 forever; on the dual build it is the measurement
 * that says whether cross-partition interference is actually being attempted.
 *
 * Word layout (uint32_t):
 *   [0] magic HVMAP_VGICD_MAGIC ("VGCD")
 *   [1] total GICD accesses trapped
 *   [2] reads passed through
 *   [3] writes passed through unmodified
 *   [4] ITARGETSR writes whose affinity field was MASKED
 *   [5] SGIR writes whose target list was masked
 *   [6] last faulting offset within the distributor page
 *   [7] last value written (post-mask)
 *   [8] accesses with ESR.ISV==0 (no register info; faked completion)
 *   [9] last CPU id to fault here */
#define HVMAP_VGICD_BC       0x50094000UL
#define HVMAP_VGICD_BC_SIZE  0x40UL
#define HVMAP_VGICD_MAGIC    0x56474344UL   /* "VGCD" */

_Static_assert(HVMAP_VGICD_BC >= HVMAP_WXDYN_BC + HVMAP_WXDYN_BC_SIZE,
               "GICD breadcrumb lane overlaps the dynamic W^X lane");
_Static_assert(HVMAP_WXDYN_BC + HVMAP_WXDYN_BC_SIZE <= 0x50100000UL,
               "dynamic W^X breadcrumb lane runs into el2_ncmap.c's "
               "non-cacheable DMA scratch window (SCRATCH_BASE 0x50100000)");

/* ---- wdt.c debug-core reset gate (`wdt_debug_hold`) ----------------------
 * wdt.c's SMP debug-core watchdog owner (see that file's "SMP debug-core
 * watchdog ownership" section) has CPU1 unconditionally pet the HW WDOG
 * every poll iteration UNLESS this word is nonzero -- the one documented way
 * to force a clean reset over the network (CPU1 stops petting, the HW WDOG
 * fires within its <=16 s window, board lands back at U-Boot).
 *
 * Used to be a plain BSS global (`extern volatile uint32_t wdt_debug_hold`),
 * which meant setting it from off-board required `nm`-resolving its address
 * out of the microkernel-dbg.elf file ON DISK -- exactly the build-vs-
 * running-image skew trap hvdbg.py's wdt_reset() docstring already warns
 * about for reboot_clean() (see that function). This flag had the identical
 * exposure and simply hadn't been given a fixed address yet; fixed here so a
 * host tool can set it with one `w <addr> <val>` MMIO poke, no ELF symbol
 * table involved -- the same "fixed address, not a linked symbol" discipline
 * HVMAP_DBGTOOLS_HOLD already established for the pause-before-entry gate
 * above.
 *
 * wdt.h now #defines wdt_debug_hold as a macro over this address (see that
 * header), so every existing call site (`wdt_debug_hold = 1;` /
 * `if (wdt_debug_hold)` in wdt.c, bmc.c, usbacm.c, el2_exc.c, smp.c) keeps
 * working completely unchanged -- no plain BSS storage backs the name
 * anymore, so smp_qemu_stub.c no longer needs (or can have) its own
 * definition of it either.
 *
 * Cross-core visibility needs no explicit cache maintenance here: CPU1
 * writes this in response to a `w`/`bmc wdt hold` command and CPU0's
 * wdt_pet() reads it on every EL2 trap, which is exactly the ordinary
 * "cross-core-coherent... storage" this whole file's header already promises
 * for a normal load/store within the ARM coherency domain. The `dc civac`
 * convention used by the breadcrumb lanes elsewhere in this file is a WARM-
 * RESET-survivability concern (surviving a reload with caches cold), not a
 * cross-core one -- irrelevant here, since nothing needs this flag's value
 * to survive the very reset it exists to trigger.
 *
 * No magic word: every caller only ever tests zero/nonzero (this is a
 * control flag, not a breadcrumb lane a human inspects via `bc`/`md.l` for
 * self-description). Placed a full 4 KiB page above HVMAP_VGICD_BC's end
 * (0x50094040) per this file's own hygiene rule; confirmed clear by
 * grepping the whole tree for `0x5009[0-9a-f]{4}` / `0x500a[0-9a-f]{4}`
 * before picking it -- only HVMAP_SCANOUT_BC/HVMAP_WXDYN_BC/HVMAP_VGICD_BC
 * matched, all below it. */
#define HVMAP_WDT_DEBUG_HOLD       0x50095000UL

/* ---- Event-trace ring ("TRC1") and profiler histogram ("PROF") -----------
 * RELOCATED 2026-08-20, and the relocation is the whole point of this entry.
 *
 * trace.c and profiler.c were written, complete, and linked into NO build
 * target, so their addresses had never been reconciled with this map. Both
 * were wrong:
 *
 *   - trace.c's ring at 0x50004000 with TRACE_CAP=512 spans
 *     0x50004000..0x50006040, and bmc.h puts BMC_HEALTH_BASE at 0x50006000.
 *     Enabling the ring as written would have had its last two slots write
 *     through the software-BMC health block -- and bmc.h's own comment
 *     claims 0x50006000 is "clear of every existing window", which was true
 *     only because the ring was dead code.
 *   - profiler.c's table at 0x50006800 (1024 buckets ->
 *     0x50006800..0x50008820) also lands inside the BMC block, and hud.c
 *     read it at 0x50004800 instead -- i.e. 2 KiB INTO the trace ring. That
 *     is why the HUD's PROFILE panel could only ever print "PROF magic not
 *     found": it was reading another subsystem's ring.
 *
 * Both now live above every other window, in the 0x50096000..0x50100000 gap
 * (emac.c's SCRATCH_BASE is 0x50100000 and is the next thing up), sized from
 * their own capacity constants rather than from a neighbour -- the rule this
 * file exists to enforce. hud.c reads THESE defines; it no longer keeps its
 * own copies, which is how it drifted 2 KiB into the ring in the first
 * place. */
#define HVMAP_TRACE_RING       0x50096000UL
#define HVMAP_TRACE_RING_SIZE  0x00002100UL   /* 0x40 hdr + 512 * 16 B */
#define HVMAP_TRACE_RING_END   (HVMAP_TRACE_RING + HVMAP_TRACE_RING_SIZE)

#define HVMAP_PROF_HIST        0x5009A000UL
#define HVMAP_PROF_HIST_SIZE   0x00002020UL   /* 0x20 hdr + 1024 * 8 B */
#define HVMAP_PROF_HIST_END    (HVMAP_PROF_HIST + HVMAP_PROF_HIST_SIZE)

_Static_assert(HVMAP_TRACE_RING >= HVMAP_WDT_DEBUG_HOLD + 0x1000UL,
               "trace ring overlaps the watchdog debug-hold word");
_Static_assert(HVMAP_TRACE_RING_END <= HVMAP_PROF_HIST,
               "trace ring runs into the profiler histogram");
_Static_assert(HVMAP_PROF_HIST_END <= 0x50100000UL,
               "profiler histogram runs into emac.c's DMA scratch (0x50100000)");

/* ---- second guest vCPU on CPU2 (vcpu2.c) -------------------------------
 * Breadcrumbs for the core that stops being an HV worker and becomes a guest
 * CPU. Placed past the profiler histogram, which is the current top of this
 * block, and asserted against BOTH neighbours rather than eyeballed -- the
 * mistake this file exists to prevent. */
#define HVMAP_VCPU2_BC        0x5009D000UL
#define HVMAP_VCPU2_BC_SIZE   0x00000100UL
#define HVMAP_VCPU2_BC_END    (HVMAP_VCPU2_BC + HVMAP_VCPU2_BC_SIZE)

_Static_assert(HVMAP_VCPU2_BC >= HVMAP_PROF_HIST_END,
               "vcpu2 breadcrumbs overlap the profiler histogram");
_Static_assert(HVMAP_VCPU2_BC_END <= 0x50100000UL,
               "vcpu2 breadcrumbs run into emac.c's DMA scratch (0x50100000)");

/* ---- virtio-input keyboard (vinput.c) ----------------------------------
 * Placed past HVMAP_VCPU2_BC, which is the current top of this block. */
#define HVMAP_VINPUT_BC        0x5009E000UL
#define HVMAP_VINPUT_BC_SIZE   0x00000040UL
#define HVMAP_VINPUT_BC_END    (HVMAP_VINPUT_BC + HVMAP_VINPUT_BC_SIZE)

_Static_assert(HVMAP_VINPUT_BC >= HVMAP_VCPU2_BC_END,
               "vinput breadcrumbs overlap the vcpu2 breadcrumb lane");
_Static_assert(HVMAP_VINPUT_BC_END <= 0x50100000UL,
               "vinput breadcrumbs run into emac.c's DMA scratch (0x50100000)");

/* ---- third guest vCPU on CPU1 (vcpu1.c) --------------------------------
 * EXPERIMENTAL — trades CPU1's independent crash-recovery witness for a
 * third vCPU's worth of guest compute; see vcpu1.h's header comment for the
 * full tradeoff. Placed past HVMAP_VINPUT_BC, the current top of this
 * block, same pattern as every other lane here. */
#define HVMAP_VCPU1_BC        0x5009E100UL
#define HVMAP_VCPU1_BC_SIZE   0x00000100UL
#define HVMAP_VCPU1_BC_END    (HVMAP_VCPU1_BC + HVMAP_VCPU1_BC_SIZE)

_Static_assert(HVMAP_VCPU1_BC >= HVMAP_VINPUT_BC_END,
               "vcpu1 breadcrumbs overlap the vinput breadcrumb lane");
_Static_assert(HVMAP_VCPU1_BC_END <= 0x50100000UL,
               "vcpu1 breadcrumbs run into emac.c's DMA scratch (0x50100000)");

/* ---- virtio-blk over the microSD card (vblk_sd.c) ----------------------
 * Placed past HVMAP_VCPU1_BC, the current top of this block. Lock word
 * first (4 bytes), breadcrumb window after — same layout convention as
 * eMMC's HVMAP_VBLK_BC + HVMAP_EMMC_LOCK pairing further up this file. */
#define HVMAP_VBLK_SD_LOCK      0x5009E200UL
#define HVMAP_VBLK_SD_LOCK_SIZE 0x00000004UL

#define HVMAP_VBLK_SD_BC        0x5009E300UL
#define HVMAP_VBLK_SD_BC_SIZE   0x00000100UL
#define HVMAP_VBLK_SD_BC_END    (HVMAP_VBLK_SD_BC + HVMAP_VBLK_SD_BC_SIZE)

_Static_assert(HVMAP_VBLK_SD_LOCK >= HVMAP_VCPU1_BC_END,
               "vblk_sd lock overlaps the vcpu1 breadcrumb lane");
_Static_assert(HVMAP_VBLK_SD_BC >= HVMAP_VBLK_SD_LOCK + HVMAP_VBLK_SD_LOCK_SIZE,
               "vblk_sd breadcrumbs overlap the vblk_sd lock word");
_Static_assert(HVMAP_VBLK_SD_BC_END <= 0x50100000UL,
               "vblk_sd breadcrumbs run into emac.c's DMA scratch (0x50100000)");

/* ---- fourth guest vCPU on CPU3 (vcpu3.c) --------------------------------
 * Same shape as HVMAP_VCPU1_BC/HVMAP_VCPU2_BC above. Placed past
 * HVMAP_VBLK_SD_BC, the current top of this block, same pattern as every
 * other lane here — asserted against its actual neighbour, not eyeballed. */
#define HVMAP_VCPU3_BC        0x5009E400UL
#define HVMAP_VCPU3_BC_SIZE   0x00000100UL
#define HVMAP_VCPU3_BC_END    (HVMAP_VCPU3_BC + HVMAP_VCPU3_BC_SIZE)

_Static_assert(HVMAP_VCPU3_BC >= HVMAP_VBLK_SD_BC_END,
               "vcpu3 breadcrumbs overlap the vblk_sd breadcrumb lane");
_Static_assert(HVMAP_VCPU3_BC_END <= 0x50100000UL,
               "vcpu3 breadcrumbs run into emac.c's DMA scratch (0x50100000)");

#define HVMAP_WDT_DEBUG_HOLD_SIZE  0x10UL   /* one word used, room to grow */

_Static_assert(HVMAP_WDT_DEBUG_HOLD >= HVMAP_VGICD_BC + HVMAP_VGICD_BC_SIZE,
               "wdt debug-hold flag overlaps the GICD breadcrumb lane");
_Static_assert(HVMAP_WDT_DEBUG_HOLD + HVMAP_WDT_DEBUG_HOLD_SIZE <= 0x50100000UL,
               "wdt debug-hold flag runs into el2_ncmap.c's non-cacheable "
               "DMA scratch window (SCRATCH_BASE 0x50100000)");

#endif /* HV_ADDRMAP_H */
