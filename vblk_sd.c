/* SPDX-License-Identifier: BSD-2-Clause */

/* vblk_sd.c — virtio-blk over the microSD card, see vblk_sd.h for the full
 * design, the honest list of what is deliberately leaner than vblk_emmc.c,
 * and the short list of things that are NOT skipped because skipping them
 * would be a correctness bug rather than a resilience tradeoff. Mirrors
 * vblk_emmc.c's virtqueue engine and serve_data() arithmetic throughout;
 * see that file for the original derivation and the hardware evidence
 * behind each fix being ported here.
 */
#include <stdint.h>
#include "vblk_sd.h"
#include "hv_addrmap.h"
#include "sd_bio.h"
#include "stage2.h"   /* STAGE2_DRAM_BASE/SIZE -- the one source for the guest DRAM window */
#include "wdt.h"       /* wdt_debug_kick() -- fed during a bounded lock wait */

/* ------------------------------------------------------------------ *
 * ESR_EL2.ISS decode — identical convention to every device in this tree.
 * ------------------------------------------------------------------ */
#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3Fu
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SAS_SHIFT     22
#define ESR_SAS_MASK      0x3u
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u

/* ------------------------------------------------------------------ *
 * Breadcrumb window (HVMAP_VBLK_SD_BC, hv_addrmap.h), magic "VBSD".
 *   [0] magic          [1] status reg        [2] queue ready
 *   [3] sd_ready        [4] reads             [5] writes
 *   [6] irqs            [7] faults            [8] gmem OOB rejects
 *   [9] partial-sector stitch events         [10] lock-wait give-ups (IOERR)
 *  [11] write-retry attempts                 [12] last LBA touched
 *  [13] read-retry attempts (added 2026-08-25, see sd_serve_data())
 * ------------------------------------------------------------------ */
#define VBLK_SD_BC_BASE   HVMAP_VBLK_SD_BC
#define VBLK_SD_BC_MAGIC  0x56425344u   /* "VBSD" */

