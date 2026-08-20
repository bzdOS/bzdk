/* SPDX-License-Identifier: BSD-2-Clause */

/* hdmi.h — HDMI display pipeline bring-up for the bzdOS microkernel
 * (Allwinner A64 / Banana Pi M64). Scans out a linear XRGB8888 framebuffer
 * from fixed DRAM to an HDMI monitor at a fixed, forced 1280x720@60 mode.
 *
 * This is v1 of the hypervisor HUD foundation: the guest OS will eventually
 * be composited into a window on this framebuffer, surrounded by live
 * diagnostic overlays (fb.h/fb.c own the drawing side; this file owns
 * getting pixels out of a DRAM buffer and onto the wire).
 *
 * Pipeline (Allwinner DE2 generation, A64/H5-class SoC):
 *
 *   DRAM framebuffer --DMA--> DE2 mixer1 (one RGB "UI" layer, full screen)
 *       --> TCON1 (timing controller, generates h/vsync + blanking)
 *       --> DesignWare HDMI TX controller + Allwinner analog PHY --> HDMI
 *
 * Ported from the KNOWN-GOOD Allwinner A64 U-Boot driver sources (this is
 * the same silicon family, same driver, same register layout — NOT
 * reverse-engineered from a generic datasheet):
 *   /opt/bzdos/build/u-boot/drivers/video/sunxi/sunxi_de2.c       (DE2 mixer)
 *   /opt/bzdos/build/u-boot/drivers/video/sunxi/lcdc.c            (TCON)
 *   /opt/bzdos/build/u-boot/drivers/video/sunxi/sunxi_dw_hdmi.c   (HDMI+PHY)
 *   /opt/bzdos/build/u-boot/drivers/video/dw_hdmi.c               (DW HDMI core)
 *   /opt/bzdos/build/u-boot/arch/arm/include/asm/arch-sunxi/{cpu_sun4i,
 *       clock_sun6i,display2,lcdc}.h                              (bases/bits)
 * File:line citations are next to each constant/sequence in hdmi.c.
 *
 * v1 SCOPE (explicitly, per the bring-up plan):
 *   - NO EDID/DDC-I2C negotiation. The mode below is FORCED regardless of
 *     what (if anything) is plugged in — later work can add EDID reads and
 *     mode negotiation on top of the same DW HDMI I2C-master block.
 *   - NO Hot-Plug-Detect gating: U-Boot's dw_hdmi_detect_hpd() would refuse
 *     to continue without a monitor signaling HPD; we skip that wait
 *     entirely and force the mode blind, so the breadcrumb tells us how far
 *     the pipeline got even with nothing plugged in.
 *   - Every step uses a BOUNDED wait (timer.h, never a naked spin-forever);
 *     hdmi_init() returns 0 (success getting through the whole sequence) or
 *     a negative stage number on the first bounded-wait timeout — the
 *     breadcrumb at BC_HDMI_BASE records exactly which stage and why even
 *     when there's no serial/monitor to look at.
 *
 * Freestanding: only <stdint.h>, no libc, no floats, MMU on (flat device
 * mapping already established by U-Boot per the same contract as
 * musb.c/emac.c), -mgeneral-regs-only.
 */
#ifndef BZDOS_HDMI_H
#define BZDOS_HDMI_H
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Fixed framebuffer location (DRAM), 32bpp XRGB8888.
 *
 * Layout note (per the bring-up brief): kernel @ 0x46000000, dtb @
 * 0x4a000000, stack/SP region @ 0x4c000000ish — the framebuffer sits at
 * 0x4d000000. At 1920x1080x4 = 8,294,400 bytes (~7.91 MiB) the buffer
 * spans 0x4d000000 .. 0x4d7e9000, which is clear of everything else in the
 * memory map: kernel segments 0x46000000..0x47146000, DTB 0x4a000000,
 * modinfo 0x4a100000, SP_EL1 0x4c000000 — all below the framebuffer base —
 * and the next round 0x01000000 boundary (0x4e000000) is ~8.09 MiB above
 * the base, comfortably past the buffer's end.
 * ------------------------------------------------------------------ */
