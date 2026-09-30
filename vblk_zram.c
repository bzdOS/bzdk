/* SPDX-License-Identifier: BSD-2-Clause */

/* vblk_zram.c — compressed-RAM virtio-blk. See vblk_zram.h for the design
 * (backing slice, geometry and growth rule, the one lock). The virtio-mmio
 * register model is vblk_sd.c's, duplicated per this tree's
 * one-file-per-device convention; the storage underneath is vzram_pool.c
 * (hosted-tested, test_vzram_pool.c) over lz4.c (test_lz4.c).
 *
 * Requests are served synchronously inside the notifying vCPU's trap --
 * there is no controller to wait for, only CPU work (LZ4 at a few hundred
 * MB/s), so there is nothing to overlap. */
#include <stdint.h>
#include "vblk_zram.h"
#include "vblk_sd.h"      /* the shared virtio-mmio register/feature numbers */
#include "vzram_pool.h"
#include "hv_addrmap.h"
#include "stage2.h"
#include "soc_a64.h"

/* ESR_EL2 field decode -- per-file, as in every sibling device. */
#define ESR_EC_SHIFT      26u
#define ESR_EC_MASK       0x3Fu
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SAS_SHIFT     22u
#define ESR_SAS_MASK      0x3u
#define ESR_SRT_SHIFT     16u
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u

_Static_assert(sizeof(vzram_slot_t) == 8u, "VZRAM_SLOTS_BYTES assumes 8");
_Static_assert(VZRAM_SLOTS_BYTES < HVMAP_VZRAM_SIZE, "slot table fills the slice");
_Static_assert(VZRAM_PAGES_INITIAL <= VZRAM_PAGES_MAX, "initial above ceiling");

#define ZR_POOL_PA    (HVMAP_VZRAM_BASE + VZRAM_SLOTS_BYTES)
#define ZR_POOL_BYTES ((uint32_t)(HVMAP_VZRAM_SIZE - VZRAM_SLOTS_BYTES))

#define ZR_INT_VRING   0x1u
#define ZR_INT_CONFIG  0x2u
#define ZR_SECTOR      512u
#define ZR_SEG_MAX     16u
#define ZR_QUEUE_MAX   256u
#define ZR_MAX_CHAIN   32u

struct zr_vq {
	uint64_t desc, avail, used;
	uint32_t num;
	uint16_t last_avail;
	uint8_t  ready;
};

static struct {
	uint32_t status, dev_feat_sel, drv_feat_sel, queue_sel;
	uint64_t driver_features;
	struct zr_vq vq;
	uint32_t int_status, config_gen;
} g_zr;

static vzram_pool_t g_zpool;
static uint16_t g_zr_isr_scan_used_idx;
static volatile uint32_t g_zr_lock;

/* Read over the debug channel via nm. */
uint32_t g_zr_reads, g_zr_writes, g_zr_errs, g_zr_grows, g_zr_used_pages,
         g_zr_irq_rearms, g_zr_faults;

/* ---- guest memory: same helpers as vblk_sd.c ---------------------- */
#define GUEST_DRAM_BASE   ((uint64_t)STAGE2_DRAM_BASE)
#define GUEST_DRAM_END    (GUEST_DRAM_BASE + (uint64_t)STAGE2_DRAM_SIZE)

static inline int gpa_in_range(uint64_t gpa, uint32_t len)
{
	if (gpa < GUEST_DRAM_BASE || gpa >= GUEST_DRAM_END)
		return 0;
	if ((GUEST_DRAM_END - gpa) < (uint64_t)len)
		return 0;
	/* Never let a descriptor point the device at its own backing store. */
	if (gpa < HVMAP_VZRAM_BASE + HVMAP_VZRAM_SIZE &&
	    gpa + len > HVMAP_VZRAM_BASE)
		return 0;
	return 1;
}

static void gmem_cmo(uint64_t gpa, uint32_t len)
{
	uint64_t p = gpa & ~63ULL, end = gpa + len;

	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int gmem_read(uint64_t gpa, void *dst, uint32_t len)
{
	const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)gpa;
	uint8_t *d = dst;

	if (!gpa_in_range(gpa, len))
		return -1;
	gmem_cmo(gpa, len);
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	return 0;
}

