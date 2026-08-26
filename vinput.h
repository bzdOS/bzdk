/* SPDX-License-Identifier: BSD-2-Clause */

/* vinput.h — virtio-input (DeviceID 18) over virtio-mmio, HOST-DRIVEN keyboard
 * for the FreeBSD/arm64 EL1 guest under the bzdOS EL2 hypervisor.
 *
 * WHY THIS EXISTS: the guest has never had any real input device. Its only
 * evdev node is kbdmux0, "System keyboard multiplexer" — a mux with nothing
 * plugged into it. The board's EHCI/OHCI entries in dmesg are clock-tree
 * definitions only; no USB host stack reaches the guest, and giving it real
 * USB is the same class of hazard that produced the EHCI SPI 106 interrupt
 * storm this project already fixed once (see memory ehci-intid106-storm) —
 * not a safe near-term move. So: a virtual keyboard, events synthesized by
 * EL2 and injected exactly like vnet_emac's RX path injects a frame. Mirrors
 * vblk_emmc.c/vnet_emac.c's structure throughout (same register-emulation
 * boilerplate, same duplicated-not-shared gmem/vq helpers, same cross-core
 * discipline): CPU0 runs the guest and owns register emulation; event
 * INJECTION can be called from CPU1 (dbgmon, on an operator's `type`/`key`
 * command), the same split vnet_emac_rx_frame() already has from CPU1's
 * emac_poll() loop.
 *
 * SCOPE, v1: EV_KEY + EV_SYN only. No EV_REL/EV_ABS (pointer/touch) yet —
 * named as the natural next device, not built here, so a mobile compositor
 * has something to click with but nothing to point with until then.
 *
 * PLACEMENT: VINPUT_MMIO_BASE (0x0A003000) sits INSIDE the same 2 MiB
 * stage-2-trapped block vblk_emmc.h opened (0x0A000000..0x0A200000), clear
 * of vblk (0x0A000000), vnet (0x0A001000), and scanout's flip doorbell
 * (0x0A002000, scanout.h):
 *
 *   0x0A000000 .. 0x0A000200   vblk_emmc   (existing, untouched)
 *   0x0A001000 .. 0x0A001200   vnet_emac   (existing, untouched)
 *   0x0A002000 .. 0x0A002100   scanout     (existing, untouched, HV_HDMI only)
 *   0x0A003000 .. 0x0A003200   vinput      (THIS device)
 *   0x0A003200 .. 0x0A200000   still spare
 *
 * Consequence, same as vnet/scanout: NO stage2.c edit needed — the whole 2
 * MiB block is already invalid at stage-2 for vblk's sake, so a guest access
 * anywhere in it (including 0x0A003000) already takes the stage-2 data abort
 * this file's fault handler relies on. Only el2_trap's dispatch chain needs
 * the one-line addition (see vinput_mmio_fault()'s doc comment).
 */
#ifndef BZDOS_VINPUT_H
#define BZDOS_VINPUT_H

#include <stdint.h>
#include "exceptions.h"     /* struct el2_frame */

#define VINPUT_MMIO_BASE     0x0A003000UL
#define VINPUT_MMIO_SIZE     0x00000200UL   /* one virtio-mmio device slot   */

/* Guest interrupt: same IMO=0 / GICD_ISPENDR strategy as VBLK_INTID/VNET_INTID,
 * duplicated here for self-containment. VBLK_SPI=105 (0x69), VNET_SPI=106
 * (0x6A) are already taken from the free 0x68..0x73 gap
 * (docs/virtio-blk-dtb.md); this is the next number in it, still clear of
 * every real A64 peripheral SPI observed in the live DTB blob (highest =
 * 0x77=119) and clear of both existing virtio devices. */
#define VINPUT_SPI      107u               /* DT: GIC_SPI 107 (0x6B), DTB-free   */
#define VINPUT_INTID    (32u + VINPUT_SPI) /* == 139 */

/* Real GICv2 distributor (identity-passed-through to the guest under IMO=0).
 * Same physical register vblk_emmc.h/vnet_emac.h use; duplicated here. */
#define VINPUT_GICD_BASE       0x01C81000UL
#define VINPUT_GICD_ISPENDR    0x200u

/* ------------------------------------------------------------------ *
 * virtio-mmio register offsets (modern transport, spec v2) — identical
 * layout to vblk_emmc.h/vnet_emac.h, duplicated per this file's
 * self-containment convention.
 * ------------------------------------------------------------------ */
