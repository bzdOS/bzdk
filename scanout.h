/* SPDX-License-Identifier: BSD-2-Clause */

/* scanout.h — zero-copy GPU scanout flip doorbell for the bzdOS EL2
 * hypervisor (Allwinner A64 / Banana Pi M64, HV_HDMI builds only).
 *
 * See docs/zero-copy-scanout.md for the full design, the evidence behind
 * every claim below, and the security analysis. Short version: the Mali-400
 * GPU's PP write-back unit is a DMA bus master with no SoC IOMMU in front of
 * it (docs/dma-bypass-stage2.md) — it can already write to hv-fb (or
 * anywhere else in DRAM) without ever going through this hypervisor's
 * stage-2 tables. What the guest's lima driver cannot do on its own is (a)
 * learn WHICH physical addresses are the HV's two scanout buffers (they are
 * `no-map` in the DTB, invisible to FreeBSD's allocator, and today NOT
 * guaranteed excluded from the guest CPU's own stage-2 map either — see the
 * design doc), and (b) tell the HV "I just finished rendering into buffer N,
 * please show it" so the DE2 mixer's scanout address actually changes. This
 * file is that two-part contract, implemented as a tiny memory-mapped
 * register file the guest can read (geometry/addresses) and write one
 * doorbell register (request a flip) — the SAME shape as vblk_emmc.h's
 * virtio-blk device and vnet_emac.h's virtio-net device, just not
 * virtio-spec (a full virtio-gpu device is a much bigger undertaking than
 * what this task needs: an address, alignment/stride, and a flip verb).
 *
 * MMIO WINDOW
 * -----------
 * SCANOUT_MMIO_BASE (0x0A002000) sits INSIDE the 2 MiB block
 * (VBLK_TRAP_BLOCK_BASE/SIZE, vblk_emmc.h) that stage2.c ALREADY leaves
 * entirely stage-2-INVALID for vblk's sake ("the rest of the 2 MiB trapped
 * block is spare for future virtio-mmio devices" — vblk_emmc.h's own
 * comment). vnet_emac.h already took the next slot at +0x1000; this device
 * takes +0x2000, comfortably clear of both:
 *
 *   0x0A000000 .. 0x0A000200   vblk_emmc   (existing, untouched)
 *   0x0A001000 .. 0x0A001200   vnet_emac   (existing, untouched)
 *   0x0A002000 .. 0x0A002100   scanout     (THIS device)
 *   0x0A002100 .. 0x0A200000   still spare
 *
 * Consequence: NO stage2.c EDIT WAS NEEDED for this device, exactly like
 * vnet_emac.h's own note about itself — the L2 entry at index 80 is already
 * unmapped for vblk's sake, so a guest access anywhere in that 2 MiB range
 * (including 0x0A002000) already takes the same stage-2 data abort into EL2
 * that vblk_mmio_fault()/vnet_mmio_fault() rely on. This file only needed
 * its fault handler wired into el2_trap's dispatch chain (el2_exc.c, one
 * `#ifdef HV_HDMI`-guarded line, mirroring vnet's own insertion).
 *
 * REGISTER FILE (all 32-bit, byte offsets from SCANOUT_MMIO_BASE)
 * -----------------------------------------------------------------
 *   0x00  R  MAGIC          0x53434e41 ("SCAN")
 *   0x04  R  VERSION        1
 *   0x08  R  BUF0_ADDR      physical base of buffer 0 (== HDMI_FB_BASE —
 *                           the pre-existing single buffer; front at boot)
 *   0x0C  R  BUF1_ADDR      physical base of buffer 1 (the new back buffer)
 *   0x10  R  WIDTH          pixels
 *   0x14  R  HEIGHT         pixels
 *   0x18  R  STRIDE         bytes per scanline (both buffers share this —
 *                           v1 does not support flipping between
 *                           differently-sized/strided buffers, see below)
 *   0x1C  R  FORMAT         0 = XRGB8888 (the only value that exists today)
 *   0x20  R  FRONT_INDEX    0 or 1: which buffer is CURRENTLY scanned out
 *   0x24  W  FLIP_REQUEST   write 0 or 1: "make that buffer the new front".
 *                           Writing the ALREADY-front index is a silent
 *                           no-op (not an error, not counted). Writing
 *                           anything else (>1) is REJECTED: no hardware
 *                           change, REJECT_COUNT increments.
 *   0x28  R  FLIP_COUNT     completed-flip counter (monotonic; the only way
 *                           v1 gives the guest to notice a flip completed —
 *                           there is no interrupt/vblank signal, see below)
 *   0x2C  R  REJECT_COUNT   out-of-range FLIP_REQUEST writes, diagnostic
 *
 * Every register except FLIP_REQUEST is read-only; a write to any of them
 * is silently ignored (matches vblk/vnet's own "unknown offset -> 0 / no-op"
 * convention rather than faulting the guest for a benign driver bug).
 *
 * WHAT v1 DELIBERATELY DOES NOT SOLVE (see docs/zero-copy-scanout.md):
 *   - No vsync/vblank interrupt. FLIP_COUNT is the only completion signal,
 *     and it increments synchronously inside the trapped write (by the time
 *     the guest's STR instruction retires, TOP_LADDR + the DE_GLB_DBUFF
 *     commit strobe have already been issued) — but whether the DE2 MIXER
 *     has actually latched it yet (i.e. whether it is still mid-frame) is
 *     NOT observable from here. FLIP_COUNT proves "the HV accepted the
 *     request", not "the new frame is on the wire".
 *   - No fences. Nothing here waits for the Mali PP's write-back job
 *     targeting the back buffer to finish before honoring a flip to it —
 *     that ordering is the (out-of-scope, guest-side) driver's job.
 *   - No back-pressure. Flipping twice inside one frame period just
 *     rewrites TOP_LADDR twice; whichever value is live at the next mixer
 *     read wins. This can skip a frame, never corrupt one (every value
 *     FLIP_REQUEST can produce is a fully valid buffer address).
 *   - PITCH/format/size are fixed at hdmi_init() time and never
 *     reprogrammed by a flip — only the base address changes. Both buffers
 *     MUST already share hdmi_stride()/hdmi_width()/hdmi_height().
 *
 * Freestanding: <stdint.h> + "exceptions.h" only, no libc, no floats,
 * -mgeneral-regs-only. Single-writer (CPU0, inside the guest's own trap) —
 * see docs/zero-copy-scanout.md's concurrency note for why hdmi_relock()
 * running concurrently on CPU1 needs no additional lock.
 */