static int gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)gpa;
	const uint8_t *s = src;

	if (!gpa_in_range(gpa, len))
		return -1;
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	gmem_cmo(gpa, len);
	return 0;
}

static inline uint16_t gmem_ld16(uint64_t gpa)
{
	uint16_t v = 0;
	(void)gmem_read(gpa, &v, 2);
	return v;
}

static inline void gmem_st16(uint64_t gpa, uint16_t v)
{
	(void)gmem_write(gpa, &v, 2);
}

/* ---- lock ---------------------------------------------------------- */
static int zr_trylock(void)
{
	uint32_t prev, status, one = 1u;

	__asm__ volatile(
		"	ldaxr	%w0, [%3]\n"
		"	cbnz	%w0, 1f\n"
		"	stlxr	%w1, %w2, [%3]\n"
		"	b	2f\n"
		"1:	mov	%w1, #1\n"
		"2:\n"
		: "=&r"(prev), "=&r"(status)
		: "r"(one), "r"(&g_zr_lock)
		: "memory");
	if (prev == 0u && status == 0u) {
		__asm__ volatile("dsb sy" ::: "memory");
		return 1;
	}
	return 0;
}

/* Holders only ever do CPU work on at most one request chain (bounded by
 * ZR_MAX_CHAIN x 4 KiB of LZ4), so plain spinning is enough. */
static void zr_lock(void)
{
	while (!zr_trylock())
		__asm__ volatile("yield" ::: "memory");
}