#define VINPUT_R_MAGIC_VALUE        0x000u
#define VINPUT_R_VERSION            0x004u
#define VINPUT_R_DEVICE_ID          0x008u
#define VINPUT_R_VENDOR_ID          0x00Cu
#define VINPUT_R_DEVICE_FEATURES    0x010u
#define VINPUT_R_DEVICE_FEATURES_SEL 0x014u
#define VINPUT_R_DRIVER_FEATURES    0x020u
#define VINPUT_R_DRIVER_FEATURES_SEL 0x024u
#define VINPUT_R_QUEUE_SEL          0x030u
#define VINPUT_R_QUEUE_NUM_MAX      0x034u
#define VINPUT_R_QUEUE_NUM          0x038u
#define VINPUT_R_QUEUE_READY        0x044u
#define VINPUT_R_QUEUE_NOTIFY       0x050u
#define VINPUT_R_INTERRUPT_STATUS   0x060u
#define VINPUT_R_INTERRUPT_ACK      0x064u
#define VINPUT_R_STATUS             0x070u
#define VINPUT_R_QUEUE_DESC_LOW     0x080u
#define VINPUT_R_QUEUE_DESC_HIGH    0x084u
#define VINPUT_R_QUEUE_DRIVER_LOW   0x090u
#define VINPUT_R_QUEUE_DRIVER_HIGH  0x094u
#define VINPUT_R_QUEUE_DEVICE_LOW   0x0A0u
#define VINPUT_R_QUEUE_DEVICE_HIGH  0x0A4u
#define VINPUT_R_CONFIG_GENERATION  0x0FCu
#define VINPUT_R_CONFIG             0x100u   /* struct virtio_input_config   */
#define VINPUT_R_CONFIG_SIZE        0x88u    /* 8 header bytes + 128 union   */

#define VINPUT_MMIO_MAGIC           0x74726976u  /* "virt" LE */
#define VINPUT_MMIO_VERSION         2u
#define VINPUT_MMIO_VENDOR          0x627A6473u  /* "bzds"    */
#define VINPUT_DEVICE_ID            18u          /* virtio-input */

#define VINPUT_S_ACKNOWLEDGE        0x01u
#define VINPUT_S_DRIVER             0x02u
#define VINPUT_S_DRIVER_OK          0x04u
#define VINPUT_S_FEATURES_OK        0x08u
#define VINPUT_S_NEEDS_RESET        0x40u
#define VINPUT_S_FAILED             0x80u

#define VINPUT_INT_VRING            0x1u

/* Feature bit 32 = VIRTIO_F_VERSION_1, word 1 bit 0 — the only feature this
 * device offers, same minimal-feature-set precedent as vblk/vnet. */
#define VINPUT_FEATWORD_HI          1u
#define VINPUT_F_VERSION_1_BIT      0x1u

/* virtio-input queue convention (spec): 0 = eventq (device -> driver,
 * key/syn events), 1 = statusq (driver -> device, e.g. LED state — accepted
 * and drained, never acted on in this v1). */
#define VINPUT_QUEUE_EVENT          0u
#define VINPUT_QUEUE_STATUS         1u
#define VINPUT_NUM_QUEUES           2u
#define VINPUT_QUEUE_MAX            64u
#define VINPUT_MAX_CHAIN            4u

#define VINPUT_VRING_DESC_F_NEXT     0x1u
#define VINPUT_VRING_DESC_F_WRITE    0x2u
#define VINPUT_VRING_DESC_F_INDIRECT 0x4u

/* ------------------------------------------------------------------ *
 * struct virtio_input_config select values (VIRTIO 1.1 §5.8.5) and the
 * Linux/BSD-shared event-type/code numbers this device advertises/emits.
 * No system header pulled in — these are stable ABI constants, duplicated
 * the same way vnet_emac.h duplicates Ethernet framing constants.
 * ------------------------------------------------------------------ */
#define VIRTIO_INPUT_CFG_UNSET       0x00u
#define VIRTIO_INPUT_CFG_ID_NAME     0x01u
#define VIRTIO_INPUT_CFG_ID_SERIAL   0x02u
#define VIRTIO_INPUT_CFG_ID_DEVIDS   0x03u
#define VIRTIO_INPUT_CFG_PROP_BITS   0x10u
#define VIRTIO_INPUT_CFG_EV_BITS     0x11u
#define VIRTIO_INPUT_CFG_ABS_INFO    0x12u

#define EV_SYN    0x00u
#define EV_KEY    0x01u
#define SYN_REPORT 0u

/* Linux/evdev keycodes this device can emit (input-event-codes.h values —
 * qwerty ROW order, not alphabetical; getting this wrong is a classic typo
 * this project has been bitten by elsewhere with register bit numbers). */
