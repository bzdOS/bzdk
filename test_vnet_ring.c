/* test_vnet_ring.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the virtqueue + Ethernet-framing logic in vnet_emac.c.
 *
 * WHY THIS FILE EXISTS: the VNET_HDR_LEN=10-vs-12 bug (fixed in
 * vnet_emac.h 2026-07-23) took a full board reload + tcpdump session on
 * real hardware to find, because nothing in the hosted test suite
 * exercised the TX ethertype-extraction / payload-slicing math at all.
 * This file is exactly the "pure logic, no real I/O" extraction
 * test_vblk_ring.c already established the convention for — it would
 * have caught the header-length bug in under a second, offline, with no
 * board involved. See test_vblk_ring.c's own header for the full
 * rationale on why this is a hand-transcribed mirror rather than
 * `#include "vnet_emac.c"` with stubs: vnet_emac.c threads raw ARMv8
 * inline asm through gmem_cmo() ("dc civac"/"dsb sy"), vnet_inject_irq()
 * (GICD store bracketed by "dsb sy") and vnet_mmio_fault() ("mrs hpfar_el2")
 * — none of which plain x86_64 gcc can assemble, and none of which affect
 * the ring-parsing/framing math under test here (cache maintenance changes
 * cache state, never the bytes read/written; IRQ injection is a pure side
 * effect).
 *
 * Mirrored, verbatim in control flow, from vnet_emac.c (line numbers as of
 * the 2026-07-23 fix commit):
 *   - vq_read_desc/vq_pop_avail/vq_push_used/vq_collect_chain  (~224-296)
 *   - vnet_gather/vnet_scatter                                 (~303-342)
 *   - vnet_tx_one's ethertype-extract + debug-block + payload-slice
 *     logic                                                     (~367-412)
 *   - vnet_emac_rx_frame's header-prepend + scatter logic       (~446-497)
 * Cache-maintenance asm and breadcrumb writes (vnet_bc()) are omitted —
 * side channels, not logic, per the same convention test_vblk_ring.c uses.
 *
 * Build: gcc -o test_vnet_ring test_vnet_ring.c && ./test_vnet_ring
 * (wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Mock guest DRAM — a gpa is a plain byte offset into this flat array,
 * exactly like test_vblk_ring.c's g_guest_mem (vnet_emac.c's gmem_read/
 * write treat a guest PA as a plain EL2 pointer under the stage-2 identity
 * map, same contract).
 * ------------------------------------------------------------------ */
#define GUEST_MEM_SIZE (1u << 20)
static uint8_t g_guest_mem[GUEST_MEM_SIZE];

/* ------------------------------------------------------------------ *
 * Mirrored types — verbatim field layout from vnet_emac.h.
 * ------------------------------------------------------------------ */

/* Mirrors vnet_emac.h:226-231 (struct vnet_vring_desc, the 16-byte wire
 * format written by the guest driver at desc_pa + 16*index). */
struct vnet_vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/* Mirrors vnet_emac.h:233-238 (struct vnet_desc, host-endian decoded form).
 * Identical layout to vnet_vring_desc on this (little-endian) host. */
struct vnet_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/* Mirrors vnet_emac.h:243-250 (struct vnet_vq). */
struct vnet_vq {
	uint32_t num;
	uint32_t ready;
	uint64_t desc;
	uint64_t avail;
	uint64_t used;
	uint16_t last_avail;
};

/* Mirrors vnet_emac.h's vnet_hdr (post-fix, 12 bytes: the modern/VERSION_1
 * form WITH num_buffers — this is the exact struct whose size mismatch
 * against the old VNET_HDR_LEN=10 caused the bug). */
struct vnet_hdr {
	uint8_t  flags;
	uint8_t  gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
	uint16_t num_buffers;
};

#define VNET_HDR_LEN               12u
#define VNET_ETH_HDR_LEN           14u
#define VNET_ETHERTYPE_DEBUG       0x88B5u
#define VNET_STAGE_BUF_SIZE        2048u
#define VNET_VRING_DESC_F_NEXT     0x1u
#define VNET_VRING_DESC_F_INDIRECT 0x4u
#define VNET_MAX_CHAIN             32u

/* ------------------------------------------------------------------ *
 * Mirrored virtqueue engine — verbatim control flow from vnet_emac.c,
 * cache-maintenance asm stripped (see file header).
 * ------------------------------------------------------------------ */
