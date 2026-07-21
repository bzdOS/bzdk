/* virtio.h — virtio-mmio (modern / spec v2) transport + device model for the
 * FreeBSD/arm64 EL1 guest running under the bzdOS from-scratch EL2 hypervisor
 * on the Allwinner A64 (Banana Pi M64).
 *
 * WHY THIS EXISTS
 * ---------------
 * vconsole.c already gives the guest a fake UART0 by trap-and-emulate on an
 * unmapped stage-2 page. This module generalises that idea to two full
 * paravirtual devices behind the virtio-mmio transport:
 *
 *   - virtio-console (DeviceID 3): the guest's console over virtqueues. TX
 *     bytes (guest -> us) are captured into a DRAM ring (readable by an
 *     external `md`) and, when a network console lane is linked, forwarded
 *     via netcon_send() so the operator sees guest output over the wire.
 *
 *   - virtio-blk (DeviceID 2): a block device backed by a fixed DRAM RAM
 *     disk (VIRTIO_RAMDISK_BASE, VIRTIO_RAMDISK_SIZE). Serves
 *     VIRTIO_BLK_T_IN / _OUT against that region so the guest gets a real
 *     root/scratch disk.
 *
 * TRANSPORT WINDOW & STAGE-2
 * --------------------------
 * The two devices live at VIRTIO_MMIO_BASE (0x0A000000) and +0x200, a 4 KiB-
 * aligned hole that is NOT a real A64 peripheral (the A64 SoC MMIO cluster is
 * 0x01000000..0x02000000; 0x0A000000 is unassigned) but IS inside the stage-2
 * identity map's low-1 GiB MMIO window. stage2.c must leave the 2 MiB block
 * containing VIRTIO_MMIO_BASE UNMAPPED (invalid level-2 descriptor) so every
 * guest access there takes a stage-2 data abort into EL2, exactly like the
 * UART0 page. See the integration snippet in the file banner of virtio.c.
 *
 * KEY DIFFERENCE FROM vconsole
 * ----------------------------
 * vconsole runs during early guest boot with the guest MMU OFF, so FAR_EL2
 * (guest VA) == IPA == PA and it can compare frame->far directly to
 * UART0_BASE. The virtio-mmio registers are touched by FreeBSD's virtio
 * driver LATE, after the guest MMU is on and the device is mapped at some
 * guest VA. FAR_EL2 then holds that VA, NOT 0x0A000000. So virtio_mmio_fault()
 * reconstructs the faulting IPA from HPFAR_EL2 (FIPA = IPA[47:12]) plus the
 * in-page offset from FAR_EL2[11:0]. This is the one subtlety that makes the
 * device work post-MMU.
 *
 * Wiring contract (owned by the el2_trap integrator, exceptions dispatch):
 *   On a lower-EL synchronous data abort (group 2, EC==0x24), after the
 *   vconsole attempt, call virtio_mmio_fault(frame); if it returns 1 the
 *   access was fully emulated (register read/write + ELR advanced) and the
 *   trap handler must return without recording a fault.
 *
 *       if (vconsole_handle_fault(frame)) return;
 *       if (virtio_mmio_fault(frame))     return;   // <-- add this
 *
 *   virtio_init() must be called once before the guest can fault on the
 *   window (i.e. before stage2_enable()), alongside vconsole_init().
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only. All breadcrumb
 * and used-ring stores are cache-coherent (dc civac + dsb sy), the same
 * pattern as every other window in this tree.
 */
#ifndef BZDOS_VIRTIO_H
#define BZDOS_VIRTIO_H

#include <stdint.h>
#include "exceptions.h"

