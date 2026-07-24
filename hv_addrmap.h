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
 *   0x50000f00..0x50010f0f  vconsole 64 KiB capture ring (UART header @0xf00)
 *   0x50011000..0x50011fff  netcon / snapshot_net staging
 *   0x50012000..            flightrec "FLTR" ring
 *   0x50001c00              vgic breadcrumb window ("VGIC")
 *   0x50001d00              vgic self-test guest ("VGST")
 *   0x50006000              software-BMC block
 *   0x50020000..0x50020fff  virtio-blk / eMMC / SD I/O storage — MAPPED BELOW
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

#endif /* HV_ADDRMAP_H */
