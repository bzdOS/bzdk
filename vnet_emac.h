/* vnet_emac.h — virtio-net (VIRTIO DeviceID 1) over virtio-mmio, multiplexed
 * onto the REAL EMAC (sun8i-emac, emac.c/emac.h) for the FreeBSD/arm64 EL1
 * guest under the bzdOS EL2 hypervisor (Allwinner A64 / Banana Pi M64).
 *
 * ROADMAP C1 ("virtio-net через EMAC-мультиплексор"): the EMAC is currently
 * exclusively the HV's own debug-protocol NIC (ethertype 0x88B5, console +
 * netcon 0x88B6 — see emac.c/emac.h/netcon.h). This device gives the GUEST a
 * virtio-net NIC that shares that same physical EMAC with the debug channel:
 *
 *   TX (guest -> wire): the guest posts an outbound Ethernet frame on the
 *     transmitq (queue 1). We inspect its ethertype:
 *       - 0x88B5 (our own debug protocol)      -> DROPPED, never reaches
 *         emac_send_frame()/real hardware. The guest must never be able to
 *         inject a fake debug-protocol frame (spoof the HV's own control
 *         channel) — this is a hard security invariant, not a performance
 *         optimization.
 *       - anything else                        -> emac_send_frame(ethertype,
 *         payload, len), i.e. actually transmitted.
 *
 *   RX (wire -> guest): emac.c's RX demux (emac_poll()) already special-cases
 *     0x88B5 (console) and 0x88B6 (netcon). A NEW hook, vnet_emac_rx_frame(),
 *     is called for every OTHER accepted ethertype (addressed to our MAC or
 *     broadcast) instead of being silently dropped. See the one-line emac.c
 *     hook this file's implementation comment describes.
 *
 * ============================================================================
 * WHY A NEW FILE (mirrors vblk_emmc.h's naming note)
 * ============================================================================
 * Exactly like vblk_emmc.c/.h versus the tree's existing (RAM-disk / vgic-IRQ)
 * virtio_blk.c, this is a NEW, SELF-CONTAINED virtio-mmio device — same modern
 * (v2) transport, same IMO=0 / real-GICD IRQ-injection strategy, same
 * <stdint.h>-only freestanding style, same gmem_read/gmem_write/gmem_cmo
 * guest-memory-access pattern (duplicated here on purpose — see vblk_emmc.c's
 * comment on why: this file must not depend on vblk_emmc.c internals, which
 * are a different, reserved lane).
 *
 * ============================================================================
 * TRANSPORT: virtio-mmio, MODERN (spec v2 / VIRTIO 1.x), NOT legacy. Only
 * VIRTIO_F_VERSION_1 is offered — no VIRTIO_NET_F_MAC, no CSUM/offload
 * features, no VIRTIO_NET_F_MRG_RXBUF, no VIRTIO_NET_F_STATUS. This means:
 *   - every virtio_net_hdr on the wire is the LEGACY 10-byte form (no trailing
 *     num_buffers field — that only exists under MRG_RXBUF);
 *   - the guest picks its own (random) virtio-net MAC; this does not matter
 *     because emac_send_frame() always transmits with src = EMAC's OWN MAC
 *     (see the "MAC identity" note in vnet_emac.c) — the guest's virtio MAC
 *     never appears on the wire.
 * ============================================================================
 */
#ifndef BZDOS_VNET_EMAC_H
#define BZDOS_VNET_EMAC_H

#include <stdint.h>
#include "exceptions.h"     /* struct el2_frame */

/* ------------------------------------------------------------------ *
 * MMIO window.
 *
 * VBLK_MMIO_BASE (vblk_emmc.h) is 0x0A000000 and stage2.c leaves the WHOLE
 * containing 2 MiB block (0x0A000000..0x0A200000, L2 index 80) INVALID at
 * stage-2 — vblk_emmc.h's own comment says as much: "the rest of the 2 MiB
 * trapped block is spare for future virtio-mmio devices". VNET_MMIO_BASE
 * below is chosen INSIDE that same already-trapped 2 MiB block, at a 4 KiB-
 * aligned offset comfortably clear of vblk's own 0x200-byte register window
 * (0x0A000000..0x0A000200):
 *
 *   0x0A000000 .. 0x0A000200   vblk_emmc (existing, untouched)
 *   0x0A000200 .. 0x0A001000   unused gap (future slop)
 *   0x0A001000 .. 0x0A001200   vnet_emac (THIS device)
 *   0x0A001200 .. 0x0A200000   still spare
 *
 * Consequence: NO stage2.c EDIT IS NEEDED for this device — the L2 entry at
 * index 80 is already unmapped/invalid for vblk's sake, so a guest access
 * anywhere in that 2 MiB range (including 0x0A001000) already takes the same
 * stage-2 data abort into EL2 that vblk_mmio_fault() relies on. This file
 * only needs its fault handler wired into el2_trap's dispatch chain (see the
 * one-line addition described in the report / vnet_mmio_fault()'s doc
 * comment below) — stage2.c stays exactly as-is.
 * ------------------------------------------------------------------ */
