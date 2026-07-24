/* vblk_emmc.h — virtio-blk (VIRTIO DeviceID 2) over virtio-mmio, backed by the
 * REAL eMMC via emmc_bio.c, for the FreeBSD/arm64 EL1 guest on the bzdOS
 * from-scratch EL2 hypervisor (Allwinner A64 / Banana Pi M64).
 *
 * ============================================================================
 * WHY A NEW FILE (naming note — READ THIS FIRST)
 * ============================================================================
 * The tree ALREADY contains virtio.c / virtio.h / virtio_blk.c / virtio_console.c.
 * That stack is (a) backed by a fixed DRAM RAM disk, not the real eMMC, and
 * (b) injects its completion IRQ via vgic / GICH list registers, i.e. it
 * assumes HCR_EL2.IMO=1. The live debug hypervisor (main_dbg.c) DELIBERATELY
 * runs with IMO=0 and has removed vgic/gic_timer entirely (see the "IRQ POLICY"
 * comment in main_dbg.c and memory ehci-intid106-storm) — so the existing
 * virtio_blk.c cannot inject an interrupt on this build at all. This module is
 * the strategic replacement described in the design brief:
 *
 *   - backing store = the real eMMC (emmc_bio_read/emmc_bio_write), NOT a RAM
 *     disk. The hypervisor owns the physical eMMC; the guest sees a clean
 *     paravirtual disk and never touches the aw_mmc controller (which is the
 *     source of the GEOM taste-storm race we are eliminating).
 *   - IRQ injection = write the guest's SPI pending bit directly into the REAL
 *     GICD (GICD_ISPENDR) so the passed-through GICv2 delivers it to the guest
 *     under IMO=0. No vgic, no GICH.
 *
 * Because the code-freeze rule for this task forbids editing the existing
 * virtio_*.c/.h, this lives in NEW files (vblk_emmc.c/.h) and is intentionally
 * SELF-CONTAINED (its own register + vring definitions, <stdint.h> only) so it
 * does not couple to the RAM-disk virtio.h. See docs/virtio-blk-design.md and
 * docs/virtio-blk-integration.md.
 *
 * ============================================================================
 * TRANSPORT: virtio-mmio, MODERN (spec v2 / VIRTIO 1.x), NOT legacy.
 * ============================================================================
 * FreeBSD's virtio_mmio(4) driver probes both legacy (Version==1) and modern
 * (Version==2). We advertise Version==2 and offer ONLY VIRTIO_F_VERSION_1
 * (feature bit 32). Rationale in docs/virtio-blk-design.md — briefly: modern
 * split virtqueues give the driver separate 64-bit QueueDesc / QueueDriver /
 * QueueDevice base registers and an explicit QueueReady handshake, so the
 * hypervisor learns the exact guest-physical ring addresses from register
 * writes (no legacy guest-page-size / QueuePFN math, no assumption that the
 * three rings are contiguous). It also removes the legacy config-endianness
 * ambiguity. The one legacy convenience we lose (rings implicitly laid out
 * from a single PFN) we do not want anyway.
 *
 * Freestanding: <stdint.h> only, no libc, -mgeneral-regs-only. Every store to
 * guest-shared memory (used ring) and to breadcrumb DRAM is followed by a
 * barrier, matching the tree convention.
 */
#ifndef BZDOS_VBLK_EMMC_H
#define BZDOS_VBLK_EMMC_H

#include <stdint.h>
#include "exceptions.h"     /* struct el2_frame */

/* ------------------------------------------------------------------ *
 * MMIO window.
 *
 * VBLK_MMIO_BASE is a 4 KiB region inside a hole in the A64 physical map
 * (real peripherals end at 0x02000000; 0x0A000000 is unassigned) that still
 * falls inside the stage-2 low-1 GiB MMIO identity window. stage2.c must
 * leave the 2 MiB block that contains it UNMAPPED (invalid L2 descriptor) so
 * every guest access there takes a stage-2 data abort into EL2 — exactly the
 * mechanism vconsole uses for the UART0 page. 0x0A000000 is also the base
 * QEMU's "virt" board uses for virtio-mmio, a layout FreeBSD's arm64
 * virtio_mmio driver is already known to probe cleanly.
 *
 * One device (blk) occupies the first 0x200 bytes of the page; the rest of
 * the 2 MiB trapped block is spare for future virtio-mmio devices.
 * ------------------------------------------------------------------ */
#define VBLK_MMIO_BASE     0x0A000000UL
#define VBLK_MMIO_SIZE     0x00000200UL   /* one virtio-mmio device slot   */