static void gmem_read(uint64_t gpa, void *dst, uint32_t len)
{
	memcpy(dst, g_guest_mem + gpa, len);
}
static void gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	memcpy(g_guest_mem + gpa, src, len);
}
static inline uint16_t gmem_ld16(uint64_t gpa)
{
	uint16_t v; gmem_read(gpa, &v, 2); return v;
}
static inline void gmem_st16(uint64_t gpa, uint16_t v)
{
	gmem_write(gpa, &v, 2);
}

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
		return 0;
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
	uint64_t e = vq->used + 4u + (uint64_t)slot * 8u;
	uint32_t id = head;
	uint16_t new_idx = (uint16_t)(used_idx + 1u);
	gmem_write(e + 0u, &id, 4u);
	gmem_write(e + 4u, &used_len, 4u);
	gmem_st16(vq->used + 2u, new_idx);
	return new_idx;
}

static uint32_t vq_collect_chain(struct vnet_vq *vq, uint16_t head,
                                  struct vnet_desc *chain, uint32_t max)
{
	uint32_t n = 0;
	uint16_t idx = head;
	for (;;) {
		if (n >= max)
			return 0;
		vq_read_desc(vq, idx, &chain[n]);
		if (chain[n].flags & VNET_VRING_DESC_F_INDIRECT)
			return 0;
		uint16_t flags = chain[n].flags;
		uint16_t next  = chain[n].next;
		n++;
		if (!(flags & VNET_VRING_DESC_F_NEXT))
			break;
		idx = next;
	}
	return n;
}

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
	return written;
}

/* Mirrors vnet_tx_one()'s ethertype-extract + debug-block + payload-slice
 * logic (vnet_emac.c ~367-412) — the EXACT code path that had the
 * VNET_HDR_LEN bug. Returns 1 if emac_send_frame() "would have been called"
 * (out params filled), 0 if blocked/malformed. No real emac_send_frame()
 * call — this test only checks the math that decides what WOULD be sent. */
static int vnet_tx_extract(const uint8_t *stage, uint32_t total,
                            uint16_t *out_ethertype,
                            const uint8_t **out_payload, uint32_t *out_plen)
{
	if (total < VNET_HDR_LEN + VNET_ETH_HDR_LEN)
		return 0;
	uint16_t ethertype = (uint16_t)((stage[VNET_HDR_LEN + 12] << 8) |
	                                  stage[VNET_HDR_LEN + 13]);
	if (ethertype == VNET_ETHERTYPE_DEBUG)
		return 0;   /* hard-blocked, mirrors the real "never send" path */
	*out_ethertype = ethertype;
	*out_payload   = stage + VNET_HDR_LEN + VNET_ETH_HDR_LEN;
	*out_plen      = total - VNET_HDR_LEN - VNET_ETH_HDR_LEN;
	return 1;
}

/* Mirrors vnet_emac_rx_frame()'s header-prepend logic (vnet_emac.c
 * ~446-497): build the all-zero legacy+num_buffers header, then the raw
 * wire frame, into one staging buffer — this is what gets scattered to the
 * guest's RX descriptor chain. */
static uint32_t vnet_rx_stage(const uint8_t *frame, uint16_t len, uint8_t *stage)
{
	struct vnet_hdr hdr;
	memset(&hdr, 0, sizeof(hdr));
	memcpy(stage, &hdr, VNET_HDR_LEN);
	memcpy(stage + VNET_HDR_LEN, frame, len);
	return VNET_HDR_LEN + (uint32_t)len;
}

/* ------------------------------------------------------------------ *
 * Test helpers
 * ------------------------------------------------------------------ */
static void setup_vq(struct vnet_vq *vq, uint64_t base, uint32_t num)
{
	memset(vq, 0, sizeof(*vq));
	vq->num   = num;
	vq->ready = 1;
	vq->desc  = base;
	vq->avail = base + 16u * num;
	vq->used  = vq->avail + 4u + 2u * num;
	gmem_st16(vq->avail + 0u, 0);   /* flags */
	gmem_st16(vq->avail + 2u, 0);   /* idx   */
	gmem_st16(vq->used  + 0u, 0);
	gmem_st16(vq->used  + 2u, 0);
}