#ifndef BZDOS_SCANOUT_H
#define BZDOS_SCANOUT_H

#include <stdint.h>
#include "exceptions.h"   /* struct el2_frame */

#define SCANOUT_MMIO_BASE   0x0A002000UL
#define SCANOUT_MMIO_SIZE   0x00000100UL   /* one device slot, same shape as
                                             * VBLK_MMIO_SIZE/VNET_MMIO_SIZE */

/* ---- register offsets ------------------------------------------------- */
#define SCANOUT_R_MAGIC         0x00u   /* R */
#define SCANOUT_R_VERSION       0x04u   /* R */
#define SCANOUT_R_BUF0_ADDR     0x08u   /* R */
#define SCANOUT_R_BUF1_ADDR     0x0Cu   /* R */
#define SCANOUT_R_WIDTH         0x10u   /* R */
#define SCANOUT_R_HEIGHT        0x14u   /* R */
#define SCANOUT_R_STRIDE        0x18u   /* R */
#define SCANOUT_R_FORMAT        0x1Cu   /* R */
#define SCANOUT_R_FRONT_INDEX   0x20u   /* R */
#define SCANOUT_R_FLIP_REQUEST  0x24u   /* W */
#define SCANOUT_R_FLIP_COUNT    0x28u   /* R */
#define SCANOUT_R_REJECT_COUNT  0x2Cu   /* R */
/* ---- guest-window presentation (v1 addition) -------------------------- *
 * The guest renders into a buffer IT allocated (a gbm_surface front buffer, the
 * path lima is designed for) and writes that buffer's physical address here; the
 * HV repoints the DE2 guest-window layer at it. Zero copy, and it sidesteps the
 * dead end of rendering into an HV-allocated buffer, which lima writes exactly
 * once and then ignores -- see bsdOS/hal/bzfb/tests/README-zerocopy.md.
 *
 * THE ADDRESS IS NOT TRUSTED. A display engine is a DMA reader with no IOMMU in
 * front of it, so an unchecked address here would let the guest put hypervisor
 * memory on the screen -- an information leak, not a crash, which is the kind
 * that goes unnoticed. scanout.c validates that the WHOLE extent
 * (stride * height) lies inside guest DRAM and touches none of hv-image,
 * hv-scratch or hv-fb, and rejects anything else into GUESTWIN_REJECT. */