#define VNET_MMIO_BASE     0x0A001000UL
#define VNET_MMIO_SIZE     0x00000200UL   /* one virtio-mmio device slot   */

/* ------------------------------------------------------------------ *
 * Guest interrupt: a single SPI injected into the REAL GICD on completion,
 * exactly like VBLK_INTID (vblk_emmc.h) — same IMO=0 / GICD_ISPENDR strategy,
 * duplicated here (not shared) to keep this file self-contained.
 *
 * SPI chosen from the SAME free contiguous gap docs/virtio-blk-dtb.md
 * identified (0x68..0x73, i.e. 104..115 decimal) that VBLK_SPI=105 (0x69)
 * already uses one slot of. VNET_SPI=106 (0x6A) is the NEXT number in that
 * gap, still clear of every real A64 peripheral SPI in the live DTB blob
 * (highest observed = 0x77=119) and clear of VBLK_SPI. ONE virtio-mmio
 * device -> ONE interrupt line for BOTH its virtqueues (RX and TX share the
 * single InterruptStatus.VRING bit, exactly as the virtio-mmio spec permits
 * — the driver re-checks both queues on any notification). Keep this
 * #define and the DTB node's `interrupts` cell in lockstep (see the parallel
 * note in vblk_emmc.h / docs/virtio-blk-dtb.md — the equivalent vnet DTB
 * node is NOT yet added; see the report). */
#define VNET_SPI      106u                /* DT: GIC_SPI 106 (0x6A), DTB-free    */
#define VNET_INTID    (32u + VNET_SPI)    /* == 138 */

/* Real GICv2 distributor (identity-passed-through to the guest under IMO=0).
 * Same physical register vblk_emmc.h uses; duplicated for self-containment. */
#define VNET_GICD_BASE       0x01C81000UL
#define VNET_GICD_ISPENDR    0x200u       /* +0x200 + 4*(intid/32), bit intid%32 */

/* ------------------------------------------------------------------ *
 * virtio-mmio register offsets — identical layout/semantics to vblk_emmc.h's
 * (the boilerplate is IDENTICAL across virtio device types, per this task's
 * brief). Duplicated rather than shared so this file has zero dependency on
 * vblk_emmc.h (a different, reserved lane).
 * ------------------------------------------------------------------ */
#define VNET_R_MAGIC_VALUE        0x000u  /* R  0x74726976 "virt"              */
#define VNET_R_VERSION            0x004u  /* R  2                              */
#define VNET_R_DEVICE_ID          0x008u  /* R  1 (net)                        */
#define VNET_R_VENDOR_ID          0x00Cu  /* R                                 */
#define VNET_R_DEVICE_FEATURES    0x010u  /* R  (word selected by _SEL)        */
#define VNET_R_DEVICE_FEATURES_SEL 0x014u /* W                                 */
#define VNET_R_DRIVER_FEATURES    0x020u  /* W  (word selected by _SEL)        */
#define VNET_R_DRIVER_FEATURES_SEL 0x024u /* W                                 */
#define VNET_R_QUEUE_SEL          0x030u  /* W                                 */
#define VNET_R_QUEUE_NUM_MAX      0x034u  /* R                                 */
#define VNET_R_QUEUE_NUM          0x038u  /* W                                 */
#define VNET_R_QUEUE_READY        0x044u  /* RW                                */
#define VNET_R_QUEUE_NOTIFY       0x050u  /* W  (kick)                         */
#define VNET_R_INTERRUPT_STATUS   0x060u  /* R                                 */
#define VNET_R_INTERRUPT_ACK      0x064u  /* W                                 */
#define VNET_R_STATUS             0x070u  /* RW                                */
#define VNET_R_QUEUE_DESC_LOW     0x080u  /* W  descriptor table PA[31:0]      */
#define VNET_R_QUEUE_DESC_HIGH    0x084u  /* W                    PA[63:32]    */
#define VNET_R_QUEUE_DRIVER_LOW   0x090u  /* W  avail-ring PA[31:0]            */
#define VNET_R_QUEUE_DRIVER_HIGH  0x094u  /* W                                 */
#define VNET_R_QUEUE_DEVICE_LOW   0x0A0u  /* W  used-ring PA[31:0]             */
#define VNET_R_QUEUE_DEVICE_HIGH  0x0A4u  /* W                                 */
#define VNET_R_CONFIG_GENERATION  0x0FCu  /* R                                 */
#define VNET_R_CONFIG             0x100u  /* R  device-specific config space   */