static inline void vblk_sd_bc(uint32_t idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VBLK_SD_BC_BASE + idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static struct vblk_sd_dev g_sd_blk;
static uint32_t g_reads, g_writes, g_irqs, g_faults, g_gmem_oob;
static uint32_t g_stitch_events, g_lock_giveups, g_write_retries, g_read_retries;

/* ------------------------------------------------------------------ *
 * Cross-core lock vs dbgmon's `sd` commands on CPU1 (see vblk_sd.h). Same
 * ldaxr/stlxr test-and-set as vblk_emmc_trylock(), duplicated per this
 * file's self-containment convention -- it protects a DIFFERENT controller
 * (SMHC0, not SMHC2), so sharing the eMMC lock would be actively wrong
 * (would serialize two independent controllers against each other for no
 * reason, and would NOT protect against a concurrent `sd` command anyway).
 * ------------------------------------------------------------------ */
static inline volatile uint32_t *sd_lock_word(void)
{
	return (volatile uint32_t *)HVMAP_VBLK_SD_LOCK;
}

int vblk_sd_trylock(void)
{
	volatile uint32_t *p = sd_lock_word();
	uint32_t prev, status, one = 1u;

	__asm__ volatile(
		"	ldaxr	%w0, [%3]\n"
		"	cbnz	%w0, 1f\n"
		"	stlxr	%w1, %w2, [%3]\n"
		"	b	2f\n"
		"1:	mov	%w1, #1\n"
		"2:\n"
		: "=&r"(prev), "=&r"(status)
		: "r"(one), "r"(p)
		: "memory");
	if (prev == 0u && status == 0u) {
		__asm__ volatile("dsb sy" ::: "memory");
		return 1;
	}
	return 0;
}

void vblk_sd_unlock(void)
{
	volatile uint32_t *p = sd_lock_word();
	__asm__ volatile("dsb sy" ::: "memory");
	*p = 0u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

#define SD_LOCK_RETRIES 64u   /* bounded -- see vblk_sd.h: fail, don't stall forever */

static int sd_lock_acquire_bounded(void)
{
	uint32_t tries = 0;
	while (!vblk_sd_trylock()) {
		if (++tries >= SD_LOCK_RETRIES)
			return 0;
		wdt_debug_kick();
	}
	return 1;
}

/* ------------------------------------------------------------------ *
 * Guest-memory helpers -- byte-for-byte the same pattern as every other
 * device file here. Duplicated per this file's self-containment convention.
 * ------------------------------------------------------------------ */
/* Derived from stage2.h, not copied. These used to be hardcoded 0x40000000
 * pairs in five separate files (vblk_emmc.c, vblk_sd.c, vinput.c, vnet_emac.c
 * and scanout.c, the last under its own spelling), each carrying a comment
 * saying it MUST stay in lockstep with stage2.h -- which is a request, not a
 * mechanism. On 2026-08-27 STAGE2_DRAM_SIZE was widened to 2 GiB and none of
 * them followed: the guest addressed a buffer above 0x80000000, gpa_in_range()
 * rejected the descriptor as "outside DRAM", virtio-blk returned S_IOERR, and
 * the guest panicked with `Going nowhere without my init!` after two
 * `vtbd0: hard error` lines. Measured, not inferred -- g_gmem_oob and
 * g_ioerr_badpa both read 2.
 *
 * Same disease soc_a64.h was created to cure earlier the same day: one value,
 * several spellings, and changing one silently breaks the rest. Local names are
 * kept so no use site changes. */
#define GUEST_DRAM_BASE   ((uint64_t)STAGE2_DRAM_BASE)
#define GUEST_DRAM_SIZE   ((uint64_t)STAGE2_DRAM_SIZE)
#define GUEST_DRAM_END    (GUEST_DRAM_BASE + GUEST_DRAM_SIZE)

static inline int gpa_in_range(uint64_t gpa, uint32_t len)
{
	if (len == 0u)
		return 1;
	if ((uint64_t)len > GUEST_DRAM_SIZE)
		return 0;
	if (gpa < GUEST_DRAM_BASE || gpa >= GUEST_DRAM_END)
		return 0;
	if ((GUEST_DRAM_END - gpa) < (uint64_t)len)
		return 0;
	return 1;
}

static void gmem_cmo(uint64_t gpa, uint32_t len)
{
	uint64_t p   = gpa & ~63ULL;
	uint64_t end = gpa + len;
	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void gmem_read(uint64_t gpa, void *dst, uint32_t len)
{
	if (!gpa_in_range(gpa, len)) {
		vblk_sd_bc(8, ++g_gmem_oob);
		return;
	}
	const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)gpa;
	uint8_t *d = (uint8_t *)dst;
	gmem_cmo(gpa, len);
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
}

static void gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	if (!gpa_in_range(gpa, len)) {
		vblk_sd_bc(8, ++g_gmem_oob);
		return;
	}
	volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)gpa;
	const uint8_t *s = (const uint8_t *)src;
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	gmem_cmo(gpa, len);
}

static inline uint16_t gmem_ld16(uint64_t gpa)
{
	uint16_t v; gmem_read(gpa, &v, 2); return v;
}
static inline void gmem_st16(uint64_t gpa, uint16_t v)
{
	gmem_write(gpa, &v, 2);
}

/* ------------------------------------------------------------------ *
 * Virtqueue engine (split ring, VIRTIO 1.x) -- identical shape to every
 * other device here.
 * ------------------------------------------------------------------ */
static void vq_read_desc(struct vblk_sd_vq *vq, uint16_t idx, struct vblk_sd_desc *out)
{
	struct vblk_sd_vring_desc d;
	uint64_t p = vq->desc + (uint64_t)idx * sizeof(struct vblk_sd_vring_desc);
	gmem_read(p, &d, sizeof(d));
	out->addr  = d.addr;
	out->len   = d.len;
	out->flags = d.flags;
	out->next  = d.next;
}

static int vq_pop_avail(struct vblk_sd_vq *vq, uint16_t *head)
{
	uint16_t avail_idx;

	if (!vq->ready || vq->num == 0)
		return 0;

	avail_idx = gmem_ld16(vq->avail + 2u);
	if (vq->last_avail == avail_idx)
		return 0;

	{
		uint16_t slot = (uint16_t)(vq->last_avail % vq->num);
		*head = gmem_ld16(vq->avail + 4u + (uint64_t)slot * 2u);
	}
	vq->last_avail++;
	return 1;
}

