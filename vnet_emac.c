/* SPDX-License-Identifier: BSD-2-Clause */

/* vnet_emac.c — virtio-net over virtio-mmio (modern/v2), multiplexed onto the
 * REAL EMAC (emac.c) alongside the HV's own debug-protocol traffic, for the
 * FreeBSD/arm64 EL1 guest under the bzdOS EL2 hypervisor (Allwinner A64 /
 * Banana Pi M64). See vnet_emac.h for the full rationale, the naming note,
 * and the register/vring/header layout. Mirrors vblk_emmc.c's structure
 * throughout, per this task's brief ("the virtio-mmio register-emulation
 * boilerplate ... is IDENTICAL across virtio device types").
 *
 * FLOW:
 *   TX (guest -> wire):
 *     guest writes QueueNotify(1)  ->  vnet_mmio_fault() -> vnet_kick(1)
 *       -> for each available descriptor chain on the transmitq:
 *            gather the WHOLE chain (virtio_net_hdr + Ethernet frame,
 *            regardless of how the guest split it across descriptors) into
 *            a linear HV-local staging buffer;
 *            read the ethertype out of the reassembled Ethernet frame;
 *            if the ethertype is one of the HV's own 0x88B5..0x88B8, bare
 *              or behind one/two VLAN tags (vnet_tx_is_hv_ethertype()): DROP. Never
 *              calls emac_send_frame(); the guest cannot spoof the debug
 *              channel. Completion is still pushed (len=0) so the guest's
 *              ring drains normally — from the driver's perspective this is
 *              indistinguishable from a real NIC silently dropping a frame,
 *              which is normal Ethernet behavior, not an error.
 *            else: emac_send_frame(ethertype, payload, payload_len) — see
 *              the "MAC identity" note below for what actually goes out on
 *              the wire.
 *       -> set InterruptStatus.VRING, inject VNET_INTID.
 *
 *   RX (wire -> guest):
 *     emac.c's emac_poll() RX demux calls vnet_emac_rx_frame(frame, len) for
 *     any accepted frame whose ethertype is neither the console's (0x88B5)
 *     nor netcon's (0x88B6) — see the emac.c hook this file's header
 *     documents (NOT applied here — described in the report).
 *       -> vnet_emac_rx_frame() pops the next available head off the
 *          receiveq (queue 0); if none, drops (breadcrumb-counted);
 *          otherwise prepends the legacy 10-byte virtio_net_hdr (all-zero:
 *          no offloads negotiated) and scatters header+frame across the
 *          guest's descriptor chain; pushes the used-ring entry with the
 *          total byte count; injects VNET_INTID.
 *
 * MAC IDENTITY NOTE: emac_send_frame(ethertype, payload, len) — the ONLY
 * transmit primitive this task's brief permits calling — always builds its
 * own Ethernet header (dst=broadcast, src=OUR_MAC, the EMAC's own fixed
 * MAC). It has NO parameter for a caller-supplied dst/src. So the guest's
 * own dst/src MAC bytes (whatever if_vtnet put in its frame) are
 * DISCARDED — we pass only the ethertype and the bytes AFTER the 14-byte
 * Ethernet header as "payload". On the wire, every guest-originated frame
 * therefore appears to come from the EMAC's own MAC, broadcast. This is a
 * real, deliberate limitation of reusing emac_send_frame() unmodified (the
 * brief explicitly calls for reusing its EXACT existing signature) — see
 * the report for the consequences and why it is not necessarily fatal for
 * the C1 DoD (ssh/pkg over a single-segment link where the peer floods
 * broadcast anyway).
 *
 * CORRECTION (2026-07-23/24, found live via tcpdump chasing why ping never
 * got an ARP reply back): the paragraph above assumed a peer's unicast
 * reply would be addressed to whatever the guest's frame carried as its
 * Ethernet SOURCE — which is always OUR_MAC, true — but that is NOT how ARP
 * actually works. A peer learns the sender's hardware address from the ARP
 * PAYLOAD's own sender-HA field (a separate, independently-substitutable
 * value the guest's if_vtnet fills in with its OWN virtio MAC, not
 * emac_send_frame()'s OUR_MAC), and addresses its REPLY to THAT learned
 * address directly. Before this fix, that was the guest's random
 * virtio-net MAC — a destination the EMAC hardware's default filter never
 * recognized as "us", so unicast replies were silently dropped before
 * emac_poll()'s software filter (widened separately, see its own comment)
 * ever saw them. Fixed by offering VIRTIO_NET_F_MAC and assigning the guest
 * VNET_GUEST_MAC == OUR_MAC (see vnet_emac.h) instead of a random one: the
 * guest's ARP sender-HA now IS OUR_MAC, so peers address replies to a MAC
 * the hardware filter already passes — no promiscuous-mode dependency.
 *
 * SUPERSEDED (2026-10-12, audit item 7): TX now keeps the guest's own dst
 * and src (emac_send_frame_from()), the EMAC filter is off (RXALL +
 * DIS_ADDR_FILTER, emac.c), and the guest no longer shares the HV's MAC: it
 * gets emac_guest_mac() (SID-derived, octet 0 | 0x10) while the HV keeps
 * emac_hv_mac() for itself. The guest may not transmit with the HV's MAC.
 *
 * ============================================================================
 * CROSS-CORE CONCURRENCY — READ THIS BEFORE THIS EVER TOUCHES HARDWARE.
 * ============================================================================
 * Unlike vblk_emmc.c (whose ENTIRE lifecycle — register writes, QueueNotify,
 * IRQ injection — runs on CPU0 inside the guest's own trap, because CPU0 is
 * the only core that ever runs the guest / takes guest traps), THIS device is
 * inherently split across two cores by the tree's existing architecture
 * (see smp.c's "CPU1 = dedicated EMAC/dbgmon debug core" block and
 * main_dbg.c's console_poll()==emac_poll()):
 *
 *   - CPU0 runs the guest and takes its traps: ALL virtio-mmio register
 *     emulation (vnet_mmio_fault -> vnet_reg_read/write) and the TX kick
 *     path (vnet_kick -> emac_send_frame()) happen HERE.
 *   - CPU1 EXCLUSIVELY owns emac_poll() (RX drain + link maintenance) today,
 *     polled from its own tight loop, specifically so the debug console
 *     keeps working even when CPU0/the guest is wedged. vnet_emac_rx_frame()
 *     is called FROM INSIDE that CPU1 loop.
 *
 * This means, for the very first time in this tree, EMAC's internal state —
 * g_tx_slot/tx_desc()/g_link_up and the whole TX descriptor ring in emac.c —
 * would be touched from TWO cores concurrently: CPU1 via the console's own
 * tx_frame()/emac_flush() (and RX ring maintenance) AND CPU0 via vnet_kick's
 * emac_send_frame() call.
 *
 * RESOLVED (this was the original "#1 RISK" banner — kept for context): emac.c
 * now takes a cross-core test-and-set spinlock (EMAC_TX_LOCK_PA, mirroring
 * vblk_emmc_trylock()/unlock()) around the WHOLE of tx_frame_raw(), which is
 * the single choke point all TX paths funnel through, so CPU0's vnet TX and
 * CPU1's console TX can no longer hand out the same g_tx_slot. See emac.c's
 * header (tx_frame_raw / EMAC_TX_LOCK_PA) and bc[32] tx_lock_contended.
 * (Review D7, closed 2026-10-12: the link watchdog's never-had-traffic
 * rings_init() now runs under the TX lock too.)
 *
 * QUEUE STATE vs DEVICE RESET (audit item 6, fixed 2026-10-12): the guest
 * may write Status=0 (reset) or reprogram/disable a queue from ANY vCPU while
 * CPU1 sits inside vnet_emac_rx_frame() scattering a frame into the RX ring
 * it just popped. The old code cleared vq->ready/desc/avail/used underneath
 * it, so RX could write a frame and a used entry into buffers the guest had
 * already freed. Each queue now has its own lock (vq_lock[], ldaxr/stlxr like
 * the EMAC TX lock): RX holds queue 0's for the whole pop->scatter->push, the
 * TX kick holds queue 1's, and every register write that changes a queue
 * (QueueNum/Desc/Driver/Device/Ready) or resets the device (Status=0) takes
 * the lock(s) first -- i.e. a reset WAITS for an in-flight RX/TX to finish,
 * and RX re-checks DRIVER_OK/ready under the lock, so it never touches a
 * queue that is being reset. RX never spins on its lock: if a reset holds it,
 * the frame is dropped and counted ([23]), which is what a NIC mid-reset does.
 * Both critical sections are bounded loops (no waits on external events), so
 * the reset side may spin until it gets the lock.
 *
 * vnet_dev.int_status is set from CPU1 (RX, link change) and from the
 * kicking vCPU (TX), and cleared from whichever vCPU acks: with two cause
 * bits (VRING, CONFIG) a lost read-modify-write is no longer benign, so both
 * directions are atomic (int_status_or/int_status_clear).
 *
 * Freestanding: <stdint.h> only.
 */
