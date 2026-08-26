/* SPDX-License-Identifier: BSD-2-Clause */

/* vblk_sd.h — virtio-blk (VIRTIO DeviceID 2) over virtio-mmio, backed by the
 * REAL microSD card (sd_bio.c / SMHC0 @ 0x01c0f000), for the FreeBSD/arm64
 * EL1 guest under the bzdOS EL2 hypervisor.
 *
 * WHY THIS EXISTS: a second disk, so the guest can put /var (and anything
 * else it wants to keep OFF the boot-critical eMMC root) on genuinely
 * separate physical media — the actual motivation is making root read-only
 * survivable: /var needs to stay writable for sshd/ntpd/pkg/logs, and a
 * writable directory living on an otherwise-read-only filesystem isn't a
 * thing UFS can do, but a second real disk is. See sd_bio.h's own "ROADMAP
 * G-3 (SD offload / eMMC read-only)" note — this file is that virtio-blk
 * layer, the missing piece it names.
 *
 * DELIBERATELY LEANER THAN vblk_emmc.c, and the differences are named, not
 * accidental:
 *   - No CPU2 async offload (vblk_async.h) — this device always services a
 *     request synchronously, in the guest's own trap. eMMC needed the
 *     offload because it backs the BOOT-critical root fs under real
 *     workload latency pressure; /var is not that path, and skipping the
 *     mailbox/second-core plumbing removes a whole class of cross-core bugs
 *     vblk_emmc.c had to solve (and re-solve) for no benefit here.
 *   - No VBLK_BOOT_GUARD_LBA — nothing boots from the SD card, so there is
 *     no fixed-offset SPL/U-Boot region to protect. Every LBA is guest-
 *     writable.
 *   - Bounded-retry-then-IOERR on a lock contention or a write stall,
 *     instead of vblk_emmc.c's "never give up, the guest has nowhere else
 *     to go" policy. That policy earns its complexity protecting the ROOT
 *     filesystem from a single lost race panicking the boot; failing a
 *     /var write occasionally is recoverable (the guest's own filesystem
 *     retries or logs it), so the simpler policy is the right tradeoff here,
 *     not a shortcut taken by accident.
 *   - sd_bio_write() is UNCONFIRMED ON SILICON as of this writing (see its
 *     own header comment: "structurally correct but treat a 0 return with a
 *     read-back compare until confirmed"). This device does not add its own
 *     verify-after-write — that is a real, named gap, not an oversight; add
 *     one (or confirm sd_bio_write() directly) before trusting this for
 *     anything precious.
 *
 * WHAT IS NOT SKIPPED, because skipping it would be a correctness bug, not
 * a resilience tradeoff:
 *   - The partial-sector stitching arithmetic (serve_data() in vblk_emmc.c,
 *     mirrored here) — a real, hardware-proven bug on this exact virtio-blk
 *     guest driver (FreeBSD's virtio_blk can start/end a data descriptor
 *     mid-sector), so assuming whole-sector-aligned descriptors would silently
 *     corrupt data the same way it did for eMMC before that fix landed.
 *   - The cache-coherency publish-to-PoC step after a direct-to-guest-buffer
 *     read (gmem_cmo()) — FreeBSD's virtio_blk completion path does a
 *     POSTREAD cache invalidate on the buffer, discarding any still-dirty
 *     write-back lines from a cached EL2 store; skipping the publish means
 *     every read comes back stale despite reporting S_OK. Same root cause,
 *     same fix, as vblk_emmc.c's own (see its 2026-07-22 comment).
 *   - The guest-PA range check (gpa_in_range()) before EITHER device touches
 *     a guest buffer directly — sd_bio_read/write dereference a raw PA, same
 *     as emmc_bio_read/write, so a corrupt/wild descriptor PA needs the same
 *     guard or it can make the HV scribble over its own .text/.data.
 *   - A cross-core lock against dbgmon's `sd init`/`sd read`/`sd write`
 *     commands (CPU1) — the SAME SMHC IP block, same "two cores touching one
 *     controller's FIFO/command registers corrupts the transfer" hazard
 *     vblk_emmc.c documents for eMMC, just against a DIFFERENT lock word
 *     (this device's own controller, not eMMC's).
 *
 * PLACEMENT: VBLK_SD_MMIO_BASE (0x0A004000) is the next free 4 KiB slot in
 * the SAME already-stage-2-trapped 2 MiB block every other virtio-mmio
 * device here uses (vblk_emmc.h's "MMIO window" comment) — no stage2.c
 * change needed, same as vnet/scanout/vinput before it.
 */
