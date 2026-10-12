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
 *       -> vnet_tx_drain(): for up to VNET_TX_BUDGET available chains:
 *            reserve the next EMAC TX descriptor (emac_tx_begin(), never
 *              waits; a full ring leaves the chain on the avail ring and
 *              CPU1 continues the drain, vnet_emac_tx_resume());
 *            gather the chain minus its virtio_net_hdr straight into that
 *              descriptor's DMA buffer;
 *            check the copy: one of the HV's own ethertypes 0x88B5..0x88B8,
 *              bare or behind one/two VLAN tags (vnet_tx_is_hv_ethertype()),
 *              or the HV's source MAC -> the slot goes back unsent; the
 *              guest cannot spoof the debug channel. Completion is still
 *              pushed (len=0) so the guest's ring drains normally — from the
 *              driver's perspective this is indistinguishable from a real
 *              NIC silently dropping a frame;
 *            else emac_tx_end() hands it to the DMA, dst/src as the guest
 *              built them;
 *            push the used entry (the frame has been copied out).
 *       -> set InterruptStatus.VRING, inject VNET_INTID once.
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
 *          total byte count. emac.c calls vnet_emac_rx_flush() at the end of
 *          each drain, which injects VNET_INTID once for the whole batch.
 *          Since 2026-10-12 the drain runs from the EMAC RX interrupt on CPU1
 *          as well as from the 10 ms tick (audit item 9).
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
#include "nc_copy.h"        /* word-wide copies to/from Non-cacheable memory */

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
/*   [27] EL2 time spent delivering RX frames (CNTPCT ticks, 24 MHz, low 32)
 *   [28] EL2 time spent in TX kicks (same unit)
 *   [29] TX kicks (QueueNotify on the transmitq)
 *   [30] RX frames dropped for not fitting the guest's buffer (were
 *        truncated before 2026-10-12)
 *   [31] TX frames dropped for exceeding the largest Ethernet frame
 *   [32] TX drains stopped because the EMAC ring was full (frame left on
 *        the avail ring for CPU1 to send, see vnet_tx_drain())
 *   [33] TX drains CPU1 continued (vnet_emac_tx_resume())
 *   [34] interrupts not raised because the driver suppressed them
 *        (EVENT_IDX used_event / VRING_AVAIL_F_NO_INTERRUPT, audit item 12)
 *   [35] control-queue commands  [36] of which refused (ack ERR)
 *   [37] EMAC receive filter in force (emac_rx_filter_mode(): 0 hardware
 *        filter, 1 + all-multicast, 2 promiscuous)
 *
 * Since 2026-10-12 (audit item 10) a counter update is a plain store into
 * g_bcv[]; vnet_bc_publish() copies the whole table to the window from the
 * CPU1 tick (vnet_emac_link_update(), every emac_poll()). The window lags by
 * at most one tick, and the per-frame path no longer pays a cache clean plus
 * a full-system barrier for each counter it bumps. */
#define VNET_BC_BASE   0x50030000UL
#define VNET_BC_MAGIC  0x564E5431u   /* "VNT1" */
#define VNET_BC_LAST   37u           /* highest slot used above */
#define VNET_BC_SLOTS  40u

static uint32_t g_bcv[VNET_BC_SLOTS];

static inline void vnet_bc(uint32_t idx, uint32_t v)
{
	g_bcv[idx] = v;
}

