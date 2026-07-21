/* virtio_blk.c — virtio-blk (DeviceID 2) backend for the bzdOS EL2
 * hypervisor, backed by a fixed DRAM RAM disk.
 *
 * RAM disk: VIRTIO_RAMDISK_BASE (0x7C000000), VIRTIO_RAMDISK_SIZE (16 MiB) =
 * 32768 x 512-byte sectors, at the top of the stage-2-mapped 1 GiB DRAM
 * window. EL2 reaches it as a flat identity PA. It is NOT mapped into the
 * guest's stage-2 IPA space as memory (the guest DTB /memory must end at, or
 * /reserved-memory must carve out, this region — see virtio.h), so only this
 * backend and the guest's block I/O ever touch it.
 *
 * Single request virtqueue (queue 0). Each request is a descriptor chain:
 *   desc[0]      read-only  16-byte header { le32 type; le32 reserved;
 *                                            le64 sector }
 *   desc[1..k-2] data       (read-only for T_OUT, write-only for T_IN)
 *   desc[k-1]    write-only 1-byte status (VIRTIO_BLK_S_OK/_IOERR)
 *
 * We offer only VIRTIO_F_VERSION_1 (modern). config.capacity advertises the
 * sector count. T_IN/T_OUT are served against the RAM region; T_FLUSH is a
 * no-op success; anything else returns VIRTIO_BLK_S_UNSUPP.
 */
#include <stdint.h>
#include "virtio.h"

#define BLK_QUEUE   0u

struct virtio_blk_hdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

static struct virtio_dev blk_dev;
static uint32_t          blk_reads, blk_writes;

/* Flat RAM-disk access (Normal cacheable DRAM, EL2-private). */
static inline uint8_t *ramdisk_ptr(uint64_t off)
{
	return (uint8_t *)(uintptr_t)(VIRTIO_RAMDISK_BASE + off);
}

/* Copy RAM disk -> guest buffer (T_IN, device writes the guest buffer). */
static void disk_to_guest(uint64_t disk_off, uint64_t guest_pa, uint32_t len)
{
	uint8_t tmp[64];
	uint8_t *src = ramdisk_ptr(disk_off);
	uint32_t done = 0;
	while (done < len) {
		uint32_t n = (len - done) > sizeof(tmp) ? (uint32_t)sizeof(tmp) : (len - done);
		for (uint32_t i = 0; i < n; i++)
			tmp[i] = src[done + i];
		virtio_write(guest_pa + done, tmp, n);
		done += n;
	}
}

/* Copy guest buffer -> RAM disk (T_OUT, device reads the guest buffer). */
static void guest_to_disk(uint64_t guest_pa, uint64_t disk_off, uint32_t len)
{
	uint8_t tmp[64];
	uint8_t *dst = ramdisk_ptr(disk_off);
	uint32_t done = 0;
	while (done < len) {
		uint32_t n = (len - done) > sizeof(tmp) ? (uint32_t)sizeof(tmp) : (len - done);
		virtio_read(guest_pa + done, tmp, n);
		for (uint32_t i = 0; i < n; i++)
			dst[done + i] = tmp[i];
		done += n;
	}
}

static void blk_request(struct virtio_dev *d, uint16_t head)
{
	struct vdesc chain[VIRTIO_MAX_CHAIN];
	uint32_t n = 0;
	uint16_t idx = head;

	/* Collect the chain. */
	for (;;) {
		if (n >= VIRTIO_MAX_CHAIN)
			break;
		virtq_read_desc(d, BLK_QUEUE, idx, &chain[n]);
		uint16_t next = chain[n].next;
		uint16_t flags = chain[n].flags;
		n++;
		if (!(flags & VRING_DESC_F_NEXT))
			break;
		idx = next;
	}
	if (n < 2) {                       /* need at least header + status */
		virtq_push_used(d, BLK_QUEUE, head, 0);
		return;
	}

	/* Header (desc 0) and status (last desc). */
	struct virtio_blk_hdr hdr;
	virtio_read(chain[0].addr, &hdr, sizeof(hdr));
	struct vdesc *st = &chain[n - 1];

	uint8_t status = VIRTIO_BLK_S_OK;
	uint32_t used_len = 0;             /* bytes written to device-writable bufs */
	uint64_t off = hdr.sector * VIRTIO_BLK_SECTOR;

	virtio_bc(9, (uint32_t)hdr.sector);

	if (hdr.type == VIRTIO_BLK_T_IN || hdr.type == VIRTIO_BLK_T_OUT) {
		for (uint32_t i = 1; i < n - 1; i++) {
			struct vdesc *dd = &chain[i];
			if (off + dd->len > VIRTIO_RAMDISK_SIZE) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			if (hdr.type == VIRTIO_BLK_T_IN) {
				disk_to_guest(off, dd->addr, dd->len);
				used_len += dd->len;   /* device wrote these bytes */
			} else {
				guest_to_disk(dd->addr, off, dd->len);
			}
			off += dd->len;
		}
		if (hdr.type == VIRTIO_BLK_T_IN)  blk_reads++;  else blk_writes++;
		virtio_bc(5, blk_reads);
		virtio_bc(6, blk_writes);
	} else if (hdr.type == VIRTIO_BLK_T_FLUSH) {
		/* RAM disk: nothing to flush. */
	} else {
		status = VIRTIO_BLK_S_UNSUPP;
	}

	/* Write the 1-byte status into the final (write-only) descriptor. */
	virtio_write(st->addr, &status, 1);
	used_len += 1;
	virtio_bc(13, status);

	virtq_push_used(d, BLK_QUEUE, head, used_len);
}

static void blk_notify(struct virtio_dev *d, uint32_t qidx)
{
	if (qidx != BLK_QUEUE)
		return;
	uint16_t head;
	while (virtq_pop_avail(d, qidx, &head))
		blk_request(d, head);
}

static void ramdisk_init(void)
{
	/* Lay a tiny known signature in sector 0 so an external memory dump of
	 * 0x7C000000 confirms the disk exists and (later) that the guest wrote
	 * over it. Rest of the disk is left as-is. */
	static const uint8_t sig[16] = { 'B','Z','D','K','R','A','M','D','I','S','K','0','0','0','0','0' };
	uint8_t *d = ramdisk_ptr(0);
	for (uint32_t i = 0; i < sizeof(sig); i++)
		d[i] = sig[i];
	for (uint64_t a = VIRTIO_RAMDISK_BASE; a < VIRTIO_RAMDISK_BASE + 64u; a += 64u)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

struct virtio_dev *virtio_blk_setup(uint64_t base)
{
	struct virtio_dev *d = &blk_dev;

	for (uint32_t i = 0; i < sizeof(*d); i++)
		((uint8_t *)d)[i] = 0;

	d->base            = base;
	d->device_id       = 2;                       /* virtio-blk */
	d->vendor_id       = VIRTIO_MMIO_VENDOR;
	d->device_features = (uint64_t)VIRTIO_F_VERSION_1_BIT << 32;
	d->intid           = VIRTIO_BLK_INTID;
	d->num_queues      = 1;
	d->notify          = blk_notify;

	/* config.capacity (offset 0, le64): sector count. */
	uint64_t cap = VIRTIO_RAMDISK_SECTORS;
	for (uint32_t i = 0; i < 8; i++)
		d->config[i] = (uint8_t)(cap >> (8u * i));

	blk_reads = blk_writes = 0;
	ramdisk_init();
	virtio_bc(2, 0);          /* blk.status */
	return d;
}