#define HDMI_FB_BASE     0x4D000000UL

/* ------------------------------------------------------------------ *
 * Fixed video mode: 1280x720@60, CEA-861 mode 4 (VIC 4) by default. Pixel
 * clock 74.25 MHz (TMDS 742.5 Mbps). Sync polarities both POSITIVE
 * (active-high) per the CEA-861 mode-4 spec.
 *
 * WHY 720p not 1080p (2026-07-25): the 1080p bump (148.5 MHz, PHY PLL
 * divider path "2") never locked the DWC-HDMI PHY on real hardware — read
 * live under HV_HDMI, PHY_STATUS bit7 (lock) stayed 0 while DE2+TCON1 were
 * up, i.e. hdmi_init() timed out at the PHY stage and NO signal reached the
 * monitor. 720p (74.25 MHz, divider path "4") is the timing this driver was
 * actually "confirmed working to scanout stage 6 on the physical monitor".
 *
 * 2026-08-20 follow-up (docs/hdmi-1080p-phy.md has the full writeup): a
 * register-level audit of stage_phy()/phy_set()'s "case 2" branch (the
 * bucket 74.25 < pixclk <= 148.5 MHz selects) against the mainline Linux
 * sun4i DRM driver (drivers/gpu/drm/sun4i/sun8i_hdmi_phy.c, same PHY
 * register block, symbolic field names) found the ported magic constants
 * decode to an EXACT, bit-for-bit match of upstream's validated 148.5 MHz
 * analog-tuning table — if anything, a closer match than "case 4"'s own
 * numbers (the ones 720p already uses successfully) get to *their* bucket.
 * That is evidence AGAINST a fixable register-recipe bug in this file for
 * 1080p specifically; the doc lays out what was checked and why a real
 * hardware/board-level bandwidth limit at the doubled 1.485 Gbps TMDS rate
 * remains the leading open explanation. NOT re-verified on real hardware
 * since — this file cannot touch the board itself.
 *
 * Given that, the correct CEA-861 mode-16 (1920x1080@60) timing is wired up
 * here as an EXPLICIT OPT-IN: build with `-DHDMI_MODE_1080P` to select it.
 * Building WITHOUT that define (the default, and every existing build
 * target) reproduces the 720p constants below byte-for-byte — this block
 * is not a behavioural change to the hardware-verified path. Framebuffer
 * 1280x720x4 = 3.7 MiB fits the 8 MiB hv-fb reservation in both cases
 * (1920x1080x4 = 7.9 MiB also still fits, see HDMI_FB_BASE's own comment).
 * ------------------------------------------------------------------ */
#if defined(HDMI_MODE_1080P)

/* CEA-861 mode 16 (VIC 16): 1920x1080@60. Pixel clock 148.5 MHz (TMDS
 * 1.485 Gbps). Sync polarities both POSITIVE, same as the 720p mode.
 * UNTESTED on real hardware — see docs/hdmi-1080p-phy.md before enabling
 * this on the board; hdmi_init() will latch HDMI_STAGE_TIMEOUT at
 * HDMI_STAGE_PHY in the breadcrumb (BC_HDMI_BASE, word[4]) if the PHY
 * still doesn't lock. */
#define HDMI_MODE_HACTIVE      1920
#define HDMI_MODE_HFRONT_PORCH   88
#define HDMI_MODE_HSYNC_LEN      44
#define HDMI_MODE_HBACK_PORCH   148
#define HDMI_MODE_HTOTAL       2200  /* 1920+88+44+148 */

#define HDMI_MODE_VACTIVE      1080
#define HDMI_MODE_VFRONT_PORCH    4
#define HDMI_MODE_VSYNC_LEN       5
#define HDMI_MODE_VBACK_PORCH    36
#define HDMI_MODE_VTOTAL       1125  /* 1080+4+5+36 */

#define HDMI_MODE_PIXEL_CLOCK_HZ 148500000UL /* 2200*1125*60 = 148,500,000 */
#define HDMI_MODE_HSYNC_ACTIVE_HIGH 1
#define HDMI_MODE_VSYNC_ACTIVE_HIGH 1