static void vnet_bc_publish(void)
{
	volatile uint32_t *w = (volatile uint32_t *)VNET_BC_BASE;
	for (uint32_t i = 1; i < VNET_BC_SLOTS; i++)
		w[i] = g_bcv[i];
	w[0] = VNET_BC_MAGIC;
	for (uint32_t off = 0; off < VNET_BC_SLOTS * 4u; off += 64u)
		__asm__ volatile("dc civac, %0" :: "r"(VNET_BC_BASE + off) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint64_t vnet_now(void)
{
	uint64_t t;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
	return t;
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
static uint32_t g_rx_irq_pending;     /* CPU1: frames queued since the last flush */
static uint32_t g_rx_too_big;         /* [30] RX frame larger than the guest buffer */
static uint32_t g_tx_too_big;         /* [31] TX frame larger than Ethernet allows */
static uint32_t g_tx_kicks;           /* [29] */
static uint64_t g_rx_time, g_tx_time; /* [27]/[28] CNTPCT ticks in EL2 */

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
	gmem_cmo(gpa, len);                  /* refetch from PoC, never stale */
	nc_copy(dst, (const void *)(uintptr_t)gpa, len);
	return 1;
}

static void gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	if (!guest_gpa_ok(gpa, len)) {
		vnet_bc(15, ++g_gmem_oob);      /* not guest RAM: drop the write */
		return;
	}
	/* Clean+invalidate BEFORE the store too: a line the guest dirtied in
	 * its own cache would otherwise be written back over our data by the
	 * clean below. After: drop anything speculatively refetched meanwhile. */
	gmem_cmo(gpa, len);
	nc_copy((void *)(uintptr_t)gpa, src, len);
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

/* Like vq_pop_avail() but leaves the entry on the ring: the caller bumps
 * vq->last_avail itself once the chain is really consumed (TX may have to
 * leave it there for a full EMAC ring, see vnet_tx_drain()). */
static int vq_peek_avail(struct vnet_vq *vq, uint16_t *head)
{
	if (!vq->ready || vq->num == 0 || !vq_rings_ok(vq))
		return 0;
	uint16_t avail_idx = gmem_ld16(vq->avail + 2u);
	if (vq->last_avail == avail_idx)
		return 0;
	*head = gmem_ld16(vq->avail + 4u + (uint64_t)(vq->last_avail % vq->num) * 2u);
	return 1;
}

static uint16_t vq_push_used(struct vnet_vq *vq, uint16_t head, uint32_t used_len)
{
	/* Re-read rather than trust a shadow: a snapshot restore rewrites
	 * guest RAM, rings included, under a running device. */
	uint16_t used_idx = gmem_ld16(vq->used + 2u);
	uint16_t slot = (uint16_t)(used_idx % vq->num);
	uint64_t e = vq->used + 4u + (uint64_t)slot * 8u;   /* &ring[slot] */
	uint32_t ent[2] = { head, used_len };
	uint16_t new_idx = (uint16_t)(used_idx + 1u);

	gmem_write(e, ent, 8u);                             /* id + len at once */
	__asm__ volatile("dsb sy" ::: "memory");            /* entry before idx */
	gmem_st16(vq->used + 2u, new_idx);
	__asm__ volatile("dsb sy" ::: "memory");
	vq->used_idx = new_idx;
	return new_idx;
}

/* Interrupt suppression (audit item 12). Does the driver want an interrupt
 * for the used entries pushed since the last decision? With EVENT_IDX:
 * did used->idx pass used_event (virtio 1.x 2.7.10, vring_need_event());
 * without: is VRING_AVAIL_F_NO_INTERRUPT clear. Caller holds the queue
 * lock, so the rings cannot be torn down under it. */
static int vq_want_irq(const struct vnet_dev *d, struct vnet_vq *vq)
{
	uint16_t now = vq->used_idx, old = vq->signalled_used;

	vq->signalled_used = now;
	if (now == old)
		return 0;                          /* nothing new */
	if (d->driver_features & VNET_F_EVENT_IDX_BIT) {
		uint16_t ev = gmem_ld16(vq->avail + 4u + 2u * (uint64_t)vq->num);
		return (uint16_t)(now - ev - 1u) < (uint16_t)(now - old);
	}
	return !(gmem_ld16(vq->avail) & VNET_VRING_AVAIL_F_NO_INTERRUPT);
}

/* Kick suppression: tell the driver whether to notify us when it adds to
 * this queue. With EVENT_IDX avail_event says "after entry N": N =
 * last_avail asks for a kick on the next new entry, last_avail + 0x8000 is
 * never reached. Without it, VRING_USED_F_NO_NOTIFY. */
static void vq_set_notify(const struct vnet_dev *d, struct vnet_vq *vq, int on)
{
	if (d->driver_features & VNET_F_EVENT_IDX_BIT) {
		uint16_t ev = on ? vq->last_avail : (uint16_t)(vq->last_avail + 0x8000u);
		gmem_st16(vq->used + 4u + 8u * (uint64_t)vq->num, ev);
	} else {
		gmem_st16(vq->used, on ? 0u : VNET_VRING_USED_F_NO_NOTIFY);
	}
	__asm__ volatile("dsb sy" ::: "memory");
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


/* Bytes the guest can take in a (write-only, RX-direction) descriptor
 * chain. */
static uint32_t vnet_chain_room(const struct vnet_desc *chain, uint32_t n)
{
	uint64_t room = 0;            /* guest-chosen lengths: no 32-bit wrap */
	for (uint32_t i = 0; i < n; i++)
		room += chain[i].len;
	return room > 0xffffffffull ? 0xffffffffu : (uint32_t)room;
}

/* Scatter the virtio-net header and then the frame straight from the EMAC
 * DMA buffer across the chain, in order -- no staging copy (audit item 10).
 * The caller has checked that the chain is big enough. Returns bytes
 * written. */
static uint32_t vnet_scatter2(const struct vnet_desc *chain, uint32_t n,
                              const uint8_t *hdr, uint32_t hdr_len,
                              const uint8_t *src, uint32_t src_len)
{
	uint32_t written = 0;
	uint32_t ci = 0, coff = 0;
	const uint8_t *p = hdr;
	uint32_t left = hdr_len;

	for (int part = 0; part < 2; part++) {
		while (left && ci < n) {
			uint32_t avail = chain[ci].len - coff;
			if (avail == 0) { ci++; coff = 0; continue; }
			uint32_t take = left < avail ? left : avail;
			gmem_write(chain[ci].addr + coff, p, take);
			p += take;
			left -= take;
			written += take;
			coff += take;
		}
		p = src;
		left = src_len;
	}
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
 *
 * Since 2026-10-12 (audit item 11) the frame is gathered straight from the
 * guest's chain into the EMAC descriptor's DMA buffer (emac_tx_begin()),
 * checked THERE -- in HV-owned memory, so a guest vCPU rewriting its buffer
 * after the check cannot change what is sent -- and handed to the DMA. If
 * the EMAC ring has no room the chain is left on the avail ring, not
 * dropped and not waited for: the drain stops, g_tx_resume asks CPU1 to
 * continue it (emac_rx_irq() on TX_INT, or the next tick), and the guest
 * vCPU returns to the guest at once.
 * ------------------------------------------------------------------ */
#define VNET_TX_DONE   1     /* chain consumed (sent or dropped)       */
#define VNET_TX_RETRY  0     /* no EMAC descriptor: leave it on the ring */

/* Frames one drain may move before it hands the rest to CPU1: a guest
 * burst never holds a vCPU in EL2 for more than ~VNET_TX_BUDGET * 10 us. */
#define VNET_TX_BUDGET 32u

static volatile uint32_t g_tx_resume;  /* TX drain left work for CPU1 */
static uint32_t g_tx_retries;          /* [32] drains stopped on a full ring */
static uint32_t g_tx_resumes;          /* [33] drains CPU1 continued */
static uint32_t g_irq_suppressed;      /* [34] used entries pushed, driver asked for no IRQ */

static int vnet_tx_one(struct vnet_dev *d, uint16_t head)
{
	struct vnet_vq *vq = &d->vq[VNET_QUEUE_TX];
	struct vnet_desc chain[VNET_MAX_CHAIN];
	uint32_t n = vq_collect_chain(vq, head, chain, VNET_MAX_CHAIN);

	if (n == 0)
		return VNET_TX_DONE;          /* malformed: completed with length 0 */

	/* D5(d), DIAGNOSTIC ONLY (same "count, don't enforce" caution as
	 * vblk_emmc.c's identical fix): every transmitq descriptor is guest-
	 * supplied, device-READ data (the device writes nothing back except the
	 * zero-length completion) — none should carry VNET_VRING_DESC_F_WRITE.
	 * Not enforced: a wrong hard-reject here would break the guest's
	 * already-working TX path outright. */
	for (uint32_t i = 0; i < n; i++)
		if (chain[i].flags & VNET_VRING_DESC_F_WRITE)
			vnet_bc(17, ++g_bad_desc_flags);

	uint32_t room = vnet_chain_room(chain, n);
	/* Larger than any Ethernet frame (1514 + one VLAN tag): drop it whole
	 * rather than send a truncated, corrupt frame (audit item 13). */
	if (room > VNET_HDR_LEN + VNET_ETH_MAX_FRAME + 4u) {
		vnet_bc(31, ++g_tx_too_big);
		return VNET_TX_DONE;
	}
	/* Need the virtio header plus an Ethernet header to have an ethertype
	 * to mux on. Anything shorter is malformed — drop cleanly. */
	if (room < VNET_HDR_LEN + VNET_ETH_HDR_LEN)
		return VNET_TX_DONE;
	uint32_t len = room - VNET_HDR_LEN;

	if (!g_net.link_up) {
		/* Link down (config.status says so too, and the guest was sent a
		 * config-change interrupt): nothing can leave. virtio-net has no
		 * per-frame TX error status, so the completion is the most a
		 * device can say; the honest signal is the link state. */
		vnet_bc(22, ++g_tx_linkdown);
		return VNET_TX_DONE;
	}

	uint8_t *buf;
	int slot = emac_tx_begin(1, &buf);
	if (slot == EMAC_TX_BUSY)
		return VNET_TX_RETRY;
	if (slot < 0) {
		vnet_bc(8, ++g_tx_dropped);       /* lock / link / wedged descriptor */
		return VNET_TX_DONE;
	}

	/* Gather, skipping the 12-byte virtio_net_hdr (no offloads were
	 * negotiated, so it carries nothing), however the guest split it. */
	uint32_t skip = VNET_HDR_LEN, off = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t l = chain[i].len;
		uint64_t a = chain[i].addr;
		if (skip) {
			uint32_t k = l < skip ? l : skip;
			skip -= k; l -= k; a += k;
		}
		if (l) {
			gmem_read(a, buf + off, l);
			off += l;
		}
	}
	if (len < VNET_ETH_MIN_FRAME) {
		nc_zero(buf + len, VNET_ETH_MIN_FRAME - len);
		len = VNET_ETH_MIN_FRAME;
	}

	/* Checks on the copy (up to two VLAN tags: 22 bytes). */
	uint8_t eh[24];
	uint32_t ehl = len < sizeof(eh) ? len : (uint32_t)sizeof(eh);
	nc_copy(eh, buf, ehl);
	uint16_t ethertype = (uint16_t)((eh[12] << 8) | eh[13]);
	vnet_bc(11, ethertype);

	int hv_type = vnet_tx_is_hv_ethertype(eh, ehl);
	/* Nor may it borrow the HV's source MAC (the guest has its own since
	 * 2026-10-12; host tools identify the HV by this address). */
	if (!hv_type && emac_mac_is_hv(eh + 6))
		hv_type = 1;
	if (hv_type) {
		/* HARD BLOCK: the guest must never be able to inject a frame that
		 * masquerades as one of the HV's own protocols (0x88B5..0x88B8,
		 * bare or VLAN-wrapped), nor use the HV's source MAC: host tools
		 * must be able to tell the two apart. The slot goes back unsent;
		 * the completion still drains the guest's ring — indistinguishable
		 * from a real NIC quietly dropping a frame. */
		emac_tx_end(slot, 0, 0);
		vnet_bc(7, ++g_tx_blocked);
		if (hv_type > 1)
			vnet_bc(21, ++g_tx_blocked_vlan);
		return VNET_TX_DONE;
	}

	/* The guest's own dst and src go out as they are (see the "MAC
	 * IDENTITY NOTE" history at the top of this file). */
	emac_tx_end(slot, len, 1);
	vnet_bc(6, ++g_tx_sent);
	return VNET_TX_DONE;
}

/* Move up to `budget` chains from the transmitq to the EMAC. Caller holds
 * the TX queue lock. Completions are pushed as each chain is consumed (the
 * frame has been copied out, so the guest may reuse its buffer -- this is
 * a copying device). Returns the number of chains completed; sets
 * g_tx_resume if work is left (full EMAC ring or budget spent). */
static uint32_t vnet_tx_drain(struct vnet_dev *d, uint32_t budget)
{
	struct vnet_vq *vq = &d->vq[VNET_QUEUE_TX];
	uint16_t head;
	uint32_t done = 0;

	g_tx_resume = 0u;
	while (vq_peek_avail(vq, &head)) {
		if (done == budget) {
			g_tx_resume = 1u;
			break;
		}
		if (vnet_tx_one(d, head) == VNET_TX_RETRY) {
			vnet_bc(32, ++g_tx_retries);
			g_tx_resume = 1u;
			break;
		}
		vq->last_avail++;
		vq_push_used(vq, head, 0);   /* TX completions carry no length */
		done++;
	}
	return done;
}

/* Drain, then set kick suppression to match: work left for CPU1 (full EMAC
 * ring or budget spent) -> no kicks, they could only find the ring full
 * again; caught up -> kick on the next frame. Re-check after re-enabling:
 * a frame the driver queued between our last look and that store saw the
 * old setting and did not kick (virtio 1.x 2.7.10 "re-check"). Caller holds
 * the TX queue lock. */
static uint32_t vnet_tx_drain_notify(struct vnet_dev *d)
{
	struct vnet_vq *vq = &d->vq[VNET_QUEUE_TX];
	uint32_t done = 0;

	for (int pass = 0; pass < 2; pass++) {
		done += vnet_tx_drain(d, VNET_TX_BUDGET);
		if (g_tx_resume) {
			vq_set_notify(d, vq, 0);
			break;
		}
		vq_set_notify(d, vq, 1);
		if (gmem_ld16(vq->avail + 2u) == vq->last_avail)
			break;
		if (pass == 1)
			g_tx_resume = 1u;            /* still racing: CPU1 picks it up */
	}
	return done;
}

static int vnet_tx_want_irq(struct vnet_dev *d)
{
	int want = vq_want_irq(d, &d->vq[VNET_QUEUE_TX]);
	if (!want)
		vnet_bc(34, ++g_irq_suppressed);
	return want;
}

static void vnet_tx_irq(struct vnet_dev *d)
{
	int_status_or((volatile uint32_t *)&d->int_status, VNET_INT_VRING);
	vnet_inject_irq();
}

/* CPU1 (emac_rx_irq() after TX_INT, and every emac_poll()): continue a
 * drain the guest's own kick could not finish. Never waits for the queue
 * lock -- whoever holds it is draining or resetting the queue already. */
void vnet_emac_tx_resume(void)
{
	struct vnet_dev *d = &g_net;

	if (!g_tx_resume || !emac_tx_space())
		return;
	if (!vq_trylock(VNET_QUEUE_TX))
		return;
	uint32_t done = 0;
	int irq = 0;
	if (d->vq[VNET_QUEUE_TX].ready && (d->status & VNET_S_DRIVER_OK)) {
		done = vnet_tx_drain_notify(d);
		irq = done && vnet_tx_want_irq(d);
	} else {
		g_tx_resume = 0u;
	}
	vq_unlock(VNET_QUEUE_TX);
	if (done)
		vnet_bc(33, ++g_tx_resumes);
	if (irq)
		vnet_tx_irq(d);
}

/* ------------------------------------------------------------------ *
 * Control queue: receive filtering (audit item 8).
 *
 * The EMAC used to run with its address filter off (RXALL +
 * DIS_ADDR_FILTER) for good, so every unicast frame on the segment went
 * through the HV and into the guest. With CTRL_RX the driver says what it
 * wants and the EMAC filters in hardware (emac_rx_filter()): ADDR1 = the
 * guest's MAC, ADDR2..7 = extra unicast addresses from MAC_TABLE_SET (more
 * than six -> promiscuous), multicast all-or-nothing (hardware verified
 * 2026-10-12: FRM_FLT bit 16 passes every multicast frame, 0 none),
 * broadcast always. PROMISC (bridge0 sets it on vtnet0, so the WiFi AP
 * bridge keeps working, and tcpdump without -p) switches back to
 * RXALL | DIS_ADDR_FILTER. Only RX_FRM_FLT and the address registers are
 * written, never RX_EN. The HV's own MAC (ADDR0) and broadcast pass in
 * every mode, so the debug channel does not depend on any of this.
 * ------------------------------------------------------------------ */
static struct {
	uint8_t promisc, allmulti, nmc_any;
	uint32_t nuc;                        /* > 6: too many -> promiscuous */
	uint8_t mac[6];                      /* the driver's address */
	uint8_t uc[6][6];
} g_rxf;
static uint8_t g_ctrl_buf[VNET_CTRL_BUF];
static uint32_t g_ctrl_cmds, g_ctrl_errs;   /* [35] / [36] */

static void vnet_rxf_apply(void)
{
	emac_rx_filter(g_rxf.promisc, g_rxf.allmulti || g_rxf.nmc_any,
	               g_rxf.mac, (const uint8_t (*)[6])g_rxf.uc, g_rxf.nuc);
	vnet_bc(37, emac_rx_filter_mode());
}

/* Device reset / init: promiscuous, the guest's assigned MAC, no table. */
static void vnet_rxf_reset(void)
{
	g_rxf.promisc = 1u;
	g_rxf.allmulti = 0u;
	g_rxf.nmc_any = 0u;
	g_rxf.nuc = 0u;
	emac_guest_mac(g_rxf.mac);
	vnet_rxf_apply();
}

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint8_t vnet_ctrl_cmd(const uint8_t *b, uint32_t n)
{
	if (n < 2u)
		return VNET_CTRL_ERR;
	uint8_t cls = b[0], cmd = b[1];
	const uint8_t *p = b + 2;
	uint32_t len = n - 2u;

	if (cls == VNET_CTRL_RX &&
	    (cmd == VNET_CTRL_RX_PROMISC || cmd == VNET_CTRL_RX_ALLMULTI)) {
		if (len < 1u)
			return VNET_CTRL_ERR;
		if (cmd == VNET_CTRL_RX_PROMISC)
			g_rxf.promisc = p[0] ? 1u : 0u;
		else
			g_rxf.allmulti = p[0] ? 1u : 0u;
		vnet_rxf_apply();
		return VNET_CTRL_OK;
	}
	if (cls == VNET_CTRL_MAC && cmd == VNET_CTRL_MAC_TABLE_SET) {
		/* le32 entries + entries * mac[6], unicast table then multicast. */
		if (len < 4u)
			return VNET_CTRL_ERR;
		uint32_t nuc = rd_le32(p);
		if (nuc > 1024u || len < 4u + nuc * 6u + 4u)
			return VNET_CTRL_ERR;
		uint32_t nmc = rd_le32(p + 4u + nuc * 6u);
		if (nmc > 1024u || len < 8u + (nuc + nmc) * 6u)
			return VNET_CTRL_ERR;
		g_rxf.nuc = nuc;
		for (uint32_t i = 0; i < nuc && i < 6u; i++)
			for (uint32_t k = 0; k < 6u; k++)
				g_rxf.uc[i][k] = p[4u + i * 6u + k];
		g_rxf.nmc_any = nmc ? 1u : 0u;
		vnet_rxf_apply();
		return VNET_CTRL_OK;
	}
	if (cls == VNET_CTRL_MAC && cmd == VNET_CTRL_MAC_ADDR_SET) {
		if (len < 6u)
			return VNET_CTRL_ERR;
		/* Not a group address, not all-zero, and never the HV's own: the
		 * HV MAC is the debug channel's identity (TX refuses it too). */
		if ((p[0] & 1u) || !(p[0] | p[1] | p[2] | p[3] | p[4] | p[5]) ||
		    emac_mac_is_hv(p))
			return VNET_CTRL_ERR;
		for (uint32_t k = 0; k < 6u; k++)
			g_rxf.mac[k] = p[k];
		vnet_rxf_apply();
		return VNET_CTRL_OK;
	}
	return VNET_CTRL_ERR;                /* VLAN, ANNOUNCE, MQ: not offered */
}

/* One control command: device-readable descriptors carry class, command
 * and data; the first device-writable one gets the 1-byte ack. */
static uint32_t vnet_ctrl_one(struct vnet_vq *vq, uint16_t head)
{
	struct vnet_desc chain[VNET_MAX_CHAIN];
	uint32_t n = vq_collect_chain(vq, head, chain, VNET_MAX_CHAIN);
	uint32_t got = 0, i;
	int too_big = 0;

	if (n == 0)
		return 0;
	for (i = 0; i < n && !(chain[i].flags & VNET_VRING_DESC_F_WRITE); i++) {
		if (chain[i].len > VNET_CTRL_BUF - got) {
			too_big = 1;
			break;
		}
		if (chain[i].len)
			gmem_read(chain[i].addr, g_ctrl_buf + got, chain[i].len);
		got += chain[i].len;
	}
	while (i < n && !(chain[i].flags & VNET_VRING_DESC_F_WRITE))
		i++;
	if (i == n || chain[i].len < 1u)
		return 0;                        /* nowhere to put the ack */

	uint8_t ack = too_big ? VNET_CTRL_ERR : vnet_ctrl_cmd(g_ctrl_buf, got);
	vnet_bc(35, ++g_ctrl_cmds);
	if (ack != VNET_CTRL_OK)
		vnet_bc(36, ++g_ctrl_errs);
	gmem_write(chain[i].addr, &ack, 1u);
	return 1u;
}

static void vnet_ctrl_kick(struct vnet_dev *d)
{
	struct vnet_vq *vq = &d->vq[VNET_QUEUE_CTRL];
	uint16_t head;
	int irq = 0;

	vq_lock(VNET_QUEUE_CTRL);
	if (vq->ready && (d->driver_features & VNET_F_CTRL_VQ_BIT)) {
		/* Ask for a kick on the next command, then look again for one
		 * queued before that store was visible. With EVENT_IDX this is
		 * mandatory: avail_event stays where we leave it, and a stale one
		 * means the driver never kicks again (found live: if_vtnet hung
		 * in virtqueue_poll() on its second command). */
		for (int pass = 0; pass < 4; pass++) {   /* bounded: guest-driven */
			while (vq_pop_avail(vq, &head))
				vq_push_used(vq, head, vnet_ctrl_one(vq, head));
			vq_set_notify(d, vq, 1);
			if (gmem_ld16(vq->avail + 2u) == vq->last_avail)
				break;
		}
		irq = vq_want_irq(d, vq);
	}
	vq_unlock(VNET_QUEUE_CTRL);
	if (irq) {
		int_status_or((volatile uint32_t *)&d->int_status, VNET_INT_VRING);
		vnet_inject_irq();
	}
}

/* QueueNotify handler: drain up to VNET_TX_BUDGET chains from the notified
 * transmitq, then raise one IRQ if anything was served. Queue 0 (RX) is
 * driven by vnet_emac_rx_frame() instead — a guest NOTIFY on queue 0 just
 * means "I posted more RX buffers", which requires no immediate action here
 * (the next inbound frame will find them via vq_pop_avail). */
static void vnet_kick(struct vnet_dev *d, uint32_t qidx)
{
	if (qidx == VNET_QUEUE_TX) {
		/* D5(e), DIAGNOSTIC ONLY (see vblk_kick()'s identical rationale in
		 * vblk_emmc.c — not enforced). vnet_emac_rx_frame() already
		 * hard-gates on DRIVER_OK for the RX direction — this counts the
		 * TX-side equivalent instead of also hard-gating it. */
		if (!(d->status & VNET_S_DRIVER_OK))
			vnet_bc(18, ++g_kick_not_ready);

		/* Held across the whole drain: a reset/QueueReady write from
		 * another vCPU waits until this ring walk is finished. */
		uint64_t t0 = vnet_now();
		vq_lock(VNET_QUEUE_TX);
		uint32_t done = 0;
		int irq = 0;
		if (d->vq[VNET_QUEUE_TX].ready) {
			done = vnet_tx_drain_notify(d);
			irq = done && vnet_tx_want_irq(d);
		}
		vq_unlock(VNET_QUEUE_TX);
		g_tx_time += vnet_now() - t0;
		vnet_bc(28, (uint32_t)g_tx_time);
		vnet_bc(29, ++g_tx_kicks);
		if (irq)
			vnet_tx_irq(d);
	}
	if (qidx == VNET_QUEUE_CTRL)
		vnet_ctrl_kick(d);
	/* qidx == VNET_QUEUE_RX: nothing to do here — see comment above. */
}

/* ------------------------------------------------------------------ *
 * RX: called from emac.c's receive path (CPU1 — see the cross-core comment
 * block at the top of this file) for any accepted inbound frame whose
 * ethertype is not already claimed by the console (0x88B5) or netcon
 * (0x88B6).
 * ------------------------------------------------------------------ */
static void vnet_rx_one(const uint8_t *frame, uint16_t len)
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

	/* Header: all-zero (no offloads negotiated) except num_buffers. */
	struct vnet_hdr hdr;
	hdr.flags = 0; hdr.gso_type = 0; hdr.hdr_len = 0;
	hdr.gso_size = 0; hdr.csum_start = 0; hdr.csum_offset = 0;
	hdr.num_buffers = 1;          /* spec: 1 without MRG_RXBUF (was stack garbage) */

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

	/* A frame that does not fit is dropped, not cut short: a truncated
	 * frame is a different, corrupt frame (audit item 13). The buffer goes
	 * back with length 0, which if_vtnet counts as an input error. */
	if (vnet_chain_room(chain, n) < VNET_HDR_LEN + (uint32_t)len) {
		vnet_bc(30, ++g_rx_too_big);
		vq_push_used(vq, head, 0);
		vq_unlock(VNET_QUEUE_RX);
		g_rx_irq_pending = 1u;
		return;
	}

	uint32_t written = vnet_scatter2(chain, n, (const uint8_t *)&hdr,
	                                 VNET_HDR_LEN, frame, (uint32_t)len);
	vq_push_used(vq, head, written);
	vq_unlock(VNET_QUEUE_RX);
	vnet_bc(4, ++g_rx_queued);
	g_rx_irq_pending = 1u;        /* one interrupt per drain: vnet_emac_rx_flush() */
}

void vnet_emac_rx_frame(const uint8_t *frame, uint16_t len)
{
	uint64_t t0 = vnet_now();
	vnet_rx_one(frame, len);
	g_rx_time += vnet_now() - t0;
	vnet_bc(27, (uint32_t)g_rx_time);
}

/* End of one EMAC RX drain (emac.c, CPU1): if anything was queued, set the
 * VRING cause and raise the guest interrupt once for the whole batch. */
void vnet_emac_rx_flush(void)
{
	struct vnet_vq *vq = &g_net.vq[VNET_QUEUE_RX];
	int want = 1;

	if (!g_rx_irq_pending)
		return;
	g_rx_irq_pending = 0u;
	/* Under the RX lock: the rings must not be reset under us. A reset in
	 * progress just gets the interrupt (harmless). */
	if (vq_trylock(VNET_QUEUE_RX)) {
		if (vq->ready && (g_net.status & VNET_S_DRIVER_OK)) {
			want = vq_want_irq(&g_net, vq);
			/* RX never needs the driver's "buffers added" kick: frames
			 * pull buffers off the ring as they arrive. Each such kick
			 * was a guest trap for nothing. */
			vq_set_notify(&g_net, vq, 0);
		}
		vq_unlock(VNET_QUEUE_RX);
	}
	if (!want) {
		vnet_bc(34, ++g_irq_suppressed);
		return;
	}
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

	vnet_bc_publish();             /* once per emac_poll(): counters -> window */
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
		/* Word 1, bit 0 = VIRTIO_F_VERSION_1. Word 0: VIRTIO_NET_F_MAC (the
		 * guest's MAC is emac_guest_mac(), see emac.h), _STATUS (link) and
		 * _MTU (1500). No CSUM/offload/MRG_RXBUF bits, matching
		 * vblk_emmc.c's minimal-feature-set precedent. */
		if (d->dev_feat_sel == VNET_FEATWORD_HI)
			return VNET_F_VERSION_1_BIT;
		if (d->dev_feat_sel == VNET_FEATWORD_LO)
			return VNET_F_MAC_BIT | VNET_F_STATUS_BIT | VNET_F_MTU_BIT |
			       VNET_F_EVENT_IDX_BIT | VNET_F_CTRL_VQ_BIT |
			       VNET_F_CTRL_RX_BIT | VNET_F_CTRL_MAC_ADDR_BIT;
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
		else if (idx == VNET_CFG_MTU_OFF)
			b = (uint8_t)VNET_MTU;
		else if (idx == VNET_CFG_MTU_OFF + 1u)
			b = (uint8_t)(VNET_MTU >> 8);
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
				vq->used_idx = 0;
				vq->signalled_used = 0;
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
			/* Driver reset, with ALL queue locks held (vnet_reg_write):
			 * any RX scatter or TX drain in flight has finished, and none
			 * can start until the queues below read not-ready. Status
			 * goes to 0 first so RX's DRIVER_OK re-check fails too. */
			d->status = 0u;
			__asm__ volatile("dmb ish" ::: "memory");
			for (uint32_t q = 0; q < VNET_NUM_QUEUES; q++) {
				struct vnet_vq *vq = &d->vq[q];
				vq->ready = 0; vq->num = 0; vq->last_avail = 0;
				vq->used_idx = 0; vq->signalled_used = 0;
				vq->desc = vq->avail = vq->used = 0;
			}
			d->driver_features = 0;   /* renegotiated after a reset */
			vnet_rxf_reset();         /* back to promiscuous */
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
	for (uint32_t q = 0; q < VNET_NUM_QUEUES; q++)
		g_vq_lock[q][0] = 0u;
	vnet_rxf_reset();
	/* Start from the EMAC's current state; vnet_emac_link_update() (every
	 * emac_poll) publishes each change after this. */
	g_net.link_up = emac_link_up() ? 1u : 0u;

	/* Every slot, not just the ready bits: the window is not cleared by
	 * anything else, and a counter that has never fired must read 0, not
	 * whatever the DRAM held. */
	for (uint32_t i = 0; i < VNET_BC_SLOTS; i++)
		g_bcv[i] = 0;
	vnet_bc(25, g_net.link_up);
	vnet_bc_publish();
}