#define VNET_MMIO_MAGIC           0x74726976u  /* "virt" LE                    */
#define VNET_MMIO_VERSION         2u
#define VNET_MMIO_VENDOR          0x627A6473u  /* "bzds"                        */
#define VNET_DEVICE_ID            1u           /* virtio-net (vblk uses 2)      */

/* Status register bits (VIRTIO 1.x) — identical to vblk_emmc.h. */
#define VNET_S_ACKNOWLEDGE        0x01u
#define VNET_S_DRIVER             0x02u
#define VNET_S_DRIVER_OK          0x04u
#define VNET_S_FEATURES_OK        0x08u
#define VNET_S_NEEDS_RESET        0x40u
#define VNET_S_FAILED             0x80u

/* InterruptStatus bits. */
#define VNET_INT_VRING            0x1u    /* used ring advanced (either queue) */

/* Feature bit 32 = VIRTIO_F_VERSION_1 lives in feature word 1, bit 0. This is
 * the ONLY feature bit offered — no VIRTIO_NET_F_MAC/CSUM/MRG_RXBUF/STATUS/
 * offloads, matching vblk_emmc.c's minimal-feature-set precedent. */
#define VNET_FEATWORD_HI          1u
#define VNET_F_VERSION_1_BIT      0x1u

/* virtio-net queue convention (spec): 0 = receiveq (RX, device->driver),
 * 1 = transmitq (TX, driver->device). Unlike vblk's single request queue. */
#define VNET_QUEUE_RX             0u
#define VNET_QUEUE_TX             1u
#define VNET_NUM_QUEUES           2u
#define VNET_QUEUE_MAX            256u    /* advertised QueueNumMax, per queue  */
#define VNET_MAX_CHAIN            32u     /* descriptors walked per request     */

/* split-virtqueue descriptor flags — identical to vblk_emmc.h. */
#define VNET_VRING_DESC_F_NEXT     0x1u    /* buffer continues in .next          */
#define VNET_VRING_DESC_F_WRITE    0x2u    /* device writes it (else reads)      */
#define VNET_VRING_DESC_F_INDIRECT 0x4u    /* .addr points at a desc array       */

/* ------------------------------------------------------------------ *
 * virtio-net packet header.
 *
 * BUG FIX (2026-07-23, found live via tcpdump on br0 during the first-ever
 * real TX test: every frame's ethertype field showed 2 bytes of GARBAGE —
 * the low 16 bits of the guest's OWN source MAC — with the TRUE ethertype
 * (0x0806, ARP) sitting as the first 2 bytes of what should have been pure
 * payload. That is exactly the signature of every downstream offset being
 * 2 bytes too small.
 *
 * The original comment here had the VIRTIO_NET_F_MRG_RXBUF rule BACKWARDS.
 * Per VIRTIO 1.0 §5.1.6.1: the trailing `num_buffers` field is omitted ONLY
 * for a LEGACY (pre-1.0) device. For a MODERN device — which is exactly
 * what this is, since only VIRTIO_F_VERSION_1 is offered (see this file's
 * top-of-file comment) — `num_buffers` is ALWAYS present, regardless of
 * whether MRG_RXBUF itself is negotiated. So the on-wire header the real
 * FreeBSD if_vtnet(4) driver actually uses is 12 bytes, not 10 — confirmed
 * by the exact 2-byte discrepancy observed on the wire. */
#define VNET_HDR_LEN               12u
struct vnet_hdr {
	uint8_t  flags;
	uint8_t  gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
	uint16_t num_buffers;   /* always present for a VERSION_1 (modern) device */
};

/* Ethernet framing constants. */
#define VNET_ETH_HDR_LEN           14u    /* dst(6)+src(6)+ethertype(2)         */
#define VNET_ETH_MIN_FRAME         60u    /* pre-CRC minimum (matches emac.c)   */
#define VNET_ETH_MAX_FRAME         1514u  /* dst+src+type+1500 payload, no VLAN */

/* Debug-protocol ethertype the guest must NEVER be able to inject or receive
 * as if it were its own traffic. MUST match ETHERTYPE_CONSOLE in emac.c
 * (0x88B5) and ETHERTYPE_NETCON (0x88B6) — both are barred from vnet's TX
 * mux (0x88B5 hard-blocked; 0x88B6 never reaches vnet's RX hook at all
 * because emac.c's existing dispatch claims it first, see vnet_emac.c). */
