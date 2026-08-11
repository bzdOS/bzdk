/* SPDX-License-Identifier: BSD-2-Clause */

/* scanout.c — zero-copy GPU scanout flip doorbell. See scanout.h for the
 * API contract, register layout, and what v1 deliberately does not solve.
 *
 * Structure deliberately separates PURE decision logic (scanout_reg_read(),
 * scanout_decide_flip() — no MMIO, no inline asm, operate only on `struct
 * scanout_dev`) from the hardware-touching wrapper (scanout_mmio_fault()'s
 * ESR/HPFAR decode, hdmi_set_scanout_addr()'s real register writes, the
 * breadcrumb stores) — test_scanout_regs.c hand-transcribes the pure half,
 * following the same convention test_sd_bio_addr.c documents in its own
 * header (a hosted x86_64 test cannot link a file containing `mrs`/`dc
 * civac`/absolute-MMIO-pointer code at all, so the pure logic is mirrored
 * with exact line citations instead of `#include`d).
 *
 * Freestanding: <stdint.h> only.
 */
#include <stdint.h>
#include "scanout.h"
#include "hv_addrmap.h"
#include "hdmi.h"

/* ------------------------------------------------------------------ *
 * ESR_EL2.ISS decode for a data abort (EC==0x24) — identical convention to
 * vblk_emmc.c/vnet_emac.c (see their comments); duplicated here to keep
 * this file self-contained, same as those two do for each other.
 * ------------------------------------------------------------------ */
#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3Fu
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u

/* ------------------------------------------------------------------ *
 * Breadcrumb: HVMAP_SCANOUT_BC (hv_addrmap.h), magic "SCAN".
 *   [0] magic          SCANOUT_MAGIC
 *   [1] buf0_addr      HVMAP_FB_BUF0_BASE
 *   [2] buf1_addr      HVMAP_FB_BUF1_BASE
 *   [3] width  [4] height  [5] stride
 *   [6] front_index    current front buffer (0 or 1)
 *   [7] flip_count     completed-flip counter
 *   [8] reject_count   out-of-range FLIP_REQUEST writes
 *   [9] init_done      1 once scanout_init() has run (0 = register file not
 *                      yet populated — a read before this would be all-zero,
 *                      not a crash, but also not meaningful)
 * ------------------------------------------------------------------ */
