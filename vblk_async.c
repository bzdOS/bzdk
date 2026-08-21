/* SPDX-License-Identifier: BSD-2-Clause */

/* vblk_async.c — CPU2 bring-up loop for the ROADMAP C2 async eMMC I/O
 * offload milestone. See vblk_async.h for the full design writeup (mailbox
 * protocol, memory ordering, the "third actor on the eMMC lock" note, the
 * weak/strong linkage trick, and why the mailbox itself lives in
 * vblk_emmc.c rather than here).
 *
 * Freestanding: <stdint.h> only.
 */
#include <stdint.h>
#include "vblk_async.h"
#include "vblk_emmc.h"   /* vblk_async_poll(), g_vblk_async_ready */
#include "cntpct.h"

/* Tiny breadcrumb, distinct from every other window in the tree (see
 * smp.h's map comment + flightrec.h's note on the same free gap): VBK1's
 * lock/EBIO/HS-testbuf occupy 0x50020000..~0x50020500 (vblk_emmc.c/
 * emmc_bio.c); this sits right after, at 0x50020600, clear of all of them.
 *   [0] magic "VBA1"
 *   [1] loop iteration counter (free-running, wraps — liveness only)
 *   [2] last CNTPCT snapshot (liveness, same idea as smp.c's CPU1 word 7)
 */
#define VBLK_ASYNC_BC_BASE   HVMAP_ASYNC_BC   /* see hv_addrmap.h */
#define VBLK_ASYNC_BC_MAGIC  0x56424131u   /* "VBA1" */

/* This window is reserved at EXACTLY the three words above -- zero headroom, so
 * a fourth counter added here overflows into the next window silently. The
 * assert makes that a build failure instead; grow HVMAP_ASYNC_BC_SIZE first.
 * (emmc_bio.c's window was already 5 words past its reservation before anyone
 * noticed, which is why this is asserted rather than commented.) */
#define VBLK_ASYNC_BC_MAX_IDX  2u
_Static_assert((VBLK_ASYNC_BC_MAX_IDX + 1u) * 4u <= HVMAP_ASYNC_BC_SIZE,
               "vblk_async.c writes more breadcrumb slots than "
               "HVMAP_ASYNC_BC_SIZE reserves");

static inline void async_bc(uint32_t idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(VBLK_ASYNC_BC_BASE + idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline uint64_t read_cntpct(void)
{
	uint64_t v;
	v = cntpct_read();   /* cntpct.h: Allwinner counter erratum */
	return v;
}

void vblk_async_cpu2_run(void)
{
	uint32_t iters = 0;

	async_bc(0, VBLK_ASYNC_BC_MAGIC);

	/* Flip the readiness handshake BEFORE entering the loop: from this
	 * instant on, vblk_async_post() (vblk_emmc.c, CPU0) is allowed to mark
	 * the mailbox POSTED, because there is now, in fact, someone guaranteed
	 * to drain it. dsb so the flag's visibility to CPU0 is ordered the same
	 * way every other cross-core flag in this tree is (see smp.c's
	 * dbg_core_active for the precedent). Harmless even if CPU0 somehow
	 * observed this a few instructions "late" — smp_init() bounded-waits
	 * for CPU2 to come online before main_dbg.c ever calls kload_enter(),
	 * and the guest cannot issue any virtio-blk I/O before it has booted
	 * far enough to attach the driver, which takes far longer than this. */
	g_vblk_async_ready = 1;
	__asm__ volatile("dsb sy" ::: "memory");

	/* IRQ stays masked on this core (inherited from _start_secondary's reset
	 * DAIF state — see start.S); we never unmask it and never arm a timer.
	 * This is a pure polling server, exactly like CPU1's debug loop in
	 * smp.c, and for the same reason: there is nothing here worth
	 * preempting mid-request (serve_data()'s eMMC calls are themselves
	 * bounded, so a "stuck" iteration is not possible, only a slow one). */
	for (;;) {
		vblk_async_poll();       /* bounded: empty-check, or one request */

		if ((iters & 0xFFFu) == 0u)
			async_bc(2, (uint32_t)read_cntpct());
		async_bc(1, ++iters);
	}
}
