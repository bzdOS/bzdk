/* virtio.c — virtio-mmio (modern / v2) transport, virtqueue engine, stage-2
 * fault dispatch and device registration for the bzdOS EL2 hypervisor.
 *
 * See virtio.h for the full rationale, the register map, the MMIO/RAM-disk/
 * INTID address choices and the el2_trap wiring contract. This file owns:
 *   - the MMIO register emulation (virtio_mmio_fault),
 *   - the split-virtqueue walker (virtq_pop_avail / read_desc / push_used),
 *   - coherent guest-memory helpers,
 *   - the device table + virtio_init().
 *
 * ==================================================================== *
 *  INTEGRATION SNIPPETS (do NOT edit shared files here — apply these):
 *
 *  1) stage2.c  — leave the virtio 2 MiB block UNMAPPED so accesses trap.
 *     In stage2_build_mmio_tables(), inside the level-2 fill loop, add a
 *     carve-out next to the UART one:
 *
 *         #define VIRTIO_L2_IDX ((unsigned)(0x0A000000UL >> 21))  // == 80
 *         for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
 *                 if (i == UART_L2_IDX) { ... existing ... continue; }
 *                 if (i == VIRTIO_L2_IDX) {         // <-- ADD
 *                         stage2_l2_mmio[i] = 0;    // INVALID: virtio window
 *                         continue;
 *                 }
 *                 ... existing identity 2 MiB block ...
 *         }
 *     (The whole 2 MiB block 0x0A000000..0x0A1FFFFF is unmapped; both device
 *      register pages live in its first 4 KiB.)
 *
 *  2) el2_exc.c — dispatch guest data aborts to us, after the vconsole try.
 *     Add #include "virtio.h" and, in the EC==0x24 lower-EL arm:
 *
 *         if (vconsole_handle_fault(frame)) return;
 *         if (virtio_mmio_fault(frame))     return;   // <-- ADD
 *
 *  3) main_fbsd.c / main_dbg.c — init alongside vconsole, before stage2_enable():
 *         #include "virtio.h"
 *         vconsole_init();
 *         virtio_init();                              // <-- ADD
 *
 *  4) Makefile — add to FBSD_OBJS and DBG_OBJS:
 *         virtio.o virtio_console.o virtio_blk.o
 * ==================================================================== */
#include <stdint.h>
#include "virtio.h"
#include "exceptions.h"

/* vgic injection: declared weak so a build without the vgic lane still links
 * (the SPI simply isn't delivered — the used ring is still updated, which a
 * polling driver would still see). */
__attribute__((weak)) void vgic_inject(uint32_t vintid, int priority)
{
	(void)vintid; (void)priority;
}

/* ------------------------------------------------------------------ *
 * ESR_EL2.ISS decode for a lower-EL data abort (EC==0x24) — identical field
 * positions to vconsole.c / vgic.c.
 * ------------------------------------------------------------------ */
#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3fu
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SAS_SHIFT     22
#define ESR_SAS_MASK      0x3u
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1fu
#define ESR_SF_BIT        (1u << 15)   /* 1 = 64-bit register transfer */
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u

/* Running counters mirrored into the breadcrumb window. */
static uint32_t vio_injects;        /* SPI injects issued (word 7)        */
static uint32_t vio_notify_total;   /* QueueNotify count (word 3)         */

/* ------------------------------------------------------------------ *
 * Breadcrumb window @ 0x50005000 ("VIO1"). Layout (32-bit words):
 *   [0]  magic "VIO1"
 *   [1]  console.status        [2]  blk.status
 *   [3]  total QueueNotify seen
 *   [4]  console TX bytes total
 *   [5]  blk read requests      [6] blk write requests
 *   [7]  SPI injects issued
 *   [8]  last notify (dev<<16 | qidx)
 *   [9]  last blk sector (lo 32)
 *   [10] console driver_features word0   [11] blk driver_features word0
 *   [12] console last used_len            [13] blk last status byte
 *   [14] console queue0 desc PA lo        [15] blk queue0 desc PA lo
 * The console TX capture buffer follows at 0x50005040 (see virtio_console.c).
 * ------------------------------------------------------------------ */