/* ------------------------------------------------------------------ *
 * MMIO window & per-device layout.
 *
 * VIRTIO_MMIO_BASE .. +VIRTIO_MMIO_STRIDE for device 0 (console),
 * +VIRTIO_MMIO_STRIDE for device 1 (blk). Each virtio-mmio device occupies
 * 0x200 bytes of register space (the spec's minimum). We place them 0x200
 * apart so both fit in the first 4 KiB page of the unmapped 2 MiB block.
 *
 * 0x0A000000 chosen because:
 *   - it is a hole in the A64 physical map (peripherals end at 0x02000000),
 *     so unmapping it in stage-2 steals nothing real;
 *   - it is the same base QEMU's "virt" board uses for virtio-mmio, which
 *     FreeBSD's arm64 virtio_mmio driver is already known to probe happily.
 * ------------------------------------------------------------------ */
#define VIRTIO_MMIO_BASE     0x0A000000UL
#define VIRTIO_MMIO_STRIDE   0x200UL
#define VIRTIO_MMIO_REGSZ    0x200UL

#define VIRTIO_DEV_CONSOLE   0   /* device index 0: base 0x0A000000 */
#define VIRTIO_DEV_BLK       1   /* device index 1: base 0x0A000200 */
#define VIRTIO_NUM_DEVS      2

/* ------------------------------------------------------------------ *
 * Guest interrupt (SPI) INTIDs injected via vgic on used-buffer completion.
 * SPIs start at INTID 32; DT "GIC_SPI n" == INTID (32 + n).
 *   console: INTID 48  == GIC_SPI 16
 *   blk:     INTID 49  == GIC_SPI 17
 * Both are well clear of the guest's real timer PPI (INTID 27) and the GIC
 * maintenance PPI (INTID 25). Injection goes straight into a GICH list
 * register (vgic_inject), bypassing the passed-through GICD, so there is no
 * contention with real peripheral SPIs.
 * ------------------------------------------------------------------ */
#define VIRTIO_CONSOLE_INTID   48u
#define VIRTIO_BLK_INTID       49u
#define VIRTIO_IRQ_PRIO        0    /* highest (0 = most urgent), like the vtimer */

/* ------------------------------------------------------------------ *
 * RAM disk backing store for virtio-blk. A fixed 16 MiB DRAM region at the
 * very top of the stage-2-mapped 1 GiB DRAM window (0x40000000..0x80000000).
 * EL2 accesses it as a flat identity PA (IPA==PA), so read/write is a direct
 * memcpy. Starts zeroed (see virtio_blk_init), with a small "BZDK" signature
 * in sector 0 so an external dump can confirm the guest is talking to it.
 *
 * DTB NOTE: because the guest believes all of 0x40000000..0x80000000 is its
 * RAM, the guest DTB /memory node MUST be trimmed to end at
 * VIRTIO_RAMDISK_BASE (reg = <0x40000000 0x3C000000>), OR a /reserved-memory
 * node must carve out [VIRTIO_RAMDISK_BASE, +VIRTIO_RAMDISK_SIZE), so the
 * guest allocator never stomps the disk. Coordinated with the DTB owner.
 * ------------------------------------------------------------------ */
#define VIRTIO_RAMDISK_BASE   0x7C000000UL
#define VIRTIO_RAMDISK_SIZE   0x01000000UL   /* 16 MiB */
#define VIRTIO_BLK_SECTOR     512u
#define VIRTIO_RAMDISK_SECTORS (VIRTIO_RAMDISK_SIZE / VIRTIO_BLK_SECTOR)  /* 32768 */

/* ------------------------------------------------------------------ *
 * virtio-mmio register offsets (modern transport, spec v2 / VIRTIO 1.x).
 * ------------------------------------------------------------------ */
