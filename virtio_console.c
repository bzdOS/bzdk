/* virtio_console.c — virtio-console (DeviceID 3) backend for the bzdOS EL2
 * hypervisor. A virtqueue-backed console for the FreeBSD/arm64 guest.
 *
 * Queues (no VIRTIO_CONSOLE_F_MULTIPORT, so just port 0):
 *   queue 0 = receiveq  (device -> driver): buffers the guest gives us to
 *             fill with input. We have no console input source wired yet, so
 *             we leave these untouched (RX is optional per the brief).
 *   queue 1 = transmitq (driver -> device): the guest's console output. Each
 *             available chain is read-only guest data; we copy the bytes out,
 *             capture them into a DRAM ring (readable via `md`, survives a WDT
 *             reset) and forward them to the network console (netcon_send) if
 *             that lane is linked, then return the buffer via the used ring.
 *
 * We offer only VIRTIO_F_VERSION_1 (modern). Config space is left zero (no
 * F_SIZE/F_EMERG_WRITE advertised, so the driver never reads it).
 *
 * TX capture ring: header counters live in the shared virtio breadcrumb
 * (0x50005000, word 4 = total bytes). The byte buffer sits just above the
 * 16-word breadcrumb header at 0x50005040, 3 KiB, wrapping.
 */
#include <stdint.h>
#include "virtio.h"

/* Optional network console. Weak so the fbsd/dbg builds (which do not link
 * netcon.o) still resolve — output is then captured to the ring only. */
__attribute__((weak)) int netcon_send(const uint8_t *data, uint32_t len)
{
	(void)data; (void)len;
	return -1;
}

#define CON_RX_QUEUE   0u
#define CON_TX_QUEUE   1u

#define CON_BUF_BASE   (VIRTIO_BC_BASE + 0x40u)   /* 0x50005040 */
#define CON_BUF_SIZE   3072u                       /* ends 0x50005C40 */

static struct virtio_dev con_dev;
static uint32_t          con_tx_total;

static inline void con_store_byte(uint32_t off, uint8_t c)
{
	volatile uint8_t *p = (volatile uint8_t *)(CON_BUF_BASE + off);
	*p = c;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static void con_capture(const uint8_t *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		con_store_byte(con_tx_total % CON_BUF_SIZE, buf[i]);
		con_tx_total++;
	}
	virtio_bc(4, con_tx_total);
}

/* Walk one TX descriptor chain: copy every readable buffer's bytes out. */
static void con_tx_chain(struct virtio_dev *d, uint16_t head)
{
	uint8_t tmp[64];
	uint16_t idx = head;
	uint32_t guard = 0;

	for (;;) {
		struct vdesc dsc;
		virtq_read_desc(d, CON_TX_QUEUE, idx, &dsc);

		if (!(dsc.flags & VRING_DESC_F_WRITE)) {   /* device-readable data */
			uint32_t rem = dsc.len;
			uint64_t pa = dsc.addr;
			while (rem) {
				uint32_t n = rem > sizeof(tmp) ? (uint32_t)sizeof(tmp) : rem;
				virtio_read(pa, tmp, n);
				con_capture(tmp, n);
				(void)netcon_send(tmp, n);
				pa += n; rem -= n;
			}
		}

		if (!(dsc.flags & VRING_DESC_F_NEXT))
			break;
		idx = dsc.next;
		if (++guard > VIRTIO_MAX_CHAIN)            /* runaway chain guard */
			break;
	}

	/* TX buffers are read-only for the device: nothing was written back. */
	virtq_push_used(d, CON_TX_QUEUE, head, 0);
}

static void con_notify(struct virtio_dev *d, uint32_t qidx)
{
	if (qidx != CON_TX_QUEUE)
		return;                                   /* RX: nothing to deliver */
	uint16_t head;
	while (virtq_pop_avail(d, qidx, &head))
		con_tx_chain(d, head);
}

struct virtio_dev *virtio_console_setup(uint64_t base)
{
	struct virtio_dev *d = &con_dev;

	for (uint32_t i = 0; i < sizeof(*d); i++)
		((uint8_t *)d)[i] = 0;

	d->base            = base;
	d->device_id       = 3;                       /* virtio-console */
	d->vendor_id       = VIRTIO_MMIO_VENDOR;
	d->device_features = (uint64_t)VIRTIO_F_VERSION_1_BIT << 32;
	d->intid           = VIRTIO_CONSOLE_INTID;
	d->num_queues      = 2;
	d->notify          = con_notify;

	con_tx_total = 0;
	virtio_bc(1, 0);          /* console.status */
	return d;
}
