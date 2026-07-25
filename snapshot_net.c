/* SPDX-License-Identifier: BSD-2-Clause */

/* snapshot_net.c — stream the guest snapshot store (snapshot.h/snapshot.c)
 * over GbE to/from the host. See snapshot_net.h for the full design rationale
 * (transport choice, zero-skip, the netcon-carried manifest/missing-list
 * reliability scheme) — this file is the implementation.
 *
 * Freestanding: <stdint.h> only, -mgeneral-regs-only, EL2, flat U-Boot map on
 * (same assumptions as snapshot.c/netcon.c/coredump.c).
 */
#include <stdint.h>
#include "snapshot_net.h"
#include "snapshot.h"
#include "netcon.h"
#include "emac.h"
#include "wdt.h"

/* netcon.c / snapshot.c provide these; declared here rather than pulling in
 * more headers than needed (same minimal-extern convention coredump.c uses
 * for emac_send_frame/emac_poll). */
extern int  netcon_send(const uint8_t *data, uint32_t len);
extern int  netcon_recv(uint32_t addr, uint32_t len, uint32_t timeout_polls);
extern int  snapshot_save(const struct el2_frame *frame);
extern int  snapshot_restore(struct el2_frame *frame);

/* ------------------------------------------------------------------ *
 * Bounded, uncalibrated poll budgets — same idiom as netcon.c's
 * NETCON_ACK_WAIT_POLLS: not wall-clock-calibrated (freestanding, no timer
 * dependency here), just "long enough" for the respective round trip many
 * times over while staying bounded so a dead link aborts cleanly.
 * ------------------------------------------------------------------ */
#define SNAPNET_BULK_IDLE_POLLS   500000u  /* fire-and-forget bulk pass: how
                                             * long to wait, with NO new chunk
                                             * accepted, before deciding the
                                             * sender's pass has ended */
#define SNAPNET_ROUND_TIMEOUT_POLLS 2000000u /* netcon_recv/-send idle budget
                                             * for the manifest / missing-list
                                             * control messages (see netcon.h:
                                             * this is an IDLE budget, not a
                                             * total-time cap) */

/* Sentinel marking an unused slot in a fixed-size, zero-padded missing-list
 * message (see file header "fixed-size control messages" note below). */
#define SNAPNET_SENTINEL   0xFFFFFFFFu

/* ------------------------------------------------------------------ *
 * FIXED-SIZE CONTROL MESSAGES (why): netcon_recv(addr, len, ...) on the
 * BOARD side requires the caller to pre-declare the exact expected transfer
 * length — netcon_rx_frame() drops any inbound DATA frame whose total_len
 * doesn't match (see netcon.c). The board can't know in advance how many
 * missing-chunk indices the host will report in a given round, so the
 * missing-list is always sent as an EXACT, fixed SNAPNET_MISSING_CAP*4-byte
 * message, unused trailing slots padded with SNAPNET_SENTINEL. The manifest
 * bitmap needs no such padding — it's already a fixed SNAPNET_MANIFEST_BYTES
 * on both ends. (The reverse direction — board calling netcon_send() with a
 * host-side recv_blob() on the other end — has no such constraint: recv_blob()
 * accepts whatever total_len the sender declares, so a board-originated
 * missing-list is sent at its own natural/dynamic length; see snapshot_net.py.)
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Static per-role scratch. Roughly 3*MANIFEST_BYTES (~94 KiB each, one image
 * worth of chunks) + one MISSING_CAP*4 (32 KiB) control buffer — comfortably
 * inside the 8 MiB hypervisor image budget (link.ld RAM region), and SEND/
 * RECV never run concurrently (single core, one dbgmon command at a time) so
 * there is no need to size these per-role separately.
 * ------------------------------------------------------------------ */
static uint8_t s_manifest[SNAPNET_MANIFEST_BYTES];  /* SEND: which chunks are
                                                      * non-zero (built while
                                                      * scanning, sent to host) */
static uint8_t s_expected[SNAPNET_MANIFEST_BYTES];  /* RECV: host's manifest,
                                                      * received over netcon */