#define KEY_ESC        1u
#define KEY_1 2u
#define KEY_2 3u
#define KEY_3 4u
#define KEY_4 5u
#define KEY_5 6u
#define KEY_6 7u
#define KEY_7 8u
#define KEY_8 9u
#define KEY_9 10u
#define KEY_0 11u
#define KEY_MINUS      12u
#define KEY_EQUAL      13u
#define KEY_BACKSPACE  14u
#define KEY_TAB        15u
#define KEY_Q 16u
#define KEY_W 17u
#define KEY_E 18u
#define KEY_R 19u
#define KEY_T 20u
#define KEY_Y 21u
#define KEY_U 22u
#define KEY_I 23u
#define KEY_O 24u
#define KEY_P 25u
#define KEY_LEFTBRACE  26u
#define KEY_RIGHTBRACE 27u
#define KEY_ENTER      28u
#define KEY_LEFTCTRL   29u
#define KEY_A 30u
#define KEY_S 31u
#define KEY_D 32u
#define KEY_F 33u
#define KEY_G 34u
#define KEY_H 35u
#define KEY_J 36u
#define KEY_K 37u
#define KEY_L 38u
#define KEY_SEMICOLON  39u
#define KEY_APOSTROPHE 40u
#define KEY_GRAVE      41u
#define KEY_LEFTSHIFT  42u
#define KEY_BACKSLASH  43u
#define KEY_Z 44u
#define KEY_X 45u
#define KEY_C 46u
#define KEY_V 47u
#define KEY_B 48u
#define KEY_N 49u
#define KEY_M 50u
#define KEY_COMMA      51u
#define KEY_DOT        52u
#define KEY_SLASH      53u
#define KEY_RIGHTSHIFT 54u
#define KEY_LEFTALT    56u
#define KEY_SPACE      57u
#define KEY_CAPSLOCK   58u
#define KEY_UP         103u
#define KEY_LEFT       105u
#define KEY_RIGHT      106u
#define KEY_DOWN       108u

/* Sized to cover every code above (highest is KEY_DOWN=108) with room to
 * spare — 16 bytes == 128 bits advertised in the EV_KEY EV_BITS reply. */
#define VINPUT_KEYBIT_BYTES   16u

/* ------------------------------------------------------------------ *
 * Split-virtqueue in-guest layout (VIRTIO 1.x, little-endian) — identical
 * shape to vblk_emmc.h/vnet_emac.h's, duplicated for self-containment.
 * ------------------------------------------------------------------ */
struct vinput_vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vinput_desc {           /* host-endian decoded form */
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vinput_vq {
	uint64_t desc, avail, used;
	uint32_t num;
	uint16_t last_avail;
	uint8_t  ready;
};

struct vinput_dev {
	uint64_t base;
	uint32_t status;
	uint32_t dev_feat_sel, drv_feat_sel;
	uint64_t driver_features;
	uint32_t queue_sel;
	struct vinput_vq vq[VINPUT_NUM_QUEUES];
	uint32_t int_status;
	uint32_t config_gen;
	/* struct virtio_input_config shadow state (see vinput_config_recompute()
	 * in vinput.c). cfg_data holds the union payload only (offset 8+ of the
	 * real struct); cfg_size is the "size" header byte. */
	uint8_t  cfg_select;
	uint8_t  cfg_subsel;
	uint8_t  cfg_size;
	uint8_t  cfg_data[128];
};

/* purpose:     wire up the device: zero state, publish the "VIN1" breadcrumb.
 * input:       none
 * output:      none
 * sideEffects: must run before stage2_init()/stage2_enable(), same ordering
 *              requirement as vblk_init()/vnet_init() (main_dbg.c) */
void vinput_init(void);

/* purpose:     el2_trap dispatch entry, same contract as vblk_mmio_fault()/
 *              vnet_mmio_fault() — returns 0 for any abort outside this
 *              device's 0x200-byte window, so calling it unconditionally
 *              after those two is safe.
 * input:       frame — the trapping guest's exception frame
 * output:      1 if handled (frame->elr already advanced), 0 otherwise
 * sideEffects: may emulate a register read/write, ack an IRQ, or drain the
 *              statusq */
int vinput_mmio_fault(struct el2_frame *frame);

/* purpose:     queue one key transition (press or release) plus its trailing
 *              SYN_REPORT, exactly as a real evdev source would.
 * input:       code — a KEY_* value above; down — 1=press, 0=release
 * output:      none
 * sideEffects: pops up to two descriptors off the eventq and injects
 *              VINPUT_INTID once; silently drops (breadcrumb-counted) if the
 *              guest hasn't posted enough empty buffers or isn't
 *              DRIVER_OK yet — callable from CPU1 (dbgmon), mirroring
 *              vnet_emac_rx_frame()'s CPU1 callsite */
void vinput_send_key(uint16_t code, int down);

/* purpose:     type one printable ASCII character as a full press+release,
 *              synthesizing a shift press/release around it when needed.
 * input:       c — an ASCII byte; unmapped characters are silently ignored
 * output:      none
 * sideEffects: see vinput_send_key() */
void vinput_send_ascii(char c);

#endif /* BZDOS_VINPUT_H */