/* The 2 MiB stage-2 block that must be left invalid to trap the window.
 * (0x0A000000 >> 21) == 80 — see docs/virtio-blk-integration.md. */
#define VBLK_TRAP_BLOCK_BASE  (VBLK_MMIO_BASE & ~0x1FFFFFUL)   /* 0x0A000000 */
#define VBLK_TRAP_BLOCK_SIZE  0x00200000UL                     /* 2 MiB      */

/* ------------------------------------------------------------------ *
 * Guest interrupt: a single SPI injected into the REAL GICD on completion.
 *
 * INTID = 32 + SPI_number. VBLK_SPI is the DT "GIC_SPI n" number; the DTB
 * virtio-mmio node MUST declare "interrupts = <GIC_SPI VBLK_SPI IRQ_TYPE>"
 * with the SAME number. It MUST NOT collide with any SPI a real A64
 * peripheral uses (the A64 has real SPIs scattered up to ~150; this one is
 * chosen from a gap — see docs/virtio-blk-design.md "IRQ injection"). It is
 * declared EDGE-triggered (see design doc): there is no real wire, we
 * synthesize the event, and edge semantics make a GICD_ISPENDR set-pending a
 * clean one-shot with no "level stays asserted / re-fires" hazard.
 * ------------------------------------------------------------------ */
/* SPI selection (IMO=0 => the guest shares the REAL GICD, so this MUST NOT
 * alias any real A64 peripheral line).
 *
 * The skeleton's original guess of SPI 50 was found OCCUPIED: decompiling the
 * live guest blob /opt/bzdos/tftpboot/bananapi-min.dtb shows
 * dma-controller@1c02000 already claims `interrupts = <0x00 0x32 0x04>`
 * (GIC_SPI 50). Injecting INTID 82 there would collide with the DMA engine.
 *
 * SPI 105 (0x69) is chosen instead: it lies in the contiguous block 0x68..0x73
 * that NO node in the real DTB uses, and above the highest real-peripheral SPI
 * observed in the blob (0x77=119 is the top, EMAC=0x52, MUSB=0x47). INTID 137.
 * Keep this #define and the DTB node's `interrupts` cell in lockstep.
 * TODO(board): confirm SPI 105 is not wired to any A64 peripheral the DTB
 * simply omits (the A64 scatters real SPIs up to ~150); if it collides, pick
 * another DTB gap and update BOTH this #define and docs/virtio-blk-dtb.md. */
#define VBLK_SPI      105u                /* DT: GIC_SPI 105 (0x69), DTB-free    */
#define VBLK_INTID    (32u + VBLK_SPI)    /* == 137 */

/* Real GICv2 distributor (identity-passed-through to the guest under IMO=0). */
#define VBLK_GICD_BASE       0x01C81000UL
#define VBLK_GICD_ISPENDR    0x200u       /* +0x200 + 4*(intid/32), bit intid%32 */

/* ------------------------------------------------------------------ *
 * virtio-mmio register offsets (modern transport, spec v2). Only the ones we
 * emulate are listed; everything else reads 0 / is ignored.
 * ------------------------------------------------------------------ */
#define VBLK_R_MAGIC_VALUE        0x000u  /* R  0x74726976 "virt"              */
#define VBLK_R_VERSION            0x004u  /* R  2                              */
#define VBLK_R_DEVICE_ID          0x008u  /* R  2 (blk)                        */
#define VBLK_R_VENDOR_ID          0x00Cu  /* R                                 */
#define VBLK_R_DEVICE_FEATURES    0x010u  /* R  (word selected by _SEL)        */
#define VBLK_R_DEVICE_FEATURES_SEL 0x014u /* W                                 */
#define VBLK_R_DRIVER_FEATURES    0x020u  /* W  (word selected by _SEL)        */
#define VBLK_R_DRIVER_FEATURES_SEL 0x024u /* W                                 */
#define VBLK_R_QUEUE_SEL          0x030u  /* W                                 */
#define VBLK_R_QUEUE_NUM_MAX      0x034u  /* R                                 */
#define VBLK_R_QUEUE_NUM          0x038u  /* W                                 */
#define VBLK_R_QUEUE_READY        0x044u  /* RW                                */
#define VBLK_R_QUEUE_NOTIFY       0x050u  /* W  (kick)                         */
#define VBLK_R_INTERRUPT_STATUS   0x060u  /* R                                 */
#define VBLK_R_INTERRUPT_ACK      0x064u  /* W                                 */
#define VBLK_R_STATUS             0x070u  /* RW                                */
#define VBLK_R_QUEUE_DESC_LOW     0x080u  /* W  descriptor table PA[31:0]      */
#define VBLK_R_QUEUE_DESC_HIGH    0x084u  /* W                    PA[63:32]    */
#define VBLK_R_QUEUE_DRIVER_LOW   0x090u  /* W  avail-ring PA[31:0]            */
#define VBLK_R_QUEUE_DRIVER_HIGH  0x094u  /* W                                 */
#define VBLK_R_QUEUE_DEVICE_LOW   0x0A0u  /* W  used-ring PA[31:0]             */
#define VBLK_R_QUEUE_DEVICE_HIGH  0x0A4u  /* W                                 */
#define VBLK_R_CONFIG_GENERATION  0x0FCu  /* R                                 */
#define VBLK_R_CONFIG             0x100u  /* R  device-specific config space   */