static inline void scan_bc(uint32_t idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_SCANOUT_BC + idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ *
 * Single device instance — same "one static struct" shape as vblk_emmc.c's
 * g_blk / vnet_emac.c's g_net.
 * ------------------------------------------------------------------ */
struct scanout_dev {
	uint32_t buf_pa[2];
	uint32_t width, height, stride, format;
	uint32_t front_index;
	uint32_t flip_count;
	uint32_t reject_count;
};

static struct scanout_dev g_scan;

void scanout_init(void)
{
	g_scan.buf_pa[0]   = (uint32_t)HVMAP_FB_BUF0_BASE;
	g_scan.buf_pa[1]   = (uint32_t)HVMAP_FB_BUF1_BASE;
	/* Read geometry through hdmi.c's OWN accessors rather than re-quoting
	 * HVMAP_FB_BUF_W/H/STRIDE a third time — if hdmi.c's real, live mode
	 * ever disagrees with hv_addrmap.h's mirror, this is a live mismatch a
	 * human can `bc`/`md` and see (buf1_addr - buf0_addr vs width*height*4),
	 * rather than a silently-wrong contract nobody notices. */
	g_scan.width       = (uint32_t)hdmi_width();
	g_scan.height      = (uint32_t)hdmi_height();
	g_scan.stride      = (uint32_t)hdmi_stride() * 4u;   /* pixels -> bytes */
	g_scan.format      = SCANOUT_FORMAT_XRGB8888;
	g_scan.front_index = 0u;      /* matches stage_de2()'s boot-time TOP_LADDR */
	g_scan.flip_count  = 0u;
	g_scan.reject_count = 0u;

	scan_bc(1, g_scan.buf_pa[0]);
	scan_bc(2, g_scan.buf_pa[1]);
	scan_bc(3, g_scan.width);
	scan_bc(4, g_scan.height);
	scan_bc(5, g_scan.stride);
	scan_bc(6, g_scan.front_index);
	scan_bc(7, g_scan.flip_count);
	scan_bc(8, g_scan.reject_count);
	scan_bc(9, 1u);            /* init_done */
	scan_bc(0, SCANOUT_MAGIC); /* magic LAST, same "stamp last" convention as
	                            * every other breadcrumb in this tree */
}

/* ------------------------------------------------------------------ *
 * PURE decision logic (no MMIO, no asm) — mirrored verbatim into
 * test_scanout_regs.c, so keep any change here in sync with that file's own
 * header comment.
 * ------------------------------------------------------------------ */

/* Register READ. Every register but FLIP_REQUEST (write-only) is handled
 * here; an unknown offset reads 0, matching vblk/vnet's own convention. */
static uint32_t scanout_reg_read(const struct scanout_dev *d, uint32_t off)
{
	switch (off) {
	case SCANOUT_R_MAGIC:        return SCANOUT_MAGIC;
	case SCANOUT_R_VERSION:      return SCANOUT_VERSION;
	case SCANOUT_R_BUF0_ADDR:    return d->buf_pa[0];
	case SCANOUT_R_BUF1_ADDR:    return d->buf_pa[1];
	case SCANOUT_R_WIDTH:        return d->width;
	case SCANOUT_R_HEIGHT:       return d->height;
	case SCANOUT_R_STRIDE:       return d->stride;
	case SCANOUT_R_FORMAT:       return d->format;
	case SCANOUT_R_FRONT_INDEX:  return d->front_index;
	case SCANOUT_R_FLIP_COUNT:   return d->flip_count;
	case SCANOUT_R_REJECT_COUNT: return d->reject_count;
	default:                     return 0u;
	}
}

/* Decide the effect of a FLIP_REQUEST write of `requested`:
 *   - requested > 1            : rejected (bump reject_count), returns 0.
 *   - requested == front_index : silent no-op (already showing it; NOT an
 *                                 error, NOT counted either way), returns 0.
 *   - requested == the OTHER buffer : front_index updates, flip_count bumps,
 *                                 *new_pa receives that buffer's physical
 *                                 address, returns 1 — caller must then
 *                                 actually reprogram the hardware.
 * Deliberately takes/mutates `d` by pointer rather than touching g_scan
 * directly so it has zero MMIO/global dependency and can be unit tested
 * against a caller-owned struct. */
static int scanout_decide_flip(struct scanout_dev *d, uint32_t requested,
                                uint32_t *new_pa)
{
	if (requested > 1u) {
		d->reject_count++;
		return 0;
	}
	if (requested == d->front_index)
		return 0;

	d->front_index = requested;
	d->flip_count++;
	*new_pa = d->buf_pa[requested];
	return 1;
}

/* Register WRITE. Only FLIP_REQUEST does anything; every other offset
 * (including unknown ones) is silently ignored — matches vblk/vnet's own
 * "write to a read-only/unknown register is a no-op, not a fault" policy. */
static void scanout_reg_write(struct scanout_dev *d, uint32_t off, uint32_t val)
{
	uint32_t new_pa;

	if (off != SCANOUT_R_FLIP_REQUEST)
		return;

	if (scanout_decide_flip(d, val, &new_pa)) {
		hdmi_set_scanout_addr(new_pa);
		scan_bc(6, d->front_index);
		scan_bc(7, d->flip_count);
	} else {
		scan_bc(8, d->reject_count);
	}
}

/* ------------------------------------------------------------------ *
 * Hardware-touching wrappers.
 * ------------------------------------------------------------------ */

int scanout_flip(uint32_t index)
{
	uint32_t new_pa;

	if (scanout_decide_flip(&g_scan, index, &new_pa)) {
		hdmi_set_scanout_addr(new_pa);
		scan_bc(6, g_scan.front_index);
		scan_bc(7, g_scan.flip_count);
		return 1;
	}
	scan_bc(8, g_scan.reject_count);
	return 0;
}

/* Same ESR/HPFAR decode idiom as vblk_mmio_fault()/vnet_mmio_fault() —
 * comments trimmed to what differs; see vblk_emmc.c for the full rationale
 * of each field. */
int scanout_mmio_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec  = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	if (addr < SCANOUT_MMIO_BASE || addr >= SCANOUT_MMIO_BASE + SCANOUT_MMIO_SIZE)
		return 0;                         /* not our window */

	uint32_t isv = esr & ESR_ISV_BIT;
	if (!isv) {
		/* No instruction syndrome to decode — skip the instruction so the
		 * guest makes forward progress rather than re-faulting forever,
		 * matching vconsole/vblk/vnet's own defensive corner. */
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	uint32_t off = (uint32_t)(addr - SCANOUT_MMIO_BASE);

	if (wnr) {
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];
		scanout_reg_write(&g_scan, off, (uint32_t)val);
	} else {
		uint32_t val = scanout_reg_read(&g_scan, off);
		if (srt != SRT_XZR)
			frame->x[srt] = (uint64_t)val;
	}

	frame->elr += 4;   /* emulated -- skip the faulting load/store */
	return 1;
}