#define VIRTIO_MMIO_MAGIC_VALUE        0x000  /* R  0x74726976 "virt"          */
#define VIRTIO_MMIO_VERSION            0x004  /* R  2 (modern)                 */
#define VIRTIO_MMIO_DEVICE_ID          0x008  /* R                             */
#define VIRTIO_MMIO_VENDOR_ID          0x00c  /* R                             */
#define VIRTIO_MMIO_DEVICE_FEATURES    0x010  /* R  (selected 32-bit word)     */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014 /* W                             */
#define VIRTIO_MMIO_DRIVER_FEATURES    0x020  /* W  (selected 32-bit word)     */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024 /* W                             */
#define VIRTIO_MMIO_QUEUE_SEL          0x030  /* W                             */
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034  /* R                             */
#define VIRTIO_MMIO_QUEUE_NUM          0x038  /* W                             */
#define VIRTIO_MMIO_QUEUE_READY        0x044  /* RW                            */
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050  /* W                             */
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060  /* R                             */
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064  /* W                             */
#define VIRTIO_MMIO_STATUS             0x070  /* RW                            */
#define VIRTIO_MMIO_QUEUE_DESC_LOW     0x080  /* W  descriptor table PA[31:0]  */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH    0x084  /* W                 PA[63:32]   */
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW   0x090  /* W  avail ring PA[31:0]        */
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH  0x094  /* W                             */
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW   0x0a0  /* W  used ring PA[31:0]         */
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH  0x0a4  /* W                             */
#define VIRTIO_MMIO_CONFIG_GENERATION  0x0fc  /* R                             */
#define VIRTIO_MMIO_CONFIG             0x100  /* RW device-specific config     */

#define VIRTIO_MMIO_MAGIC              0x74726976u  /* "virt" little-endian    */
#define VIRTIO_MMIO_VERSION_MODERN     2u
#define VIRTIO_MMIO_VENDOR             0x627A6473u  /* "bzds" — our vendor tag */

/* Status register bits (VIRTIO 1.x). */
#define VIRTIO_STATUS_ACKNOWLEDGE      0x01u
#define VIRTIO_STATUS_DRIVER           0x02u
#define VIRTIO_STATUS_DRIVER_OK        0x04u
#define VIRTIO_STATUS_FEATURES_OK      0x08u
#define VIRTIO_STATUS_NEEDS_RESET      0x40u
#define VIRTIO_STATUS_FAILED           0x80u

/* InterruptStatus / used-buffer notification. */
#define VIRTIO_INT_VRING               0x1u   /* used ring advanced          */
#define VIRTIO_INT_CONFIG              0x2u   /* config changed (unused)     */

/* Feature bit 32: VIRTIO_F_VERSION_1 (modern). Lives in feature word 1. */
#define VIRTIO_F_VERSION_1_WORD        1u
#define VIRTIO_F_VERSION_1_BIT         0x1u   /* bit 0 of word 1 == global bit 32 */

/* Maximum virtqueue size we advertise / support, and max devices/queues. */
#define VIRTIO_QUEUE_MAX               256u
#define VIRTIO_MAX_QUEUES              2u     /* console: rx+tx; blk: 1        */
#define VIRTIO_MAX_CHAIN               16u    /* descriptors walked per request*/

/* split-virtqueue descriptor flags. */
#define VRING_DESC_F_NEXT     0x1u   /* buffer continues in .next            */
#define VRING_DESC_F_WRITE    0x2u   /* device writes (else device reads)    */
#define VRING_DESC_F_INDIRECT 0x4u

/* virtio-blk request types + status (little-endian in the ring header). */
#define VIRTIO_BLK_T_IN       0u     /* read from device into guest          */
#define VIRTIO_BLK_T_OUT      1u     /* write from guest to device           */
#define VIRTIO_BLK_T_FLUSH    4u
#define VIRTIO_BLK_S_OK       0u
#define VIRTIO_BLK_S_IOERR    1u
#define VIRTIO_BLK_S_UNSUPP   2u

/* ------------------------------------------------------------------ *
 * Per-queue and per-device transport state.
 * ------------------------------------------------------------------ */
