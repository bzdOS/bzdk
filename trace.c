/* SPDX-License-Identifier: BSD-2-Clause */

/* trace.c — event-trace ring (ftrace-lite) for the bzdOS EL2 hypervisor.
 *
 * See trace.h for the DRAM layout contract (base HVMAP_TRACE_RING, "TRC1").
 * Freestanding, no libc. Single global ring, lock-free/wait-free across the 4
 * A53 cores: a producer claims a unique slot with one exclusive fetch-add on
 * the header's total_events word (the "ticket"), writes its 16-byte entry, and
 * cleans every touched word to the point of coherency so the HUD track and an
 * external md.l reader (and a post-reset dump) see it.
 */
#include <stdint.h>
#include "trace.h"
#include "timer.h"
#include "hv_addrmap.h"

/* Base comes from hv_addrmap.h, which is where window collisions are
 * checked. It used to be a local 0x50004000 that ran into the BMC block. */
#define TRACE_BASE   HVMAP_TRACE_RING
#define TRACE_MAGIC  0x54524331u        /* "TRC1" */
#define TRACE_CAP    512u               /* number of ring slots */
#define TRACE_HDR    0x40UL             /* header stride: entries start +0x40 */

_Static_assert(TRACE_HDR + (unsigned long)TRACE_CAP * 16UL
               <= HVMAP_TRACE_RING_SIZE,
               "TRACE_CAP outgrew the window reserved in hv_addrmap.h");

/* Header word indices. */
#define H_MAGIC   0u
#define H_TOTAL   1u
#define H_HEAD    2u
#define H_CAP     3u
#define H_FREQ    4u

static volatile uint32_t *const trc = (volatile uint32_t *)TRACE_BASE;
/* Entry array base as a word pointer (0x50004040). */
static volatile uint32_t *const trc_ent =
	(volatile uint32_t *)(TRACE_BASE + TRACE_HDR);

static int trace_ready;   /* set by trace_init(); guards emits before init */

/* Clean one word to PoC so a reader / a post-reset dump sees it (D-cache on,
 * house style: dc cvac + dsb sy). */
static inline void clean_word(volatile uint32_t *p)
{
	__asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void hdr_set(unsigned i, uint32_t v)
{
	trc[i] = v;
	clean_word(&trc[i]);
}

/* Atomic 32-bit fetch-add on a DRAM word (cached, MMU on -> exclusives work).
 * Bounded: the ldxr/stxr pair retries only on a real contended conflict, which
 * on a 4-core machine resolves in a handful of iterations; there is no
 * unbounded spin. Returns the PRE-increment value (the ticket). */
static inline uint32_t fetch_add32(volatile uint32_t *p, uint32_t inc)
{
	uint32_t old, nw, ok;

	do {
		__asm__ volatile("ldxr %w0, %2" : "=&r"(old) : "Q"(*p), "m"(*p));
		nw = old + inc;
		__asm__ volatile("stxr %w0, %w2, %1"
		                 : "=&r"(ok), "=Q"(*p) : "r"(nw));
	} while (ok != 0u);
	return old;
}

void
trace_init(void)
{
	unsigned i;

	/* Zero the whole window (header + all slots), then stamp the header. */
	for (i = 0; i < (TRACE_HDR / 4u) + TRACE_CAP * 4u; i++) {
		trc[i] = 0u;
		clean_word(&trc[i]);
	}
	hdr_set(H_CAP,  TRACE_CAP);
	hdr_set(H_FREQ, (uint32_t)timer_freq());
	hdr_set(H_HEAD, 0u);
	hdr_set(H_TOTAL, 0u);
	hdr_set(H_MAGIC, TRACE_MAGIC);   /* magic last: reader sees a ready ring */
	trace_ready = 1;
}

void
trace_emit(uint8_t type, uint8_t cpu, uint16_t arg, uint32_t ctx)
{
	uint64_t ts;
	uint32_t ticket, slot;
	volatile uint32_t *e;

	if (!trace_ready)
		return;

	ts = timer_now();

	/* Claim a unique slot: one exclusive fetch-add gives this producer a
	 * ticket no other core can get. slot = ticket % capacity. */
	ticket = fetch_add32(&trc[H_TOTAL], 1u);
	slot   = ticket % TRACE_CAP;
	e      = &trc_ent[slot * 4u];

	e[0] = (uint32_t)ts;
	e[1] = (uint32_t)(ts >> 32);
	e[2] = ((uint32_t)type & 0xffu)
	     | (((uint32_t)cpu & 0xffu) << 8)
	     | (((uint32_t)arg & 0xffffu) << 16);
	e[3] = ctx;

	/* All four words are 16-byte aligned within a single 64-byte cache line
	 * (4 entries per line), so one dc cvac on the entry base cleans the whole
	 * 16-byte record. Clean word0 last-touched too for safety, then publish
	 * the head hint. */
	clean_word(&e[0]);
	clean_word(&e[3]);

	/* head is an advisory "next write" hint for readers; a racy store here is
	 * harmless (total_events is the authoritative monotonic count). */
	trc[H_HEAD] = (ticket + 1u) % TRACE_CAP;
	clean_word(&trc[H_HEAD]);
	(void)cpu;
}
