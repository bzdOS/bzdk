/* SPDX-License-Identifier: BSD-2-Clause */

/* vinput.c — virtio-input keyboard, see vinput.h for the design/placement
 * rationale. Mirrors vnet_emac.c's structure throughout (gmem helpers,
 * virtqueue engine, IRQ injection, MMIO register emulation, fault dispatch)
 * per this tree's established "duplicate, don't share" convention for
 * self-contained virtio-mmio device files.
 *
 * FLOW:
 *   Guest -> device (setup only): the driver negotiates features, sets up
 *     the eventq (0) and statusq (1) exactly like any virtio-mmio device,
 *     then posts a pool of empty, device-writable 8-byte buffers on the
 *     eventq — this is what makes events possible at all; until the guest
 *     does this, vinput_send_key() has nothing to write into and drops.
 *
 *   Device -> guest (the actual point of this file): an operator on the
 *     debug channel runs `type <text>` or `key <code> <0|1>` (dbgmon.c).
 *     vinput_send_key() pops one avail descriptor, writes a virtio_input_event
 *     {type=EV_KEY, code, value=down}, pushes the used ring, then pops a
 *     second descriptor for the mandatory trailing {EV_SYN, SYN_REPORT, 0}
 *     — real evdev sources always terminate a batch this way, and FreeBSD's
 *     vtinput(4)/evdev won't surface events to userland without it — then
 *     injects ONE interrupt covering both.
 *
 *   statusq (guest -> device, e.g. LED state): drained and completed with
 *     zero length on QueueNotify(1). Nothing to act on for a keyboard with
 *     no LEDs; not draining it would stall the driver's output path forever.
 *
 * CROSS-CORE: CPU0 runs the guest's primary vCPU and owns ALL register
 * emulation (vinput_mmio_fault -> vinput_reg_read/write); since vcpu1/2/3
 * the OTHER three cores take guest traps too and reach the same emulation
 * (the tables and ring state are shared, not banked). vinput_send_key() is
 * called from CPU1 (dbgmon.c, on an operator command) — the exact same
 * cross-core shape as
 * vnet_emac_rx_frame() being called from CPU1's emac_poll() loop, so the
 * same reasoning applies: steady-state per-queue ring state (desc/avail/
 * used/last_avail) is safe because only CPU1 ever walks the eventq (queue 0)
 * — CPU0 never does, since nothing about eventq is guest-writable except
 * QueueNotify, which this file treats as a no-op (see vinput_kick()). The
 * one-time negotiation window (CPU0 writing QueueDesc/QueueDriver/
 * QueueDevice/QueueReady for queue 0 while CPU1 could in principle already
 * be calling vinput_send_key()) has the same single dsb-before-ready
 * protection vnet_emac.c's QUEUE_READY write uses, and the same caveat: an
 * operator should not run `type`/`key` before the guest has actually
 * attached (checked via `vinput` status, mirroring `vcpu2`'s own pattern).
 *
 * Freestanding: <stdint.h> only.
 */
#include <stdint.h>
#include "vinput.h"
#include "hv_addrmap.h"
#include "stage2.h"   /* STAGE2_DRAM_BASE/SIZE -- the one source for the guest DRAM window */

/* ------------------------------------------------------------------ *
 * ESR_EL2.ISS decode for a data abort (EC==0x24) — identical convention to
 * vconsole.c/vblk_emmc.c/vnet_emac.c; duplicated here for self-containment.
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
 * Breadcrumb window (DRAM, survives a warm WDT reset, readable via `md`/bc).
 * Placed right after HVMAP_VCPU2_BC, before emac.c's DMA scratch at
 * 0x50100000 — see hv_addrmap.h.
 *   [0] magic "VIN1"       [1] status reg           [2] eventq ready
 *   [3] statusq ready      [4] key events sent       [5] key events dropped
 *      (no eventq buffer posted, or device not DRIVER_OK)
 *   [6] irq injections     [7] mmio fault count      [8] last code sent
 *   [9] gmem out-of-range rejects
 * ------------------------------------------------------------------ */
#define VINPUT_BC_BASE   HVMAP_VINPUT_BC
#define VINPUT_BC_MAGIC  0x56494E31u   /* "VIN1" */