#define VNET_ETHERTYPE_DEBUG       0x88B5u
#define VNET_ETHERTYPE_NETCON      0x88B6u

/* Internal linear staging buffer for one frame (header + full Ethernet
 * frame), used both to concatenate a TX descriptor chain before inspecting
 * its ethertype, and to assemble one RX completion before scattering it back
 * across the guest's descriptor chain. Sized for the legacy header plus the
 * largest untagged Ethernet frame emac.c can produce/consume (ETH_BUFSIZE
 * there is 2048; we stay well under it). */
#define VNET_STAGE_BUF_SIZE        2048u

/* ------------------------------------------------------------------ *
 * Split-virtqueue in-guest layout (VIRTIO 1.x, little-endian) — identical
 * shape to vblk_emmc.h's, duplicated for self-containment.
 * ------------------------------------------------------------------ */
struct vnet_vring_desc {     /* 16 bytes, at desc_pa + 16*index */
	uint64_t addr;           /* guest PA of the buffer          */
	uint32_t len;
	uint16_t flags;          /* VNET_VRING_DESC_F_*             */
	uint16_t next;
};

struct vnet_desc {           /* host-endian decoded form */
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/* ------------------------------------------------------------------ *
 * Per-queue + device state.
 * ------------------------------------------------------------------ */
struct vnet_vq {
	uint32_t num;            /* negotiated queue size                       */
	uint32_t ready;          /* QueueReady                                  */
	uint64_t desc;           /* guest PA of descriptor table                */
	uint64_t avail;          /* guest PA of avail ring (driver area)        */
	uint64_t used;           /* guest PA of used ring (device area)         */
	uint16_t last_avail;     /* next avail->ring[] index not yet consumed   */
};

struct vnet_dev {
	uint64_t base;           /* MMIO IPA base                               */
	uint32_t dev_feat_sel;
	uint32_t drv_feat_sel;
	uint64_t driver_features;
	uint32_t queue_sel;
	uint32_t status;
	uint32_t int_status;
	uint32_t config_gen;
	struct vnet_vq vq[VNET_NUM_QUEUES];
};

/* ------------------------------------------------------------------ *
 * Public API.
 * ------------------------------------------------------------------ */

/* One-time setup: zero device state, lay a breadcrumb. Call once alongside
 * vblk_init()/vconsole_init(), BEFORE stage2_enable(). Idempotent. Always
 * "succeeds" (there is no separate backing-hardware bring-up like eMMC —
 * this device rides on emac_init(), already brought up independently by
 * main_*.c). */
void vnet_init(void);

/* el2_trap dispatch entry. Same contract as vblk_mmio_fault(): called on a
 * lower-EL data abort (ESR_EL2.EC==0x24) AFTER vconsole/vblk. Returns 0 for
 * any IPA outside [VNET_MMIO_BASE, VNET_MMIO_BASE+VNET_MMIO_SIZE) (not our
 * window — safe to call unconditionally on every guest data abort); handles
 * + advances frame->elr and returns 1 otherwise. See vnet_emac.c's dispatch
 * comment for the ONE-LINE el2_exc.c addition this implies (not applied —
 * el2_exc.c is a reserved file for this task; described in the report). */
int vnet_mmio_fault(struct el2_frame *frame);

/* RX hook for emac.c's receive path. Called (from CPU1's emac_poll(), see
 * the concurrency note in vnet_emac.c) for every accepted inbound Ethernet
 * frame whose ethertype is neither the debug console's (0x88B5) nor
 * netcon's (0x88B6) — i.e. every frame emac.c's EXISTING dispatch does not
 * already claim. `frame` points at the FULL Ethernet frame (dst MAC first
 * byte) and `len` is its total length (>=14, <=ETH_RXSIZE, already CRC/
 * length/error-checked by emac_poll() before this is called). Prepends the
 * legacy 10-byte virtio_net_hdr, scatters into the guest's next available RX
 * descriptor chain (queue 0) if one exists, and injects VNET_INTID; drops
 * (with a breadcrumb count) if the guest has posted no RX buffer, or if the
 * device is not yet DRIVER_OK. Never blocks. NOT reentrant against itself
 * (only ever called from the single EMAC-polling core) but IS reentrant
 * against vnet_mmio_fault() running concurrently on the OTHER core — see
 * the cross-core concurrency comment block in vnet_emac.c before relying on
 * this in a build that matters. */
void vnet_emac_rx_frame(const uint8_t *frame, uint16_t len);

#endif /* BZDOS_VNET_EMAC_H */