static uint16_t post_avail(struct vnet_vq *vq, uint16_t desc_head)
{
	uint16_t idx = gmem_ld16(vq->avail + 2u);
	uint16_t slot = (uint16_t)(idx % vq->num);
	gmem_st16(vq->avail + 4u + (uint64_t)slot * 2u, desc_head);
	gmem_st16(vq->avail + 2u, (uint16_t)(idx + 1u));
	return idx;
}

static void write_desc(struct vnet_vq *vq, uint16_t idx, uint64_t addr,
                        uint32_t len, uint16_t flags, uint16_t next)
{
	struct vnet_vring_desc d = { addr, len, flags, next };
	gmem_write(vq->desc + (uint64_t)idx * sizeof(d), &d, sizeof(d));
}

#define CHECK(cond, name) do { \
	if (cond) { printf("[ OK  ] %s\n", name); } \
	else { printf("[FAIL ] %s\n", name); g_failed = 1; } \
} while (0)

static int g_failed = 0;

/* ------------------------------------------------------------------ *
 * Test 1: sizeof(struct vnet_hdr) == VNET_HDR_LEN.
 *
 * This assertion alone would have caught the exact bug fixed 2026-07-23:
 * the OLD code defined VNET_HDR_LEN=10 while the (correct, modern-device)
 * struct vnet_hdr already had 7 fields totaling 12 bytes -- a silent
 * mismatch between a manually-counted constant and the real struct size,
 * with no compile-time or run-time check tying them together anywhere in
 * the tree until this test.
 * ------------------------------------------------------------------ */
static void test_hdr_size_matches_constant(void)
{
	CHECK(sizeof(struct vnet_hdr) == VNET_HDR_LEN,
	      "sizeof(struct vnet_hdr) == VNET_HDR_LEN (12)");
}

/* ------------------------------------------------------------------ *
 * Test 2: TX ethertype extraction on a well-formed ARP frame.
 *
 * This is the DIRECT regression test for the bug. CRITICAL DESIGN POINT:
 * the synthetic frame is placed at REAL_GUEST_HDR_OFFSET (a FIXED 12,
 * hardcoded independently of VNET_HDR_LEN) -- this is what the real
 * FreeBSD if_vtnet(4) driver ACTUALLY writes on the wire (a modern/
 * VERSION_1 device's virtio_net_hdr is unconditionally 12 bytes, see
 * vnet_emac.h's fix comment), regardless of what OUR code's VNET_HDR_LEN
 * constant currently believes. If this test built its input using
 * VNET_HDR_LEN itself (the constant under test), it would be tautological
 * and could never fail even with the old, wrong value -- this exact
 * mistake was caught by hand-validating this test against a reverted
 * VNET_HDR_LEN=10 build, which is why the offset here is a separate,
 * spec-derived literal. With that fixed offset, vnet_tx_extract() (which
 * reads at VNET_HDR_LEN) only computes the right answer when
 * VNET_HDR_LEN == REAL_GUEST_HDR_OFFSET == 12.
 * ------------------------------------------------------------------ */
#define REAL_GUEST_HDR_OFFSET 12u   /* spec-mandated, NOT derived from VNET_HDR_LEN */