static void vq_push_used(struct vblk_sd_vq *vq, uint16_t head, uint32_t used_len)
{
	uint16_t used_idx = gmem_ld16(vq->used + 2u);
	uint16_t slot = (uint16_t)(used_idx % vq->num);
	uint64_t e = vq->used + 4u + (uint64_t)slot * 8u;
	uint32_t id = head;
	uint16_t new_idx = (uint16_t)(used_idx + 1u);

	gmem_write(e + 0u, &id, 4u);
	gmem_write(e + 4u, &used_len, 4u);
	__asm__ volatile("dsb sy" ::: "memory");
	gmem_st16(vq->used + 2u, new_idx);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * IRQ injection (IMO=0) -- identical mechanism to every device here.
 * ------------------------------------------------------------------ */
static void vblk_sd_inject_irq(void)
{
	uint32_t intid = VBLK_SD_INTID;
	uint32_t word  = intid / 32u;
	uint32_t bit   = intid % 32u;
	volatile uint32_t *ispendr =
	    (volatile uint32_t *)(VBLK_SD_GICD_BASE + VBLK_SD_GICD_ISPENDR + word * 4u);

	__asm__ volatile("dsb sy" ::: "memory");
	*ispendr = (1u << bit);
	__asm__ volatile("dsb sy" ::: "memory");

	g_sd_blk.int_status |= VBLK_SD_INT_VRING;
	vblk_sd_bc(6, ++g_irqs);
}

/* ------------------------------------------------------------------ *
 * Request servicing.
 *
 * Bounce buffer: a plain EL2-private static buffer, deliberately its OWN
 * (not vblk_emmc.c's BOUNCE_PA) -- this device's requests are always
 * serviced synchronously in the guest's own trap, so there is no cross-
 * core aliasing risk with eMMC's async CPU2 path, but sharing the buffer
 * would still be a pointless, easy-to-get-wrong coupling between two
 * otherwise-independent devices for zero benefit.
 * ------------------------------------------------------------------ */
static uint64_t g_bounce_q[VBLK_SD_SECTOR_BYTES / 8];
#define SD_BOUNCE_PA ((uint64_t)(uintptr_t)&g_bounce_q[0])

/* Return codes serve_data() itself can produce, kept disjoint from
 * VIRTIO_BLK_S_* since the caller maps them, same pattern as vblk_emmc.c. */
#define SD_RC_OK        0
#define SD_RC_BADPA    -1
#define SD_RC_BUSY     -2
#define SD_RC_IOERR    -3

/* Mirrors vblk_emmc.c's serve_data(): walks one data descriptor's worth of
 * bytes, `chunk` at a time, stitching across the 512-byte sector boundary
 * exactly like it does -- see vblk_sd.h's header for why this arithmetic is
 * NOT optional. `*sector`/`*sector_fill` are the caller's running position,
 * carried across calls for a multi-descriptor request. */
static int sd_serve_data(uint32_t is_read, uint64_t gpa, uint32_t len,
                          uint64_t *sector, uint32_t *sector_fill)
{
	uint32_t done = 0;

	while (done < len) {
		uint32_t off = *sector_fill;
		uint32_t chunk = VBLK_SD_SECTOR_BYTES - off;
		uint32_t lba = (uint32_t)*sector;
		uint64_t buf_gpa = gpa + done;
		uint32_t whole;
		int rc;

		if (chunk > len - done)
			chunk = len - done;
		whole = (off == 0u && chunk == VBLK_SD_SECTOR_BYTES);
		if (!whole)
			vblk_sd_bc(9, ++g_stitch_events);

		if (!gpa_in_range(buf_gpa, chunk)) {
			g_gmem_oob++;
			vblk_sd_bc(8, g_gmem_oob);
			return SD_RC_BADPA;
		}

		if (!sd_lock_acquire_bounded()) {
			vblk_sd_bc(10, ++g_lock_giveups);
			return SD_RC_BUSY;
		}

		vblk_sd_bc(12, lba);

		if (is_read) {
			rc = whole ? sd_bio_read(lba, buf_gpa)
			           : sd_bio_read(lba, SD_BOUNCE_PA);
			/* CONFIRMED live 2026-08-25: sd_bio_read() had ZERO retry here,
			 * asymmetric with the write path's existing bounded retry below
			 * -- and a real hard read error (FreeBSD's own dmesg: "vtbd1:
			 * hard error cmd=read", errno EIO then ENXIO) forced an unmount
			 * of /opt during a background fsck's first-ever exhaustive scan
			 * of the SD card's full LBA range (earlier testing this session
			 * only ever exercised a small, hand-picked set of LBAs, never a
			 * broad sweep). Same SMHC IP block as the eMMC sibling that DOES
			 * have a documented retryable transient stall -- there is no
			 * reason reads would be immune when writes are not. Mirror the
			 * write path's exact retry shape. */
			if (rc != 0) {
				int tries;
				for (tries = 0; tries < 3 && rc != 0; tries++) {
					vblk_sd_bc(13, ++g_read_retries);
					wdt_debug_kick();
					rc = whole ? sd_bio_read(lba, buf_gpa)
					           : sd_bio_read(lba, SD_BOUNCE_PA);
				}
			}
			if (rc == 0) {
				if (whole)
					gmem_cmo(buf_gpa, VBLK_SD_SECTOR_BYTES);
				else
					gmem_write(buf_gpa, (uint8_t *)g_bounce_q + off, chunk);
			}
		} else {
			if (!whole) {
				/* Partial write MUST read-modify-write, or the bytes
				 * outside [off, off+chunk) are destroyed -- the exact
				 * silent-corruption case vblk_emmc.c's own comment names.
				 * Same retry gap and fix as the plain-read path above. */
				rc = sd_bio_read(lba, SD_BOUNCE_PA);
				if (rc != 0) {
					int tries;
					for (tries = 0; tries < 3 && rc != 0; tries++) {
						vblk_sd_bc(13, ++g_read_retries);
						wdt_debug_kick();
						rc = sd_bio_read(lba, SD_BOUNCE_PA);
					}
				}
				if (rc != 0) {
					vblk_sd_unlock();
					return SD_RC_IOERR;
				}
			}
			gmem_read(buf_gpa, (uint8_t *)g_bounce_q + off, chunk);
			__asm__ volatile("dsb sy" ::: "memory");
			rc = sd_bio_write(lba, SD_BOUNCE_PA);
			/* Bounded retry: sd_bio_write() is now confirmed reliable on
			 * silicon (2026-08-24, after sd_bio.c's 25 MHz reclock -- see
			 * sd_bio_set_highspeed()), but it shares the same SMHC IP block
			 * whose eMMC sibling DOES exhibit a real, retryable transient
			 * stall on this exact hardware family -- keep the retry rather
			 * than assume SD can't have the same class of hiccup. */
			if (rc != 0) {
				int tries;
				for (tries = 0; tries < 3 && rc != 0; tries++) {
					vblk_sd_bc(11, ++g_write_retries);
					wdt_debug_kick();
					rc = sd_bio_write(lba, SD_BOUNCE_PA);
				}
			}
		}

		vblk_sd_unlock();
		if (rc != 0)
			return SD_RC_IOERR;

		done += chunk;
		off += chunk;
		if (off >= VBLK_SD_SECTOR_BYTES) {
			off = 0;
			(*sector)++;
		}
		*sector_fill = off;
	}
	return SD_RC_OK;
}

static void vblk_sd_request(struct vblk_sd_dev *d, uint16_t head)
{
	struct vblk_sd_vq *vq = &d->vq[VBLK_SD_QUEUE];
	struct vblk_sd_desc chain[VBLK_SD_MAX_CHAIN];
	uint32_t n = 0;
	uint16_t idx = head;

	for (;;) {
		if (n >= VBLK_SD_MAX_CHAIN) {
			/* CONFIRMED live 2026-08-24: this path (and the two below) used
			 * to push_used WITHOUT injecting an IRQ. FreeBSD's vtblk driver
			 * waits on the completion purely interrupt-driven (biowait()) --
			 * newfs's larger, multi-descriptor writes hit this path (unlike
			 * dd's small single-descriptor ones), so the bio NEVER completed
			 * and the process wedged forever in an uninterruptible sleep
			 * (procstat -kk: sys_pwrite -> physio -> biowait -> _sleep). See
			 * vblk_emmc.c's vblk_request()/its caller: EVERY exit path there
			 * is treated as "served" and gets exactly one inject_irq() call.
			 * Mirror that contract here. */
			vq_push_used(vq, head, 0);
			vblk_sd_inject_irq();
			return;
		}
		vq_read_desc(vq, idx, &chain[n]);
		if (chain[n].flags & VBLK_SD_VRING_DESC_F_INDIRECT) {
			vq_push_used(vq, head, 0);
			vblk_sd_inject_irq();
			return;
		}
		uint16_t flags = chain[n].flags;
		uint16_t next  = chain[n].next;
		n++;
		if (!(flags & VBLK_SD_VRING_DESC_F_NEXT))
			break;
		idx = next;
	}
	if (n < 2) {
		vq_push_used(vq, head, 0);
		vblk_sd_inject_irq();
		return;
	}

	struct { uint32_t type; uint32_t reserved; uint64_t sector; } hdr;
	gmem_read(chain[0].addr, &hdr, sizeof(hdr));

	struct vblk_sd_desc *stdesc = &chain[n - 1];
	uint8_t  status   = VIRTIO_BLK_S_OK;
	uint32_t used_len = 0;
	uint64_t sector   = hdr.sector;
	uint32_t sfill    = 0;

	if (!d->sd_ready) {
		status = VIRTIO_BLK_S_IOERR;
	} else if (hdr.type == VIRTIO_BLK_T_IN || hdr.type == VIRTIO_BLK_T_OUT) {
		uint32_t is_read = (hdr.type == VIRTIO_BLK_T_IN);
		uint32_t i;

		for (i = 1; i < n - 1; i++) {
			int rc = sd_serve_data(is_read, chain[i].addr, chain[i].len,
			                       &sector, &sfill);
			if (rc != SD_RC_OK) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			if (is_read)
				used_len += chain[i].len;
		}
		if (status == VIRTIO_BLK_S_OK)
			vblk_sd_bc(is_read ? 4 : 5, is_read ? ++g_reads : ++g_writes);
	} else if (hdr.type == VIRTIO_BLK_T_GET_ID) {
		static const char id[] = "bzdk-sd0";
		uint8_t buf[VBLK_SD_ID_BYTES];
		uint32_t i;
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = (i < sizeof(id)) ? (uint8_t)id[i] : 0;
		gmem_write(chain[1].addr, buf, sizeof(buf));
		used_len = sizeof(buf);
	} else if (hdr.type == VIRTIO_BLK_T_FLUSH) {
		/* Nothing buffered on our side to flush -- every write above is
		 * already synchronous. */
	} else {
		status = VIRTIO_BLK_S_UNSUPP;
	}

	gmem_write(stdesc->addr, &status, 1);
	vq_push_used(vq, head, used_len);
	vblk_sd_inject_irq();
}

/* ------------------------------------------------------------------ *
 * MMIO register emulation.
 * ------------------------------------------------------------------ */
static uint32_t vblk_sd_reg_read(struct vblk_sd_dev *d, uint32_t off)
{
	switch (off) {
	case VBLK_SD_R_MAGIC_VALUE: return VBLK_SD_MMIO_MAGIC;
	case VBLK_SD_R_VERSION:     return VBLK_SD_MMIO_VERSION;
	case VBLK_SD_R_DEVICE_ID:   return VBLK_SD_DEVICE_ID;
	case VBLK_SD_R_VENDOR_ID:   return VBLK_SD_MMIO_VENDOR;
	case VBLK_SD_R_DEVICE_FEATURES:
		if (d->dev_feat_sel == VBLK_SD_FEATWORD_HI)
			return VBLK_SD_F_VERSION_1_BIT;
		return VBLK_SD_F_SEG_MAX_BIT;
	case VBLK_SD_R_QUEUE_NUM_MAX:
		return (d->queue_sel < VBLK_SD_NUM_QUEUES) ? VBLK_SD_QUEUE_MAX : 0u;
	case VBLK_SD_R_QUEUE_READY:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES)
			return d->vq[d->queue_sel].ready;
		return 0u;
	case VBLK_SD_R_INTERRUPT_STATUS: return d->int_status;
	case VBLK_SD_R_STATUS:        return d->status;
	case VBLK_SD_R_CONFIG_GENERATION: return d->config_gen;
	default:
		return 0u;
	}
}