#include <stdint.h>
#include "vnet_emac.h"
#include "guest_mem.h"   /* guest_gpa_ok(): guest RAM minus every HV-owned window */
#include "emac.h"           /* emac_send_frame() */
#include "flightrec.h"      /* B4: flightrec_log(FLTR_K_VIRTIO, ...) on QueueNotify */

/* ------------------------------------------------------------------ *
 * ESR_EL2.ISS decode for a data abort (EC==0x24) — identical convention to
 * vconsole.c/vblk_emmc.c; duplicated here to keep this file self-contained.
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
 * 0x50030000 — clear of every other lane's window (EMAC 0x50000100 +
 * scratch DMA 0x50100000..0x50109000, vblk BC 0x50020000 + lock word
 * 0x50020100, BMC 0x50006000, etc. — see the address-map survey in the
 * report).
 *   [0] magic "VNT1"        [1] status reg          [2] rx queue ready
 *   [3] tx queue ready      [4] rx frames queued     [5] rx frames dropped
 *      (no RX buffer posted, or device not DRIVER_OK)
 *   [6] tx frames sent      [7] tx frames blocked (ethertype 0x88B5)
 *   [8] tx frames dropped by emac_send_frame() (link down / TX ring busy)
 *   [9] irq injections      [10] mmio fault count
 *   [11] last tx ethertype  [12] last rx ethertype
 *   [13] tx chain-truncation events (VNET_MAX_CHAIN exceeded)
 *   [14] rx scatter-truncation events (guest RX buffer too small)
 *
 * D2/D5 diagnostics (code-review fixes; see each spot's own comment):
 *   [15] gmem_read/gmem_write rejects of an out-of-DRAM-range guest PA (D2 —
 *        should stay 0 against a real, well-behaved guest)
 *   [16] QueueNum writes refused for not being a power of two (D5(b))
 *   [17] descriptor W/R flag mismatches observed, NOT enforced (D5(d))
 *   [18] TX QueueNotify seen before DRIVER_OK, NOT enforced (D5(e); RX side
 *        already hard-gates on DRIVER_OK in vnet_emac_rx_frame(), unchanged)
 *   [19] descriptor chains dropped because a descriptor's buffer is not
 *        wholly guest RAM, or its index is outside the ring
 *   [20] QueueReady refused: desc/avail/used ring not wholly guest RAM
 *   [21] of [7], frames whose HV ethertype sat inside a VLAN tag
 *   [22] guest TX frames dropped because the link is down (the guest also
 *        sees VIRTIO_NET_S_LINK_UP clear and should stop sending)
 *   [23] RX frames dropped because a reset/queue reprogram held the RX lock
 *   [24] link changes published to the guest (config-change interrupts)
 *   [25] link state currently published in config.status (1 = up)
 *   [26] config generation
 *
 * [7] counts every guest frame dropped on TX because its ethertype, bare or
 * behind one or two 802.1Q/802.1ad tags, is one of the HV's own 0x88B5..0x88B8
 * (debug console, netcon, dbgtools, snapnet), plus tagged frames too short to
 * show their inner ethertype.
 * ------------------------------------------------------------------ */
#define VNET_BC_BASE   0x50030000UL
#define VNET_BC_MAGIC  0x564E5431u   /* "VNT1" */
#define VNET_BC_LAST   26u           /* highest slot used above */