void virtio_bc(uint32_t word_idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VIRTIO_BC_BASE + word_idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Coherent guest-memory access. IPA==PA identity map, so the guest PA is a
 * flat host pointer at EL2. Clean+invalidate around the access so we and the
 * (cache-coherent, inner-shareable) guest agree — same idiom as the rest of
 * the tree. Byte-granular to tolerate unaligned virtqueue fields.
 * ------------------------------------------------------------------ */
void virtio_read(uint64_t pa, void *dst, uint32_t len)
{
	__asm__ volatile("dsb sy" ::: "memory");
	const volatile uint8_t *s = (const volatile uint8_t *)pa;
	uint8_t *d = (uint8_t *)dst;
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
}

void virtio_write(uint64_t pa, const void *src, uint32_t len)
{
	volatile uint8_t *d = (volatile uint8_t *)pa;
	const uint8_t *s = (const uint8_t *)src;
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	/* Push the whole written range to the point of coherency so the guest
	 * observes it, then order it before the interrupt. Clean by cache line. */
	for (uint64_t a = pa & ~63ull; a < pa + len; a += 64u)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint16_t gr16(uint64_t pa)
{
	uint16_t v; virtio_read(pa, &v, 2); return v;
}
static inline uint32_t gr32(uint64_t pa)
{
	uint32_t v; virtio_read(pa, &v, 4); return v;
}
static inline void gw16(uint64_t pa, uint16_t v) { virtio_write(pa, &v, 2); }
static inline void gw32(uint64_t pa, uint32_t v) { virtio_write(pa, &v, 4); }

/* ------------------------------------------------------------------ *
 * Split-virtqueue layout offsets (VIRTIO 1.x, packed, little-endian).
 *   desc[i]  @ desc  + i*16 : le64 addr, le32 len, le16 flags, le16 next
 *   avail    @ avail        : le16 flags, le16 idx, le16 ring[num], le16 used_ev
 *   used     @ used         : le16 flags, le16 idx,
 *                             { le32 id; le32 len; } ring[num], le16 avail_ev
 * ------------------------------------------------------------------ */
#define VRING_AVAIL_RING_OFF  4u              /* after flags(2)+idx(2)        */
#define VRING_USED_RING_OFF   4u

int virtq_pop_avail(struct virtio_dev *d, uint32_t qidx, uint16_t *head)
{
	struct virtq *q = &d->vq[qidx];
	if (!q->ready || q->num == 0 || q->avail == 0 || q->desc == 0)
		return 0;

	uint16_t avail_idx = gr16(q->avail + 2u);   /* avail->idx */
	if (avail_idx == q->last_avail)
		return 0;                               /* nothing new */

	uint32_t slot = (uint32_t)(q->last_avail % q->num);
	*head = gr16(q->avail + VRING_AVAIL_RING_OFF + slot * 2u);
	q->last_avail++;
	return 1;
}

void virtq_read_desc(struct virtio_dev *d, uint32_t qidx, uint16_t idx,
                     struct vdesc *out)
{
	struct virtq *q = &d->vq[qidx];
	uint64_t p = q->desc + (uint64_t)idx * 16u;
	virtio_read(p, &out->addr, 8);
	out->len   = gr32(p + 8u);
	out->flags = gr16(p + 12u);
	out->next  = gr16(p + 14u);
}

void virtq_push_used(struct virtio_dev *d, uint32_t qidx, uint16_t head,
                     uint32_t used_len)
{
	struct virtq *q = &d->vq[qidx];
	uint16_t used_idx = gr16(q->used + 2u);     /* used->idx */
	uint32_t slot = (uint32_t)(used_idx % q->num);
	uint64_t e = q->used + VRING_USED_RING_OFF + (uint64_t)slot * 8u;
	gw32(e + 0u, head);                         /* used_elem.id  */
	gw32(e + 4u, used_len);                     /* used_elem.len */
	__asm__ volatile("dsb sy" ::: "memory");
	gw16(q->used + 2u, (uint16_t)(used_idx + 1u)); /* publish new idx */

	/* Raise the used-buffer notification and interrupt the guest. */
	d->int_status |= VIRTIO_INT_VRING;
	vgic_inject(d->intid, VIRTIO_IRQ_PRIO);
	virtio_bc(7, ++vio_injects);
}

/* ------------------------------------------------------------------ *
 * Device table.
 * ------------------------------------------------------------------ */
static struct virtio_dev *vio_devs[VIRTIO_NUM_DEVS];
static uint32_t           vio_ndevs;

static struct virtio_dev *virtio_find(uint64_t ipa)
{
	for (uint32_t i = 0; i < vio_ndevs; i++) {
		struct virtio_dev *d = vio_devs[i];
		if (d && ipa >= d->base && ipa < d->base + VIRTIO_MMIO_REGSZ)
			return d;
	}
	return (struct virtio_dev *)0;
}

void virtio_register(struct virtio_dev *d)
{
	if (d && vio_ndevs < VIRTIO_NUM_DEVS)
		vio_devs[vio_ndevs++] = d;
}

/* ------------------------------------------------------------------ *
 * MMIO register read/write emulation.
 * ------------------------------------------------------------------ */
static uint32_t mmio_read(struct virtio_dev *d, uint32_t off)
{
	struct virtq *q = &d->vq[d->queue_sel < VIRTIO_MAX_QUEUES ? d->queue_sel : 0];

	switch (off) {
	case VIRTIO_MMIO_MAGIC_VALUE:   return VIRTIO_MMIO_MAGIC;
	case VIRTIO_MMIO_VERSION:       return VIRTIO_MMIO_VERSION_MODERN;
	case VIRTIO_MMIO_DEVICE_ID:     return d->device_id;
	case VIRTIO_MMIO_VENDOR_ID:     return d->vendor_id;
	case VIRTIO_MMIO_DEVICE_FEATURES:
		return (uint32_t)(d->device_features >> (32u * (d->dev_feat_sel & 1u)));
	case VIRTIO_MMIO_QUEUE_NUM_MAX: return VIRTIO_QUEUE_MAX;
	case VIRTIO_MMIO_QUEUE_READY:   return q->ready;
	case VIRTIO_MMIO_INTERRUPT_STATUS: return d->int_status;
	case VIRTIO_MMIO_STATUS:        return d->status;
	case VIRTIO_MMIO_CONFIG_GENERATION: return d->config_generation;
	default: break;
	}
	if (off >= VIRTIO_MMIO_CONFIG && off < VIRTIO_MMIO_CONFIG + sizeof(d->config)) {
		uint32_t c = off - VIRTIO_MMIO_CONFIG, v = 0;
		for (uint32_t i = 0; i < 4 && c + i < sizeof(d->config); i++)
			v |= (uint32_t)d->config[c + i] << (8u * i);
		return v;
	}
	return 0;
}

static void mmio_write(struct virtio_dev *d, uint32_t off, uint32_t val)
{
	struct virtq *q = &d->vq[d->queue_sel < VIRTIO_MAX_QUEUES ? d->queue_sel : 0];

	switch (off) {
	case VIRTIO_MMIO_DEVICE_FEATURES_SEL: d->dev_feat_sel = val; return;
	case VIRTIO_MMIO_DRIVER_FEATURES_SEL: d->drv_feat_sel = val; return;
	case VIRTIO_MMIO_DRIVER_FEATURES: {
		uint32_t sh = 32u * (d->drv_feat_sel & 1u);
		d->driver_features &= ~((uint64_t)0xffffffffu << sh);
		d->driver_features |= (uint64_t)val << sh;
		virtio_bc(d->device_id == VIRTIO_DEV_BLK ? 11 : 10, (uint32_t)d->driver_features);
		return;
	}
	case VIRTIO_MMIO_QUEUE_SEL:
		if (val < VIRTIO_MAX_QUEUES) d->queue_sel = val;
		return;
	case VIRTIO_MMIO_QUEUE_NUM:
		if (val <= VIRTIO_QUEUE_MAX) q->num = val;
		return;
	case VIRTIO_MMIO_QUEUE_READY:
		q->ready = val & 1u;
		if (q->ready) q->last_avail = gr16(q->avail + 2u); /* sync to current */
		return;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		virtio_bc(3, ++vio_notify_total);
		virtio_bc(8, ((uint32_t)d->device_id << 16) | (val & 0xffffu));
		if (val < d->num_queues && d->notify)
			d->notify(d, val);
		return;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		d->int_status &= ~val;
		return;
	case VIRTIO_MMIO_STATUS:
		d->status = val;
		virtio_bc(d->device_id == VIRTIO_DEV_BLK ? 2 : 1, val);
		if (val == 0) {           /* driver reset: clear transient state */
			for (uint32_t i = 0; i < VIRTIO_MAX_QUEUES; i++) {
				d->vq[i].ready = 0; d->vq[i].last_avail = 0;
			}
			d->int_status = 0; d->driver_features = 0;
		}
		return;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:    q->desc  = (q->desc  & 0xffffffff00000000ull) | val; return;
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:   q->desc  = (q->desc  & 0x00000000ffffffffull) | ((uint64_t)val << 32); return;
	case VIRTIO_MMIO_QUEUE_DRIVER_LOW:  q->avail = (q->avail & 0xffffffff00000000ull) | val; return;
	case VIRTIO_MMIO_QUEUE_DRIVER_HIGH: q->avail = (q->avail & 0x00000000ffffffffull) | ((uint64_t)val << 32); return;
	case VIRTIO_MMIO_QUEUE_DEVICE_LOW:  q->used  = (q->used  & 0xffffffff00000000ull) | val; return;
	case VIRTIO_MMIO_QUEUE_DEVICE_HIGH: q->used  = (q->used  & 0x00000000ffffffffull) | ((uint64_t)val << 32); return;
	default: break;
	}
	if (off >= VIRTIO_MMIO_CONFIG && off < VIRTIO_MMIO_CONFIG + sizeof(d->config)) {
		uint32_t c = off - VIRTIO_MMIO_CONFIG;
		for (uint32_t i = 0; i < 4 && c + i < sizeof(d->config); i++)
			d->config[c + i] = (uint8_t)(val >> (8u * i));
	}
	/* Any other register (QueueNumMax etc.) is read-only: writes ignored. */
}

/* ------------------------------------------------------------------ *
 * Fault dispatch.
 * ------------------------------------------------------------------ */
int virtio_mmio_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	if (((esr >> ESR_EC_SHIFT) & ESR_EC_MASK) != ESR_EC_DABT_LOWER)
		return 0;

	/* Reconstruct the faulting IPA. Unlike vconsole (guest MMU off, so
	 * FAR==IPA), the guest hits these registers with its MMU ON, so FAR_EL2
	 * holds a guest VA. HPFAR_EL2.FIPA[39:4] == IPA[47:12]; the in-page
	 * offset comes from FAR_EL2[11:0]. */
	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t ipa = ((hpfar & 0xFFFFFFFFF0ull) << 8) | (frame->far & 0xFFFull);

	struct virtio_dev *d = virtio_find(ipa);
	if (!d)
		return 0;                      /* not a virtio window — not ours */

	uint32_t off = (uint32_t)(ipa - d->base);

	if (!(esr & ESR_ISV_BIT)) {
		/* No decoded syndrome: skip the instruction so we don't spin. */
		frame->elr += 4;
		return 1;
	}

	uint32_t is_write = esr & ESR_WNR_BIT;
	uint32_t srt      = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;

	if (is_write) {
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];
		mmio_write(d, off, (uint32_t)val);
	} else {
		uint32_t val = mmio_read(d, off);
		if (srt != SRT_XZR)
			frame->x[srt] = val;       /* zero-extends into x[srt] */
	}

	frame->elr += 4;
	return 1;
}

/* ------------------------------------------------------------------ *
 * Init.
 * ------------------------------------------------------------------ */
void virtio_init(void)
{
	vio_ndevs = 0;
	vio_notify_total = 0;
	vio_injects = 0;

	virtio_bc(0, VIRTIO_BC_MAGIC);
	for (uint32_t i = 1; i < 16; i++)
		virtio_bc(i, 0);

	virtio_register(virtio_console_setup(VIRTIO_MMIO_BASE + 0 * VIRTIO_MMIO_STRIDE));
	virtio_register(virtio_blk_setup(VIRTIO_MMIO_BASE + 1 * VIRTIO_MMIO_STRIDE));
}