#else /* !HDMI_MODE_1080P -- the default, hardware-verified path */

#define HDMI_MODE_HACTIVE      1280
#define HDMI_MODE_HFRONT_PORCH  110
#define HDMI_MODE_HSYNC_LEN      40
#define HDMI_MODE_HBACK_PORCH   220
#define HDMI_MODE_HTOTAL       1650  /* 1280+110+40+220 */

#define HDMI_MODE_VACTIVE       720
#define HDMI_MODE_VFRONT_PORCH    5
#define HDMI_MODE_VSYNC_LEN       5
#define HDMI_MODE_VBACK_PORCH    20
#define HDMI_MODE_VTOTAL        750  /* 720+5+5+20 */

#define HDMI_MODE_PIXEL_CLOCK_HZ 74250000UL /* 1650 * 750 * 60 = 74,250,000 */
#define HDMI_MODE_HSYNC_ACTIVE_HIGH 1
#define HDMI_MODE_VSYNC_ACTIVE_HIGH 1

#endif /* HDMI_MODE_1080P */

/* Bring up the full display pipeline (CCU clocks/PLL_VIDEO -> DE2 mixer ->
 * TCON1 -> DW HDMI controller -> Allwinner HDMI PHY -> scanout enable) and
 * force the fixed mode above onto the wire, no EDID/HPD gating. Every wait
 * is bounded (timer.h) — hdmi_init() NEVER hangs. Writes progress into the
 * breadcrumb at BC_HDMI_BASE (see below) at every stage, BEFORE attempting
 * the next one, so `md.l 0x50003000` after a run shows how far it got even
 * with the pipeline stuck or no monitor attached.
 *
 * Always returns 0 — including when a bounded wait times out. This is a
 * "does anything show up" bring-up, not a hot-pluggable driver with error
 * recovery: a stuck bounded wait (e.g. the PHY status bit never sets
 * because nothing is plugged in) is not treated as a hard failure that
 * aborts the sequence, it's recorded in the breadcrumb (word [1]=99, word
 * [4]=the stage that stalled) and the function presses on to the next
 * stage best-effort, then returns. The breadcrumb, not the return value,
 * is the primary diagnostic here. */
int hdmi_init(void);

/* Framebuffer accessors — the geometry actually programmed into the DE2
 * mixer / TCON1, so callers (fb.c, the future HUD compositor) never have to
 * duplicate the mode constants above. hdmi_fb() always returns the ORIGINAL
 * single buffer (HDMI_FB_BASE, == hv_addrmap.h's HVMAP_FB_BUF0_BASE) — this
 * is deliberate: hud.c and hdmi_demo() must keep working completely
 * unchanged whether or not anything ever calls hdmi_set_scanout_addr()
 * below. It is NOT necessarily the buffer currently being scanned out once
 * a flip has happened — see hdmi_scanout_addr(). */
uint32_t *hdmi_fb(void);
int hdmi_width(void);
int hdmi_height(void);
int hdmi_stride(void); /* pixels per scanline (== hdmi_width() in v1: no
                         * padding, tightly packed XRGB8888) */

/* ------------------------------------------------------------------ *
 * Zero-copy scanout flip primitive (ROADMAP: zero-copy GPU scanout — see
 * docs/zero-copy-scanout.md for the full design, evidence and the guest-
 * side HV-call contract this backs). This is the ONLY new HV-side entry
 * point that touches real DE2 hardware for a flip; scanout.c (the guest-
 * facing doorbell device) and any board-side manual test both funnel
 * through it, so there is exactly one place that knows how a scanout
 * address change is actually committed.
 * ------------------------------------------------------------------ */