static inline void vinput_bc(uint32_t idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VINPUT_BC_BASE + idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static struct vinput_dev g_input;
static uint32_t g_sent, g_dropped, g_irqs, g_faults, g_gmem_oob;

/* ------------------------------------------------------------------ *
 * Guest-memory helpers — byte-for-byte the same pattern as vblk_emmc.c/
 * vnet_emac.c's gmem_cmo/gmem_read/gmem_write. Duplicated per this file's
 * self-containment convention.
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
		vinput_bc(9, ++g_gmem_oob);
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
		vinput_bc(9, ++g_gmem_oob);
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
 * Virtqueue engine (split ring, VIRTIO 1.x) — identical shape to
 * vnet_emac.c's, duplicated for self-containment.
 * ------------------------------------------------------------------ */
static void vq_read_desc(struct vinput_vq *vq, uint16_t idx, struct vinput_desc *out)
{
	struct vinput_vring_desc d;
	uint64_t p = vq->desc + (uint64_t)idx * sizeof(struct vinput_vring_desc);
	gmem_read(p, &d, sizeof(d));
	out->addr  = d.addr;
	out->len   = d.len;
	out->flags = d.flags;
	out->next  = d.next;
}

static int vq_pop_avail(struct vinput_vq *vq, uint16_t *head)
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

static void vq_push_used(struct vinput_vq *vq, uint16_t head, uint32_t used_len)
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

/* Drain (and discard) every currently-available descriptor on a queue,
 * completing each with zero length — used for statusq, which this v1 never
 * interprets but must not let stall the driver. */
static void vq_drain_discard(struct vinput_vq *vq)
{
	uint16_t head;
	while (vq_pop_avail(vq, &head))
		vq_push_used(vq, head, 0);
}

/* ------------------------------------------------------------------ *
 * IRQ injection (IMO=0) — identical mechanism to vblk/vnet's.
 * ------------------------------------------------------------------ */
static void vinput_inject_irq(void)
{
	uint32_t intid = VINPUT_INTID;
	uint32_t word  = intid / 32u;
	uint32_t bit   = intid % 32u;
	volatile uint32_t *ispendr =
	    (volatile uint32_t *)(VINPUT_GICD_BASE + VINPUT_GICD_ISPENDR + word * 4u);

	__asm__ volatile("dsb sy" ::: "memory");
	*ispendr = (1u << bit);
	__asm__ volatile("dsb sy" ::: "memory");

	g_input.int_status |= VINPUT_INT_VRING;
	vinput_bc(6, ++g_irqs);
}

/* ------------------------------------------------------------------ *
 * struct virtio_input_config emulation. Recomputed whenever the driver
 * writes `select` or `subsel` (offsets 0/1 of VINPUT_R_CONFIG) — see
 * vinput.h's doc comment for why the code (not a one-off wire probe) is the
 * best record of the actual virtio-input select/subsel protocol this
 * mirrors (Linux's virtinput_cfg_bits()/virtinput_init(), which FreeBSD's
 * vtinput(4) ports closely).
 * ------------------------------------------------------------------ */
static void cfg_clear(void)
{
	g_input.cfg_size = 0;
	for (uint32_t i = 0; i < sizeof(g_input.cfg_data); i++)
		g_input.cfg_data[i] = 0;
}

static void cfg_set_str(const char *s)
{
	uint32_t n = 0;
	while (s[n] && n < sizeof(g_input.cfg_data)) {
		g_input.cfg_data[n] = (uint8_t)s[n];
		n++;
	}
	g_input.cfg_size = (uint8_t)n;
}

static void cfg_set_bit(uint32_t code)
{
	if (code < VINPUT_KEYBIT_BYTES * 8u)
		g_input.cfg_data[code / 8u] |= (uint8_t)(1u << (code % 8u));
}

static void vinput_config_recompute(void)
{
	cfg_clear();

	switch (g_input.cfg_select) {
	case VIRTIO_INPUT_CFG_ID_NAME:
		cfg_set_str("bzdk-vkbd");
		break;
	case VIRTIO_INPUT_CFG_ID_SERIAL:
		cfg_set_str("0001");
		break;
	case VIRTIO_INPUT_CFG_ID_DEVIDS:
		/* struct virtio_input_devids: u16 bustype, vendor, product, version.
		 * BUS_VIRTUAL == 0x06 in Linux's input.h numbering. */
		g_input.cfg_data[0] = 0x06; g_input.cfg_data[1] = 0x00; /* bustype */
		g_input.cfg_data[2] = 0xF4; g_input.cfg_data[3] = 0x1A; /* vendor 0x1AF4 */
		g_input.cfg_data[4] = 0x01; g_input.cfg_data[5] = 0x00; /* product */
		g_input.cfg_data[6] = 0x01; g_input.cfg_data[7] = 0x00; /* version */
		g_input.cfg_size = 8;
		break;
	case VIRTIO_INPUT_CFG_PROP_BITS:
		/* No INPUT_PROP_* bits — not a pointer/touch device (v1 scope). */
		g_input.cfg_size = 0;
		break;
	case VIRTIO_INPUT_CFG_EV_BITS:
		if (g_input.cfg_subsel == EV_SYN) {
			cfg_set_bit(SYN_REPORT);
			g_input.cfg_size = 1;    /* nonzero size == "EV_SYN supported" */
		} else if (g_input.cfg_subsel == EV_KEY) {
			/* Every KEY_* this device can ever emit — see vinput.h. */
			cfg_set_bit(KEY_ESC);
			cfg_set_bit(KEY_1); cfg_set_bit(KEY_2); cfg_set_bit(KEY_3);
			cfg_set_bit(KEY_4); cfg_set_bit(KEY_5); cfg_set_bit(KEY_6);
			cfg_set_bit(KEY_7); cfg_set_bit(KEY_8); cfg_set_bit(KEY_9);
			cfg_set_bit(KEY_0);
			cfg_set_bit(KEY_MINUS); cfg_set_bit(KEY_EQUAL);
			cfg_set_bit(KEY_BACKSPACE); cfg_set_bit(KEY_TAB);
			cfg_set_bit(KEY_Q); cfg_set_bit(KEY_W); cfg_set_bit(KEY_E);
			cfg_set_bit(KEY_R); cfg_set_bit(KEY_T); cfg_set_bit(KEY_Y);
			cfg_set_bit(KEY_U); cfg_set_bit(KEY_I); cfg_set_bit(KEY_O);
			cfg_set_bit(KEY_P);
			cfg_set_bit(KEY_LEFTBRACE); cfg_set_bit(KEY_RIGHTBRACE);
			cfg_set_bit(KEY_ENTER); cfg_set_bit(KEY_LEFTCTRL);
			cfg_set_bit(KEY_A); cfg_set_bit(KEY_S); cfg_set_bit(KEY_D);
			cfg_set_bit(KEY_F); cfg_set_bit(KEY_G); cfg_set_bit(KEY_H);
			cfg_set_bit(KEY_J); cfg_set_bit(KEY_K); cfg_set_bit(KEY_L);
			cfg_set_bit(KEY_SEMICOLON); cfg_set_bit(KEY_APOSTROPHE);
			cfg_set_bit(KEY_GRAVE); cfg_set_bit(KEY_LEFTSHIFT);
			cfg_set_bit(KEY_BACKSLASH);
			cfg_set_bit(KEY_Z); cfg_set_bit(KEY_X); cfg_set_bit(KEY_C);
			cfg_set_bit(KEY_V); cfg_set_bit(KEY_B); cfg_set_bit(KEY_N);
			cfg_set_bit(KEY_M);
			cfg_set_bit(KEY_COMMA); cfg_set_bit(KEY_DOT); cfg_set_bit(KEY_SLASH);
			cfg_set_bit(KEY_RIGHTSHIFT); cfg_set_bit(KEY_LEFTALT);
			cfg_set_bit(KEY_SPACE); cfg_set_bit(KEY_CAPSLOCK);
			cfg_set_bit(KEY_UP); cfg_set_bit(KEY_LEFT);
			cfg_set_bit(KEY_RIGHT); cfg_set_bit(KEY_DOWN);
			g_input.cfg_size = VINPUT_KEYBIT_BYTES;
		} else {
			g_input.cfg_size = 0;   /* EV_REL/EV_ABS/etc: unsupported, v1 */
		}
		break;
	case VIRTIO_INPUT_CFG_ABS_INFO:
	case VIRTIO_INPUT_CFG_UNSET:
	default:
		g_input.cfg_size = 0;
		break;
	}
}

static uint32_t vinput_config_read(uint32_t byte_off, uint32_t sas)
{
	uint32_t nbytes = (sas == 0u) ? 1u : (sas == 1u) ? 2u : 4u;
	uint32_t v = 0;

	for (uint32_t i = 0; i < nbytes; i++) {
		uint32_t idx = byte_off + i;
		uint8_t b;
		if (idx == 0u)      b = g_input.cfg_select;
		else if (idx == 1u) b = g_input.cfg_subsel;
		else if (idx == 2u) b = g_input.cfg_size;
		else if (idx >= 8u && idx < 8u + sizeof(g_input.cfg_data))
			b = g_input.cfg_data[idx - 8u];
		else
			b = 0u;
		v |= (uint32_t)b << (8u * i);
	}
	return v;
}

static void vinput_config_write(uint32_t byte_off, uint32_t val)
{
	/* select/subsel are the only writable config bytes; FreeBSD's vtinput(4)
	 * (like Linux's virtio_input.c) writes them one byte at a time. */
	if (byte_off == 0u) {
		g_input.cfg_select = (uint8_t)val;
		vinput_config_recompute();
	} else if (byte_off == 1u) {
		g_input.cfg_subsel = (uint8_t)val;
		vinput_config_recompute();
	}
	/* everything else in config space is read-only: ignored */
}

/* ------------------------------------------------------------------ *
 * Event injection — the actual point of this device.
 * ------------------------------------------------------------------ */
struct vinput_event { uint16_t type; uint16_t code; uint32_t value; };

static int vinput_push_event(uint16_t type, uint16_t code, uint32_t value)
{
	struct vinput_vq *vq = &g_input.vq[VINPUT_QUEUE_EVENT];
	struct vinput_desc d;
	struct vinput_event ev;
	uint16_t head;

	if (!(g_input.status & VINPUT_S_DRIVER_OK))
		return 0;
	if (!vq_pop_avail(vq, &head))
		return 0;

	vq_read_desc(vq, head, &d);
	if (!(d.flags & VINPUT_VRING_DESC_F_WRITE) || d.len < sizeof(ev)) {
		vq_push_used(vq, head, 0);      /* malformed/undersized: complete, drop */
		return 0;
	}

	ev.type = type; ev.code = code; ev.value = value;
	gmem_write(d.addr, &ev, sizeof(ev));
	vq_push_used(vq, head, sizeof(ev));
	return 1;
}

void vinput_send_key(uint16_t code, int down)
{
	int ok1 = vinput_push_event(EV_KEY, code, down ? 1u : 0u);
	int ok2 = ok1 && vinput_push_event(EV_SYN, SYN_REPORT, 0u);

	if (ok1 && ok2) {
		vinput_bc(8, code);
		vinput_bc(4, ++g_sent);
		vinput_inject_irq();
	} else {
		vinput_bc(5, ++g_dropped);
	}
}

/* ASCII -> (keycode, needs-shift) — covers lowercase/uppercase letters,
 * digits, space, tab, backspace/DEL, enter/newline, and the punctuation
 * needed to type an ordinary shell command line. Unmapped bytes (control
 * chars other than \b/\t/\n/\r, anything above 0x7e) are silently ignored:
 * this is an operator convenience tool, not a general terminal emulator. */
static int ascii_to_key(char c, uint16_t *code, int *shift)
{
	*shift = 0;

	if (c >= 'a' && c <= 'z') {
		static const uint16_t row[] = {
			KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
			KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P,
			KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X,
			KEY_Y, KEY_Z,
		};
		*code = row[c - 'a'];
		return 1;
	}
	if (c >= 'A' && c <= 'Z') {
		int ok = ascii_to_key((char)(c - 'A' + 'a'), code, shift);
		*shift = 1;
		return ok;
	}
	switch (c) {
	case '1': *code = KEY_1; return 1;
	case '2': *code = KEY_2; return 1;
	case '3': *code = KEY_3; return 1;
	case '4': *code = KEY_4; return 1;
	case '5': *code = KEY_5; return 1;
	case '6': *code = KEY_6; return 1;
	case '7': *code = KEY_7; return 1;
	case '8': *code = KEY_8; return 1;
	case '9': *code = KEY_9; return 1;
	case '0': *code = KEY_0; return 1;
	case ' ':  *code = KEY_SPACE; return 1;
	case '\t': *code = KEY_TAB; return 1;
	case '\b': *code = KEY_BACKSPACE; return 1;
	case 0x7f: *code = KEY_BACKSPACE; return 1;
	case '\n': *code = KEY_ENTER; return 1;
	case '\r': *code = KEY_ENTER; return 1;
	case '-': *code = KEY_MINUS; return 1;
	case '_': *code = KEY_MINUS; *shift = 1; return 1;
	case '=': *code = KEY_EQUAL; return 1;
	case '+': *code = KEY_EQUAL; *shift = 1; return 1;
	case '/': *code = KEY_SLASH; return 1;
	case '.': *code = KEY_DOT; return 1;
	case ',': *code = KEY_COMMA; return 1;
	case ';': *code = KEY_SEMICOLON; return 1;
	case '\'': *code = KEY_APOSTROPHE; return 1;
	case '`':  *code = KEY_GRAVE; return 1;
	case '~':  *code = KEY_GRAVE; *shift = 1; return 1;
	case '[': *code = KEY_LEFTBRACE; return 1;
	case ']': *code = KEY_RIGHTBRACE; return 1;
	case '\\': *code = KEY_BACKSLASH; return 1;
	default: return 0;
	}
}

void vinput_send_ascii(char c)
{
	uint16_t code;
	int shift;

	if (!ascii_to_key(c, &code, &shift))
		return;

	if (shift) vinput_send_key(KEY_LEFTSHIFT, 1);
	vinput_send_key(code, 1);
	vinput_send_key(code, 0);
	if (shift) vinput_send_key(KEY_LEFTSHIFT, 0);
}

/* ------------------------------------------------------------------ *
 * MMIO register emulation.
 * ------------------------------------------------------------------ */
static uint32_t vinput_reg_read(struct vinput_dev *d, uint32_t off)
{
	switch (off) {
	case VINPUT_R_MAGIC_VALUE: return VINPUT_MMIO_MAGIC;
	case VINPUT_R_VERSION:     return VINPUT_MMIO_VERSION;
	case VINPUT_R_DEVICE_ID:   return VINPUT_DEVICE_ID;
	case VINPUT_R_VENDOR_ID:   return VINPUT_MMIO_VENDOR;
	case VINPUT_R_DEVICE_FEATURES:
		if (d->dev_feat_sel == VINPUT_FEATWORD_HI)
			return VINPUT_F_VERSION_1_BIT;
		return 0u;
	case VINPUT_R_QUEUE_NUM_MAX:
		return (d->queue_sel < VINPUT_NUM_QUEUES) ? VINPUT_QUEUE_MAX : 0u;
	case VINPUT_R_QUEUE_READY:
		if (d->queue_sel < VINPUT_NUM_QUEUES)
			return d->vq[d->queue_sel].ready;
		return 0u;
	case VINPUT_R_INTERRUPT_STATUS: return d->int_status;
	case VINPUT_R_STATUS:        return d->status;
	case VINPUT_R_CONFIG_GENERATION: return d->config_gen;
	default:
		return 0u;
	}
}

static void vinput_kick(struct vinput_dev *d, uint32_t qidx)
{
	if (qidx == VINPUT_QUEUE_STATUS)
		vq_drain_discard(&d->vq[VINPUT_QUEUE_STATUS]);
	/* qidx == VINPUT_QUEUE_EVENT: nothing to do — the guest is only ever
	 * posting more empty buffers, discovered lazily by the next
	 * vinput_send_key(), never acted on here. */
}

static void vinput_reg_write(struct vinput_dev *d, uint32_t off, uint32_t val)
{
	switch (off) {
	case VINPUT_R_DEVICE_FEATURES_SEL: d->dev_feat_sel = val; break;
	case VINPUT_R_DRIVER_FEATURES_SEL: d->drv_feat_sel = val; break;
	case VINPUT_R_DRIVER_FEATURES:
		if (d->drv_feat_sel < 2u)
			d->driver_features |= ((uint64_t)val) << (32u * d->drv_feat_sel);
		break;
	case VINPUT_R_QUEUE_SEL:
		d->queue_sel = val;
		break;
	case VINPUT_R_QUEUE_NUM:
		if (d->queue_sel < VINPUT_NUM_QUEUES) {
			struct vinput_vq *vq = &d->vq[d->queue_sel];
			uint32_t v = (val > VINPUT_QUEUE_MAX) ? VINPUT_QUEUE_MAX : val;
			if (v != 0u && (v & (v - 1u)) != 0u)
				break;   /* not a power of two: refuse, leave num unchanged */
			vq->num = v;
		}
		break;
	case VINPUT_R_QUEUE_DESC_LOW:
		if (d->queue_sel < VINPUT_NUM_QUEUES)
			d->vq[d->queue_sel].desc =
			    (d->vq[d->queue_sel].desc & ~0xFFFFFFFFULL) | val;
		break;
	case VINPUT_R_QUEUE_DESC_HIGH:
		if (d->queue_sel < VINPUT_NUM_QUEUES)
			d->vq[d->queue_sel].desc =
			    (d->vq[d->queue_sel].desc & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VINPUT_R_QUEUE_DRIVER_LOW:
		if (d->queue_sel < VINPUT_NUM_QUEUES)
			d->vq[d->queue_sel].avail =
			    (d->vq[d->queue_sel].avail & ~0xFFFFFFFFULL) | val;
		break;
	case VINPUT_R_QUEUE_DRIVER_HIGH:
		if (d->queue_sel < VINPUT_NUM_QUEUES)
			d->vq[d->queue_sel].avail =
			    (d->vq[d->queue_sel].avail & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VINPUT_R_QUEUE_DEVICE_LOW:
		if (d->queue_sel < VINPUT_NUM_QUEUES)
			d->vq[d->queue_sel].used =
			    (d->vq[d->queue_sel].used & ~0xFFFFFFFFULL) | val;
		break;
	case VINPUT_R_QUEUE_DEVICE_HIGH:
		if (d->queue_sel < VINPUT_NUM_QUEUES)
			d->vq[d->queue_sel].used =
			    (d->vq[d->queue_sel].used & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VINPUT_R_QUEUE_READY:
		if (d->queue_sel < VINPUT_NUM_QUEUES) {
			struct vinput_vq *vq = &d->vq[d->queue_sel];
			if (val & 1u) {
				vq->last_avail = 0;
				__asm__ volatile("dsb sy" ::: "memory");
				vq->ready = 1u;
				__asm__ volatile("dsb sy" ::: "memory");
			} else {
				vq->ready = 0u;
			}
			vinput_bc(d->queue_sel == VINPUT_QUEUE_EVENT ? 2u : 3u,
			          vq->ready);
		}
		break;
	case VINPUT_R_QUEUE_NOTIFY:
		vinput_kick(d, val);
		break;
	case VINPUT_R_INTERRUPT_ACK:
		d->int_status &= ~val;
		break;
	case VINPUT_R_STATUS:
		d->status = val;
		vinput_bc(1, val);
		if (val == 0u) {
			for (uint32_t q = 0; q < VINPUT_NUM_QUEUES; q++) {
				struct vinput_vq *vq = &d->vq[q];
				vq->ready = 0; vq->num = 0; vq->last_avail = 0;
				vq->desc = vq->avail = vq->used = 0;
			}
			d->int_status = 0;
			vinput_bc(2, 0); vinput_bc(3, 0);
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ *
 * el2_trap dispatch entry — same contract as vblk_mmio_fault()/
 * vnet_mmio_fault(). See el2_exc.c's call site (after vnet_mmio_fault()).
 * ------------------------------------------------------------------ */
int vinput_mmio_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec  = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	if (addr < g_input.base || addr >= g_input.base + VINPUT_MMIO_SIZE)
		return 0;

	vinput_bc(7, ++g_faults);

	uint32_t isv = esr & ESR_ISV_BIT;
	if (!isv) {
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;
	uint32_t off = (uint32_t)(addr - g_input.base);

	if (wnr) {
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];
		if (off >= VINPUT_R_CONFIG && off < VINPUT_R_CONFIG + VINPUT_R_CONFIG_SIZE)
			vinput_config_write(off - VINPUT_R_CONFIG, (uint32_t)val);
		else
			vinput_reg_write(&g_input, off, (uint32_t)val);
	} else {
		uint32_t val = (off >= VINPUT_R_CONFIG && off < VINPUT_R_CONFIG + VINPUT_R_CONFIG_SIZE)
		             ? vinput_config_read(off - VINPUT_R_CONFIG, sas)
		             : vinput_reg_read(&g_input, off);
		if (srt != SRT_XZR)
			frame->x[srt] = (uint64_t)val;
	}

	frame->elr += 4;
	return 1;
}

/* ------------------------------------------------------------------ *
 * Init.
 * ------------------------------------------------------------------ */
void vinput_init(void)
{
	int i;

	for (i = 0; i < (int)sizeof(g_input); i++)
		((uint8_t *)&g_input)[i] = 0;

	g_input.base = VINPUT_MMIO_BASE;
	g_sent = g_dropped = g_irqs = g_faults = g_gmem_oob = 0;

	/* Real zeros first, on every lane -- not just [0]/[2]/[3] -- so a cold
	 * read of [1]/[4]/[5]/[6] means "never touched" rather than leftover
	 * DRAM content from a previous boot generation. Same fix vcpu2.c's own
	 * init already applies; this device just missed it originally. */
	for (i = 0; i < 10; i++)
		vinput_bc(i, 0);
	vinput_bc(0, VINPUT_BC_MAGIC);
}