static inline void vnet_bc(uint32_t idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VNET_BC_BASE + idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Single device instance + counters.
 * ------------------------------------------------------------------ */
static struct vnet_dev g_net;
static uint32_t g_rx_queued, g_rx_dropped, g_tx_sent, g_tx_blocked,
                g_tx_dropped, g_irqs, g_faults, g_tx_truncated, g_rx_truncated,
                g_gmem_oob;
/* D5 diagnostics (see vblk_emmc.c's identical fields/rationale). */
static uint32_t g_bad_queue_num;      /* (b) non-power-of-two QueueNum, refused */
static uint32_t g_bad_desc_flags;     /* (d) descriptor W/R flag mismatch, NOT enforced */
static uint32_t g_kick_not_ready;     /* (e) TX kick before DRIVER_OK, NOT enforced */
static uint32_t g_bad_chain;          /* chain with a non-guest-RAM buffer / bad index */
static uint32_t g_bad_ring;           /* QueueReady with a non-guest-RAM ring, refused */
static uint32_t g_tx_blocked_vlan;    /* HV ethertype hidden behind a VLAN tag */
static uint32_t g_tx_linkdown;        /* [22] guest TX dropped, link down */
static uint32_t g_rx_reset_busy;      /* [23] RX dropped, queue being reset */
static uint32_t g_link_changes;       /* [24] link changes published */
static volatile uint32_t g_force_down_polls;  /* nettest vlink: polls left */

/* Per-queue locks (see the QUEUE STATE vs DEVICE RESET note at the top).
 * Plain BSS words: HV memory is Normal cacheable and the cores run with
 * SMPEN, so ldaxr/stlxr are coherent across them (same as smp.c g_online).
 * One cache line each so the RX and TX sides never false-share. */
static volatile uint32_t g_vq_lock[VNET_NUM_QUEUES][16] __attribute__((aligned(64)));

static int vq_trylock(uint32_t q)
{
	volatile uint32_t *p = &g_vq_lock[q][0];
	uint32_t prev, status, one = 1u;
	__asm__ volatile(
		"	ldaxr	%w0, [%3]\n"
		"	cbnz	%w0, 1f\n"
		"	stlxr	%w1, %w2, [%3]\n"
		"	b	2f\n"
		"1:	clrex\n"
		"	mov	%w1, #1\n"
		"2:\n"
		: "=&r"(prev), "=&r"(status)
		: "r"(one), "r"(p)
		: "memory");
	return prev == 0u && status == 0u;
}

/* Waits for the holder: every critical section under these locks is a
 * bounded walk of a guest ring (plus, for TX, emac.c's bounded send), so
 * this cannot wait forever. */
static void vq_lock(uint32_t q)
{
	while (!vq_trylock(q))
		__asm__ volatile("yield" ::: "memory");
}

static void vq_unlock(uint32_t q)
{
	__asm__ volatile("dmb ish" ::: "memory");
	g_vq_lock[q][0] = 0u;
	__asm__ volatile("dsb ish\n\tsev" ::: "memory");
}

static void int_status_or(volatile uint32_t *p, uint32_t bits)
{
	uint32_t v, st;
	__asm__ volatile(
		"1:	ldaxr	%w0, [%2]\n"
		"	orr	%w0, %w0, %w3\n"
		"	stlxr	%w1, %w0, [%2]\n"
		"	cbnz	%w1, 1b\n"
		: "=&r"(v), "=&r"(st)
		: "r"(p), "r"(bits)
		: "memory");
}

static void int_status_clear(volatile uint32_t *p, uint32_t bits)
{
	uint32_t v, st;
	__asm__ volatile(
		"1:	ldaxr	%w0, [%2]\n"
		"	bic	%w0, %w0, %w3\n"
		"	stlxr	%w1, %w0, [%2]\n"
		"	cbnz	%w1, 1b\n"
		: "=&r"(v), "=&r"(st)
		: "r"(p), "r"(bits)
		: "memory");
}

/* HV-local staging buffer for one frame at a time. Only ever touched from
 * ONE core per call (vnet_kick on CPU0, vnet_emac_rx_frame on CPU1), but
 * NOT reentrancy-safe if the SAME core ever recursed into this file (it
 * cannot: neither call site is reentrant with itself). Two SEPARATE buffers
 * (one per direction) avoid even the theoretical cross-core aliasing a
 * single shared buffer would risk if that assumption ever changed. */
static uint8_t g_tx_stage[VNET_STAGE_BUF_SIZE];
static uint8_t g_rx_stage[VNET_STAGE_BUF_SIZE];

/* ------------------------------------------------------------------ *
 * Guest-memory helpers — BYTE-FOR-BYTE the same pattern as vblk_emmc.c's
 * gmem_cmo/gmem_read/gmem_write (see that file's block comment for the full
 * "why civac on every access" rationale: EL2 runs on U-Boot's inherited
 * stage-1 tables of unknown attributes, so every guest-memory access is
 * bracketed with cache maintenance to behave like a maintenance-correct
 * non-coherent DMA master regardless of what U-Boot left us). Duplicated
 * (not shared) per this file's self-containment rule — do NOT weaken this;
 * this is the exact coherency lesson the task brief calls out as
 * hardware-found and mandatory to reuse.
 * ------------------------------------------------------------------ */
/* Every guest PA this file dereferences is checked with guest_gpa_ok()
 * (guest_mem.h): wholly inside guest RAM and clear of every HV-owned window
 * (hv-image, hv-scratch with the EMAC DMA rings and lock words, the HV
 * framebuffer, the vzram pool / U-Boot tail). The same table builds the
 * guest's stage-2 map, so a buffer the guest could not write itself cannot be
 * written through this device either. Rings are checked once at QueueReady and
 * per pop, descriptor buffers once per chain; gmem_read/gmem_write keep a
 * per-access check as the backstop and zero the destination on a refusal. */
static void gmem_cmo(uint64_t gpa, uint32_t len)
{
	uint64_t p   = gpa & ~63ULL;
	uint64_t end = gpa + len;
	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int gmem_read(uint64_t gpa, void *dst, uint32_t len)
{
	if (!guest_gpa_ok(gpa, len)) {
		vnet_bc(15, ++g_gmem_oob);      /* not guest RAM: refuse, never guess */
		for (uint32_t i = 0; i < len; i++)
			((uint8_t *)dst)[i] = 0;
		return 0;
	}
	const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)gpa;
	uint8_t *d = (uint8_t *)dst;
	gmem_cmo(gpa, len);                  /* refetch from PoC, never stale */
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	return 1;
}

static void gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	if (!guest_gpa_ok(gpa, len)) {
		vnet_bc(15, ++g_gmem_oob);      /* not guest RAM: drop the write */
		return;
	}
	volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)gpa;
	const uint8_t *s = (const uint8_t *)src;
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
	gmem_cmo(gpa, len);                  /* publish to PoC for every observer */
}

static inline uint16_t gmem_ld16(uint64_t gpa)
{
	uint16_t v; gmem_read(gpa, &v, 2); return v;    /* guest is LE == our LE */
}
static inline void gmem_st16(uint64_t gpa, uint16_t v)
{
	gmem_write(gpa, &v, 2);
}

/* ------------------------------------------------------------------ *
 * Virtqueue engine (split ring, VIRTIO 1.x) — generic over EITHER queue
 * (RX=0 or TX=1), unlike vblk_emmc.c which only ever has one queue.
 * ------------------------------------------------------------------ */
static int vq_rings_ok(const struct vnet_vq *vq)
{
	return guest_vring_ok(vq->desc, vq->avail, vq->used, vq->num);
}