#define SCANOUT_R_GUESTWIN_ADDR   0x30u  /* W: physical base to display */
#define SCANOUT_R_GUESTWIN_COUNT  0x34u  /* R: accepted presents */
#define SCANOUT_R_GUESTWIN_REJECT 0x38u  /* R: rejected addresses */

#define SCANOUT_MAGIC          0x53434e41u   /* "SCAN", MSB-first, same
                                               * spelling convention as every
                                               * other magic in this tree
                                               * (see hdmi.h's BC_HDMI_MAGIC) */
#define SCANOUT_VERSION        1u
#define SCANOUT_FORMAT_XRGB8888 0u

/* One-time setup: populates the register file's read-only fields from
 * hv_addrmap.h's HVMAP_FB_BUF0_BASE/BUF1_BASE/BUF_W/BUF_H/BUF_STRIDE and
 * hdmi.c's own hdmi_width()/hdmi_height()/hdmi_stride() accessors (so a
 * mismatch between the two — the exact class of bug this task's own hard
 * constraints asked to guard against — fails loudly instead of silently
 * lying to the guest), sets FRONT_INDEX=0/FLIP_COUNT=0/REJECT_COUNT=0
 * (buffer 0 is exactly what stage_de2() already programmed at boot), and
 * writes the SCAN breadcrumb (hv_addrmap.h HVMAP_SCANOUT_BC). Call once,
 * after hdmi_init() — see main_dbg.c's existing `#ifdef HV_HDMI` block. */
void scanout_init(void);

/* el2_trap dispatch entry. Same contract as vblk_mmio_fault()/
 * vnet_mmio_fault(): called on every guest data abort (EC==0x24); returns 1
 * and advances ELR past the faulting instruction if the IPA was inside
 * [SCANOUT_MMIO_BASE, SCANOUT_MMIO_BASE+SCANOUT_MMIO_SIZE) (handled), 0
 * otherwise (not our window — caller tries the next handler / falls through
 * to the generic fault record). Safe to call unconditionally on every
 * abort. */
int scanout_mmio_fault(struct el2_frame *frame);

/* Direct C entry point performing EXACTLY what a FLIP_REQUEST MMIO write
 * does (validate index, reprogram DE2 via hdmi_set_scanout_addr(), bump
 * FLIP_COUNT/REJECT_COUNT, update the breadcrumb) — WITHOUT needing a guest
 * or a trap at all. Exists so a human doing the hardware verification this
 * task cannot do itself can exercise the real flip mechanism (DE2 register
 * writes, on the real monitor) via dbgmon/hvdbg.py's existing `call`
 * command, e.g. `hv.call(<&scanout_flip>, 1)`, entirely independent of
 * whether the guest-side driver exists yet. Returns 1 if the flip happened,
 * 0 if `index` was rejected (not 0 or 1, or already the front buffer). */
int scanout_flip(uint32_t index);

#endif /* BZDOS_SCANOUT_H */