static uint8_t s_received[SNAPNET_MANIFEST_BYTES];  /* RECV: which chunks this
                                                      * board has actually
                                                      * gotten (RX-hook-set) */
static uint8_t s_ctrl[SNAPNET_MISSING_CAP * 4u];    /* shared scratch: the
                                                      * fixed-size missing-list
                                                      * message, either role */

/* RX-hook state (mirrors netcon.c's s_recv_active pattern): frames on
 * SNAPNET_ETHERTYPE are only acted on while a snapshot_net_recv() bulk
 * collection window is open; otherwise dropped (host resending stray tail
 * frames after we've moved on is harmless — just ignored). */
static volatile int      s_rx_active;
static volatile uint32_t s_rx_count;   /* number of NEWLY-set bits in
                                         * s_received, used as a cheap
                                         * "did anything change" signal for
                                         * the idle-detection loops below —
                                         * avoids re-popcounting a ~94 KiB
                                         * bitmap on every single poll. */

/* ------------------------------------------------------------------ *
 * Little-endian byte accessors — same rationale as netcon.c: the 16-byte
 * snapnet header sits right after the 14-byte Ethernet header in the RX/TX
 * DMA buffer (2-byte aligned, not 4-byte), so byte-wise access sidesteps any
 * unaligned-load concern entirely.
 * ------------------------------------------------------------------ */
static inline uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff); }

static inline void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* ------------------------------------------------------------------ *
 * Bitmap helpers (1 bit per chunk index).
 * ------------------------------------------------------------------ */
static inline int bit_test(const uint8_t *bm, uint32_t idx)
{ return (bm[idx >> 3] >> (idx & 7u)) & 1u; }

static inline void bit_set(uint8_t *bm, uint32_t idx)
{ bm[idx >> 3] |= (uint8_t)(1u << (idx & 7u)); }

/* ------------------------------------------------------------------ *
 * CRC32 (IEEE 802.3 / zlib polynomial 0xEDB88320) — table-based, duplicated
 * locally rather than exported from netcon.c/snapshot.c, matching this
 * tree's established "keep each module link-independent" convention (see
 * netcon.c's own nc_cache_clean comment). Mathematically identical to
 * snapshot.c's bit-at-a-time crc32_update_word (same poly/init/final-xor
 * over the same byte stream — see snapshot_net.h "whole-DRAM CRC32 recheck"),
 * just faster, so the final recheck against the store header's crc32 field
 * agrees bit-for-bit.
 * ------------------------------------------------------------------ */
static uint32_t crc_table[256];
static int      crc_table_ready;