/* 0 if the descriptor slot is outside the ring or unreadable; *out is then
 * all-zero, never stale stack. */
static int vq_read_desc(struct vnet_vq *vq, uint16_t idx, struct vnet_desc *out)
{
	struct vnet_vring_desc d;
	uint64_t p = vq->desc + (uint64_t)idx * sizeof(struct vnet_vring_desc);
	int ok = idx < vq->num && gmem_read(p, &d, sizeof(d));
	if (!ok)
		d.addr = 0, d.len = 0, d.flags = 0, d.next = 0;
	out->addr  = d.addr;
	out->len   = d.len;
	out->flags = d.flags;
	out->next  = d.next;
	return ok;
}

static int vq_pop_avail(struct vnet_vq *vq, uint16_t *head)
{
	uint16_t avail_idx;

	if (!vq->ready || vq->num == 0)
		return 0;
	/* Ring addresses are registers the guest can rewrite after QueueReady;
	 * re-check them (three compares each) rather than trust the snapshot. */
	if (!vq_rings_ok(vq))
		return 0;

	avail_idx = gmem_ld16(vq->avail + 2u);
	if (vq->last_avail == avail_idx)
		return 0;                          /* caught up */

	{
		uint16_t slot = (uint16_t)(vq->last_avail % vq->num);
		*head = gmem_ld16(vq->avail + 4u + (uint64_t)slot * 2u);
	}
	vq->last_avail++;
	return 1;
}

static uint16_t vq_push_used(struct vnet_vq *vq, uint16_t head, uint32_t used_len)
{
	uint16_t used_idx = gmem_ld16(vq->used + 2u);
	uint16_t slot = (uint16_t)(used_idx % vq->num);
	uint64_t e = vq->used + 4u + (uint64_t)slot * 8u;   /* &ring[slot] */
	uint32_t id = head;
	uint16_t new_idx = (uint16_t)(used_idx + 1u);

	gmem_write(e + 0u, &id, 4u);
	gmem_write(e + 4u, &used_len, 4u);
	__asm__ volatile("dsb sy" ::: "memory");            /* entry before idx */
	gmem_st16(vq->used + 2u, new_idx);
	__asm__ volatile("dsb sy" ::: "memory");
	return new_idx;
}

/* Collect one descriptor chain starting at `head` into `chain[]` (bounded by
 * VNET_MAX_CHAIN). Returns the chain length, or 0 on a malformed/indirect/
 * over-long chain (caller pushes a zero-length completion and moves on —
 * same defensive contract as vblk_request()'s chain collector). */
static uint32_t vq_collect_chain(struct vnet_vq *vq, uint16_t head,
                                  struct vnet_desc *chain, uint32_t max)
{
	uint32_t n = 0;
	uint16_t idx = head;

	for (;;) {
		if (n >= max) {
			vnet_bc(13, ++g_tx_truncated);
			return 0;
		}
		if (!vq_read_desc(vq, idx, &chain[n]) ||
		    !guest_gpa_ok(chain[n].addr, chain[n].len)) {
			/* Buffer (or the descriptor itself) is not wholly guest
			 * RAM: drop the whole chain, touch none of it. */
			vnet_bc(19, ++g_bad_chain);
			return 0;
		}
		if (chain[n].flags & VNET_VRING_DESC_F_INDIRECT)
			return 0;      /* not handled — advertise off, treat as malformed */
		uint16_t flags = chain[n].flags;
		uint16_t next  = chain[n].next;
		n++;
		if (!(flags & VNET_VRING_DESC_F_NEXT))
			break;
		idx = next;
	}
	return n;
}

/* Gather up to dst_cap bytes out of a (read-only, TX-direction) descriptor
 * chain into a linear HV-local buffer, in order, regardless of how the
 * guest split header vs data across descriptors (robust to either the
 * "header in its own descriptor" or "header+data combined" layout — we
 * never assumed VIRTIO_NET_F_ANY_LAYOUT either way). */
static uint32_t vnet_gather(struct vnet_desc *chain, uint32_t n,
                             uint8_t *dst, uint32_t dst_cap)
{
	uint32_t total = 0;
	for (uint32_t i = 0; i < n && total < dst_cap; i++) {
		uint32_t take = chain[i].len;
		if (total + take > dst_cap)
			take = dst_cap - total;
		if (take)
			gmem_read(chain[i].addr, dst + total, take);
		total += take;
	}
	return total;
}

/* Scatter `src_len` bytes of `src` across a (write-only, RX-direction)
 * descriptor chain, in order. Returns bytes actually written (may be less
 * than src_len if the guest's posted buffers are too small — counted as a
 * truncation, never a correctness bug: we simply stop, the guest's device
 * driver sees a short completion). */
static uint32_t vnet_scatter(struct vnet_desc *chain, uint32_t n,
                              const uint8_t *src, uint32_t src_len)
{
	uint32_t written = 0;
	uint32_t ci = 0, coff = 0;

	while (written < src_len && ci < n) {
		uint32_t avail = chain[ci].len - coff;
		if (avail == 0) { ci++; coff = 0; continue; }
		uint32_t take = src_len - written;
		if (take > avail) take = avail;
		gmem_write(chain[ci].addr + coff, src + written, take);
		written += take;
		coff += take;
		if (coff >= chain[ci].len) { ci++; coff = 0; }
	}
	if (written < src_len)
		vnet_bc(14, ++g_rx_truncated);
	return written;
}

/* ------------------------------------------------------------------ *
 * IRQ injection (IMO=0) — identical mechanism/caveats to vblk_inject_irq();
 * see vblk_emmc.c's comment block for the edge-vs-level rationale (this
 * device's DTB node MUST also declare IRQ_TYPE_EDGE_RISING, not level).
 * ------------------------------------------------------------------ */
static void vnet_inject_irq(void)
{
	uint32_t intid = VNET_INTID;
	uint32_t word  = intid / 32u;
	uint32_t bit   = intid % 32u;
	volatile uint32_t *ispendr =
	    (volatile uint32_t *)(VNET_GICD_BASE + VNET_GICD_ISPENDR + word * 4u);

	__asm__ volatile("dsb sy" ::: "memory");
	*ispendr = (1u << bit);
	__asm__ volatile("dsb sy" ::: "memory");

	vnet_bc(9, ++g_irqs);
}

/* ------------------------------------------------------------------ *
 * TX: one descriptor chain off the transmitq (queue 1).
 * ------------------------------------------------------------------ */