/* struct virtio_blk_config: `capacity` (u64 LE) at offset 0, and (since the
 * SEG_MAX fix) `seg_max` (le32 at offset 12 — offset 8..11 is `size_max`,
 * left 0 since VIRTIO_BLK_F_SIZE_MAX isn't offered). Mirrors vblk_emmc.c's
 * vblk_config_read() exactly -- see VBLK_SD_F_SEG_MAX_BIT's header comment
 * for why this field must exist. */
#define VBLK_SD_CONFIG_SEG_MAX  16u   /* data segments/request; well under VBLK_SD_MAX_CHAIN */

static uint32_t vblk_sd_config_read(uint32_t byte_off, uint32_t sas)
{
	/* Real capacity (2026-08-25), parsed from the card's own CSD by
	 * sd_bio_init() -- see sd_bio_capacity_sectors()'s own comment. Falls
	 * back to VBLK_SD_CAPACITY_SECTORS's old conservative 1 GiB stub
	 * whenever sd_ready is false (init never succeeded) or CSD parsing
	 * itself fell back, so this can never claim more space than the card
	 * actually has. */
	uint64_t cap = g_sd_blk.sd_ready ? sd_bio_capacity_sectors()
	                                 : VBLK_SD_CAPACITY_SECTORS;
	uint32_t nbytes = (sas == 0u) ? 1u : (sas == 1u) ? 2u : 4u;
	uint32_t v = 0;

	for (uint32_t i = 0; i < nbytes; i++) {
		uint32_t idx = byte_off + i;
		uint8_t b;
		if (idx < 8u)
			b = (uint8_t)(cap >> (8u * idx));
		else if (idx >= 12u && idx < 16u)
			b = (uint8_t)(VBLK_SD_CONFIG_SEG_MAX >> (8u * (idx - 12u)));
		else
			b = 0u;
		v |= (uint32_t)b << (8u * i);
	}
	return v;
}