/* Reprogram the DE2 mixer1 UI layer's scanout base address to the physical
 * address `pa` and commit it via the SAME double-buffer "apply" strobe
 * (DE_GLB_DBUFF) stage_de2()/stage_scanout()/hdmi_relock() already use after
 * every batch of mode-set register writes (hdmi.c:574,912,1021 — always
 * write-then-strobe, never left pending) — see docs/zero-copy-scanout.md for
 * why that register is believed to be a vblank-latched shadow commit rather
 * than an immediate one, and exactly what is NOT verified about that timing
 * without hardware.
 *
 * `pa` is used EXACTLY as given — no bounds/alignment check against hv-fb —
 * because this function is EL2-trusted-caller-only (scanout.c has already
 * validated the buffer index against its own fixed table before calling
 * this; a raw dbgmon/gdb `call` from a human tester is equally trusted).
 * Does not change PITCH/format/size — v1 requires every buffer flipped
 * between to share hdmi_stride()/hdmi_width()/hdmi_height() exactly (see
 * the design doc's "what the guest needs" section for why).
 *
 * Records the new address into breadcrumb word [8] (below) so a post-run
 * `md.l` shows the last-programmed scanout address independently of
 * scanout.c even being linked in. Cheap (two MMIO writes); safe to call
 * from CPU0 (guest-triggered) or CPU1 (a human's dbgmon `call`) — see the
 * design doc's concurrency note for why no additional lock is needed. */
void hdmi_set_scanout_addr(uint32_t pa);

/* The address last programmed by hdmi_set_scanout_addr(), or HDMI_FB_BASE
 * if it has never been called (matches what stage_de2() programmed at
 * boot). This is the buffer CURRENTLY being scanned out — unlike hdmi_fb(),
 * which always names buffer 0 regardless of what is live. */
uint32_t hdmi_scanout_addr(void);

/* Clear to a dark blue, draw a title bar, a border box (the future "guest
 * window"), and some sample hex/text (fake register readouts) using fb.h's
 * drawing API over the hdmi_fb() buffer. Confirms the whole pipeline +
 * drawing layer end to end once run in front of a monitor, and doubles as
 * the visual skeleton the HUD compositor will build on. Safe to call even
 * if hdmi_init() returned nonzero (it still draws into DRAM — you just
 * won't see it on a screen that never got scanned out to). */
void hdmi_demo(void);

/* Live PHY lock check: returns 1 if PHY_STATUS bit7 (lock) is currently set,
 * 0 otherwise. Cheap (one MMIO read) -- safe to poll from CPU1 every loop
 * iteration. */
int hdmi_phy_locked(void);

/* Post-boot PHY relock. Call this from CPU1 (the debug/display-owning core,
 * see smp.c) when hdmi_phy_locked() reports 0 sometime after a clean
 * hdmi_init() scanout, to force the analog HDMI PHY to re-lock without
 * re-running the whole pipeline.
 *
 * Live-board-confirmed (2026-07-25): after the guest boots and the monitor
 * goes dark, EVERY CCU/DE2/TCON/PHY-config register hdmi_init() programmed
 * (PLL_VIDEO0_CFG, AHB_GATE1, AHB_RESET1_CFG, DE_CLK_*, HDMI_CLK_CFG,
 * LCD1_CLK_CFG, TCON_CTRL, TCON1_CTRL, DE_GLB_CTL, PHY_CTRL/PLL/CLK/UNK1-3)
 * still reads back bit-for-bit identical to what was written at boot --
 * only PHY_STATUS's own lock bit has dropped. So this function's CCU/DE2/
 * TCON re-assertions are defensive belt-and-suspenders (SET-only, so they
 * can never clobber a guest-owned bit sharing the same 32-bit register);
 * the operation that actually restores the signal is redriving the analog
 * PHY bring-up sequence (phy_init()+phy_set()) using the same divider
 * hdmi_init() computed at boot. Bounded (~200ms worth of the same
 * timer-bounded waits hdmi_init() itself uses) -- never hangs. */
void hdmi_relock(void);