static void vnet_tx_one(struct vnet_dev *d, uint16_t head)
{
	struct vnet_vq *vq = &d->vq[VNET_QUEUE_TX];
	struct vnet_desc chain[VNET_MAX_CHAIN];
	uint32_t n = vq_collect_chain(vq, head, chain, VNET_MAX_CHAIN);

	if (n == 0) {
		vq_push_used(vq, head, 0);
		return;
	}

	/* D5(d), DIAGNOSTIC ONLY (same "count, don't enforce" caution as
	 * vblk_emmc.c's identical fix): every transmitq descriptor is guest-
	 * supplied, device-READ data (the device writes nothing back except the
	 * zero-length completion) — none should carry VNET_VRING_DESC_F_WRITE.
	 * Not enforced: no hardware access in this task to confirm if_vtnet never
	 * does otherwise on some code path, and a wrong hard-reject here would
	 * break the guest's already-working TX path outright. */
	for (uint32_t i = 0; i < n; i++)
		if (chain[i].flags & VNET_VRING_DESC_F_WRITE)
			vnet_bc(17, ++g_bad_desc_flags);

	uint32_t total = vnet_gather(chain, n, g_tx_stage, VNET_STAGE_BUF_SIZE);

	/* Need at least the legacy header + a minimal Ethernet header to have an
	 * ethertype to mux on. Anything shorter is malformed — drop cleanly. */
	if (total < VNET_HDR_LEN + VNET_ETH_HDR_LEN) {
		vq_push_used(vq, head, 0);
		return;
	}

	uint16_t ethertype = (uint16_t)((g_tx_stage[VNET_HDR_LEN + 12] << 8) |
	                                  g_tx_stage[VNET_HDR_LEN + 13]);
	vnet_bc(11, ethertype);

	int hv_type = vnet_tx_is_hv_ethertype(g_tx_stage + VNET_HDR_LEN,
	                                       total - VNET_HDR_LEN);
	/* Nor may it borrow the HV's source MAC (the guest has its own since
	 * 2026-10-12; host tools identify the HV by this address). */
	if (!hv_type && emac_mac_is_hv(g_tx_stage + VNET_HDR_LEN + 6))
		hv_type = 1;
	if (hv_type) {
		/* HARD BLOCK: the guest must never be able to inject a frame that
		 * masquerades as one of the HV's own protocols (0x88B5..0x88B8,
		 * bare or VLAN-wrapped), nor use the HV's source MAC: host tools
		 * must be able to tell the two apart. Never reaches
		 * emac_send_frame() / real hardware. Completion still pushed so the
		 * guest's ring drains — indistinguishable from a real NIC quietly
		 * dropping a frame. */
		vnet_bc(7, ++g_tx_blocked);
		if (hv_type > 1)
			vnet_bc(21, ++g_tx_blocked_vlan);
	} else if (!g_net.link_up) {
		/* Link down (config.status says so too, and the guest was sent a
		 * config-change interrupt): nothing can leave. Count it here rather
		 * than let it vanish into emac.c's generic drop counter. virtio-net
		 * has no per-frame TX error status, so the completion below is the
		 * most a device can say; the honest signal is the link state. */
		vnet_bc(22, ++g_tx_linkdown);
	} else {
		const uint8_t *payload = g_tx_stage + VNET_HDR_LEN + VNET_ETH_HDR_LEN;
		uint32_t plen = total - VNET_HDR_LEN - VNET_ETH_HDR_LEN;
		/* Preserve the guest's own Ethernet destination instead of
		 * broadcasting (the "MAC IDENTITY NOTE" above described the old
		 * behaviour). ARP tolerated broadcast because a peer learns the
		 * sender's hardware address from the ARP payload's sender-HA field,
		 * not the frame header -- but TCP does not: Linux drops a unicast-IP
		 * packet delivered to the broadcast MAC, so the guest's SYN-ACK
		 * showed up in tcpdump and was ignored by the host's stack, leaving
		 * the host retransmitting SYN until it gave up (measured live
		 * 2026-07-30, with `ip neigh` stuck FAILED for the guest). The dst
		 * MAC is the first 6 bytes of the guest's Ethernet header, which sits
		 * immediately after the virtio_net_hdr in the staging buffer. */
		const uint8_t *dst = g_tx_stage + VNET_HDR_LEN;
		/* Keep the guest's source MAC too: a bridged frame carries its
		 * client's address (emac.h, emac_send_frame_from()). */
		const uint8_t *src = g_tx_stage + VNET_HDR_LEN + 6;
		int ok = emac_send_frame_from(dst, src, ethertype, payload,
		                              (uint16_t)plen);
		if (ok)
			vnet_bc(6, ++g_tx_sent);
		else
			vnet_bc(8, ++g_tx_dropped);      /* link down / TX ring busy */
	}

	/* virtio-net TX completions carry no meaningful "bytes written" (the
	 * device writes nothing back on the transmitq) — 0 is correct per spec,
	 * matching every other real virtio-net device. */
	vq_push_used(vq, head, 0);
}

/* QueueNotify handler: drain every available request on the notified queue,
 * then raise one IRQ if anything was served. Queue 0 (RX) is driven by
 * vnet_emac_rx_frame() instead — a guest NOTIFY on queue 0 just means "I
 * posted more RX buffers", which requires no immediate action here (the
 * next inbound frame will find them via vq_pop_avail), so it is a no-op
 * that still counts as "served" for symmetry/diagnostics only if you choose
 * to extend it; today it simply returns. */
static void vnet_kick(struct vnet_dev *d, uint32_t qidx)
{
	if (qidx == VNET_QUEUE_TX) {
		struct vnet_vq *vq = &d->vq[VNET_QUEUE_TX];
		uint16_t head;
		int served = 0;

		/* D5(e), DIAGNOSTIC ONLY (see vblk_kick()'s identical rationale in
		 * vblk_emmc.c — not enforced, no hardware access in this task to
		 * confirm it never fires for a working driver). vnet_emac_rx_frame()
		 * already hard-gates on DRIVER_OK for the RX direction (existing,
		 * unchanged, proven-safe code, predating this fix) — this counts the
		 * TX-side equivalent instead of also hard-gating it. */
		if (!(d->status & VNET_S_DRIVER_OK))
			vnet_bc(18, ++g_kick_not_ready);

		/* Held across the whole drain: a reset/QueueReady write from
		 * another vCPU waits until this ring walk is finished. */
		vq_lock(VNET_QUEUE_TX);
		while (vq_pop_avail(vq, &head)) {
			vnet_tx_one(d, head);
			served = 1;
		}
		vq_unlock(VNET_QUEUE_TX);
		if (served) {
			int_status_or((volatile uint32_t *)&d->int_status, VNET_INT_VRING);
			vnet_inject_irq();
		}
	}
	/* qidx == VNET_QUEUE_RX: nothing to do here — see comment above. */
}

