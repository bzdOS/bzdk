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
 *            if ethertype == 0x88B5 (our own debug protocol): DROP. Never
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
 * Residual gap (review D7): emac_link_watchdog() re-runs rings_init() + the DMA
 * enables WITHOUT the TX lock, guarded only by g_rx_count==0 — which does not
 * preclude a concurrent guest TX. That corner still wants closing.
 *
 * A second, narrower hazard: vnet_dev.int_status is read-modify-written from
 * BOTH cores (CPU0 clears acked bits on VNET_R_INTERRUPT_ACK writes, sets
 * VNET_INT_VRING on TX completions; CPU1 sets the SAME single bit on RX
 * completions). Because there is only one cause bit in this device, a lost
 * update between the two OR-writes converges to the same final value either
 * way — benign by construction, NOT by real synchronization. Per-queue ring
 * state (vq[RX] vs vq[TX] and their desc/avail/used/last_avail) is safe in
 * STEADY STATE because each queue's ring is only ever walked by one core
 * (CPU1 for RX/queue 0, CPU0 for TX/queue 1) — but the ONE-TIME negotiation
 * window (CPU0 writing QueueDesc/QueueDriver/QueueDevice/QueueReady for
 * queue 0 while CPU1 could, in principle, already be polling) has no barrier
 * protecting it beyond the single dsb added to the QueueReady write path
 * below. Narrow, not proven safe, flagged rather than solved.
 *
 * Freestanding: <stdint.h> only.
 */
#include <stdint.h>
#include "vnet_emac.h"
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
 * ------------------------------------------------------------------ */
#define VNET_BC_BASE   0x50030000UL
#define VNET_BC_MAGIC  0x564E5431u   /* "VNT1" */

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
/* D2 fix: guest-PA range check — see vblk_emmc.c's gpa_in_range() block
 * comment for the full rationale (identical reasoning applies here: desc.addr/
 * avail/used-ring PAs are guest-supplied, EL2 dereferences them directly with
 * no stage-2 protection). Duplicated (not shared) per this file's
 * self-containment convention, same as gmem_cmo/gmem_read/gmem_write
 * themselves. MUST stay in lockstep with stage2.h's STAGE2_DRAM_BASE/
 * STAGE2_DRAM_SIZE (currently 0x40000000 / 0x40000000). Does NOT protect the
 * in-DRAM hv-image/hv-scratch windows (see vblk_emmc.c's comment) — that
 * needs real stage-2/DMA isolation (milestone A1), out of scope here. */
#define GUEST_DRAM_BASE   0x40000000ULL
#define GUEST_DRAM_SIZE   0x40000000ULL
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
		vnet_bc(15, ++g_gmem_oob);      /* D2: corrupt/wild guest PA — drop */
		return;
	}
	const volatile uint8_t *s = (const volatile uint8_t *)(uintptr_t)gpa;
	uint8_t *d = (uint8_t *)dst;
	gmem_cmo(gpa, len);                  /* refetch from PoC, never stale */
	for (uint32_t i = 0; i < len; i++)
		d[i] = s[i];
}