#ifndef BZDOS_VBLK_SD_H
#define BZDOS_VBLK_SD_H

#include <stdint.h>
#include "exceptions.h"

#define VBLK_SD_MMIO_BASE   0x0A004000UL
#define VBLK_SD_MMIO_SIZE   0x00000200UL

/* SPI 108 (0x6C): the next number in the documented free gap 0x68..0x73
 * (docs/virtio-blk-dtb.md), after VBLK_SPI=105, VNET_SPI=106, VINPUT_SPI=107. */
#define VBLK_SD_SPI      108u
#define VBLK_SD_INTID    (32u + VBLK_SD_SPI)   /* == 140 */

#define VBLK_SD_GICD_BASE       0x01C81000UL
#define VBLK_SD_GICD_ISPENDR    0x200u

/* Register offsets — identical layout to vblk_emmc.h's (the virtio-mmio
 * boilerplate is the same across every device in this tree; duplicated per
 * this file's self-containment convention). */
#define VBLK_SD_R_MAGIC_VALUE        0x000u
#define VBLK_SD_R_VERSION            0x004u
#define VBLK_SD_R_DEVICE_ID          0x008u
#define VBLK_SD_R_VENDOR_ID          0x00Cu
#define VBLK_SD_R_DEVICE_FEATURES    0x010u
#define VBLK_SD_R_DEVICE_FEATURES_SEL 0x014u
#define VBLK_SD_R_DRIVER_FEATURES    0x020u
#define VBLK_SD_R_DRIVER_FEATURES_SEL 0x024u
#define VBLK_SD_R_QUEUE_SEL          0x030u
#define VBLK_SD_R_QUEUE_NUM_MAX      0x034u
#define VBLK_SD_R_QUEUE_NUM          0x038u
#define VBLK_SD_R_QUEUE_READY        0x044u
#define VBLK_SD_R_QUEUE_NOTIFY       0x050u
#define VBLK_SD_R_INTERRUPT_STATUS   0x060u
#define VBLK_SD_R_INTERRUPT_ACK      0x064u
#define VBLK_SD_R_STATUS             0x070u
#define VBLK_SD_R_QUEUE_DESC_LOW     0x080u
#define VBLK_SD_R_QUEUE_DESC_HIGH    0x084u
#define VBLK_SD_R_QUEUE_DRIVER_LOW   0x090u
#define VBLK_SD_R_QUEUE_DRIVER_HIGH  0x094u
#define VBLK_SD_R_QUEUE_DEVICE_LOW   0x0A0u
#define VBLK_SD_R_QUEUE_DEVICE_HIGH  0x0A4u
#define VBLK_SD_R_CONFIG_GENERATION  0x0FCu
#define VBLK_SD_R_CONFIG             0x100u   /* struct virtio_blk_config */

#define VBLK_SD_MMIO_MAGIC           0x74726976u  /* "virt" LE */
#define VBLK_SD_MMIO_VERSION         2u
#define VBLK_SD_MMIO_VENDOR          0x627A6473u  /* "bzds" */
#define VBLK_SD_DEVICE_ID            2u           /* virtio-blk */

#define VBLK_SD_S_ACKNOWLEDGE        0x01u
#define VBLK_SD_S_DRIVER             0x02u
#define VBLK_SD_S_DRIVER_OK          0x04u
#define VBLK_SD_S_FEATURES_OK        0x08u
#define VBLK_SD_S_NEEDS_RESET        0x40u
#define VBLK_SD_S_FAILED             0x80u

#define VBLK_SD_INT_VRING            0x1u

/* Feature bit 32 = VIRTIO_F_VERSION_1, word 1 bit 0.
 *
 * CORRECTED 2026-08-24 (confirmed live): word 0 also advertises
 * VIRTIO_BLK_F_SEG_MAX (bit 2). The comment this replaces claimed omitting
 * it "keeps FreeBSD's virtio_blk capping every request to one page-aligned
 * data segment, n==3 always" -- that is FALSE. Confirmed live: a plain
 * single dd write of >4 KiB (i.e. more than one page, needing >1 data
 * descriptor) wedges the guest forever in an uninterruptible sleep
 * (procstat -kk: sys_write -> physio -> biowait -> _sleep; virtio ring
 * avail_idx/used_idx/last_avail all frozen and equal -- the request never
 * even reaches the ring). Without VIRTIO_BLK_F_SEG_MAX negotiated,
 * FreeBSD's vtblk driver does NOT split the I/O into single-page requests
 * as the old comment assumed; it just never resolves the DMA mapping for
 * a request the driver assumed (with no seg_max signal) it could send as
 * one segment. vblk_emmc.c had ALREADY found and fixed this exact class
 * of bug (see its own "SEG_MAX fix" comment on vblk_config_read()) --
 * this file simply never got that fix ported when it was written from
 * that same template. See sd-write-fixed-by-25mhz-reclock.md's sibling
 * memory entry for the live evidence (4 KiB writes always worked, 8 KiB
 * always wedged, on a fully quiescent virtio ring both times). */
