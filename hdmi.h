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
 * Fixed video mode: 1280x720@60, CEA-861 mode 4 (VIC 4). Pixel clock
 * 74.25 MHz (TMDS 742.5 Mbps). Sync polarities both POSITIVE (active-high)
 * per the CEA-861 mode-4 spec.
 *
 * WHY 720p not 1080p (2026-07-25): the 1080p bump (148.5 MHz, PHY PLL
 * divider path "2") never locked the DWC-HDMI PHY on real hardware — read
 * live under HV_HDMI, PHY_STATUS bit7 (lock) stayed 0 while DE2+TCON1 were
 * up, i.e. hdmi_init() timed out at the PHY stage and NO signal reached the
 * monitor. 720p (74.25 MHz, divider path "4") is the timing this driver was
 * actually "confirmed working to scanout stage 6 on the physical monitor".
 * Re-fixing the 1080p PHY PLL config is a separate follow-up; 720p is the
 * mode that produces a real signal today. Framebuffer 1280x720x4 = 3.7 MiB
 * still fits the 8 MiB hv-fb reservation.
 * ------------------------------------------------------------------ */
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
 * duplicate the mode constants above. */
uint32_t *hdmi_fb(void);
int hdmi_width(void);
int hdmi_height(void);
int hdmi_stride(void); /* pixels per scanline (== hdmi_width() in v1: no
                         * padding, tightly packed XRGB8888) */

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
 * ------------------------------------------------------------------ */
/* Relocated 2026-07-25 from 0x50003000 — that address sits INSIDE the
 * vconsole 64 KiB capture ring (0x50000f10..0x50010f10) and was being
 * clobbered by guest console bytes, so the pipeline stage/PHY breadcrumb was
 * unreadable post-boot. 0x50011800 is in the free gap 0x50011100..0x50020000
 * (past el2_ncmap 0x50011000, before flightrec 0x50012000). */
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