struct virtq {
	uint32_t num;         /* negotiated queue size (<= VIRTIO_QUEUE_MAX)   */
	uint32_t ready;       /* QueueReady latched by the driver              */
	uint64_t desc;        /* guest PA of the descriptor table              */
	uint64_t avail;       /* guest PA of the avail ring (driver area)      */
	uint64_t used;        /* guest PA of the used ring (device area)       */
	uint16_t last_avail;  /* next avail->ring[] index we have not consumed */
};

struct virtio_dev;
typedef void (*virtio_notify_fn)(struct virtio_dev *d, uint32_t qidx);

struct virtio_dev {
	uint64_t base;                 /* MMIO IPA base                        */
	uint32_t device_id;            /* 2=blk, 3=console                     */
	uint32_t vendor_id;
	uint64_t device_features;      /* what we offer (bit32 VERSION_1 set)  */
	uint64_t driver_features;      /* what the driver accepted             */
	uint32_t dev_feat_sel;         /* DeviceFeaturesSel                    */
	uint32_t drv_feat_sel;         /* DriverFeaturesSel                    */
	uint32_t queue_sel;            /* QueueSel                             */
	uint32_t status;               /* Status register                      */
	uint32_t int_status;           /* InterruptStatus                      */
	uint32_t config_generation;
	uint32_t intid;                /* SPI INTID injected on completion     */
	uint32_t num_queues;           /* how many queues this device has      */
	struct virtq vq[VIRTIO_MAX_QUEUES];
	virtio_notify_fn notify;       /* device-specific QueueNotify handler  */
	uint8_t  config[256];          /* device-specific config space         */
};

/* A fully-read descriptor (host-endian). */
struct vdesc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/* ------------------------------------------------------------------ *
 * Public API.
 * ------------------------------------------------------------------ */

/* Register the console + blk devices and lay down breadcrumbs. Call once,
 * before stage2_enable(). Idempotent. */
void virtio_init(void);

/* el2_trap dispatch entry (see wiring contract in the banner). Decodes the
 * faulting IPA from HPFAR_EL2/FAR_EL2, matches it to a registered device's
 * register window, emulates the access, advances frame->elr, and returns 1.
 * Returns 0 if the fault is not inside any virtio device window (not ours). */
int virtio_mmio_fault(struct el2_frame *frame);

/* Internal device backends (implemented in virtio_console.c / virtio_blk.c),
 * registered by virtio_init(). Exposed so virtio.c can install them. */
struct virtio_dev *virtio_console_setup(uint64_t base);
struct virtio_dev *virtio_blk_setup(uint64_t base);

/* ---- Virtqueue engine helpers, used by the device backends. -------- */

/* Pop the next available buffer-chain head from queue qidx. Returns 1 and
 * writes *head on success, 0 if the avail ring has nothing new. */
int  virtq_pop_avail(struct virtio_dev *d, uint32_t qidx, uint16_t *head);

/* Read descriptor `idx` of queue qidx into *out (host-endian). */
void virtq_read_desc(struct virtio_dev *d, uint32_t qidx, uint16_t idx,
                     struct vdesc *out);

/* Publish a completed chain (head, total bytes written to device-writable
 * buffers = used_len) to the used ring, raise InterruptStatus.VRING, and
 * inject the device's SPI into the guest via vgic. */
void virtq_push_used(struct virtio_dev *d, uint32_t qidx, uint16_t head,
                     uint32_t used_len);

/* Coherent guest-memory access helpers (IPA==PA identity map). */
void     virtio_read(uint64_t pa, void *dst, uint32_t len);
void     virtio_write(uint64_t pa, const void *src, uint32_t len);

/* virtio breadcrumb window base (see virtio.c). Exposed for the backends. */
#define VIRTIO_BC_BASE   0x50005000UL
#define VIRTIO_BC_MAGIC  0x56494F31u   /* "VIO1" */
void virtio_bc(uint32_t word_idx, uint32_t v);

#endif /* BZDOS_VIRTIO_H */