/* ------------------------------------------------------------------ *
 * RX: called from emac.c's receive path (CPU1 — see the cross-core comment
 * block at the top of this file) for any accepted inbound frame whose
 * ethertype is not already claimed by the console (0x88B5) or netcon
 * (0x88B6).
 * ------------------------------------------------------------------ */
void vnet_emac_rx_frame(const uint8_t *frame, uint16_t len)
{
	struct vnet_vq *vq = &g_net.vq[VNET_QUEUE_RX];
	uint16_t head;

	if (len < VNET_ETH_HDR_LEN || (uint32_t)len > VNET_STAGE_BUF_SIZE - VNET_HDR_LEN) {
		vnet_bc(5, ++g_rx_dropped);          /* malformed / oversize */
		return;
	}
	/* Never wait here: a reset or queue reprogram holding the lock means
	 * the ring is going away -- drop the frame, as a NIC mid-reset would. */
	if (!vq_trylock(VNET_QUEUE_RX)) {
		vnet_bc(23, ++g_rx_reset_busy);
		return;
	}
	/* DRIVER_OK and the queue registers are re-read UNDER the lock: a reset
	 * that completed just before we took it is seen here, and one that
	 * starts after waits for vq_unlock() below. */
	if (!(g_net.status & VNET_S_DRIVER_OK)) {
		vq_unlock(VNET_QUEUE_RX);
		vnet_bc(5, ++g_rx_dropped);          /* driver not attached yet */
		return;
	}
	if (!vq_pop_avail(vq, &head)) {
		vq_unlock(VNET_QUEUE_RX);
		vnet_bc(5, ++g_rx_dropped);          /* guest posted no RX buffer */
		return;
	}

	{
		uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
		vnet_bc(12, ethertype);
	}

	/* Assemble header (all-zero: no offloads negotiated) + frame into the
	 * staging buffer, then scatter across the guest's descriptor chain. */
	struct vnet_hdr hdr;
	hdr.flags = 0; hdr.gso_type = 0; hdr.hdr_len = 0;
	hdr.gso_size = 0; hdr.csum_start = 0; hdr.csum_offset = 0;
	hdr.num_buffers = 1;          /* spec: 1 without MRG_RXBUF (was stack garbage) */
	for (uint32_t i = 0; i < VNET_HDR_LEN; i++)
		g_rx_stage[i] = ((const uint8_t *)&hdr)[i];
	for (uint32_t i = 0; i < len; i++)
		g_rx_stage[VNET_HDR_LEN + i] = frame[i];

	struct vnet_desc chain[VNET_MAX_CHAIN];
	uint32_t n = vq_collect_chain(vq, head, chain, VNET_MAX_CHAIN);
	if (n == 0) {
		vq_push_used(vq, head, 0);
		vq_unlock(VNET_QUEUE_RX);
		return;
	}

	/* D5(d), DIAGNOSTIC ONLY (see vnet_tx_one()'s identical-rationale check):
	 * every receiveq descriptor should be device-WRITABLE (the device fills
	 * it with the received frame). Not enforced, same caution as above. */
	for (uint32_t i = 0; i < n; i++)
		if (!(chain[i].flags & VNET_VRING_DESC_F_WRITE))
			vnet_bc(17, ++g_bad_desc_flags);

	uint32_t written = vnet_scatter(chain, n, g_rx_stage,
	                                 VNET_HDR_LEN + (uint32_t)len);
	vq_push_used(vq, head, written);
	vq_unlock(VNET_QUEUE_RX);
	vnet_bc(4, ++g_rx_queued);

	int_status_or((volatile uint32_t *)&g_net.int_status, VNET_INT_VRING);
	vnet_inject_irq();
}

/* ------------------------------------------------------------------ *
 * Link status (VIRTIO_NET_F_STATUS). Runs on CPU1 from emac_poll(); the
 * only writer of g_net.link_up / config_gen after vnet_init().
 * ------------------------------------------------------------------ */
void vnet_emac_force_link_down(uint32_t polls)
{
	g_force_down_polls = polls;
}

void vnet_emac_link_update(int emac_link_up)
{
	uint32_t up = emac_link_up ? 1u : 0u;

	if (g_force_down_polls) {
		g_force_down_polls--;
		up = 0u;
	}
	if (up == g_net.link_up)
		return;

	/* Status first, then the generation: a driver that read the old
	 * generation before our status store sees a different one after it
	 * and re-reads (virtio 1.x 4.2.2.2 ConfigGeneration). */
	g_net.link_up = up;
	__asm__ volatile("dmb ish" ::: "memory");
	g_net.config_gen++;
	__asm__ volatile("dsb ish" ::: "memory");
	vnet_bc(24, ++g_link_changes);
	vnet_bc(25, up);
	vnet_bc(26, g_net.config_gen);

	if ((g_net.status & VNET_S_DRIVER_OK) &&
	    (g_net.driver_features & VNET_F_STATUS_BIT)) {
		int_status_or((volatile uint32_t *)&g_net.int_status, VNET_INT_CONFIG);
		vnet_inject_irq();
	}
}

/* ------------------------------------------------------------------ *
 * MMIO register emulation.
 * ------------------------------------------------------------------ */
static uint32_t vnet_reg_read(struct vnet_dev *d, uint32_t off)
{
	switch (off) {
	case VNET_R_MAGIC_VALUE:   return VNET_MMIO_MAGIC;
	case VNET_R_VERSION:       return VNET_MMIO_VERSION;
	case VNET_R_DEVICE_ID:     return VNET_DEVICE_ID;         /* 1: virtio-net */
	case VNET_R_VENDOR_ID:     return VNET_MMIO_VENDOR;
	case VNET_R_DEVICE_FEATURES:
		/* Word 1, bit 0 = VIRTIO_F_VERSION_1. Word 0, bit 5 = VIRTIO_NET_F_MAC
		 * (the guest's MAC is emac_guest_mac(), see emac.h) — no other
		 * MAC/CSUM/offload/MRG_RXBUF/STATUS bits offered, matching
		 * vblk_emmc.c's minimal-feature-set precedent. */
		if (d->dev_feat_sel == VNET_FEATWORD_HI)
			return VNET_F_VERSION_1_BIT;
		if (d->dev_feat_sel == VNET_FEATWORD_LO)
			return VNET_F_MAC_BIT | VNET_F_STATUS_BIT;
		return 0u;
	case VNET_R_QUEUE_NUM_MAX:
		/* D5(a): a nonexistent queue_sel must read back 0 (spec: QueueNumMax
		 * ==0 tells the driver the queue is not available), not the same 256
		 * every real queue (RX=0, TX=1) advertises. */
		return (d->queue_sel < VNET_NUM_QUEUES) ? VNET_QUEUE_MAX : 0u;
	case VNET_R_QUEUE_READY:
		if (d->queue_sel < VNET_NUM_QUEUES)
			return d->vq[d->queue_sel].ready;
		return 0u;
	case VNET_R_INTERRUPT_STATUS: return d->int_status;
	case VNET_R_STATUS:        return d->status;
	case VNET_R_CONFIG_GENERATION: return d->config_gen;
	default:
		/* Config space (VNET_R_CONFIG..+6, the `mac` field) is handled by
		 * vnet_config_read() directly in vnet_mmio_fault() — it needs the
		 * access width (SAS) to return a correctly narrow, zero-extended
		 * value, which this word-only function's callers don't carry. Any
		 * other offset: 0 (harmless if a driver peeks anyway). */
		return 0u;
	}
}