/* ------------------------------------------------------------------ *
 * Breadcrumb: fixed DRAM window at 0x50003000, magic "HDMI", so a post-run
 * `md.l 0x50003000` (from U-Boot, over the existing debug UART/USB path)
 * shows how far the pipeline got even with no monitor attached to look at.
 * Same coherent-store pattern as gtrace.c/timer.c: every word is written
 * then cleaned to the point of coherency (`dc civac` + `dsb sy`).
 *
 * Word layout (uint32_t, byte offsets from BC_HDMI_BASE):
 *   [0] magic        0x48444D49 ("HDMI", MSB-first hex the way the rest of
 *                     this codebase writes its magics — see timer.c TIMR)
 *   [1] stage         1=clocks-done, 2=DE2-done, 3=TCON-done,
 *                      4=HDMI-ctrl-done, 5=PHY-done, 6=scanout-on (final
 *                      success value), 99=timed out at the stage named in
 *                      word [4]
 *   [2] clk_readback  last PLL/clock-control register read back (which
 *                      register depends on the stage — see hdmi.c; always
 *                      the most recent one at the time of the write)
 *   [3] phy_status    last Allwinner HDMI PHY status register readback
 *                      (0 until stage 5 is reached)
 *   [4] timeout_stage  which stage (1..6) a bounded wait gave up in; 0 if
 *                      nothing has timed out (yet)
 *   [5] pixclk_khz    achieved PLL_VIDEO pixel clock, kHz (debug extra)
 *   [6] tcon1_div     TCON1/LCD1 module clock divider actually programmed
 *                      (debug extra)
 *   [7] relock_count  # times CPU1 (smp.c) observed PHY_STATUS bit7 drop
 *                      post-boot and called hdmi_relock() to restore it
 *                      (0 = never needed it yet)
 *   [8] scanout_addr  last physical address hdmi_set_scanout_addr() (below)
 *                      programmed into DE_UI1_CFG0_TOP_LADDR — added for
 *                      zero-copy scanout (docs/zero-copy-scanout.md); reads
 *                      back HDMI_FB_BASE until the first flip ever happens
 * ------------------------------------------------------------------ */
/* Relocated 2026-07-25 from 0x50003000 — that address sits INSIDE the
 * vconsole 64 KiB capture ring (0x50000f10..0x50010f10) and was being
 * clobbered by guest console bytes, so the pipeline stage/PHY breadcrumb was
 * unreadable post-boot. 0x50011800 is in the free gap 0x50011100..0x50020000
 * (past el2_ncmap 0x50011000, before flightrec 0x50012000). */
/* ── GUEST WINDOW: a second DE2 layer, composited by hardware ─────────────
 *
 * The HUD occupies UI1 layer 0 full-screen. The guest gets UI1 **layer 1**,
 * positioned over the HUD's own guest-window rectangle, reading from a buffer in
 * ORDINARY GUEST DRAM.
 *
 * Why a second layer and not a blit, and not a shared scanout buffer:
 *
 *   - ZERO COPY is a requirement. A per-frame memcpy of the window area is
 *     ~1.2 MiB at 4 bpp on a Cortex-A53; the mixer fetches it for free.
 *   - A WINDOW cannot be shared through stage-2 anyway. Stage-2 granularity is
 *     4 KiB and contiguous; a rectangle's scanlines are 5120 B apart, so
 *     "columns 16..1149 of rows 66..341" is not expressible as pages. Only
 *     full-width horizontal bands are.
 *   - And it does not need to be. The guest's buffer lives in its own DRAM,
 *     which stage-2 already identity-maps, so NOTHING about the isolation
 *     boundary changes: the guest never touches BUF0/BUF1, hv-image or
 *     hv-scratch. The DE2 mixer's own DMA reads the guest buffer, and DE2 is
 *     already a bus master with no IOMMU in front of it either way.
 *
 * A64 mixer1 is ui_num=1, vi_num=1 (Linux sun50i_a64_mixer1_cfg), and each UI
 * channel has FOUR layer configs at 0x20 stride (sun8i_ui_layer.h). The HUD uses
 * layer 0; layers 1..3 were sitting unused.
 *
 * GEOMETRY IS THE HUD'S TO DECIDE. These mirror hud.c's CON_* rectangle -- the
 * area its own comment calls the guest surface -- and must move with it. The DTB's
 * simple-framebuffer node must carry the SAME width/height/stride, or the guest
 * will draw at one geometry while the mixer fetches at another and the result is
 * skewed rather than obviously broken.
 *
 * The buffer address is guest DRAM chosen from a full-tree occupancy sweep:
 * 0x4B000000 sits in the gap between the guest's modinfo (0x4A100000) and its
 * SP_EL1 (0x4C000000), 14 MiB clear of that stack, which grows DOWN. It is
 * reserved `no-map` in the guest DTB so FreeBSD's allocator stays off it while
 * simplefb(4) maps it explicitly.
 */