static void vblk_sd_reg_write(struct vblk_sd_dev *d, uint32_t off, uint32_t val)
{
	switch (off) {
	case VBLK_SD_R_DEVICE_FEATURES_SEL: d->dev_feat_sel = val; break;
	case VBLK_SD_R_DRIVER_FEATURES_SEL: d->drv_feat_sel = val; break;
	case VBLK_SD_R_DRIVER_FEATURES:
		if (d->drv_feat_sel < 2u)
			d->driver_features |= ((uint64_t)val) << (32u * d->drv_feat_sel);
		break;
	case VBLK_SD_R_QUEUE_SEL:
		d->queue_sel = val;
		break;
	case VBLK_SD_R_QUEUE_NUM:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES) {
			struct vblk_sd_vq *vq = &d->vq[d->queue_sel];
			uint32_t v = (val > VBLK_SD_QUEUE_MAX) ? VBLK_SD_QUEUE_MAX : val;
			if (v != 0u && (v & (v - 1u)) != 0u)
				break;
			vq->num = v;
		}
		break;
	case VBLK_SD_R_QUEUE_DESC_LOW:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES)
			d->vq[d->queue_sel].desc =
			    (d->vq[d->queue_sel].desc & ~0xFFFFFFFFULL) | val;
		break;
	case VBLK_SD_R_QUEUE_DESC_HIGH:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES)
			d->vq[d->queue_sel].desc =
			    (d->vq[d->queue_sel].desc & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VBLK_SD_R_QUEUE_DRIVER_LOW:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES)
			d->vq[d->queue_sel].avail =
			    (d->vq[d->queue_sel].avail & ~0xFFFFFFFFULL) | val;
		break;
	case VBLK_SD_R_QUEUE_DRIVER_HIGH:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES)
			d->vq[d->queue_sel].avail =
			    (d->vq[d->queue_sel].avail & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VBLK_SD_R_QUEUE_DEVICE_LOW:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES)
			d->vq[d->queue_sel].used =
			    (d->vq[d->queue_sel].used & ~0xFFFFFFFFULL) | val;
		break;
	case VBLK_SD_R_QUEUE_DEVICE_HIGH:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES)
			d->vq[d->queue_sel].used =
			    (d->vq[d->queue_sel].used & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VBLK_SD_R_QUEUE_READY:
		if (d->queue_sel < VBLK_SD_NUM_QUEUES) {
			struct vblk_sd_vq *vq = &d->vq[d->queue_sel];
			if (val & 1u) {
				vq->last_avail = 0;
				__asm__ volatile("dsb sy" ::: "memory");
				vq->ready = 1u;
				__asm__ volatile("dsb sy" ::: "memory");
				vblk_sd_bc(2, 1);
			} else {
				vq->ready = 0u;
				vblk_sd_bc(2, 0);
			}
		}
		break;
	case VBLK_SD_R_QUEUE_NOTIFY:
		if (val == VBLK_SD_QUEUE) {
			struct vblk_sd_vq *vq = &d->vq[VBLK_SD_QUEUE];
			uint16_t head;
			while (vq_pop_avail(vq, &head))
				vblk_sd_request(d, head);
		}
		break;
	case VBLK_SD_R_INTERRUPT_ACK:
		d->int_status &= ~val;
		break;
	case VBLK_SD_R_STATUS:
		d->status = val;
		vblk_sd_bc(1, val);
		if (val == 0u) {
			for (uint32_t q = 0; q < VBLK_SD_NUM_QUEUES; q++) {
				struct vblk_sd_vq *vq = &d->vq[q];
				vq->ready = 0; vq->num = 0; vq->last_avail = 0;
				vq->desc = vq->avail = vq->used = 0;
			}
			d->int_status = 0;
			vblk_sd_bc(2, 0);
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ *
 * el2_trap dispatch entry -- same contract as every other *_mmio_fault()
 * in this tree.
 * ------------------------------------------------------------------ */
int vblk_sd_mmio_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec  = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	if (addr < g_sd_blk.base || addr >= g_sd_blk.base + VBLK_SD_MMIO_SIZE)
		return 0;

	vblk_sd_bc(7, ++g_faults);

	uint32_t isv = esr & ESR_ISV_BIT;
	if (!isv) {
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;
	uint32_t off = (uint32_t)(addr - g_sd_blk.base);

	if (wnr) {
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];
		vblk_sd_reg_write(&g_sd_blk, off, (uint32_t)val);
	} else {
		uint32_t val = (off >= VBLK_SD_R_CONFIG && off < VBLK_SD_R_CONFIG + 16u)
		             ? vblk_sd_config_read(off - VBLK_SD_R_CONFIG, sas)
		             : vblk_sd_reg_read(&g_sd_blk, off);
		if (srt != SRT_XZR)
			frame->x[srt] = (uint64_t)val;
	}

	frame->elr += 4;
	return 1;
}

/* ------------------------------------------------------------------ *
 * Init.
 * ------------------------------------------------------------------ */
void vblk_sd_init(void)
{
	int rc;

	for (uint32_t i = 0; i < sizeof(g_sd_blk); i++)
		((uint8_t *)&g_sd_blk)[i] = 0;
	*sd_lock_word() = 0u;

	g_sd_blk.base = VBLK_SD_MMIO_BASE;
	g_reads = g_writes = g_irqs = g_faults = g_gmem_oob = 0;
	g_stitch_events = g_lock_giveups = g_write_retries = g_read_retries = 0;

	rc = sd_bio_init();
	g_sd_blk.sd_ready = (rc == 0);

	vblk_sd_bc(0, VBLK_SD_BC_MAGIC);
	vblk_sd_bc(2, 0);
	vblk_sd_bc(3, g_sd_blk.sd_ready);
}