/* Config-space byte-granular read of the guest MAC (emac_guest_mac()).
 * `byte_off` is relative to VNET_R_CONFIG; `sas` is the ESR_EL2.ISS access
 * size (0=byte,1=halfword,2/3=word). FreeBSD's if_vtnet reads a uint8_t[6]
 * config field with individual byte-width accesses, so — unlike every other
 * register here, which is always word-width per the virtio-mmio spec — this
 * one must return a correctly narrow, zero-extended value or corrupt
 * whatever the guest packs into the unread high bits of its destination
 * register. */
static uint32_t vnet_config_read(uint32_t byte_off, uint32_t sas)
{
	uint8_t mac[VNET_GUEST_MAC_LEN];
	uint32_t nbytes = (sas == 0u) ? 1u : (sas == 1u) ? 2u : 4u;
	uint32_t status = g_net.link_up ? VNET_S_LINK_UP : 0u;   /* le16 @6 */
	uint32_t v = 0;

	emac_guest_mac(mac);
	for (uint32_t i = 0; i < nbytes; i++) {
		uint32_t idx = byte_off + i;
		uint8_t b = 0u;
		if (idx < VNET_GUEST_MAC_LEN)
			b = mac[idx];
		else if (idx == VNET_CFG_STATUS_OFF)
			b = (uint8_t)status;
		else if (idx == VNET_CFG_STATUS_OFF + 1u)
			b = (uint8_t)(status >> 8);
		v |= (uint32_t)b << (8u * i);
	}
	return v;
}

static void vnet_reg_write_locked(struct vnet_dev *d, uint32_t off, uint32_t val);

/* Which queue locks a register write needs (bit q = lock queue q): every
 * write that changes a queue's ring registers or readiness takes that
 * queue's lock, a reset (Status=0) takes both. Everything else -- notably
 * QueueNotify (vnet_kick locks TX itself) and InterruptACK -- takes none. */
static uint32_t vnet_write_locks(const struct vnet_dev *d, uint32_t off, uint32_t val)
{
	switch (off) {
	case VNET_R_QUEUE_NUM:
	case VNET_R_QUEUE_DESC_LOW:   case VNET_R_QUEUE_DESC_HIGH:
	case VNET_R_QUEUE_DRIVER_LOW: case VNET_R_QUEUE_DRIVER_HIGH:
	case VNET_R_QUEUE_DEVICE_LOW: case VNET_R_QUEUE_DEVICE_HIGH:
	case VNET_R_QUEUE_READY:
		return d->queue_sel < VNET_NUM_QUEUES ? (1u << d->queue_sel) : 0u;
	case VNET_R_STATUS:
		return val == 0u ? ((1u << VNET_NUM_QUEUES) - 1u) : 0u;
	default:
		return 0u;
	}
}

static void vnet_reg_write(struct vnet_dev *d, uint32_t off, uint32_t val)
{
	uint32_t locks = vnet_write_locks(d, off, val);

	/* Fixed order (RX then TX) so two resets on two vCPUs cannot deadlock. */
	for (uint32_t q = 0; q < VNET_NUM_QUEUES; q++)
		if (locks & (1u << q))
			vq_lock(q);
	vnet_reg_write_locked(d, off, val);
	for (uint32_t q = VNET_NUM_QUEUES; q-- > 0; )
		if (locks & (1u << q))
			vq_unlock(q);
}