static void zr_unlock(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	g_zr_lock = 0u;
	__asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* ---- virtqueue ----------------------------------------------------- */
struct zr_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

static void vq_read_desc(uint16_t idx, struct zr_desc *out)
{
	(void)gmem_read(g_zr.vq.desc + (uint64_t)idx * 16u, out, 16u);
}

static int vq_pop_avail(uint16_t *head)
{
	struct zr_vq *vq = &g_zr.vq;
	uint16_t avail_idx;

	if (!vq->ready || vq->num == 0)
		return 0;
	avail_idx = gmem_ld16(vq->avail + 2u);
	if (vq->last_avail == avail_idx)
		return 0;
	*head = gmem_ld16(vq->avail + 4u +
	                  (uint64_t)(vq->last_avail % vq->num) * 2u);
	vq->last_avail++;
	return 1;
}

static inline uint16_t zr_used_idx(void)
{
	return g_zr.vq.used ? gmem_ld16(g_zr.vq.used + 2u) : 0u;
}

static void zr_inject(uint32_t why)
{
	volatile uint32_t *ispendr = (volatile uint32_t *)(SOC_A64_GICD_BASE +
	    0x200u + (VBLK_ZRAM_INTID / 32u) * 4u);

	g_zr.int_status |= why;
	__asm__ volatile("dsb sy" ::: "memory");
	*ispendr = 1u << (VBLK_ZRAM_INTID % 32u);
	__asm__ volatile("dsb sy" ::: "memory");
}

static void zr_complete(uint16_t head, uint32_t used_len)
{
	struct zr_vq *vq = &g_zr.vq;
	uint16_t used_idx, slot;
	uint64_t e;
	uint32_t id = head;

	if (vq->num == 0 || vq->used == 0)
		return;
	used_idx = gmem_ld16(vq->used + 2u);
	slot = (uint16_t)(used_idx % vq->num);
	e = vq->used + 4u + (uint64_t)slot * 8u;
	(void)gmem_write(e, &id, 4u);
	(void)gmem_write(e + 4u, &used_len, 4u);
	__asm__ volatile("dsb sy" ::: "memory");
	gmem_st16(vq->used + 2u, (uint16_t)(used_idx + 1u));
	__asm__ volatile("dsb sy" ::: "memory");
	zr_inject(ZR_INT_VRING);
}

/* ---- storage ------------------------------------------------------- */
static uint8_t g_zr_page[VZRAM_PAGE] __attribute__((aligned(64)));

/* Growth: see vblk_zram.h. Called after a write, lock held. */
static void zr_maybe_grow(void)
{
	uint32_t n = g_zpool.nslots;

	if (n >= VZRAM_PAGES_MAX)
		return;
	if ((uint64_t)g_zr_used_pages * 4u < (uint64_t)n * 3u)
		return;
	if ((uint64_t)g_zpool.bump_next * 2u > ZR_POOL_BYTES)
		return;
	n += VZRAM_PAGES_STEP;
	if (n > VZRAM_PAGES_MAX)
		n = VZRAM_PAGES_MAX;
	if (vzram_pool_grow(&g_zpool, n) != 0)
		return;
	g_zr.config_gen++;
	g_zr_grows++;
	zr_inject(ZR_INT_CONFIG);
}

static int zr_page_write(uint32_t page, const uint8_t *src)
{
	int was_empty = g_zpool.slots[page].off == VZRAM_NONE;

	if (vzram_page_write(&g_zpool, page, src) != 0)
		return -1;
	if (was_empty)
		g_zr_used_pages++;
	return 0;
}

/* Move `len` bytes between guest `gpa` and logical byte offset `pos`. */
static int zr_serve(uint32_t is_read, uint64_t gpa, uint32_t len, uint64_t *pos)
{
	while (len) {
		uint32_t page = (uint32_t)(*pos / VZRAM_PAGE);
		uint32_t poff = (uint32_t)(*pos % VZRAM_PAGE);
		uint32_t n = VZRAM_PAGE - poff;

		if (n > len)
			n = len;
		if (page >= g_zpool.nslots)
			return -1;
		if (is_read) {
			if (vzram_page_read(&g_zpool, page, g_zr_page) != 0 ||
			    gmem_write(gpa, g_zr_page + poff, n) != 0)
				return -1;
		} else {
			/* A partial page keeps the bytes around it. */
			if (n != VZRAM_PAGE &&
			    vzram_page_read(&g_zpool, page, g_zr_page) != 0)
				return -1;
			if (gmem_read(gpa, g_zr_page + poff, n) != 0 ||
			    zr_page_write(page, g_zr_page) != 0)
				return -1;
		}
		gpa += n;
		*pos += n;
		len -= n;
	}
	return 0;
}

static void zr_request(uint16_t head)
{
	struct zr_desc chain[ZR_MAX_CHAIN];
	struct { uint32_t type; uint32_t reserved; uint64_t sector; } hdr;
	uint32_t n = 0, used_len = 0, i;
	uint16_t idx = head;
	uint8_t status = VIRTIO_BLK_S_OK;
	uint64_t pos;

	for (;;) {
		if (n >= ZR_MAX_CHAIN) {
			zr_complete(head, 0);
			return;
		}
		vq_read_desc(idx, &chain[n]);
		if (chain[n].flags & VBLK_SD_VRING_DESC_F_INDIRECT) {
			zr_complete(head, 0);
			return;
		}
		if (!(chain[n++].flags & VBLK_SD_VRING_DESC_F_NEXT))
			break;
		idx = chain[n - 1].next;
	}
	if (n < 2 || gmem_read(chain[0].addr, &hdr, sizeof(hdr)) != 0) {
		zr_complete(head, 0);
		return;
	}

	pos = hdr.sector * ZR_SECTOR;
	if (hdr.type == VIRTIO_BLK_T_IN || hdr.type == VIRTIO_BLK_T_OUT) {
		uint32_t is_read = hdr.type == VIRTIO_BLK_T_IN;

		for (i = 1; i < n - 1; i++) {
			if (zr_serve(is_read, chain[i].addr, chain[i].len, &pos) != 0) {
				status = VIRTIO_BLK_S_IOERR;
				g_zr_errs++;
				break;
			}
			if (is_read)
				used_len += chain[i].len;
		}
		if (status == VIRTIO_BLK_S_OK) {
			if (is_read) {
				g_zr_reads++;
			} else {
				g_zr_writes++;
				zr_maybe_grow();
			}
		}
	} else if (hdr.type == VIRTIO_BLK_T_GET_ID) {
		static const char id[VBLK_SD_ID_BYTES] = "bzdk-zram0";
		uint32_t cpy = chain[1].len < sizeof(id) ? chain[1].len : sizeof(id);

		if (n >= 3 && gmem_write(chain[1].addr, id, cpy) == 0)
			used_len = cpy;
	} else if (hdr.type != VIRTIO_BLK_T_FLUSH) {
		status = VIRTIO_BLK_S_UNSUPP;   /* nothing to flush: it is RAM */
	}

	(void)gmem_write(chain[n - 1].addr, &status, 1);
	zr_complete(head, used_len);
}

/* ---- registers ----------------------------------------------------- */
static uint32_t zr_config_read(uint32_t byte_off, uint32_t sas)
{
	uint64_t cap = (uint64_t)g_zpool.nslots * (VZRAM_PAGE / ZR_SECTOR);
	uint32_t nbytes = (sas == 0u) ? 1u : (sas == 1u) ? 2u : 4u;
	uint32_t v = 0;

	for (uint32_t i = 0; i < nbytes; i++) {
		uint32_t k = byte_off + i;
		uint8_t b = 0;

		if (k < 8u)
			b = (uint8_t)(cap >> (8u * k));
		else if (k >= 12u && k < 16u)
			b = (uint8_t)(ZR_SEG_MAX >> (8u * (k - 12u)));
		v |= (uint32_t)b << (8u * i);
	}
	return v;
}

static uint32_t zr_reg_read(uint32_t off)
{
	switch (off) {
	case VBLK_SD_R_MAGIC_VALUE:   return VBLK_SD_MMIO_MAGIC;
	case VBLK_SD_R_VERSION:       return VBLK_SD_MMIO_VERSION;
	case VBLK_SD_R_DEVICE_ID:     return VBLK_SD_DEVICE_ID;
	case VBLK_SD_R_VENDOR_ID:     return 0x627A7A72u;   /* "rzzb" */
	case VBLK_SD_R_DEVICE_FEATURES:
		return g_zr.dev_feat_sel == VBLK_SD_FEATWORD_HI ?
		       VBLK_SD_F_VERSION_1_BIT : VBLK_SD_F_SEG_MAX_BIT;
	case VBLK_SD_R_QUEUE_NUM_MAX:
		return g_zr.queue_sel == 0u ? ZR_QUEUE_MAX : 0u;
	case VBLK_SD_R_QUEUE_READY:
		return g_zr.queue_sel == 0u ? g_zr.vq.ready : 0u;
	case VBLK_SD_R_INTERRUPT_STATUS:
		g_zr_isr_scan_used_idx = zr_used_idx();
		return g_zr.int_status;
	case VBLK_SD_R_STATUS:            return g_zr.status;
	case VBLK_SD_R_CONFIG_GENERATION: return g_zr.config_gen;
	default:                          return 0u;
	}
}

static void set_lo(uint64_t *r, uint32_t v) { *r = (*r & ~0xFFFFFFFFULL) | v; }
static void set_hi(uint64_t *r, uint32_t v)
{
	*r = (*r & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
}

static void zr_reg_write(uint32_t off, uint32_t val)
{
	struct zr_vq *vq = &g_zr.vq;
	uint16_t head;

	if (off >= VBLK_SD_R_QUEUE_NUM && off <= VBLK_SD_R_QUEUE_DEVICE_HIGH &&
	    off != VBLK_SD_R_QUEUE_NOTIFY && off != VBLK_SD_R_INTERRUPT_ACK &&
	    off != VBLK_SD_R_STATUS && g_zr.queue_sel != 0u)
		return;                   /* one queue only */

	switch (off) {
	case VBLK_SD_R_DEVICE_FEATURES_SEL: g_zr.dev_feat_sel = val; break;
	case VBLK_SD_R_DRIVER_FEATURES_SEL: g_zr.drv_feat_sel = val; break;
	case VBLK_SD_R_DRIVER_FEATURES:
		if (g_zr.drv_feat_sel < 2u)
			g_zr.driver_features |= (uint64_t)val << (32u * g_zr.drv_feat_sel);
		break;
	case VBLK_SD_R_QUEUE_SEL: g_zr.queue_sel = val; break;
	case VBLK_SD_R_QUEUE_NUM: {
		uint32_t v = val > ZR_QUEUE_MAX ? ZR_QUEUE_MAX : val;
		if (v == 0u || (v & (v - 1u)) == 0u)
			vq->num = v;
		break;
	}
	case VBLK_SD_R_QUEUE_DESC_LOW:    set_lo(&vq->desc, val);  break;
	case VBLK_SD_R_QUEUE_DESC_HIGH:   set_hi(&vq->desc, val);  break;
	case VBLK_SD_R_QUEUE_DRIVER_LOW:  set_lo(&vq->avail, val); break;
	case VBLK_SD_R_QUEUE_DRIVER_HIGH: set_hi(&vq->avail, val); break;
	case VBLK_SD_R_QUEUE_DEVICE_LOW:  set_lo(&vq->used, val);  break;
	case VBLK_SD_R_QUEUE_DEVICE_HIGH: set_hi(&vq->used, val);  break;
	case VBLK_SD_R_QUEUE_READY:
		if (val & 1u)
			vq->last_avail = 0;
		__asm__ volatile("dsb sy" ::: "memory");
		vq->ready = (val & 1u) ? 1u : 0u;
		break;
	case VBLK_SD_R_QUEUE_NOTIFY:
		if (val == 0u)
			while (vq_pop_avail(&head))
				zr_request(head);
		break;
	case VBLK_SD_R_INTERRUPT_ACK:
		/* vblk_emmc.c's LOST-COMPLETION FIX: re-notify if the ring moved
		 * after the ISR sampled InterruptStatus. */
		g_zr.int_status &= ~val;
		if ((val & ZR_INT_VRING) && zr_used_idx() != g_zr_isr_scan_used_idx) {
			zr_inject(ZR_INT_VRING);
			g_zr_irq_rearms++;
		}
		break;
	case VBLK_SD_R_STATUS:
		g_zr.status = val;
		if (val == 0u) {
			vq->ready = 0; vq->num = 0; vq->last_avail = 0;
			vq->desc = vq->avail = vq->used = 0;
			g_zr.int_status = 0;
			g_zr.driver_features = 0;
		}
		break;
	default:
		break;
	}
}

int vblk_zram_mmio_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint64_t hpfar, addr;
	uint32_t off, srt, sas;

	if (((esr >> ESR_EC_SHIFT) & ESR_EC_MASK) != ESR_EC_DABT_LOWER)
		return 0;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);
	if (addr < VBLK_ZRAM_MMIO_BASE ||
	    addr >= VBLK_ZRAM_MMIO_BASE + VBLK_ZRAM_MMIO_SIZE)
		return 0;

	g_zr_faults++;
	if (!(esr & ESR_ISV_BIT)) {
		frame->elr += 4;
		return 1;
	}
	off = (uint32_t)(addr - VBLK_ZRAM_MMIO_BASE);
	srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;

	zr_lock();
	if (esr & ESR_WNR_BIT) {
		zr_reg_write(off, srt == SRT_XZR ? 0u : (uint32_t)frame->x[srt]);
	} else {
		uint32_t v = (off >= VBLK_SD_R_CONFIG && off < VBLK_SD_R_CONFIG + 16u)
		           ? zr_config_read(off - VBLK_SD_R_CONFIG, sas)
		           : zr_reg_read(off);
		if (srt != SRT_XZR)
			frame->x[srt] = v;
	}
	zr_unlock();
	frame->elr += 4;
	return 1;
}

void vblk_zram_init(void)
{
	uint8_t *z = (uint8_t *)&g_zr;

	for (uint32_t i = 0; i < sizeof(g_zr); i++)
		z[i] = 0;
	g_zr_lock = 0;
	g_zr_reads = g_zr_writes = g_zr_errs = g_zr_grows = 0;
	g_zr_used_pages = g_zr_irq_rearms = g_zr_faults = 0;
	/* Warm resets keep DRAM, never its meaning: every boot starts empty. */
	vzram_pool_init(&g_zpool, (uint8_t *)(uintptr_t)ZR_POOL_PA, ZR_POOL_BYTES,
	                (vzram_slot_t *)(uintptr_t)HVMAP_VZRAM_BASE,
	                VZRAM_PAGES_MAX, VZRAM_PAGES_INITIAL);
}