static void gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	if (!gpa_in_range(gpa, len)) {
		vnet_bc(15, ++g_gmem_oob);      /* D2: corrupt/wild guest PA — drop */
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
static void vq_read_desc(struct vnet_vq *vq, uint16_t idx, struct vnet_desc *out)
{
	struct vnet_vring_desc d;
	uint64_t p = vq->desc + (uint64_t)idx * sizeof(struct vnet_vring_desc);
	gmem_read(p, &d, sizeof(d));
	out->addr  = d.addr;
	out->len   = d.len;
	out->flags = d.flags;
	out->next  = d.next;
}

static int vq_pop_avail(struct vnet_vq *vq, uint16_t *head)
{
	uint16_t avail_idx;

	if (!vq->ready || vq->num == 0)
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
		vq_read_desc(vq, idx, &chain[n]);
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

	if (ethertype == VNET_ETHERTYPE_DEBUG) {
		/* HARD BLOCK: the guest must never be able to inject a frame that
		 * masquerades as the HV's own debug protocol. Never reaches
		 * emac_send_frame() / real hardware. Completion still pushed so the
		 * guest's ring drains — indistinguishable from a real NIC quietly
		 * dropping a frame. */
		vnet_bc(7, ++g_tx_blocked);
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
		int ok = emac_send_frame_to(dst, ethertype, payload, (uint16_t)plen);
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

		while (vq_pop_avail(vq, &head)) {
			vnet_tx_one(d, head);
			served = 1;
		}
		if (served) {
			d->int_status |= VNET_INT_VRING;
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

	if (!(g_net.status & VNET_S_DRIVER_OK)) {
		vnet_bc(5, ++g_rx_dropped);          /* driver not attached yet */
		return;
	}
	if (len < VNET_ETH_HDR_LEN || (uint32_t)len > VNET_STAGE_BUF_SIZE - VNET_HDR_LEN) {
		vnet_bc(5, ++g_rx_dropped);          /* malformed / oversize */
		return;
	}
	if (!vq_pop_avail(vq, &head)) {
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
	for (uint32_t i = 0; i < VNET_HDR_LEN; i++)
		g_rx_stage[i] = ((const uint8_t *)&hdr)[i];
	for (uint32_t i = 0; i < len; i++)
		g_rx_stage[VNET_HDR_LEN + i] = frame[i];

	struct vnet_desc chain[VNET_MAX_CHAIN];
	uint32_t n = vq_collect_chain(vq, head, chain, VNET_MAX_CHAIN);
	if (n == 0) {
		vq_push_used(vq, head, 0);
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
	vnet_bc(4, ++g_rx_queued);

	g_net.int_status |= VNET_INT_VRING;      /* see cross-core note: benign
	                                          * single-bit OR from CPU1 */
	vnet_inject_irq();
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
		 * (see vnet_emac.h's VNET_GUEST_MAC comment for why) — no other
		 * MAC/CSUM/offload/MRG_RXBUF/STATUS bits offered, matching
		 * vblk_emmc.c's minimal-feature-set precedent. */
		if (d->dev_feat_sel == VNET_FEATWORD_HI)
			return VNET_F_VERSION_1_BIT;
		if (d->dev_feat_sel == VNET_FEATWORD_LO)
			return VNET_F_MAC_BIT;
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

/* Config-space byte-granular read for VNET_GUEST_MAC (see vnet_emac.h).
 * `byte_off` is relative to VNET_R_CONFIG; `sas` is the ESR_EL2.ISS access
 * size (0=byte,1=halfword,2/3=word). FreeBSD's if_vtnet reads a uint8_t[6]
 * config field with individual byte-width accesses, so — unlike every other
 * register here, which is always word-width per the virtio-mmio spec — this
 * one must return a correctly narrow, zero-extended value or corrupt
 * whatever the guest packs into the unread high bits of its destination
 * register. */
static uint32_t vnet_config_read(uint32_t byte_off, uint32_t sas)
{
	static const uint8_t mac[VNET_GUEST_MAC_LEN] = VNET_GUEST_MAC;
	uint32_t nbytes = (sas == 0u) ? 1u : (sas == 1u) ? 2u : 4u;
	uint32_t v = 0;

	for (uint32_t i = 0; i < nbytes; i++) {
		uint32_t idx = byte_off + i;
		uint8_t b = (idx < VNET_GUEST_MAC_LEN) ? mac[idx] : 0u;
		v |= (uint32_t)b << (8u * i);
	}
	return v;
}

static void vnet_reg_write(struct vnet_dev *d, uint32_t off, uint32_t val)
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
			if (val & 1u) {
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
		d->int_status &= ~val;
		break;
	case VNET_R_STATUS:
		d->status = val;
		if (val == 0u) {
			/* Driver reset: clear queue state (device stays registered). */
			for (uint32_t q = 0; q < VNET_NUM_QUEUES; q++) {
				struct vnet_vq *vq = &d->vq[q];
				vq->ready = 0; vq->num = 0; vq->last_avail = 0;
				vq->desc = vq->avail = vq->used = 0;
			}
			d->int_status = 0;
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
	 * only actually consulted below for the config-space (VNET_GUEST_MAC)
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
		uint32_t val = (off >= VNET_R_CONFIG && off < VNET_R_CONFIG + VNET_GUEST_MAC_LEN)
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

	vnet_bc(0, VNET_BC_MAGIC);
	vnet_bc(2, 0); vnet_bc(3, 0);
}
