/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vblk_ring.c — hosted (x86_64, plain gcc, no cross-compiler) unit
 * tests for the virtqueue ring-parsing logic in vblk_emmc.c.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "vblk_emmc.c"` WITH STUBS
 * ============================================================================
 * vblk_emmc.c was read in full before writing this file. Its virtqueue engine
 * (gmem_read/gmem_write/gmem_cmo, vq_read_desc, vq_pop_avail, vq_push_used,
 * vblk_request()'s chain walk) is pure address/index arithmetic with NO real
 * I/O — exactly the "pure logic" the task asked to extract. But it is not
 * ISOLATED pure logic in the source: raw ARMv8 inline asm is threaded through
 * nearly every function in the file, not just the cache-maintenance ops:
 *   - gmem_cmo()          "dc civac", "dsb sy"           (line 122-123)
 *   - vblk_bc()           "dc civac, %0\n\tdsb sy"        (line 80)
 *   - vblk_emmc_trylock() "ldaxr"/"stlxr" exclusive loop  (lines 177-186)
 *   - vblk_emmc_unlock()  "dsb sy"/"sev"                  (lines 197-199)
 *   - vblk_inject_irq()   "dsb sy" bracketing a GICD store (lines 320-322)
 *   - vblk_mmio_fault()   "mrs %0, hpfar_el2"             (line 734)
 * `dc civac`/`ldaxr`/`stlxr`/`mrs hpfar_el2` are not gcc builtins or macros —
 * they are raw asm mnemonics the assembler rejects outright on an x86_64
 * target. Redefining them away would mean either (a) globally overriding
 * `__asm__` (which also silently deletes the exclusive-monitor spinlock and
 * the IPA-reconstruction read — i.e. neutering logic well beyond "cache
 * maintenance", the exact "too invasive/fragile" case the task called out),
 * or (b) wrapping every one of those asm blocks in its own overridable static
 * function first — which would mean editing vblk_emmc.c, explicitly
 * forbidden here (it is live, hardware-verified code).
 *
 * So: this file hand-transcribes ONLY the ring-parsing functions, verbatim in
 * control flow, with an exact vblk_emmc.c line-number citation on each one so
 * a future reader can diff them by hand whenever vblk_emmc.c changes. Every
 * mirrored function omits ONLY the cache-maintenance instructions (which
 * change cache state, never the bytes read/written, so they cannot affect the
 * ring-parsing math under test) and the breadcrumb/diagnostic stores
 * (vblk_bc()/vblk_diag() — pure side-channel telemetry, not logic).
 *
 * Build: gcc -o test_vblk_ring test_vblk_ring.c && ./test_vblk_ring
 * (also wired into `make test`, see Makefile).
 */
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * Mock guest DRAM. Real vblk_emmc.c treats a guest PA as a plain EL2
 * pointer (stage-2 identity map — see the file's top comment); here a
 * "gpa" is simply a byte offset into this flat array.
 * ------------------------------------------------------------------ */
#define GUEST_MEM_SIZE (1u << 20)   /* 1 MiB: plenty for test rings */
static uint8_t g_guest_mem[GUEST_MEM_SIZE];

/* ------------------------------------------------------------------ *
 * Mirrored types — verbatim field layout from vblk_emmc.h.
 * ------------------------------------------------------------------ */

/* Mirrors vblk_emmc.h:231-236 (struct vring_desc, the 16-byte wire format
 * written by the guest driver at desc_pa + 16*index). */
struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/* Mirrors vblk_emmc.h:241-246 (struct vblk_desc, the host-endian decoded
 * form vq_read_desc() produces). Identical layout to vring_desc on this
 * (little-endian) host, kept as a distinct type to mirror the source. */
struct vblk_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

/* Mirrors vblk_emmc.h:251-258 (struct vblk_vq), only the fields the
 * ring-parsing logic under test reads/writes. */
struct vblk_vq {
	uint32_t num;
	uint32_t ready;
	uint64_t desc;
	uint64_t avail;
	uint64_t used;
	uint16_t last_avail;
};

/* Mirrors vblk_emmc.h:167-169 descriptor flags and :164 VBLK_MAX_CHAIN. */
#define VRING_DESC_F_NEXT     0x1u
#define VRING_DESC_F_WRITE    0x2u
#define VRING_DESC_F_INDIRECT 0x4u
#define VBLK_MAX_CHAIN        32u
/* mirrors vblk_emmc.h:157 — InterruptStatus bit "used ring advanced" */
#define VBLK_INT_VRING        0x1u

/* ------------------------------------------------------------------ *
 * Guest-memory helpers — mirrors vblk_emmc.c:126-151, MINUS the gmem_cmo()
 * cache-maintenance call (vblk_emmc.c:130/141, "dc civac"+"dsb sy"). That
 * call changes only cache state, never the bytes transferred, so omitting
 * it cannot change the ring-parsing math under test; it is also literally
 * unbuildable on x86_64 (raw ARM asm mnemonic).
 * ------------------------------------------------------------------ */

/* Mirrors vblk_emmc.c:126-133 gmem_read(). */
static void gmem_read(uint64_t gpa, void *dst, uint32_t len)
{
	assert(gpa + len <= GUEST_MEM_SIZE);
	memcpy(dst, &g_guest_mem[gpa], len);
}

/* Mirrors vblk_emmc.c:135-142 gmem_write(). */
static void gmem_write(uint64_t gpa, const void *src, uint32_t len)
{
	assert(gpa + len <= GUEST_MEM_SIZE);
	memcpy(&g_guest_mem[gpa], src, len);
}

/* Mirrors vblk_emmc.c:144-147 gmem_ld16(). */
static uint16_t gmem_ld16(uint64_t gpa)
{
	uint16_t v; gmem_read(gpa, &v, 2); return v;
}

/* Mirrors vblk_emmc.c:148-151 gmem_st16(). */
static void gmem_st16(uint64_t gpa, uint16_t v)
{
	gmem_write(gpa, &v, 2);
}

/* ------------------------------------------------------------------ *
 * Virtqueue engine — mirrors vblk_emmc.c:238-291 verbatim (control flow
 * and arithmetic unchanged; only the breadcrumb-free guest-memory helpers
 * above are substituted for the real gmem_*).
 * ------------------------------------------------------------------ */

/* Mirrors vblk_emmc.c:239-248 vq_read_desc(). */
static void vq_read_desc(struct vblk_vq *vq, uint16_t idx, struct vblk_desc *out)
{
	struct vring_desc d;
	uint64_t p = vq->desc + (uint64_t)idx * sizeof(struct vring_desc);
	gmem_read(p, &d, sizeof(d));
	out->addr  = d.addr;
	out->len   = d.len;
	out->flags = d.flags;
	out->next  = d.next;
}

/* Mirrors vblk_emmc.c:252-271 vq_pop_avail(). */
static int vq_pop_avail(struct vblk_vq *vq, uint16_t *head)
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

/* Mirrors vblk_emmc.c:277-291 vq_push_used(). */
static uint16_t vq_push_used(struct vblk_vq *vq, uint16_t head, uint32_t used_len)
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

/* ------------------------------------------------------------------ *
 * Descriptor-chain-walking logic — mirrors vblk_emmc.c:437-494, the part
 * of vblk_request() BEFORE it dispatches into serve_data()/emmc_bio (that
 * part is real eMMC I/O, out of scope here — see the task's own framing).
 *
 * This mirrors, verbatim in control flow:
 *   - the chain-collection loop incl. VBLK_MAX_CHAIN truncation check
 *     (vblk_emmc.c:446-450) and the defensive INDIRECT check (:458-461)
 *   - the truncated-chain handling (:468-490) -- THE BUG FIX this task was
 *     asked to pin a regression test on: a chain longer than VBLK_MAX_CHAIN
 *     must be a clean zero-length completion, NEVER treat chain[n-1] as the
 *     status descriptor (that used to silently corrupt guest data — see the
 *     block comment at vblk_emmc.c:468-490 for the full story).
 *   - the n<2 rejection (:491-494)
 *
 * Return value:
 *   0 => the chain was rejected (INDIRECT / MAX_CHAIN truncation / n<2) and
 *        a zero-length completion was ALREADY pushed to the used ring
 *        (mirrors every `vq_push_used(vq, head, 0); return;` in the source).
 *   1 => a genuine chain was collected into out_chain[0..*out_n) and NOTHING
 *        has been pushed to the used ring yet — mirrors control reaching
 *        vblk_emmc.c:496 (the header-parse step) with nothing pushed.
 * *out_truncated is set to 1 only for the VBLK_MAX_CHAIN case, so tests can
 * tell that path apart from INDIRECT/n<2.
 */
static int vq_collect_chain(struct vblk_vq *vq, uint16_t head,
                             struct vblk_desc *out_chain, uint32_t *out_n,
                             int *out_truncated)
{
	uint32_t n = 0;
	uint16_t idx = head;
	int truncated = 0;

	for (;;) {
		if (n >= VBLK_MAX_CHAIN) {
			truncated = 1;
			break;
		}
		vq_read_desc(vq, idx, &out_chain[n]);
		uint16_t flags = out_chain[n].flags;
		uint16_t next  = out_chain[n].next;
		if (flags & VRING_DESC_F_INDIRECT) {
			vq_push_used(vq, head, 0);
			*out_n = n;
			*out_truncated = 0;
			return 0;
		}
		n++;
		if (!(flags & VRING_DESC_F_NEXT))
			break;
		idx = next;
	}

	if (truncated) {
		vq_push_used(vq, head, 0);
		*out_n = n;
		*out_truncated = 1;
		return 0;
	}
	if (n < 2) {
		vq_push_used(vq, head, 0);
		*out_n = n;
		*out_truncated = 0;
		return 0;
	}

	*out_n = n;
	*out_truncated = 0;
	return 1;
}

/* ==================================================================== *
 * Test scaffolding: a small fake virtqueue laid out in g_guest_mem.
 * ==================================================================== */
#define DESC_BASE   0x1000ULL
#define AVAIL_BASE  0x4000ULL
#define USED_BASE   0x6000ULL
#define DATA_BASE   0x8000ULL

static void reset_guest_mem(void)
{
	memset(g_guest_mem, 0xAA, sizeof(g_guest_mem)); /* poison, catch stray reads */
}

static void write_desc(uint16_t idx, uint64_t addr, uint32_t len,
                        uint16_t flags, uint16_t next)
{
	struct vring_desc d = { .addr = addr, .len = len, .flags = flags, .next = next };
	gmem_write(DESC_BASE + (uint64_t)idx * sizeof(d), &d, sizeof(d));
}

static void avail_init(uint16_t idx_value, const uint16_t *ring, uint32_t n)
{
	uint16_t flags = 0;
	gmem_write(AVAIL_BASE + 0u, &flags, 2);
	gmem_st16(AVAIL_BASE + 2u, idx_value);
	for (uint32_t i = 0; i < n; i++)
		gmem_st16(AVAIL_BASE + 4u + (uint64_t)i * 2u, ring[i]);
}

static void used_set_idx(uint16_t idx_value)
{
	uint16_t flags = 0;
	gmem_write(USED_BASE + 0u, &flags, 2);
	gmem_st16(USED_BASE + 2u, idx_value);
}

static void used_get_entry(uint32_t num, uint16_t slot, uint32_t *id, uint32_t *len)
{
	(void)num;
	uint64_t e = USED_BASE + 4u + (uint64_t)slot * 8u;
	gmem_read(e + 0u, id, 4u);
	gmem_read(e + 4u, len, 4u);
}

static void vq_setup(struct vblk_vq *vq, uint32_t num)
{
	memset(vq, 0, sizeof(*vq));
	vq->num = num;
	vq->ready = 1;
	vq->desc = DESC_BASE;
	vq->avail = AVAIL_BASE;
	vq->used = USED_BASE;
	vq->last_avail = 0;
	used_set_idx(0);
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* Two-descriptor chain: header + status, the minimum valid request
 * (vblk_emmc.c:491 requires n>=2). Verifies vq_collect_chain() accepts it,
 * n==2, and both descriptors decode with the exact bytes we wrote. */
static void test_two_descriptor_chain(void)
{
	reset_guest_mem();
	struct vblk_vq vq; vq_setup(&vq, 8);

	write_desc(0, 0xDEAD0000ULL, 16, VRING_DESC_F_NEXT, 1);
	write_desc(1, 0xBEEF0000ULL, 1, VRING_DESC_F_WRITE, 0);

	struct vblk_desc chain[VBLK_MAX_CHAIN];
	uint32_t n; int truncated;
	int rc = vq_collect_chain(&vq, 0, chain, &n, &truncated);

	assert(rc == 1);
	assert(truncated == 0);
	assert(n == 2);
	assert(chain[0].addr == 0xDEAD0000ULL && chain[0].len == 16);
	assert(chain[0].flags == VRING_DESC_F_NEXT);
	assert(chain[1].addr == 0xBEEF0000ULL && chain[1].len == 1);
	assert(chain[1].flags == VRING_DESC_F_WRITE);
}

/* Four-descriptor chain: header + 2 data buffers + status. Verifies the
 * NEXT-chasing walk visits every descriptor in the right order and stops
 * exactly at the one without VRING_DESC_F_NEXT. */
static void test_multi_descriptor_chain(void)
{
	reset_guest_mem();
	struct vblk_vq vq; vq_setup(&vq, 8);

	write_desc(0, 0x1000ULL, 16, VRING_DESC_F_NEXT, 1);
	write_desc(1, 0x2000ULL, 512, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 2);
	write_desc(2, 0x3000ULL, 512, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 3);
	write_desc(3, 0x4000ULL, 1, VRING_DESC_F_WRITE, 0);

	struct vblk_desc chain[VBLK_MAX_CHAIN];
	uint32_t n; int truncated;
	int rc = vq_collect_chain(&vq, 0, chain, &n, &truncated);

	assert(rc == 1);
	assert(truncated == 0);
	assert(n == 4);
	assert(chain[0].addr == 0x1000ULL);
	assert(chain[1].addr == 0x2000ULL && chain[1].len == 512);
	assert(chain[2].addr == 0x3000ULL && chain[2].len == 512);
	assert(chain[3].addr == 0x4000ULL && (chain[3].flags & VRING_DESC_F_NEXT) == 0);
}

/* Single descriptor, no NEXT flag: n==1 < 2, so vblk_emmc.c:491-494 rejects
 * it with a zero-length completion rather than treating it as a real
 * request. Verifies the used-ring push that accompanies rejection. */
static void test_single_descriptor_rejected(void)
{
	reset_guest_mem();
	struct vblk_vq vq; vq_setup(&vq, 8);

	write_desc(0, 0x1000ULL, 16, 0 /* no NEXT */, 0);

	struct vblk_desc chain[VBLK_MAX_CHAIN];
	uint32_t n; int truncated;
	int rc = vq_collect_chain(&vq, 0, chain, &n, &truncated);

	assert(rc == 0);
	assert(truncated == 0);
	assert(n == 1);

	uint32_t id, len;
	used_get_entry(vq.num, 0, &id, &len);
	assert(id == 0);      /* head */
	assert(len == 0);     /* zero-length completion */
	assert(gmem_ld16(USED_BASE + 2u) == 1);  /* used->idx bumped once */
}

/* VBLK_MAX_CHAIN truncation — regression test for the fixed bug documented
 * at vblk_emmc.c:468-490. Build a chain of VBLK_MAX_CHAIN+8 descriptors,
 * every one chained with VRING_DESC_F_NEXT so it NEVER naturally terminates
 * before the cap. The old broken code fell through and (mis-)treated
 * chain[VBLK_MAX_CHAIN-1] (a real DATA descriptor) as the status
 * descriptor, corrupting guest memory at that address with a status byte
 * while never writing the real ack. The fixed code must: (a) stop
 * collecting at exactly VBLK_MAX_CHAIN entries without reading descriptor
 * #33, (b) push a zero-length completion, (c) NEVER write anything to
 * chain[VBLK_MAX_CHAIN-1].addr (or any other descriptor's data region). */
static void test_max_chain_truncation(void)
{
	reset_guest_mem();
	struct vblk_vq vq; vq_setup(&vq, 64);

	const uint32_t total = VBLK_MAX_CHAIN + 8;
	for (uint32_t i = 0; i < total; i++) {
		uint16_t next = (uint16_t)(i + 1);
		/* Give each descriptor a distinct, recognizable data address so we
		 * can prove none of them got clobbered by a misdirected status
		 * write. */
		write_desc((uint16_t)i, DATA_BASE + (uint64_t)i * 0x100u, 512,
		           VRING_DESC_F_NEXT, next);
	}

	/* Snapshot every data-region byte before the call. */
	uint8_t before[VBLK_MAX_CHAIN + 8];
	for (uint32_t i = 0; i < total; i++)
		before[i] = g_guest_mem[DATA_BASE + (uint64_t)i * 0x100u];

	struct vblk_desc chain[VBLK_MAX_CHAIN];
	uint32_t n; int truncated;
	int rc = vq_collect_chain(&vq, 0, chain, &n, &truncated);

	assert(rc == 0);
	assert(truncated == 1);
	assert(n == VBLK_MAX_CHAIN);   /* stopped exactly at the cap, never grew past it */

	/* THE regression check: no descriptor's data byte was touched — in
	 * particular chain[VBLK_MAX_CHAIN-1] (index 31), which is the
	 * descriptor the old buggy code misused as "the" status descriptor. */
	for (uint32_t i = 0; i < total; i++) {
		uint8_t after = g_guest_mem[DATA_BASE + (uint64_t)i * 0x100u];
		assert(after == before[i]);
	}

	/* Zero-length completion was pushed, keyed on head, not on any
	 * mid-chain descriptor. */
	uint32_t id, len;
	used_get_entry(vq.num, 0, &id, &len);
	assert(id == 0);
	assert(len == 0);
}

/* INDIRECT descriptor: defensive rejection (vblk_emmc.c:458-461), same
 * zero-length-completion contract as the other rejection paths. */
static void test_indirect_descriptor_rejected(void)
{
	reset_guest_mem();
	struct vblk_vq vq; vq_setup(&vq, 8);

	write_desc(0, 0x1000ULL, 16, VRING_DESC_F_INDIRECT, 0);

	struct vblk_desc chain[VBLK_MAX_CHAIN];
	uint32_t n; int truncated;
	int rc = vq_collect_chain(&vq, 0, chain, &n, &truncated);

	assert(rc == 0);
	assert(truncated == 0);

	uint32_t id, len;
	used_get_entry(vq.num, 0, &id, &len);
	assert(id == 0);
	assert(len == 0);
}

/* Avail-ring pop + wraparound: queue size 4, push 6 logical entries (more
 * than the ring's physical slot count) and confirm vq_pop_avail() computes
 * slot = last_avail % num correctly across the wrap, in the exact order
 * the driver published them. */
static void test_avail_ring_wraparound(void)
{
	reset_guest_mem();
	struct vblk_vq vq; vq_setup(&vq, 4);

	/* Driver has published 6 heads over time; only the last 4 physical
	 * ring[] slots are meaningful at any instant (num==4), but idx is a
	 * monotonically increasing free-running counter per the virtio spec,
	 * so avail->idx == 6 while ring[] holds (heads[2..5] at slots 2,3,0,1
	 * from wrapping writes) is a legal, common device-side view; here we
	 * drive the simpler/still-real case: idx counts 0..6 as the ring
	 * slots are filled in order, wrapping physical placement each time. */
	uint16_t heads[6] = { 10, 11, 12, 13, 14, 15 };
	uint16_t ring[4];
	for (uint32_t i = 0; i < 6; i++) {
		ring[i % 4] = heads[i];
		avail_init((uint16_t)(i + 1), ring, 4);

		uint16_t popped;
		int rc = vq_pop_avail(&vq, &popped);
		assert(rc == 1);
		assert(popped == heads[i]);
	}
	/* Caught up: no new entries left. */
	uint16_t popped;
	assert(vq_pop_avail(&vq, &popped) == 0);
}

/* Used-ring index wraparound: two flavors.
 *  (1) slot = idx % num wraps correctly when num does not divide evenly
 *      and more entries are pushed than physical slots exist.
 *  (2) the uint16_t used->idx itself wraps from 0xFFFF to 0x0000, and the
 *      slot computation still uses the PRE-wrap idx value (matching
 *      vblk_emmc.c:279-280, which reads used_idx before computing slot). */
static void test_used_ring_wraparound(void)
{
	/* (1) num=3, push 7 entries, confirm slot cycles 0,1,2,0,1,2,0 and
	 * used->idx increments 1..7 with no truncation. */
	{
		reset_guest_mem();
		struct vblk_vq vq; vq_setup(&vq, 3);

		for (uint16_t head = 0; head < 7; head++) {
			uint16_t idx_before = gmem_ld16(USED_BASE + 2u);
			uint16_t expected_slot = (uint16_t)(idx_before % vq.num);
			uint16_t new_idx = vq_push_used(&vq, head, 100u + head);

			assert(new_idx == (uint16_t)(idx_before + 1));

			uint32_t id, len;
			used_get_entry(vq.num, expected_slot, &id, &len);
			assert(id == head);
			assert(len == 100u + head);
		}
		assert(gmem_ld16(USED_BASE + 2u) == 7);
	}

	/* (2) used->idx wraps 0xFFFF -> 0x0000. num=4 so slot = 0xFFFF % 4 == 3
	 * for the wrapping push, and the FOLLOWING push lands at slot 0 with
	 * idx back to 1 -- proving the modulo arithmetic (a uint16_t op in the
	 * mirrored source) is unaffected by the idx counter itself wrapping. */
	{
		reset_guest_mem();
		struct vblk_vq vq; vq_setup(&vq, 4);
		used_set_idx(0xFFFFu);

		uint16_t new_idx = vq_push_used(&vq, 77, 555);
		assert(new_idx == 0u);           /* (uint16_t)(0xFFFF + 1) == 0 */

		uint32_t id, len;
		used_get_entry(vq.num, (uint16_t)(0xFFFFu % vq.num), &id, &len);
		assert(id == 77);
		assert(len == 555);

		/* Next push starts from the wrapped idx (0). */
		new_idx = vq_push_used(&vq, 78, 556);
		assert(new_idx == 1u);
		used_get_entry(vq.num, 0, &id, &len);
		assert(id == 78);
		assert(len == 556);
	}
}

/* ==================================================================== *
 * InterruptACK window — regression test for the LOST-COMPLETION deadlock
 * found live 2026-08-01 (growfs wedged the guest: device-side everything was
 * finished, guest spun QueueNotify -> InterruptACK forever waiting on a
 * completion already sitting in the used ring).
 *
 * Mirrors vblk_emmc.c's injection watermark + the re-arm in
 * VBLK_R_INTERRUPT_ACK. The losing interleaving needs no torn write, so the
 * used-lock that was already there cannot prevent it:
 *   push N, notify, guest scans to the idx it read, THEN push N+1 (VRING
 *   already set, so |= is a no-op), guest acks -> N+1's notification is gone.
 * ==================================================================== */
static uint16_t t_scan_idx;           /* == g_isr_scan_used_idx */
static uint32_t t_int_status;
static uint32_t t_injections;
static uint32_t t_rearms;

static void t_inject(void)
{
	t_injections++;
}

static void t_notify(void)
{
	t_int_status |= VBLK_INT_VRING;
	t_inject();
}

/* The guest ISR reading InterruptStatus. This is where the HV samples the
 * watermark — NOT at injection: a completion injects too, so a watermark taken
 * there is moved by the very event we want to detect. */
static uint32_t t_read_status(struct vblk_vq *vq)
{
	t_scan_idx = gmem_ld16(vq->used + 2u);
	return t_int_status;
}

/* The fixed InterruptACK handler. `rearm_enabled` lets the test run the OLD
 * behaviour too, so it proves the bug is real rather than only that the new
 * code does something. */
static void t_ack(struct vblk_vq *vq, uint32_t val, int rearm_enabled)
{
	t_int_status &= ~val;
	if (rearm_enabled && (val & VBLK_INT_VRING) &&
	    gmem_ld16(vq->used + 2u) != t_scan_idx) {
		t_int_status |= VBLK_INT_VRING;
		t_inject();
		t_rearms++;
	}
}

static void run_ack_window(int rearm_enabled, uint32_t *out_status,
                           uint32_t *out_rearms, struct vblk_vq *vqp)
{
	reset_guest_mem();
	struct vblk_vq vq; vq_setup(&vq, 8);
	t_scan_idx = 0; t_int_status = 0; t_injections = 0; t_rearms = 0;

	/* 1. completion N lands, device notifies */
	vq_push_used(&vq, 0, 513);
	t_notify();
	assert(t_int_status & VBLK_INT_VRING);

	/* 2. guest ISR reads status (opening its scan window) and scans the ring up
	 *    to the used->idx it observed */
	assert(t_read_status(&vq) & VBLK_INT_VRING);
	uint16_t guest_saw = t_scan_idx;
	assert(guest_saw == 1);

	/* 3. completion N+1 lands AFTER that read. VRING is already set, so the
	 *    |= changes nothing — this is what makes the bit ambiguous. */
	vq_push_used(&vq, 1, 513);
	t_notify();

	/* 4. guest acks what it believes it fully handled */
	t_ack(&vq, VBLK_INT_VRING, rearm_enabled);

	*out_status = t_int_status;
	*out_rearms = t_rearms;
	*vqp = vq;                 /* hand the ring out so the caller can ack again */

	/* The ring really does hold an entry the guest never consumed. */
	assert(gmem_ld16(vq.used + 2u) == 2);
	assert(guest_saw == 1);
}

static void test_ack_window_rearm(void)
{
	uint32_t status_old = 0, rearms_old = 0;
	uint32_t status_new = 0, rearms_new = 0;
	struct vblk_vq vq_old, vq_new;

	/* OLD behaviour: the ack clears VRING and nothing re-notifies. The guest's
	 * next ISR would read InterruptStatus==0, skip the scan, and the unconsumed
	 * completion would sit there forever. This assert IS the bug. */
	run_ack_window(0, &status_old, &rearms_old, &vq_old);
	assert((status_old & VBLK_INT_VRING) == 0u);
	assert(rearms_old == 0u);

	/* FIXED behaviour: the ring advanced past the watermark, so VRING is
	 * re-asserted and a fresh IRQ injected — the guest gets another ISR that
	 * finds a non-zero status and rescans. */
	run_ack_window(1, &status_new, &rearms_new, &vq_new);
	assert((status_new & VBLK_INT_VRING) != 0u);
	assert(rearms_new == 1u);

	/* Convergence: the re-armed IRQ makes the guest run its ISR again — status
	 * read (re-sampling the watermark), rescan, ack. Nothing new landed inside
	 * THAT window, so this ack must re-arm NOTHING, or the fix would be an
	 * interrupt storm instead of a fix. */
	uint32_t rearms_before = t_rearms;
	assert(t_read_status(&vq_new) & VBLK_INT_VRING);
	t_ack(&vq_new, VBLK_INT_VRING, 1);
	assert(t_rearms == rearms_before);
	assert((t_int_status & VBLK_INT_VRING) == 0u);
}

/* ==================================================================== *
 * main() — runs every test, reports pass/fail, exits nonzero on failure.
 * ==================================================================== */
struct test_case { const char *name; void (*fn)(void); };

static const struct test_case k_tests[] = {
	{ "two_descriptor_chain",       test_two_descriptor_chain },
	{ "multi_descriptor_chain",     test_multi_descriptor_chain },
	{ "single_descriptor_rejected", test_single_descriptor_rejected },
	{ "max_chain_truncation",       test_max_chain_truncation },
	{ "indirect_descriptor_rejected", test_indirect_descriptor_rejected },
	{ "avail_ring_wraparound",      test_avail_ring_wraparound },
	{ "used_ring_wraparound",       test_used_ring_wraparound },
	{ "ack_window_rearm",           test_ack_window_rearm },
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- vblk ring tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
