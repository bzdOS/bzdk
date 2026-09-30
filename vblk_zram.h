/* SPDX-License-Identifier: BSD-2-Clause */

/* vblk_zram.h — compressed-RAM virtio-blk (HANDOFF item 3, "vzram"): a
 * third guest disk whose sectors live LZ4-compressed in a slice of DRAM the
 * guest is never told about, meant to be the guest's swap device.
 *
 * BACKING: HVMAP_VZRAM_BASE..+HVMAP_VZRAM_SIZE (hv_addrmap.h), carved from
 * the top of what the guest used to be given: board-config.xml's `vzram`
 * feature shrinks the DTB /memory node by exactly that slice, and stage-2
 * already maps all 2 GiB in 1 GiB blocks (stage2.h), so EL2 can use it with
 * no guest coordination. The first VZRAM_SLOTS_BYTES hold the per-page slot
 * table (vzram_pool.h), the rest is the compressed-page pool.
 *
 * GEOMETRY: 4 KiB logical pages. The guest first sees VZRAM_PAGES_INITIAL
 * of them -- about what the pool holds uncompressed, so an incompressible
 * workload cannot overcommit it -- and the device grows the disk by
 * VZRAM_PAGES_STEP (config-change interrupt, which FreeBSD's
 * vtblk_config_change() turns into a live GEOM resize) only while the
 * data is proving compressible: >= 3/4 of the exposed pages written and
 * the pool no more than half used. Ceiling VZRAM_PAGES_MAX. Never shrinks.
 *
 * LOCKING: one lock over everything -- request service, completion, the
 * ISR registers and reset. lz4.c and vzram_pool.c keep file-static scratch
 * state, and four vCPUs can notify at once; this is the "one lock across
 * the whole compress/decompress/copy path" HANDOFF asks for.
 *
 * MMIO slot 0x0A005000, SPI 109 (0x6D, inside the documented 0x68..0x73
 * gap), device-id 2. Registered via board-config.xml like every sibling. */
#ifndef BZDOS_VBLK_ZRAM_H
#define BZDOS_VBLK_ZRAM_H
#include <stdint.h>
#include "exceptions.h"

#define VBLK_ZRAM_MMIO_BASE  0x0A005000UL
#define VBLK_ZRAM_MMIO_SIZE  0x00000200UL
#define VBLK_ZRAM_SPI        109u
#define VBLK_ZRAM_INTID      (32u + VBLK_ZRAM_SPI)   /* == 141 */

#define VZRAM_PAGES_MAX      262144u   /* 1 GiB logical ceiling */
#define VZRAM_PAGES_INITIAL  32768u    /* 128 MiB */
#define VZRAM_PAGES_STEP     32768u    /* +128 MiB per grow */
#define VZRAM_SLOTS_BYTES    (VZRAM_PAGES_MAX * 8u)   /* sizeof(vzram_slot_t) */

void vblk_zram_init(void);
int vblk_zram_mmio_fault(struct el2_frame *frame);

#endif /* BZDOS_VBLK_ZRAM_H */