#define VBLK_MMIO_MAGIC           0x74726976u  /* "virt" LE                    */
#define VBLK_MMIO_VERSION         2u
#define VBLK_MMIO_VENDOR          0x627A6473u  /* "bzds"                        */

/* Status register bits (VIRTIO 1.x). */
#define VBLK_S_ACKNOWLEDGE        0x01u
#define VBLK_S_DRIVER             0x02u
#define VBLK_S_DRIVER_OK          0x04u
#define VBLK_S_FEATURES_OK        0x08u
#define VBLK_S_NEEDS_RESET        0x40u
#define VBLK_S_FAILED             0x80u

/* InterruptStatus bits. */
#define VBLK_INT_VRING            0x1u    /* used ring advanced                */

/* Feature bit 32 = VIRTIO_F_VERSION_1 lives in feature word 1, bit 0. */
#define VBLK_FEATWORD_HI          1u
#define VBLK_F_VERSION_1_BIT      0x1u

/* Only one request queue for virtio-blk. */
#define VBLK_QUEUE                0u
#define VBLK_NUM_QUEUES           1u
#define VBLK_QUEUE_MAX            256u    /* advertised QueueNumMax             */
#define VBLK_MAX_CHAIN            32u     /* descriptors walked per request     */

/* split-virtqueue descriptor flags. */
#define VRING_DESC_F_NEXT         0x1u    /* buffer continues in .next          */
#define VRING_DESC_F_WRITE        0x2u    /* device writes it (else reads)      */
#define VRING_DESC_F_INDIRECT     0x4u    /* .addr points at a desc array       */

/* virtio-blk request header type + status byte. */
#define VIRTIO_BLK_T_IN           0u      /* read: device -> guest              */
#define VIRTIO_BLK_T_OUT          1u      /* write: guest -> device             */
#define VIRTIO_BLK_T_FLUSH        4u
#define VIRTIO_BLK_T_GET_ID       8u      /* fetch a fixed device-id string     */
#define VIRTIO_BLK_S_OK           0u
#define VIRTIO_BLK_S_IOERR        1u
#define VIRTIO_BLK_S_UNSUPP       2u

/* VIRTIO_BLK_ID_BYTES: FreeBSD's vtblk_ident() requests
 * MIN(VIRTIO_BLK_ID_BYTES, DISK_IDENT_SIZE) == 20 bytes for GET_ID. */
#define VBLK_ID_BYTES             20u

/* Backing eMMC geometry. The disk the guest sees is the whole eMMC, exposed
 * 1:1: virtio sector N == eMMC LBA N (both 512-byte units, emmc_bio does
 * SET_BLOCKLEN(512)). config.capacity advertises this count. TODO(board):
 * read the real capacity from the eMMC CSD/EXT_CSD during emmc_bio_init();
 * until then advertise a conservative fixed size (see vblk_emmc.c). */
#define VBLK_SECTOR_BYTES         512u

/* ------------------------------------------------------------------ *
 * eMMC-CONTROLLER MUTUAL EXCLUSION (design §8.1, the stated TOP RISK).
 *
 * The single aw_mmc controller at 0x01c11000 is driven from TWO cores:
 *   - CPU0: this virtio-blk device, inside the guest's QueueNotify trap.
 *   - CPU1: the debug core, which can be told to run emmc_bio_read/write via
 *     dbgmon's `call <pa>` command (the whole reason emmc_bio.c exists — see
 *     emmc_bio.h). Two cores poking the FIFO/command registers concurrently
 *     corrupts the in-flight transfer.
 *
 * Guard: a single test-and-set spinlock in a FIXED DRAM word (below), taken
 * around every emmc_bio call. virtio-blk (CPU0) takes it internally. The CPU1
 * side cannot be hooked at compile time (it enters emmc_bio via a runtime
 * `call`), so the lock is exposed here: the operator brackets a CPU1 eMMC
 * access with `call vblk_emmc_trylock` / `call vblk_emmc_unlock` over dbgmon,
 * OR simply keeps the CPU1 bio path quiescent while the guest is doing
 * virtio-blk I/O. The word lives in DRAM (not .bss) so it is cross-core
 * coherent AND inspectable/resettable over EMAC; vblk_init() zeroes it so a
 * stale "locked" value can never survive a warm WDT reset into a deadlock.
 * ------------------------------------------------------------------ */
