/* SPDX-License-Identifier: BSD-2-Clause */

/* guest_mem.h — which guest-physical ranges the hypervisor may touch on the
 * guest's behalf.
 *
 * Every emulated device (virtio-net, virtio-blk on eMMC/SD/zram, virtio-input,
 * the scanout plane) dereferences guest-supplied addresses from EL2, or hands
 * them to a DMA engine. Stage-2 does not apply to EL2 or to DMA, so the device
 * is the only thing standing between a guest-chosen pointer and the
 * hypervisor's own memory. Before this header each device checked "inside the
 * stage-2 DRAM window" only, which accepted the hv-image and hv-scratch
 * windows (EMAC DMA rings, the TX lock word, breadcrumbs, dbgtools/WDEP), so a
 * descriptor pointing there let the guest write HV state through the device
 * even though the same write from the guest itself takes a stage-2 fault.
 *
 * ONE TABLE, TWO CONSUMERS. hv_dram_holes[] below is the list of HV-owned
 * ranges inside the stage-2 DRAM window. stage2.c builds the guest's map from
 * it (a 2 MiB block wholly inside a hole is left invalid), and
 * guest_gpa_ok() rejects any range that touches one. Adding a window here
 * therefore removes it from both at once; there is no second list to forget.
 *
 * Guest RAM is also bounded above by GUEST_RAM_END, the top of the DTB
 * /memory node, which is lower than the stage-2 window under GUEST_DRAM_2G:
 * above it sit the vzram pool (VZRAM) and U-Boot's reserved region, which
 * holds the translation tables EL2 still runs on. That tail is a hole too.
 */
#ifndef GUEST_MEM_H
#define GUEST_MEM_H

#include <stdint.h>
#include "stage2.h"       /* STAGE2_DRAM_BASE/SIZE */
#include "hv_addrmap.h"   /* HVMAP_FB_*, HVMAP_VZRAM_BASE */

/* DTB reserved-memory hv-image@42000000 / hv-scratch@50000000, each rounded
 * up to one 2 MiB stage-2 block (see stage2.c's A1 comment). */
#define HVMAP_HVIMG_BASE   0x42000000UL
#define HVMAP_HVSCR_BASE   0x50000000UL
/* One stage-2 level-2 block; stage2.c asserts it equals its STAGE2_L2_BLOCK_SIZE. */
#define GUEST_MEM_BLOCK    0x200000UL
#define HVMAP_HVWIN_SIZE   GUEST_MEM_BLOCK

/* No casts in these three: they are also used in #if. */
#define GUEST_STAGE2_END   (STAGE2_DRAM_BASE + STAGE2_DRAM_SIZE)

/* Top of the RAM the guest is told about. Must match board-config.xml's
 * dtb-memory-size for the same feature set (gen_config.py writes the DTB from
 * it): vzram 0x70000000, guest_dram_2g 0x78000000, otherwise the whole
 * 1 GiB window. */
#if defined(VZRAM) && VZRAM
#define GUEST_RAM_END      HVMAP_VZRAM_BASE
#elif defined(GUEST_DRAM_2G) && GUEST_DRAM_2G
#define GUEST_RAM_END      0xB8000000UL
#else
#define GUEST_RAM_END      GUEST_STAGE2_END
#endif

_Static_assert(GUEST_RAM_END <= GUEST_STAGE2_END,
               "guest RAM ends above the stage-2 DRAM window");
_Static_assert(GUEST_RAM_END % GUEST_MEM_BLOCK == 0,
               "guest RAM top must be 2 MiB aligned to be carved by block");

struct hv_dram_hole {
	uint64_t base;
	uint64_t end;     /* exclusive */
};

/* HV-owned ranges inside [STAGE2_DRAM_BASE, GUEST_STAGE2_END). stage2.c
 * invalidates every L2 block wholly inside one of these; a partially covered
 * block must be split by hand (only the shared-framebuffer boundary is). */
static const struct hv_dram_hole hv_dram_holes[] = {
	{ HVMAP_HVIMG_BASE, HVMAP_HVIMG_BASE + HVMAP_HVWIN_SIZE },
	{ HVMAP_HVSCR_BASE, HVMAP_HVSCR_BASE + HVMAP_HVWIN_SIZE },
#if defined(HV_HDMI) && !defined(HV_FB_GUEST)
	{ HVMAP_FB_BASE, HVMAP_FB_BASE + HVMAP_FB_WINDOW_SIZE },
#elif defined(HV_HDMI)
	/* Front buffer shared with the guest; back buffer and spare are not. */
	{ HVMAP_FB_BUF1_BASE, HVMAP_FB_BASE + HVMAP_FB_WINDOW_SIZE },
#endif
#if GUEST_RAM_END < GUEST_STAGE2_END
	{ GUEST_RAM_END, GUEST_STAGE2_END },
#endif
};
#define HV_DRAM_NHOLES (sizeof(hv_dram_holes) / sizeof(hv_dram_holes[0]))

/* 1 if [gpa, gpa+len) lies wholly in guest RAM and touches no HV hole.
 * len == 0 touches nothing and is accepted. A handful of compares; callers
 * check once per descriptor or ring, not per byte. */
static inline int guest_gpa_ok(uint64_t gpa, uint64_t len)
{
	if (len == 0u)
		return 1;
	if (gpa < (uint64_t)STAGE2_DRAM_BASE || gpa >= GUEST_RAM_END)
		return 0;
	if (GUEST_RAM_END - gpa < len)
		return 0;
	uint64_t end = gpa + len;
	for (unsigned i = 0; i < HV_DRAM_NHOLES; i++)
		if (gpa < hv_dram_holes[i].end && end > hv_dram_holes[i].base)
			return 0;
	return 1;
}

/* The three rings of a split virtqueue of `num` entries (VIRTIO 1.x 2.7):
 * descriptor table 16*num, avail 6+2*num, used 6+8*num bytes. */
static inline int guest_vring_ok(uint64_t desc, uint64_t avail, uint64_t used,
                                 uint32_t num)
{
	uint64_t n = num;
	return n != 0u &&
	       guest_gpa_ok(desc, 16u * n) &&
	       guest_gpa_ok(avail, 6u + 2u * n) &&
	       guest_gpa_ok(used, 6u + 8u * n);
}

/* 1 if the 2 MiB stage-2 block at `pa` lies wholly inside an HV hole. */
static inline int hv_dram_block_is_hole(uint64_t pa)
{
	uint64_t end = pa + GUEST_MEM_BLOCK;
	for (unsigned i = 0; i < HV_DRAM_NHOLES; i++)
		if (pa >= hv_dram_holes[i].base && end <= hv_dram_holes[i].end)
			return 1;
	return 0;
}

#endif /* GUEST_MEM_H */