static void vnet_reg_write_locked(struct vnet_dev *d, uint32_t off, uint32_t val)
{
	switch (off) {
	case VNET_R_DEVICE_FEATURES_SEL: d->dev_feat_sel = val; break;
	case VNET_R_DRIVER_FEATURES_SEL: d->drv_feat_sel = val; break;
	case VNET_R_DRIVER_FEATURES:
		if (d->drv_feat_sel < 2u)
			d->driver_features |= ((uint64_t)val) << (32u * d->drv_feat_sel);
		break;
	case VNET_R_QUEUE_SEL:
		d->queue_sel = val;             /* 0 = RX, 1 = TX */
		break;
	case VNET_R_QUEUE_NUM:
		if (d->queue_sel < VNET_NUM_QUEUES) {
			struct vnet_vq *vq = &d->vq[d->queue_sel];
			uint32_t v = (val > VNET_QUEUE_MAX) ? VNET_QUEUE_MAX : val;
			/* D5(b): must be a power of two (or 0) — see vblk_emmc.c's
			 * identical fix/rationale. FreeBSD's if_vtnet always negotiates a
			 * power-of-two size, so this never fires for the real guest;
			 * refuses (leaves vq->num UNCHANGED) rather than silently
			 * substituting a different size than the guest configured. */
			if (v != 0u && (v & (v - 1u)) != 0u) {
				vnet_bc(16, ++g_bad_queue_num);
				break;
			}
			vq->num = v;
		}
		break;
	case VNET_R_QUEUE_DESC_LOW:
		if (d->queue_sel < VNET_NUM_QUEUES) {
			struct vnet_vq *vq = &d->vq[d->queue_sel];
			vq->desc = (vq->desc & ~0xFFFFFFFFULL) | val;
		}
		break;
	case VNET_R_QUEUE_DESC_HIGH:
		if (d->queue_sel < VNET_NUM_QUEUES)
			d->vq[d->queue_sel].desc =
			    (d->vq[d->queue_sel].desc & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VNET_R_QUEUE_DRIVER_LOW:
		if (d->queue_sel < VNET_NUM_QUEUES) {
			struct vnet_vq *vq = &d->vq[d->queue_sel];
			vq->avail = (vq->avail & ~0xFFFFFFFFULL) | val;
		}
		break;
	case VNET_R_QUEUE_DRIVER_HIGH:
		if (d->queue_sel < VNET_NUM_QUEUES)
			d->vq[d->queue_sel].avail =
			    (d->vq[d->queue_sel].avail & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VNET_R_QUEUE_DEVICE_LOW:
		if (d->queue_sel < VNET_NUM_QUEUES) {
			struct vnet_vq *vq = &d->vq[d->queue_sel];
			vq->used = (vq->used & ~0xFFFFFFFFULL) | val;
		}
		break;
	case VNET_R_QUEUE_DEVICE_HIGH:
		if (d->queue_sel < VNET_NUM_QUEUES)
			d->vq[d->queue_sel].used =
			    (d->vq[d->queue_sel].used & 0xFFFFFFFFULL) | ((uint64_t)val << 32);
		break;
	case VNET_R_QUEUE_READY:
		if (d->queue_sel < VNET_NUM_QUEUES) {
			struct vnet_vq *vq = &d->vq[d->queue_sel];
			if ((val & 1u) && !vq_rings_ok(vq)) {
				/* A ring outside guest RAM: refuse. The queue stays
				 * not-ready, which the driver sees on read-back. */
				vnet_bc(20, ++g_bad_ring);
				vq->ready = 0u;
			} else if (val & 1u) {
				vq->last_avail = 0;      /* fresh negotiation */
				/* Barrier BEFORE the ready store, not after: publish the
				 * whole vq struct (desc/avail/used/num, all written just
				 * above/before this) so CPU1 cannot observe ready==1 while
				 * the ring pointers are still stale. Setting ready first and
				 * fencing after (the old order) left exactly that window open
				 * for queue 0 (RX), which CPU1 starts polling the moment it
				 * sees ready==1. */
				__asm__ volatile("dsb sy" ::: "memory");
				vq->ready = 1u;
				__asm__ volatile("dsb sy" ::: "memory");
			} else {
				vq->ready = 0u;
			}
		}
		break;
	case VNET_R_QUEUE_NOTIFY:
		vnet_kick(d, val);
		break;
	case VNET_R_INTERRUPT_ACK:
		int_status_clear((volatile uint32_t *)&d->int_status, val);
		break;
	case VNET_R_STATUS:
		if (val == 0u) {
			/* Driver reset, with BOTH queue locks held (vnet_reg_write):
			 * any RX scatter or TX drain in flight has finished, and none
			 * can start until the queues below read not-ready. Status
			 * goes to 0 first so RX's DRIVER_OK re-check fails too. */
			d->status = 0u;
			__asm__ volatile("dmb ish" ::: "memory");
			for (uint32_t q = 0; q < VNET_NUM_QUEUES; q++) {
				struct vnet_vq *vq = &d->vq[q];
				vq->ready = 0; vq->num = 0; vq->last_avail = 0;
				vq->desc = vq->avail = vq->used = 0;
			}
			d->driver_features = 0;   /* renegotiated after a reset */
			int_status_clear((volatile uint32_t *)&d->int_status, ~0u);
			__asm__ volatile("dsb ish" ::: "memory");
		} else {
			d->status = val;
		}
		break;
	default:
		/* Writes to RO/unknown registers are silently ignored. */
		break;
	}
}

/* ------------------------------------------------------------------ *
 * el2_trap dispatch entry — SAME contract as vblk_mmio_fault(). This
 * function is written but NOT wired into el2_exc.c (a reserved file for
 * this task); see the report for the one-line addition el2_trap needs
 * (immediately after the existing vblk_mmio_fault() call, same pattern:
 *
 *     if (vnet_mmio_fault(frame)) {
 *         if (!dbg_core_active)
 *             dbgmon_service(frame);
 *         return;
 *     }
 *
 * ). vnet_mmio_fault() returns 0 for any abort outside its 0x200-byte
 * window (disjoint from both the UART0 page and vblk's window), so calling
 * it unconditionally after vblk_mmio_fault() on every guest data abort is
 * safe.
 * ------------------------------------------------------------------ */
int vnet_mmio_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec  = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	if (addr < g_net.base || addr >= g_net.base + VNET_MMIO_SIZE)
		return 0;                         /* not our window */

	vnet_bc(10, ++g_faults);

	uint32_t isv = esr & ESR_ISV_BIT;
	if (!isv) {
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;
	/* Every STANDARD virtio-mmio register is a 32-bit word access — sas is
	 * only actually consulted below for the config-space (guest MAC)
	 * read path, which FreeBSD accesses byte-at-a-time. */

	uint32_t off = (uint32_t)(addr - g_net.base);

	if (wnr) {
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];
		vnet_reg_write(&g_net, off, (uint32_t)val);
		/* B4 flight recorder: log only the kick (QueueNotify), not every
		 * register write — a0=off, a1=val (queue index notified). */
		if (off == VNET_R_QUEUE_NOTIFY)
			flightrec_log(FLTR_K_VIRTIO, off, val);
	} else {
		uint32_t val = (off >= VNET_R_CONFIG && off < VNET_R_CONFIG + VNET_CFG_LEN)
		             ? vnet_config_read(off - VNET_R_CONFIG, sas)
		             : vnet_reg_read(&g_net, off);
		if (srt != SRT_XZR)
			frame->x[srt] = (uint64_t)val;
	}

	frame->elr += 4;   /* emulated — skip the faulting load/store */
	return 1;
}

/* ------------------------------------------------------------------ *
 * Init.
 * ------------------------------------------------------------------ */
void vnet_init(void)
{
	for (uint32_t i = 0; i < sizeof(g_net); i++)
		((uint8_t *)&g_net)[i] = 0;

	g_net.base = VNET_MMIO_BASE;
	g_rx_queued = g_rx_dropped = g_tx_sent = g_tx_blocked = g_tx_dropped = 0;
	g_irqs = g_faults = g_tx_truncated = g_rx_truncated = 0;

	g_gmem_oob = g_bad_queue_num = g_bad_desc_flags = g_kick_not_ready = 0;
	g_bad_chain = g_bad_ring = g_tx_blocked_vlan = 0;
	g_tx_linkdown = g_rx_reset_busy = g_link_changes = 0;
	g_force_down_polls = 0;
	g_vq_lock[0][0] = g_vq_lock[1][0] = 0u;
	/* Start from the EMAC's current state; vnet_emac_link_update() (every
	 * emac_poll) publishes each change after this. */
	g_net.link_up = emac_link_up() ? 1u : 0u;

	/* Every slot, not just the ready bits: the window is not cleared by
	 * anything else, and a counter that has never fired must read 0, not
	 * whatever the DRAM held. */
	for (uint32_t i = 1; i <= VNET_BC_LAST; i++)
		vnet_bc(i, 0);
	vnet_bc(25, g_net.link_up);
	vnet_bc(0, VNET_BC_MAGIC);
}