#define VBLK_EMMC_LOCK_PA   0x50020100UL   /* MOVED from 0x50005100 — that was INSIDE the
                                            * 64KB vconsole ring (0x50000f10..0x50010f10),
                                            * so console output clobbered the lock word ->
                                            * it read "locked" (garbage) -> every guest read
                                            * failed emmc_lock_acquire_bounded -> S_IOERR ->
                                            * no vtbd0pN partitions. THE root virtio-blk bug.
                                            * 0x50020100 is above the ring, clear. */

/* Try to acquire the eMMC-controller lock: returns 1 on success (caller now
 * owns the controller and MUST call vblk_emmc_unlock()), 0 if already held.
 * Non-blocking, safe to `call` from the CPU1 debug core over dbgmon. */
int  vblk_emmc_trylock(void);

/* Release the eMMC-controller lock. */
void vblk_emmc_unlock(void);

/* ------------------------------------------------------------------ *
 * USED-RING / COMPLETION-PUBLISH MUTUAL EXCLUSION.
 *
 * ROOT CAUSE (found live 2026-07-24 chasing why `fsck -y /dev/vtbd0p3`
 * reproducibly corrupted the guest — manifesting as the console rapidly
 * re-printing stale early-boot text, megabytes of it in seconds): the
 * ROADMAP-C2 async I/O offload (vblk_async.c, CPU2) completes a request by
 * calling vq_push_used() + setting int_status + vblk_inject_irq() — and
 * vblk_request()'s SYNCHRONOUS fallback path (CPU0) does the exact same
 * three things for its own completions, with NO exclusion between them.
 * vq_push_used() is a classic unlocked read-modify-write (read used->idx,
 * write the ring slot, write idx+1) on a virtqueue struct BOTH cores can
 * complete into. Under light I/O (mount, ls) the two completions never
 * actually overlap in real time, so the race never fired — but fsck's
 * sustained back-to-back request pattern was the first workload to ever
 * actually exercise the C2 async path under enough concurrent load to hit
 * it: CPU0 and CPU2 completing two DIFFERENT requests at genuinely the same
 * moment lose one ring entry (both write the SAME slot, idx only advances
 * by 1 for what should have been 2 completions), permanently desyncing the
 * guest's view of the used ring from reality — exactly the kind of
 * corruption that can make a completed read appear to hand back a stale/
 * wrong buffer's content instead of the real disk data.
 *
 * Same test-and-set-in-fixed-DRAM-word pattern as VBLK_EMMC_LOCK_PA above,
 * zeroed in vblk_init() for the same warm-reset-safety reason. Deliberately
 * a SEPARATE word/lock: this protects the virtqueue completion-publish step
 * (a CPU0-vs-CPU2 race), which is an entirely different critical section
 * than the eMMC controller register access the other lock guards. */
#define VBLK_USED_LOCK_PA   0x50020200UL   /* clear of the vconsole ring, the
                                            * eMMC lock (0x50020100), and every
                                            * vblk_bc() breadcrumb slot. */

/* Try to acquire the used-ring publish lock: 1 on success (caller now owns
 * the virtqueue completion step and MUST call vblk_used_unlock()), 0 if
 * already held (the other core is mid-publish; caller should spin briefly
 * and retry — the critical section is a handful of word writes, never a
 * real hardware wait). */
int  vblk_used_trylock(void);
void vblk_used_unlock(void);

/* ------------------------------------------------------------------ *
 * Split-virtqueue in-guest layout (VIRTIO 1.x, little-endian). We read these
 * out of guest DRAM (identity-mapped, IPA==PA) into host-endian structs.
 * ------------------------------------------------------------------ */
struct vring_desc {          /* 16 bytes, at desc_pa + 16*index */
	uint64_t addr;           /* guest PA of the buffer          */
	uint32_t len;
	uint16_t flags;          /* VRING_DESC_F_*                  */
	uint16_t next;
};
/* avail ring: le16 flags; le16 idx; le16 ring[num]; (le16 used_event)      */
/* used  ring: le16 flags; le16 idx; struct { le32 id; le32 len; } ring[num]*/

