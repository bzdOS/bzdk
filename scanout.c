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
	uint32_t guestwin_count;
	uint32_t guestwin_reject;
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
	case SCANOUT_R_GUESTWIN_COUNT:  return d->guestwin_count;
	case SCANOUT_R_GUESTWIN_REJECT: return d->guestwin_reject;
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

/* Is `pa` a physical base the guest is ALLOWED to put on the screen?
 *
 * The display engine is a DMA reader with no IOMMU in front of it, so an address
 * accepted here is displayed verbatim -- an unchecked value would let the guest
 * read hypervisor memory out through the monitor. That is an information leak
 * rather than a crash, which is the sort that goes unnoticed, so this is a
 * whitelist and not a blacklist of the obvious mistakes.
 *
 * Requirements, all of them:
 *   - the WHOLE extent (stride * height, the bytes the mixer will fetch) lies
 *     inside guest DRAM. Checking only the base would let the guest place a
 *     buffer so that it runs off the end into whatever follows.
 *   - it overlaps none of the HV's three carve-outs: hv-image, hv-scratch, and
 *     hv-fb (the HV's own scanout buffers -- BUF1 in particular is private, and
 *     stage2.c's isolation self-check asserts it stays that way).
 *   - 8-byte aligned, which DE2 requires of a layer base anyway.
 *
 * Pure arithmetic on the passed values, no MMIO and no globals, so
 * test_scanout_regs.c can mirror it the way it already mirrors
 * scanout_decide_flip().
 */
/* Guest DRAM and the HV's carve-outs, mirrored here rather than #included:
 * stage2.c defines HVIMG_BASE/HVSCR_BASE as file-local macros, and hv_addrmap.h
 * documents the same numbers. Mirroring is this tree's convention for exactly
 * this situation, so the _Static_asserts below tie the copies to the header's
 * values and a divergence fails the build instead of silently opening a hole. */
#define SCANOUT_GUEST_DRAM_BASE  0x40000000ULL
#define SCANOUT_GUEST_DRAM_END   0x80000000ULL   /* 1 GiB DRAM */
#define SCANOUT_HVIMG_BASE       0x42000000ULL   /* DTB hv-image@42000000  */
#define SCANOUT_HVIMG_SIZE       0x00200000ULL   /* 2 MiB */
#define SCANOUT_HVSCR_BASE       0x50000000ULL   /* DTB hv-scratch@50000000 */
#define SCANOUT_HVSCR_SIZE       0x00200000ULL   /* 2 MiB */
/* hv_addrmap.h has no symbol for the scratch window's base (it is documented in
 * that file's map comment only), so tie the mirror to a real lane INSIDE it: if
 * HVMAP_VGICD_BC ever stops falling within the range this validates against, the
 * range is wrong and the guest could be handed an address inside hv-scratch. */
_Static_assert((uint64_t)HVMAP_VGICD_BC >= SCANOUT_HVSCR_BASE &&
               (uint64_t)HVMAP_VGICD_BC <  SCANOUT_HVSCR_BASE + SCANOUT_HVSCR_SIZE,
               "a known hv-scratch lane falls outside the hv-scratch range this "
               "file rejects -- the range is stale");
_Static_assert(SCANOUT_GUEST_DRAM_BASE <= (uint64_t)HVMAP_FB_BASE &&
               (uint64_t)HVMAP_FB_BASE < SCANOUT_GUEST_DRAM_END,
               "hv-fb is not inside the guest DRAM window this validates against");

int scanout_addr_allowed(uint32_t pa, uint32_t stride, uint32_t height)
{
	uint64_t base = (uint64_t)pa;
	uint64_t len  = (uint64_t)stride * (uint64_t)height;
	uint64_t end;

	if (stride == 0u || height == 0u)
		return 0;
	if ((base & 7u) != 0u)
		return 0;

	end = base + len;
	if (end <= base)                       /* wrapped */
		return 0;
	if (base < SCANOUT_GUEST_DRAM_BASE || end > SCANOUT_GUEST_DRAM_END)
		return 0;

	/* Overlap test against each HV region: [base,end) vs [r,r+size). */
#define SCANOUT_OVERLAPS(r, sz) \
	(base < ((uint64_t)(r) + (uint64_t)(sz)) && ((uint64_t)(r)) < end)
	if (SCANOUT_OVERLAPS(SCANOUT_HVIMG_BASE, SCANOUT_HVIMG_SIZE))
		return 0;
	if (SCANOUT_OVERLAPS(SCANOUT_HVSCR_BASE, SCANOUT_HVSCR_SIZE))
		return 0;
	if (SCANOUT_OVERLAPS(HVMAP_FB_BASE, HVMAP_FB_WINDOW_SIZE))
		return 0;
#undef SCANOUT_OVERLAPS
	return 1;
}

/* Register WRITE. Only FLIP_REQUEST and GUESTWIN_ADDR do anything; every other offset
 * (including unknown ones) is silently ignored — matches vblk/vnet's own
 * "write to a read-only/unknown register is a no-op, not a fault" policy. */
static void scanout_reg_write(struct scanout_dev *d, uint32_t off, uint32_t val)
{
	uint32_t new_pa;

	if (off == SCANOUT_R_GUESTWIN_ADDR) {
		/* The GUEST WINDOW's geometry, not d->stride/d->height -- those
		 * describe the HUD framebuffer this device scans out, which is a
		 * different and much larger surface. Validating the wrong extent
		 * would reject perfectly good buffers that sit near the top of
		 * guest DRAM. */
		if (scanout_addr_allowed(val, HDMI_GUESTWIN_STRIDE,
		    HDMI_GUESTWIN_H)) {
			hdmi_guestwin_set_addr(val);
			d->guestwin_count++;
			scan_bc(9, d->guestwin_count);
		} else {
			d->guestwin_reject++;
			scan_bc(10, d->guestwin_reject);
		}
		return;
	}

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