#define HDMI_GUESTWIN_PA      0x4B000000UL   /* guest-fb@4b000000, DTB-reserved */
#define HDMI_GUESTWIN_SIZE    0x00200000UL   /* 2 MiB reservation (1.19 MiB used) */
#define HDMI_GUESTWIN_X       16             /* mirrors hud.c CON_X */
#define HDMI_GUESTWIN_Y       66             /* mirrors hud.c CON_Y */
/* hud.c's CON_W is 1134, but this is 1120 ON PURPOSE: lima refuses to import a
 * linear dma-buf whose stride is not 64-byte aligned --
 *   "linear imported buffer stride is smaller than minimal: 4536 (BO) < 4544 (min)"
 * followed by eglCreateImageKHR -> EGL_BAD_ALLOC. 1134*4 = 4536 is not a multiple
 * of 64; 1120*4 = 4480 is (70*64). Rounding DOWN keeps the window inside the HUD's
 * box, where rounding up to 1136 would overhang it by two pixels. */
#define HDMI_GUESTWIN_W       1120           /* <= hud.c CON_W (1134), 16px-aligned */
#define HDMI_GUESTWIN_H       276            /* mirrors hud.c CON_H */
#define HDMI_GUESTWIN_STRIDE  (HDMI_GUESTWIN_W * 4)
/* The constraint above, enforced rather than trusted to a comment. If the window
 * width changes to something whose stride is not 64-byte aligned, the build fails
 * here instead of the guest failing at eglCreateImageKHR with EGL_BAD_ALLOC. */
_Static_assert(HDMI_GUESTWIN_STRIDE % 64u == 0u,
               "guest-window stride must be 64-byte aligned or lima will refuse "
               "to import it as a linear dma-buf");
#define HDMI_GUESTWIN_LAYER   1              /* UI1 layer index (HUD owns 0) */

/* Program the guest-window layer and commit it. Returns 0 on success, -1 if the
 * display never came up (hdmi_init() failed), in which case nothing is touched.
 * Idempotent: safe to call again to move/resize the window. */
int hdmi_guestwin_enable(void);

/* Repoint the guest-window layer at a different physical buffer, and commit.
 *
 * This is what makes zero-copy presentation possible: the guest renders into a BO
 * that lima allocated (the path lima is actually designed for -- render to a
 * gbm_surface, eglSwapBuffers, lock the front buffer) and then tells the HV where
 * that buffer is, instead of trying to render into a buffer the HV allocated,
 * which lima writes exactly once and then ignores (see
 * bsdOS/hal/bzfb/tests/README-zerocopy.md for the nine experiments that
 * established that).
 *
 * `pa` is NOT trusted. The caller (scanout.c) validates it against guest DRAM and
 * the HV's own carve-outs before calling; this function only programs the layer.
 * Returns 0 on success, -1 if the display never came up. */
int hdmi_guestwin_set_addr(uint32_t pa);

#define BC_HDMI_BASE   0x50011800UL
#define BC_HDMI_MAGIC  0x48444D49u /* "HDMI" */

#define HDMI_STAGE_CLOCKS   1
#define HDMI_STAGE_DE2      2
#define HDMI_STAGE_TCON     3
#define HDMI_STAGE_HDMICTRL 4
#define HDMI_STAGE_PHY      5
#define HDMI_STAGE_SCANOUT  6
#define HDMI_STAGE_TIMEOUT  99

#endif /* BZDOS_HDMI_H */