#define VBLK_SD_FEATWORD_HI          1u
#define VBLK_SD_F_VERSION_1_BIT      0x1u
#define VBLK_SD_F_SEG_MAX_BIT        0x4u

#define VBLK_SD_QUEUE                0u
#define VBLK_SD_NUM_QUEUES           1u
#define VBLK_SD_QUEUE_MAX            256u
#define VBLK_SD_MAX_CHAIN            32u

#define VBLK_SD_VRING_DESC_F_NEXT     0x1u
#define VBLK_SD_VRING_DESC_F_WRITE    0x2u
#define VBLK_SD_VRING_DESC_F_INDIRECT 0x4u

#define VIRTIO_BLK_T_IN           0u
#define VIRTIO_BLK_T_OUT          1u
#define VIRTIO_BLK_T_FLUSH        4u
#define VIRTIO_BLK_T_GET_ID       8u
#define VIRTIO_BLK_S_OK           0u
#define VIRTIO_BLK_S_IOERR        1u
#define VIRTIO_BLK_S_UNSUPP       2u

#define VBLK_SD_ID_BYTES          20u
#define VBLK_SD_SECTOR_BYTES      512u

/* FALLBACK capacity only, as of 2026-08-25 — vblk_sd_config_read() now
 * advertises the REAL capacity parsed from the card's own CSD (see
 * sd_bio_capacity_sectors()); this constant is used only when sd_ready is
 * false (sd_bio_init() never succeeded), so the device still advertises
 * *something* sane rather than an uninitialized value. Kept deliberately
 * conservative for that fallback case, same reasoning as before. */
#define VBLK_SD_CAPACITY_SECTORS  ((uint64_t)1024 * 1024 * 1024 / VBLK_SD_SECTOR_BYTES)

/* ------------------------------------------------------------------ *
 * Split-virtqueue layout (VIRTIO 1.x, little-endian) — identical shape to
 * every other device here, duplicated for self-containment.
 * ------------------------------------------------------------------ */
struct vblk_sd_vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vblk_sd_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vblk_sd_vq {
	uint64_t desc, avail, used;
	uint32_t num;
	uint16_t last_avail;
	uint8_t  ready;
};

struct vblk_sd_dev {
	uint64_t base;
	uint32_t status;
	uint32_t dev_feat_sel, drv_feat_sel;
	uint64_t driver_features;
	uint32_t queue_sel;
	struct vblk_sd_vq vq[VBLK_SD_NUM_QUEUES];
	uint32_t int_status;
	uint32_t config_gen;
	uint32_t sd_ready;   /* 1 once sd_bio_init() succeeded */
};

/* purpose:     wire up the device: bring the SD card up (sd_bio_init()), zero
 *              state, publish the "VBSD" breadcrumb.
 * input:       none
 * output:      none
 * sideEffects: must run before stage2_init()/stage2_enable(), same ordering
 *              requirement as vblk_init()/vnet_init()/vinput_init()
 *              (main_dbg.c). Registers the device even if sd_bio_init()
 *              fails — every request then completes VIRTIO_BLK_S_IOERR,
 *              same "always register, report failure per-request" contract
 *              as vblk_emmc.c's own emmc_ready flag. */
void vblk_sd_init(void);

/* purpose:     el2_trap dispatch entry, same contract as every other
 *              *_mmio_fault() in this tree.
 * input:       frame — the trapping guest's exception frame
 * output:      1 if handled, 0 otherwise (not this device's window)
 * sideEffects: may emulate a register read/write, drain a request, or ack
 *              an IRQ */
int vblk_sd_mmio_fault(struct el2_frame *frame);

/* Cross-core lock against dbgmon's `sd` commands (CPU1) — see this file's
 * header for why. Mirrors vblk_emmc_trylock()/unlock()'s exact contract:
 * bracket any CPU1 `call`-driven sd_bio_read/write with these. */
int vblk_sd_trylock(void);
void vblk_sd_unlock(void);

#endif /* BZDOS_VBLK_SD_H */