static void test_tx_ethertype_extract_arp(void)
{
	uint8_t stage[VNET_STAGE_BUF_SIZE];
	memset(stage, 0, sizeof(stage));

	/* [0, REAL_GUEST_HDR_OFFSET) : virtio-net header, all zero. */
	/* [+0, +6) : dst MAC (broadcast). */
	memset(stage + REAL_GUEST_HDR_OFFSET, 0xff, 6);
	/* [+6, +12) : src MAC (arbitrary guest MAC). */
	uint8_t src_mac[6] = { 0xb2, 0xad, 0xa3, 0x7d, 0x4d, 0x8c };
	memcpy(stage + REAL_GUEST_HDR_OFFSET + 6, src_mac, 6);
	/* [+12, +14) : ethertype = 0x0806 (ARP). */
	stage[REAL_GUEST_HDR_OFFSET + 12] = 0x08;
	stage[REAL_GUEST_HDR_OFFSET + 13] = 0x06;
	/* [+14, +18) : 4-byte fake ARP payload marker. */
	uint8_t payload_marker[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
	memcpy(stage + REAL_GUEST_HDR_OFFSET + VNET_ETH_HDR_LEN, payload_marker, 4);

	uint32_t total = REAL_GUEST_HDR_OFFSET + VNET_ETH_HDR_LEN + 4;
	uint16_t ethertype = 0;
	const uint8_t *payload = NULL;
	uint32_t plen = 0;
	int ok = vnet_tx_extract(stage, total, &ethertype, &payload, &plen);

	CHECK(ok == 1, "tx_extract: ARP frame accepted (not blocked)");
	CHECK(ethertype == 0x0806u, "tx_extract: ethertype == 0x0806 (ARP), not garbage");
	CHECK(plen == 4, "tx_extract: payload length excludes both headers");
	CHECK(payload != NULL && memcmp(payload, payload_marker, 4) == 0,
	      "tx_extract: payload bytes match (not shifted by 2)");

	/* The exact failure mode the old VNET_HDR_LEN=10 bug produced: reading
	 * ethertype 2 bytes too early lands on the LAST 2 BYTES OF THE SOURCE
	 * MAC (0x4d, 0x8c in this test's src_mac) instead of the real
	 * ethertype -- confirm that is NOT what this (fixed) code computes. */
	uint16_t buggy_old_value = (uint16_t)((src_mac[4] << 8) | src_mac[5]);
	CHECK(ethertype != buggy_old_value,
	      "tx_extract: does NOT reproduce the old off-by-2 garbage value");
}

/* ------------------------------------------------------------------ *
 * Test 3: TX debug-ethertype hard block (0x88B5 must never be "sent").
 * ------------------------------------------------------------------ */
static void test_tx_debug_ethertype_blocked(void)
{
	uint8_t stage[VNET_STAGE_BUF_SIZE];
	memset(stage, 0, sizeof(stage));
	stage[VNET_HDR_LEN + 12] = 0x88;
	stage[VNET_HDR_LEN + 13] = 0xB5;

	uint32_t total = VNET_HDR_LEN + VNET_ETH_HDR_LEN;
	uint16_t ethertype = 0;
	const uint8_t *payload = NULL;
	uint32_t plen = 0;
	int ok = vnet_tx_extract(stage, total, &ethertype, &payload, &plen);

	CHECK(ok == 0, "tx_extract: ethertype 0x88B5 (debug protocol) is blocked");
}

/* ------------------------------------------------------------------ *
 * Test 4: TX malformed/too-short frame is rejected, not misread.
 * ------------------------------------------------------------------ */
static void test_tx_too_short_rejected(void)
{
	uint8_t stage[VNET_STAGE_BUF_SIZE];
	memset(stage, 0, sizeof(stage));
	uint32_t total = VNET_HDR_LEN + VNET_ETH_HDR_LEN - 1; /* one byte short */
	uint16_t ethertype = 0;
	const uint8_t *payload = NULL;
	uint32_t plen = 0;
	int ok = vnet_tx_extract(stage, total, &ethertype, &payload, &plen);
	CHECK(ok == 0, "tx_extract: too-short frame rejected cleanly");
}

/* ------------------------------------------------------------------ *
 * Test 5: full TX virtqueue walk (descriptor chain -> gather -> extract),
 * end to end, mirroring vnet_tx_one()'s real call sequence.
 * ------------------------------------------------------------------ */
static void test_tx_full_chain_walk(void)
{
	struct vnet_vq vq;
	setup_vq(&vq, 0x1000, 8);

	/* Two-descriptor chain: header-only descriptor, then frame descriptor
	 * (exercises the "guest split header vs data" case vnet_gather's
	 * comment explicitly calls out). */
	uint64_t hdr_buf = 0x10000, frame_buf = 0x11000;
	uint8_t hdrbytes[VNET_HDR_LEN];
	memset(hdrbytes, 0, sizeof(hdrbytes));
	gmem_write(hdr_buf, hdrbytes, VNET_HDR_LEN);

	uint8_t framebytes[VNET_ETH_HDR_LEN + 4];
	memset(framebytes, 0, sizeof(framebytes));
	memset(framebytes, 0xff, 6);             /* dst */
	framebytes[6] = 0xb2;                    /* src (arbitrary) */
	framebytes[12] = 0x08; framebytes[13] = 0x00; /* ethertype = IPv4 */
	framebytes[14] = 0x11; framebytes[15] = 0x22;
	framebytes[16] = 0x33; framebytes[17] = 0x44;
	gmem_write(frame_buf, framebytes, sizeof(framebytes));

	write_desc(&vq, 0, hdr_buf, VNET_HDR_LEN, VNET_VRING_DESC_F_NEXT, 1);
	write_desc(&vq, 1, frame_buf, sizeof(framebytes), 0, 0);
	uint16_t avail_idx = post_avail(&vq, 0);
	(void)avail_idx;

	uint16_t head;
	int popped = vq_pop_avail(&vq, &head);
	CHECK(popped == 1, "full_chain: vq_pop_avail finds the posted chain");

	struct vnet_desc chain[VNET_MAX_CHAIN];
	uint32_t n = vq_collect_chain(&vq, head, chain, VNET_MAX_CHAIN);
	CHECK(n == 2, "full_chain: two-descriptor chain collected");

	uint8_t stage[VNET_STAGE_BUF_SIZE];
	uint32_t total = vnet_gather(chain, n, stage, sizeof(stage));
	CHECK(total == VNET_HDR_LEN + VNET_ETH_HDR_LEN + 4,
	      "full_chain: gather concatenates both descriptors correctly");

	uint16_t ethertype = 0;
	const uint8_t *payload = NULL;
	uint32_t plen = 0;
	int ok = vnet_tx_extract(stage, total, &ethertype, &payload, &plen);
	CHECK(ok == 1, "full_chain: extraction succeeds on the assembled frame");
	CHECK(ethertype == 0x0800u, "full_chain: ethertype == 0x0800 (IPv4)");
	CHECK(plen == 4 && payload[0] == 0x11 && payload[3] == 0x44,
	      "full_chain: payload correctly sliced after split header+data");

	vq_push_used(&vq, head, 0);
	uint16_t used_idx = gmem_ld16(vq.used + 2u);
	CHECK(used_idx == 1, "full_chain: used ring advanced after completion");
}

/* ------------------------------------------------------------------ *
 * Test 6: RX header-prepend + scatter, mirroring vnet_emac_rx_frame().
 * ------------------------------------------------------------------ */
static void test_rx_header_prepend_and_scatter(void)
{
	uint8_t frame[VNET_ETH_HDR_LEN + 4];
	memset(frame, 0, sizeof(frame));
	memset(frame, 0xaa, 6);                   /* dst */
	frame[12] = 0x08; frame[13] = 0x06;       /* ethertype = ARP */
	frame[14] = 1; frame[15] = 2; frame[16] = 3; frame[17] = 4;

	uint8_t stage[VNET_STAGE_BUF_SIZE];
	uint32_t staged = vnet_rx_stage(frame, sizeof(frame), stage);
	CHECK(staged == VNET_HDR_LEN + sizeof(frame),
	      "rx_stage: staged length is header + frame, no offset error");

	/* Header region is all-zero (no offloads negotiated). */
	int hdr_all_zero = 1;
	for (uint32_t i = 0; i < VNET_HDR_LEN; i++)
		if (stage[i] != 0) hdr_all_zero = 0;
	CHECK(hdr_all_zero, "rx_stage: prepended header is all-zero");

	/* Frame bytes start EXACTLY at VNET_HDR_LEN, not shifted. */
	CHECK(memcmp(stage + VNET_HDR_LEN, frame, sizeof(frame)) == 0,
	      "rx_stage: frame bytes land exactly at offset VNET_HDR_LEN");

	/* Now scatter into a guest RX descriptor chain and confirm round-trip. */
	struct vnet_vq vq;
	setup_vq(&vq, 0x2000, 8);
	uint64_t rx_buf = 0x20000;
	write_desc(&vq, 0, rx_buf, VNET_STAGE_BUF_SIZE, 0, 0);
	struct vnet_desc chain[1];
	vq_read_desc(&vq, 0, &chain[0]);

	uint32_t written = vnet_scatter(chain, 1, stage, staged);
	CHECK(written == staged, "rx_scatter: full staged buffer written");

	uint8_t readback[VNET_HDR_LEN + sizeof(frame)];
	gmem_read(rx_buf, readback, sizeof(readback));
	CHECK(memcmp(readback, stage, sizeof(readback)) == 0,
	      "rx_scatter: guest buffer matches staged header+frame exactly");
}

int main(void)
{
	test_hdr_size_matches_constant();
	test_tx_ethertype_extract_arp();
	test_tx_debug_ethertype_blocked();
	test_tx_too_short_rejected();
	test_tx_full_chain_walk();
	test_rx_header_prepend_and_scatter();

	if (g_failed) {
		printf("---- vnet ring tests: FAILED ----\n");
		return 1;
	}
	printf("---- vnet ring tests: 20/20 passed ----\n");
	return 0;
}