static void crc32_init(void)
{
	uint32_t i;
	for (i = 0; i < 256u; i++) {
		uint32_t c = i;
		int k;
		for (k = 0; k < 8; k++)
			c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
	crc_table_ready = 1;
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFu, i;
	if (!crc_table_ready)
		crc32_init();
	for (i = 0; i < len; i++)
		crc = crc_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

/* Same as crc32_calc but over a raw physical-memory region rather than a
 * local buffer, petting the watchdog periodically — this is a whole-DRAM
 * (up to 1 GiB) pass, the same cost class as snapshot.c's dram_copy(). */
static uint32_t crc32_region(uint64_t pa, uint64_t len)
{
	const uint8_t *p = (const uint8_t *)(uintptr_t)pa;
	uint32_t crc = 0xFFFFFFFFu;
	uint64_t i;
	if (!crc_table_ready)
		crc32_init();
	for (i = 0; i < len; i++) {
		crc = crc_table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
		if ((i & 0xFFFFFu) == 0xFFFFFu)
			wdt_pet();
	}
	return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ *
 * Cache maintenance for bytes we just wrote into the DRAM store via the RX
 * hook — same coherent-store convention as netcon.c's nc_cache_clean
 * (duplicated here for the same link-independence reason).
 * ------------------------------------------------------------------ */
static inline void store_cache_clean(uintptr_t addr, uint32_t size)
{
	uintptr_t p = addr & ~63UL, end = addr + size;
	for (; p < end; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------ *
 * Zero-skip check: is this (8-byte-aligned, since SNAP_STORE_BASE and
 * SNAPNET_CHUNK are both multiples of 8) chunk entirely zero?
 * ------------------------------------------------------------------ */
static int chunk_is_zero(const uint8_t *p, uint32_t n)
{
	const uint64_t *w = (const uint64_t *)(uintptr_t)p;
	uint32_t nwords = n / 8u, i;
	for (i = 0; i < nwords; i++)
		if (w[i] != 0)
			return 0;
	for (i = nwords * 8u; i < n; i++)
		if (p[i] != 0)
			return 0;
	return 1;
}

/* Bulk-zero the whole store (header + DRAM copy) before receiving, so any
 * chunk the sender legitimately never transmits (all-zero, zero-skipped)
 * reconstructs correctly at zero cost. Same wdt_pet()/dc-cvac discipline as
 * snapshot.c's dram_copy(). */
static void store_zero(void)
{
	volatile uint64_t *d = (volatile uint64_t *)(uintptr_t)SNAP_STORE_BASE;
	uint64_t n = SNAPNET_STORE_LEN / 8u, i;

	for (i = 0; i < n; i++) {
		d[i] = 0;
		if ((i & 7u) == 7u)
			__asm__ volatile("dc cvac, %0" :: "r"(&d[i]) : "memory");
		if ((i & 0xFFFFFu) == 0xFFFFFu)
			wdt_pet();
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static volatile struct snapshot_hdr *snapnet_hdr(void)
{
	return (volatile struct snapshot_hdr *)(uintptr_t)SNAP_STORE_BASE;
}

/* ------------------------------------------------------------------ *
 * Send one bulk DATA frame for chunk `seq` (fire-and-forget: bounded TX-ring
 * retry, same discipline as coredump.c's frame_flush(), but no ACK wait at
 * all — reliability is the netcon-carried manifest/missing-list handshake,
 * not per-frame ACKing).
 * ------------------------------------------------------------------ */
static void snapnet_send_one(uint32_t seq)
{
	uint8_t frame[SNAPNET_HDR_LEN + SNAPNET_CHUNK];
	uint64_t off = (uint64_t)seq * SNAPNET_CHUNK;
	uint32_t remain = (uint32_t)(SNAPNET_STORE_LEN - off);
	uint16_t len = (remain < SNAPNET_CHUNK) ? (uint16_t)remain : (uint16_t)SNAPNET_CHUNK;
	const uint8_t *src = (const uint8_t *)(uintptr_t)(SNAP_STORE_BASE + off);
	uint32_t crc;
	int i;

	for (i = 0; i < (int)len; i++)
		frame[SNAPNET_HDR_LEN + i] = src[i];
	crc = crc32_calc(frame + SNAPNET_HDR_LEN, len);

	wr32(frame + 0, SNAPNET_MAGIC);
	wr32(frame + 4, seq);
	wr16(frame + 8, len);
	wr16(frame + 10, 0);
	wr32(frame + 12, crc);

	for (i = 0; i < 64; i++) {
		if (emac_send_frame(SNAPNET_ETHERTYPE, frame, (uint16_t)(SNAPNET_HDR_LEN + len)))
			break;
		emac_poll();
	}
	emac_poll();   /* reap this frame's TX completion */
}

/* ------------------------------------------------------------------ *
 * RX hook — called from emac_poll() for every ethertype-0x88B8 frame. Only
 * meaningful while a snapshot_net_recv() bulk-collection window is open
 * (s_rx_active); otherwise dropped. Never blocks; does at most one bounds/
 * crc check + one memory write + one cache-clean.
 * ------------------------------------------------------------------ */
void snapshot_net_rx_frame(const uint8_t *payload, uint16_t len)
{
	uint32_t magic, seq, crc, i;
	uint16_t plen, expect_len;
	uint64_t off;
	uint32_t remain;
	const uint8_t *data;
	uint8_t *dst;

	if (len < SNAPNET_HDR_LEN)
		return;
	magic = rd32(payload + 0);
	if (magic != SNAPNET_MAGIC)
		return;
	if (!s_rx_active)
		return;

	seq  = rd32(payload + 4);
	plen = rd16(payload + 8);
	crc  = rd32(payload + 12);

	if ((uint32_t)plen > (uint32_t)len - SNAPNET_HDR_LEN)
		return;                          /* corrupt/truncated: drop */
	if (seq >= SNAPNET_TOTAL_CHUNKS)
		return;                          /* bogus chunk index: drop */

	off    = (uint64_t)seq * SNAPNET_CHUNK;
	remain = (uint32_t)(SNAPNET_STORE_LEN - off);
	expect_len = (remain < SNAPNET_CHUNK) ? (uint16_t)remain : (uint16_t)SNAPNET_CHUNK;
	if (plen != expect_len)
		return;                          /* wrong length for this index: drop */

	data = payload + SNAPNET_HDR_LEN;
	if (crc32_calc(data, plen) != crc)
		return;                          /* corrupt payload: drop, shows up as
	                                          * "missing" in the next manifest-
	                                          * diff round, same recovery path
	                                          * as an outright-lost frame */

	dst = (uint8_t *)(uintptr_t)(SNAP_STORE_BASE + off);
	for (i = 0; i < plen; i++)
		dst[i] = data[i];
	store_cache_clean((uintptr_t)dst, plen);

	if (!bit_test(s_received, seq)) {
		bit_set(s_received, seq);
		s_rx_count++;
	}
}

/* Spin, servicing emac_poll()+wdt_pet(), until s_rx_count stops changing for
 * SNAPNET_BULK_IDLE_POLLS consecutive iterations. Caller must have already
 * set s_rx_active=1 (and reset s_rx_count's baseline) before calling. */
static void snapnet_collect_until_idle(void)
{
	uint32_t idle = 0, last = s_rx_count;

	while (idle < SNAPNET_BULK_IDLE_POLLS) {
		emac_poll();
		wdt_pet();
		if (s_rx_count != last) {
			last = s_rx_count;
			idle = 0;
		} else {
			idle++;
		}
	}
}

/* ================================================================== *
 * PUBLIC API
 * ================================================================== */

int snapshot_net_send(const struct el2_frame *frame)
{
	uint32_t seq, i, round;

	if (snapshot_save(frame) != 0)
		return -1;

	for (i = 0; i < SNAPNET_MANIFEST_BYTES; i++)
		s_manifest[i] = 0;

	/* Single fire-and-forget pass: skip all-zero chunks entirely (never
	 * transmitted, bit stays 0 -- the receiver's own pre-zeroed destination
	 * reconstructs them at no wire cost), send everything else. */
	for (seq = 0; seq < SNAPNET_TOTAL_CHUNKS; seq++) {
		uint64_t off = (uint64_t)seq * SNAPNET_CHUNK;
		uint32_t remain = (uint32_t)(SNAPNET_STORE_LEN - off);
		uint16_t len = (remain < SNAPNET_CHUNK) ? (uint16_t)remain : (uint16_t)SNAPNET_CHUNK;
		const uint8_t *src = (const uint8_t *)(uintptr_t)(SNAP_STORE_BASE + off);

		if (chunk_is_zero(src, len)) {
			if ((seq & 0xFFFu) == 0xFFFu)
				wdt_pet();
			continue;
		}
		bit_set(s_manifest, seq);
		snapnet_send_one(seq);
		if ((seq & 0xFFFu) == 0xFFFu)
			wdt_pet();
	}

	/* Deliver the manifest reliably (small, ~94 KiB -- well inside netcon's
	 * existing 2 MiB cap, no netcon.h/.c change needed at all). */
	if (!netcon_send(s_manifest, SNAPNET_MANIFEST_BYTES))
		return -2;

	/* Bounded rounds servicing the host's missing-chunk requests (fixed-size
	 * SNAPNET_MISSING_CAP*4-byte messages -- see file header). */
	for (round = 0; round < SNAPNET_MAX_ROUNDS; round++) {
		int n = netcon_recv((uint32_t)(uintptr_t)s_ctrl,
		                    SNAPNET_MISSING_CAP * 4u,
		                    SNAPNET_ROUND_TIMEOUT_POLLS);
		uint32_t count, j;

		if (n < 4)
			break;   /* nothing meaningful within the timeout: host is done
			          * (either satisfied, or has given up requesting more) */

		count = (uint32_t)n / 4u;
		for (j = 0; j < count; j++) {
			uint32_t mseq = rd32(s_ctrl + j * 4u);
			if (mseq == SNAPNET_SENTINEL)
				break;               /* padding reached: no more real entries */
			if (mseq < SNAPNET_TOTAL_CHUNKS && bit_test(s_manifest, mseq))
				snapnet_send_one(mseq);
			if ((j & 0xFFu) == 0xFFu)
				wdt_pet();
		}
	}

	return 0;
}

int snapshot_net_recv(struct el2_frame *frame)
{
	uint32_t i, round, got;
	volatile struct snapshot_hdr *h;

	if (!frame)
		return -1;

	/* 1. Zero the whole store first (see store_zero() comment). */
	store_zero();

	/* 2. Collect the sender's fire-and-forget bulk pass. */
	for (i = 0; i < SNAPNET_MANIFEST_BYTES; i++)
		s_received[i] = 0;
	s_rx_count  = 0;
	s_rx_active = 1;
	snapnet_collect_until_idle();
	s_rx_active = 0;

	/* 3. Reliably receive the sender's manifest (fixed SNAPNET_MANIFEST_BYTES
	 *    -- no padding needed, both ends already agree on this exact size). */
	got = (uint32_t)netcon_recv((uint32_t)(uintptr_t)s_expected,
	                            SNAPNET_MANIFEST_BYTES,
	                            SNAPNET_ROUND_TIMEOUT_POLLS);
	if (got != SNAPNET_MANIFEST_BYTES)
		return -2;

	/* 4. Bounded rounds: diff expected (manifest) vs. received, request
	 *    exactly what's missing, wait for the resend. */
	for (round = 0; round < SNAPNET_MAX_ROUNDS; round++) {
		uint32_t seq, count = 0;
		int any_missing = 0;

		for (seq = 0; seq < SNAPNET_TOTAL_CHUNKS && count < SNAPNET_MISSING_CAP; seq++) {
			if (bit_test(s_expected, seq) && !bit_test(s_received, seq)) {
				wr32(s_ctrl + count * 4u, seq);
				count++;
				any_missing = 1;
			}
			if ((seq & 0xFFFFu) == 0xFFFFu)
				wdt_pet();
		}
		if (!any_missing)
			break;

		for (i = count; i < SNAPNET_MISSING_CAP; i++)
			wr32(s_ctrl + i * 4u, SNAPNET_SENTINEL);

		/* Board -> host direction: netcon_send()'s length is whatever we say
		 * it is (the host's recv_blob() accepts it dynamically), so this one
		 * does NOT need to be a fixed size -- sent at the fixed cap length
		 * anyway for symmetry/simplicity with the other direction. */
		if (!netcon_send(s_ctrl, SNAPNET_MISSING_CAP * 4u))
			break;   /* couldn't deliver the request at all: give up asking */

		s_rx_count  = 0;
		s_rx_active = 1;
		snapnet_collect_until_idle();
		s_rx_active = 0;
	}

	/* 5. Final completeness check against the manifest. Refuse to restore
	 *    from a known-incomplete image. */
	{
		uint32_t seq;
		for (seq = 0; seq < SNAPNET_TOTAL_CHUNKS; seq++)
			if (bit_test(s_expected, seq) && !bit_test(s_received, seq))
				return -3;
	}

	/* 6. Whole-DRAM CRC32 recheck against the store header's own crc32 field
	 *    (computed by the ORIGINAL snapshot_save() on the sending board) --
	 *    belt-and-suspenders against any reconstruction bug even though every
	 *    individual chunk's own CRC already passed. Refuse to restore on
	 *    mismatch. */
	h = snapnet_hdr();
	if (crc32_region(h->dram_store, h->dram_size) != h->crc32)
		return -4;

	/* 7. Commit into the live guest via the EXISTING, unmodified restore
	 *    path -- nothing in snapshot.c is touched by this file. */
	return snapshot_restore(frame);
}