/* A descriptor read out into host-endian form. */
struct vblk_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/* ------------------------------------------------------------------ *
 * Per-queue + device state.
 * ------------------------------------------------------------------ */
struct vblk_vq {
	uint32_t num;            /* negotiated queue size                       */
	uint32_t ready;          /* QueueReady                                  */
	uint64_t desc;           /* guest PA of descriptor table                */
	uint64_t avail;          /* guest PA of avail ring (driver area)        */
	uint64_t used;           /* guest PA of used ring (device area)         */
	uint16_t last_avail;     /* next avail->ring[] index not yet consumed   */
};

struct vblk_dev {
	uint64_t base;           /* MMIO IPA base                               */
	uint32_t dev_feat_sel;
	uint32_t drv_feat_sel;
	uint64_t driver_features;
	uint32_t queue_sel;
	uint32_t status;
	uint32_t int_status;
	uint32_t config_gen;
	uint64_t capacity;       /* sectors; advertised in config.capacity      */
	uint32_t emmc_ready;     /* emmc_bio_init() returned 0                   */
	struct vblk_vq vq[VBLK_NUM_QUEUES];
};

/* ------------------------------------------------------------------ *
 * Public API.
 * ------------------------------------------------------------------ */

/* One-time setup: zero device state, bring the eMMC up (emmc_bio_init), latch
 * capacity, lay a breadcrumb. Call once BEFORE stage2_enable() (so the guest
 * cannot fault on the window before we are ready), alongside vconsole_init().
 * Idempotent. Returns 0 on success, negative if emmc_bio_init() failed (the
 * device still registers but every request will complete VIRTIO_BLK_S_IOERR
 * until the eMMC comes up). */
int  vblk_init(void);

/* el2_trap dispatch entry. On a lower-EL (group 2) synchronous data abort
 * (ESR_EL2.EC==0x24), the trap handler calls this AFTER vconsole. It:
 *   - reconstructs the faulting IPA from HPFAR_EL2 + FAR_EL2[11:0];
 *   - returns 0 immediately if the IPA is not inside our MMIO window (the
 *     access is not ours — let the generic fault path record it);
 *   - otherwise decodes the ISS (ISV/SAS/SRT/WnR), emulates the register
 *     read/write (a QueueNotify write drives vblk_kick()), advances
 *     frame->elr past the faulting instruction, and returns 1.
 * Mirrors vconsole_handle_fault()'s contract exactly. */
int  vblk_mmio_fault(struct el2_frame *frame);

/* ------------------------------------------------------------------ *
 * ASYNC I/O OFFLOAD (ROADMAP milestone C2): CPU2 mailbox consumer.
 *
 * vblk_kick()/vblk_request() (CPU0, inside the guest's QueueNotify trap) try
 * to hand every T_IN/T_OUT request to a single-slot mailbox instead of
 * running the eMMC PIO inline; vblk_async_poll() is the CPU2-side consumer
 * that drains it. See vblk_async.c/.h for the CPU2 bring-up loop that calls
 * this in a tight bounded loop, and for the full mailbox-protocol / memory-
 * ordering writeup (the mailbox producer+consumer both live HERE, in
 * vblk_emmc.c, not in vblk_async.c — they need this file's already-static
 * helpers: gmem_read/gmem_write, vq_push_used, vblk_inject_irq, serve_data).
 *
 * Call ONLY from CPU2's dedicated loop, never from CPU0/the guest trap path.
 * Each call does AT MOST one mailbox request's worth of work (itself bounded
 * by emmc_bio.c's iteration-capped polls) and returns immediately if the
 * mailbox is empty — it never blocks. */
void vblk_async_poll(void);

/* Set to 1 by vblk_async_cpu2_run() (vblk_async.c) once CPU2's mailbox-
 * draining loop is actually running; stays 0 forever in any build that
 * doesn't link vblk_async.o (e.g. the gdb build's GDB_OBJS, which links this
 * file + emmc_bio.o but not vblk_async.o). vblk_async_post() (vblk_emmc.c)
 * checks this BEFORE ever marking the mailbox POSTED, so such builds fall
 * back to the pre-C2 fully-synchronous path for every request, unchanged.
 * This is THE on/off switch for the whole async path — do not add a second,
 * independent one that gates only the CPU2 loop or only the producer: a
 * request posted with nobody guaranteed to drain it hangs the guest's I/O
 * forever (see vblk_async.h's design note). */
extern volatile uint32_t g_vblk_async_ready;

#endif /* BZDOS_VBLK_EMMC_H */
