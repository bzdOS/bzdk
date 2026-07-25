/* hdmi.c — HDMI display pipeline bring-up for the bzdOS microkernel
 * (Allwinner A64 / Banana Pi M64). See hdmi.h for the API, scope (v1: no
 * EDID/HPD, fixed forced mode, always-bounded waits) and the breadcrumb
 * layout.
 *
 * Every register base, bitfield and bring-up SEQUENCE below is ported from
 * the known-good Allwinner A64 U-Boot sources (this is the same silicon,
 * same DE2-generation display engine, same DesignWare HDMI IP — not a
 * generic-datasheet guess):
 *
 *   CCU bases/bits:
 *     u-boot/arch/arm/include/asm/arch-sunxi/cpu_sun4i.h
 *       SUNXI_CCM_BASE   = 0x01c20000  (line 54)
 *       SUNXI_DE2_BASE   = 0x01000000  (line 31)
 *       SUNXI_LCD0_BASE  = 0x01c0c000  (line 43) -- TCON0
 *       SUNXI_LCD1_BASE  = 0x01c0d000  (line 44) -- TCON1 (HDMI uses this)
 *       SUNXI_HDMI_BASE  = 0x01ee0000  (line 144, A64/H3-H5 branch)
 *       SUNXI_SRAMC_BASE = 0x01c00000  (line 37)
 *     u-boot/arch/arm/include/asm/arch-sunxi/clock_sun6i.h
 *       struct sunxi_ccm_reg field offsets (pll3_cfg=0x10 "PLL_VIDEO0",
 *       pll10_cfg=0x48, de_clk_cfg=0x104, lcd1_clk_cfg=0x11c,
 *       hdmi_clk_cfg=0x150, hdmi_slow_clk_cfg=0x154, ahb_gate1=0x64,
 *       ahb_reset1_cfg=0x2c4) and the AHB_GATE_OFFSET_/AHB_RESET_OFFSET_/
 *       CCM_ bit definitions (lines ~220-534), CONFIG_SUNXI_DE2
 *       branch (A64 is DE2-generation).
 *   DE2 mixer:
 *     u-boot/drivers/video/sunxi/sunxi_de2.c
 *       sunxi_de2_composer_init()  lines 33-59  (PLL10 432MHz, DE ahb/gate)
 *       sunxi_de2_mode_set()       lines 61-178 (mux/glb/bld/ui/csc regs)
 *     u-boot/arch/arm/include/asm/arch-sunxi/display2.h
 *       struct de_glb/de_bld/de_ui/de_csc layouts, SUNXI_DE2_MUX1_BASE,
 *       SUNXI_DE2_MUX_*_REGS offsets, SUNXI_DE2_FORMAT_XRGB_8888=4.
 *   TCON1:
 *     u-boot/drivers/video/sunxi/lcdc.c
 *       lcdc_init()             lines 30-40
 *       lcdc_tcon1_mode_set()   lines 158-207
 *       lcdc_enable()           lines 43-65 (non-LVDS path)
 *     u-boot/arch/arm/include/asm/arch-sunxi/lcdc.h
 *       struct sunxi_lcdc_reg field offsets (tcon1_ctrl=0x90 .. 0xa8),
 *       SUNXI_LCDC_TCON1_ / SUNXI_LCDC_X/Y macros.
 *   DW HDMI controller + Allwinner PHY:
 *     u-boot/drivers/video/sunxi/sunxi_dw_hdmi.c
 *       sunxi_dw_hdmi_phy_init()  lines 64-119 (verbatim Allwinner BSP
 *         magic numbers -- "There is no documentation", their words)
 *       sunxi_dw_hdmi_phy_set()   lines 121-194 (per-divider magic numbers)
 *       sunxi_dw_hdmi_pll_set()   lines 196-236 (PLL_VIDEO/pll3 search)
 *       sunxi_dw_hdmi_lcdc_init() lines 238-271 (LCD1 clock div + TCON1)
 *       sunxi_dw_hdmi_probe()     lines 330-367 (pll3->297MHz, hdmi clk
 *         parent select, AHB_RESET_OFFSET_HDMI2 bit)
 *     u-boot/drivers/video/dw_hdmi.c (generic DW HDMI core)
 *       hdmi_av_composer()        lines 458-524 (FC_IN.../timing registers)
 *       hdmi_video_packetize()    lines 253-297
 *       hdmi_video_sample()       lines 186-251 (RGB888 -> color_format 1)
 *       hdmi_enable_video_path()  lines 701-744 (MC_CLKDIS/FLOWCTRL)
 *       hdmi_clear_overflow()     lines 747-758
 *       dw_hdmi_init()            lines 1032-1067 (IH_MUTE, i2cm irq masks)
 *     u-boot/include/dw_hdmi.h    register offsets + bitfield enums.
 *
 * v1 DEVIATIONS from the U-Boot sequence (deliberate, see hdmi.h):
 *   - dw_hdmi_detect_hpd()'s 300ms wait-for-HPD is SKIPPED entirely: we
 *     force the mode blind, we don't refuse to proceed without a monitor.
 *   - No EDID read, no I2C-DDC.
 *   - No audio path (hdmi_audio_*), no CSC (source is already RGB888, no
 *     format conversion needed for a plain framebuffer).
 *   - No AVI infoframe (documented as a later step, needed for correct
 *     colorimetry/aspect-ratio signaling on picky TVs but not required for
 *     a monitor to sync at all).
 *
 * Freestanding: <stdint.h> + "hdmi.h" + "timer.h" only. No libc, no FP,
 * -mgeneral-regs-only. MMIO via absolute volatile pointers (MMU already on
 * with U-Boot's flat device mapping, same contract as musb.c/emac.c).
 */
#include <stdint.h>
#include "hdmi.h"
#include "timer.h"
#include "rsb.h"    /* re-enable the PHY's dldo1 supply FreeBSD gates off */

/* ==================================================================== *
 * MMIO helpers
 * ==================================================================== */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void set32(uintptr_t a, uint32_t bits) { wr32(a, rd32(a) | bits); }
static inline void clr32(uintptr_t a, uint32_t bits) { wr32(a, rd32(a) & ~bits); }
static inline void clrset32(uintptr_t a, uint32_t mask, uint32_t bits)
{
	wr32(a, (rd32(a) & ~mask) | bits);
}

/* DW HDMI controller registers are byte-addressed (reg_io_width=1 in the
 * U-Boot driver: sunxi_dw_hdmi.c line 381) -- one byte per register. */
static inline uint8_t hrd8(uintptr_t a) { return *(volatile uint8_t *)a; }
static inline void hwr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

/* ==================================================================== *
 * Breadcrumb 0x50003000 "HDMI" -- cache-coherent store pattern shared with
 * gtrace.c/timer.c: *p = v; dc civac, p; dsb sy.
 * ==================================================================== */
static inline void bc_write(int idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(BC_HDMI_BASE + (uint32_t)idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static int g_timeout_latched = 0; /* first stage that timed out, 0 = none */

/* Stashed away by stage_tcon()/hdmi_init() so a later hdmi_relock() (CPU1,
 * post-boot PHY lock-loss defense -- see hdmi.h) can redrive the PHY without
 * recomputing anything: both dividers are fixed for the life of the boot
 * (v1 has exactly one forced mode, never renegotiated). */
static uint32_t g_tcon1_div = 1;
static uint32_t g_phy_div = 1;
static uint32_t g_relock_count = 0;

static void bc_stage(int stage)
{
	bc_write(0, BC_HDMI_MAGIC);
	bc_write(1, (uint32_t)stage);
}

static void bc_clk_readback(uint32_t v) { bc_write(2, v); }
static void bc_phy_status(uint32_t v)   { bc_write(3, v); }

/* Called the moment a bounded wait in `stage` gives up. Marks word[1]=99
 * transiently (so a crash/reset happening in the next few instructions
 * still shows "stalled" rather than silently looking like clean progress)
 * and latches word[4] to the FIRST stage that ever stalls -- later stages
 * still run best-effort and will overwrite word[1] with their own stage
 * number on completion, so a full post-run dump shows both "got all the
 * way to stage N" AND "stage M's wait never confirmed" simultaneously. */
static void bc_timeout(int stage)
{
	bc_write(0, BC_HDMI_MAGIC);
	bc_write(1, (uint32_t)HDMI_STAGE_TIMEOUT);
	if (g_timeout_latched == 0) {
		g_timeout_latched = stage;
		bc_write(4, (uint32_t)stage);
	}
}

/* Bounded poll: returns 1 if (rd32(addr) & mask) == want within timeout_us,
 * 0 on timeout. Never spins unboundedly -- timer.h's monotonic counter is
 * the bound, not an iteration count, so it's correct regardless of core
 * clock/pipeline changes. */
static int wait_bits(uintptr_t addr, uint32_t mask, uint32_t want, uint32_t timeout_us)
{
	uint64_t start = timer_now();
	for (;;) {
		if ((rd32(addr) & mask) == want)
			return 1;
		if (timer_us(timer_now() - start) >= timeout_us)
			return 0;
	}
}

/* ==================================================================== *
 * Physical bases (all cited above; repeated here as the single source of
 * truth this file actually compiles against).
 * ==================================================================== */
#define CCU_BASE        0x01C20000UL
#define SRAMC_BASE      0x01C00000UL
#define DE2_BASE        0x01000000UL
#define DE2_MUX1_BASE   (DE2_BASE + 0x200000UL) /* mux1 == the path routed
                                                  * to HDMI on non-H3/H5
                                                  * DE2 SoCs (A64 included) --
                                                  * sunxi_de2.c line 262 */
#define TCON1_BASE      0x01C0D000UL /* SUNXI_LCD1_BASE */
#define HDMI_CTRL_BASE  0x01EE0000UL
#define HDMI_PHY_BASE   (HDMI_CTRL_BASE + 0x10000UL) /* sunxi_dw_hdmi.c:45 */

/* ---- CCU register offsets (clock_sun6i.h struct sunxi_ccm_reg) -------- */
#define CCU_PLL_VIDEO0_CFG  0x10U  /* pll3_cfg -- "PLL_VIDEO0" */
#define CCU_PLL10_CFG       0x48U  /* pll10_cfg -- fixed 432MHz DE2 clock */
#define CCU_AHB_GATE1       0x64U
#define CCU_DE_CLK_CFG      0x104U
#define CCU_LCD1_CLK_CFG    0x11CU
#define CCU_HDMI_CLK_CFG    0x150U
#define CCU_HDMI_SLOW_CLK   0x154U
#define CCU_AHB_RESET1_CFG  0x2C4U

/* ---- bitfields (clock_sun6i.h, CONFIG_SUNXI_DE2 branch = A64) --------- */
#define AHB_GATE_OFFSET_DE     12
#define AHB_GATE_OFFSET_HDMI   11
#define AHB_GATE_OFFSET_LCD1   4
#define AHB_RESET_OFFSET_DE    12
#define AHB_RESET_OFFSET_HDMI  11
#define AHB_RESET_OFFSET_HDMI2 10 /* PHY-only reset, DT-referenced */
#define AHB_RESET_OFFSET_LCD1  4

#define CCM_PLL3_M(n)        ((((n) - 1) & 0xFu) << 0)
#define CCM_PLL3_N(n)        ((((n) - 1) & 0x7Fu) << 8)
#define CCM_PLL3_INTEGER     (1u << 24)
#define CCM_PLL3_LOCK        (1u << 28)
#define CCM_PLL3_EN          (1u << 31)

#define CCM_PLL10_M(n)       ((((n) - 1) & 0xFu) << 0)
#define CCM_PLL10_N(n)       ((((n) - 1) & 0x7Fu) << 8)
#define CCM_PLL10_INTEGER    (1u << 24)
#define CCM_PLL10_LOCK       (1u << 28)
#define CCM_PLL10_EN         (1u << 31)

#define CCM_DE2_PLL_MASK     (3u << 24)
#define CCM_DE2_PLL10        (1u << 24)
#define CCM_DE2_GATE         (1u << 31)

#define CCM_HDMI_PLL_MASK    (3u << 24)
#define CCM_HDMI_PLL3        (0u << 24)
#define CCM_HDMI_GATE        (1u << 31)
#define CCM_HDMI_SLOW_DDC_GATE (1u << 31)

#define CCM_LCD1_GATE        (1u << 31)
#define CCM_LCD1_M(n)        ((((n) - 1) & 0xFu) << 0)

/* ---- DE2 mixer1 register block offsets (display2.h struct layouts, byte
 * offsets computed from the actual struct field order) ------------------ */
#define DE_CLK_GATE_CFG  (DE2_BASE + 0x00) /* struct de_clk @ DE2_BASE      */
#define DE_CLK_BUS_CFG   (DE2_BASE + 0x04)
#define DE_CLK_RST_CFG   (DE2_BASE + 0x08)
#define DE_CLK_SEL_CFG   (DE2_BASE + 0x10)

#define DE_GLB_CTL       (DE2_MUX1_BASE + 0x0000)
#define DE_GLB_STATUS    (DE2_MUX1_BASE + 0x0004)
#define DE_GLB_DBUFF     (DE2_MUX1_BASE + 0x0008)
#define DE_GLB_SIZE      (DE2_MUX1_BASE + 0x000C)

#define DE_BLD_BASE          (DE2_MUX1_BASE + 0x1000)
#define DE_BLD_FCOLOR_CTL    (DE_BLD_BASE + 0x00)
#define DE_BLD_ATTR0_FCOLOR  (DE_BLD_BASE + 0x04)
#define DE_BLD_ATTR0_INSIZE  (DE_BLD_BASE + 0x08)
#define DE_BLD_ROUTE         (DE_BLD_BASE + 0x80)
#define DE_BLD_PREMULTIPLY   (DE_BLD_BASE + 0x84)
#define DE_BLD_BKCOLOR       (DE_BLD_BASE + 0x88)
#define DE_BLD_OUTPUT_SIZE   (DE_BLD_BASE + 0x8C)
#define DE_BLD_MODE0         (DE_BLD_BASE + 0x90)
#define DE_BLD_CK_CTL        (DE_BLD_BASE + 0xB0)
#define DE_BLD_OUT_CTL       (DE_BLD_BASE + 0xFC)

#define DE_CHAN_REGS_BASE   (DE2_MUX1_BASE + 0x2000)
#define DE_CHAN_SZ          0x1000U
#define DE_CHAN_ZERO_SPAN   0x100U /* covers struct de_vi (largest of the
                                    * per-channel layouts), so zeroing this
                                    * many bytes at each channel's base
                                    * matches U-Boot's memset(ch, 0,
                                    * sizeof(de_vi/de_ui)) for every slot */

/* the UI layer we actually use is channel index 1 (a de_ui overlay),
 * exactly like sunxi_de2_mode_set()'s de_ui_regs pointer */
#define DE_UI1_BASE       (DE_CHAN_REGS_BASE + DE_CHAN_SZ * 1u)
#define DE_UI1_CFG0_ATTR      (DE_UI1_BASE + 0x00)
#define DE_UI1_CFG0_SIZE      (DE_UI1_BASE + 0x04)
#define DE_UI1_CFG0_COORD     (DE_UI1_BASE + 0x08)
#define DE_UI1_CFG0_PITCH     (DE_UI1_BASE + 0x0C)
#define DE_UI1_CFG0_TOP_LADDR (DE_UI1_BASE + 0x10)
#define DE_UI1_OVL_SIZE       (DE_UI1_BASE + 0x88)

#define DE_VSU_REGS   (DE2_MUX1_BASE + 0x20000)
#define DE_GSU1_REGS  (DE2_MUX1_BASE + 0x30000)
#define DE_GSU2_REGS  (DE2_MUX1_BASE + 0x40000)
#define DE_GSU3_REGS  (DE2_MUX1_BASE + 0x50000)
#define DE_FCE_REGS   (DE2_MUX1_BASE + 0xA0000)
#define DE_BWS_REGS   (DE2_MUX1_BASE + 0xA2000)
#define DE_LTI_REGS   (DE2_MUX1_BASE + 0xA4000)
#define DE_PEAK_REGS  (DE2_MUX1_BASE + 0xA6000)
#define DE_ASE_REGS   (DE2_MUX1_BASE + 0xA8000)
#define DE_FCC_REGS   (DE2_MUX1_BASE + 0xAA000)
#define DE_DCSC_CTL   (DE2_MUX1_BASE + 0xB0000)

#define DE2_FORMAT_XRGB8888  4u
#define DE2_UI_ATTR_EN       (1u << 0)
#define DE2_UI_ATTR_FMT(f)   (((f) & 0xFu) << 8)
#define DE2_MUX_GLB_CTL_EN   (1u << 0)
#define DE2_WH(w, h)         ((((uint32_t)(h) - 1u) << 16) | ((uint32_t)(w) - 1u))

/* ---- TCON1 (lcdc.h struct sunxi_lcdc_reg field offsets) --------------- */
#define TCON_CTRL             (TCON1_BASE + 0x00)
#define TCON_INT0             (TCON1_BASE + 0x04)
#define TCON0_DCLK             (TCON1_BASE + 0x44)
#define TCON0_IO_TRISTATE      (TCON1_BASE + 0x8C)
#define TCON1_CTRL             (TCON1_BASE + 0x90)
#define TCON1_TIMING_SOURCE    (TCON1_BASE + 0x94)
#define TCON1_TIMING_SCALE     (TCON1_BASE + 0x98)
#define TCON1_TIMING_OUT       (TCON1_BASE + 0x9C)
#define TCON1_TIMING_H         (TCON1_BASE + 0xA0)
#define TCON1_TIMING_V         (TCON1_BASE + 0xA4)
#define TCON1_TIMING_SYNC      (TCON1_BASE + 0xA8)
#define TCON1_IO_TRISTATE      (TCON1_BASE + 0xF4)

#define TCON_CTRL_TCON_ENABLE      (1u << 31)
#define TCON0_DCLK_ENABLE_MASK     (0xFu << 28)
#define TCON1_CTRL_ENABLE          (1u << 31)
#define TCON1_CTRL_CLK_DELAY(n)    (((n) & 0x1Fu) << 4)
#define TCON1_H_BP(n)              (((n) - 1u) << 0)
#define TCON1_H_TOTAL(n)           (((n) - 1u) << 16)
#define TCON1_V_BP(n)              (((n) - 1u) << 0)
#define TCON1_V_TOTAL(n)           ((n) << 16)
#define LCDC_X(x)                  (((uint32_t)(x) - 1u) << 16)
#define LCDC_Y(y)                  (((uint32_t)(y) - 1u) << 0)

/* ---- DW HDMI controller registers (dw_hdmi.h, byte-addressed) -------- */
#define HDMI_IH_MUTE               0x01FFU
#define HDMI_TX_INVID0             0x0200U
#define HDMI_TX_INSTUFFING         0x0201U
#define HDMI_TX_GYDATA0            0x0202U
#define HDMI_TX_GYDATA1            0x0203U
#define HDMI_TX_RCRDATA0           0x0204U
#define HDMI_TX_RCRDATA1           0x0205U
#define HDMI_TX_BCBDATA0           0x0206U
#define HDMI_TX_BCBDATA1           0x0207U
#define HDMI_VP_PR_CD              0x0801U
#define HDMI_VP_STUFF              0x0802U
#define HDMI_VP_REMAP              0x0803U
#define HDMI_VP_CONF               0x0804U
#define HDMI_FC_INVIDCONF          0x1000U
#define HDMI_FC_INHACTV0           0x1001U
#define HDMI_FC_INHACTV1           0x1002U
#define HDMI_FC_INHBLANK0          0x1003U
#define HDMI_FC_INHBLANK1          0x1004U
#define HDMI_FC_INVACTV0           0x1005U
#define HDMI_FC_INVACTV1           0x1006U
#define HDMI_FC_INVBLANK           0x1007U
#define HDMI_FC_HSYNCINDELAY0      0x1008U
#define HDMI_FC_HSYNCINDELAY1      0x1009U
#define HDMI_FC_HSYNCINWIDTH0      0x100AU
#define HDMI_FC_HSYNCINWIDTH1      0x100BU
#define HDMI_FC_VSYNCINDELAY       0x100CU
#define HDMI_FC_VSYNCINWIDTH       0x100DU
#define HDMI_FC_CTRLDUR            0x1011U
#define HDMI_FC_EXCTRLDUR          0x1012U
#define HDMI_FC_EXCTRLSPAC         0x1013U
#define HDMI_FC_CH0PREAM           0x1014U
#define HDMI_FC_CH1PREAM           0x1015U
#define HDMI_FC_CH2PREAM           0x1016U
#define HDMI_PHY_STAT0             0x3004U
#define HDMI_PHY_MASK0             0x3006U
#define HDMI_PHY_I2CM_INT_ADDR     0x3027U
#define HDMI_PHY_I2CM_CTLINT_ADDR  0x3028U
#define HDMI_MC_CLKDIS             0x4001U
#define HDMI_MC_SWRSTZ             0x4002U
#define HDMI_MC_FLOWCTRL           0x4004U
#define HDMI_IH_PHY_STAT0          0x0104U
#define HDMI_I2CM_INT              0x7E05U
#define HDMI_I2CM_CTLINT           0x7E06U

#define HDMI_FC_INVIDCONF_HDCP_KEEPOUT_INACTIVE      0x00U
#define HDMI_FC_INVIDCONF_VSYNC_ACTIVE_HIGH           0x40U
#define HDMI_FC_INVIDCONF_HSYNC_ACTIVE_HIGH           0x20U
#define HDMI_FC_INVIDCONF_DE_ACTIVE_HIGH              0x10U
#define HDMI_FC_INVIDCONF_DVI_MODEZ_HDMI               0x08U
#define HDMI_FC_INVIDCONF_R_V_BLANK_OSC_ACTIVE_LOW     0x02U
#define HDMI_FC_INVIDCONF_PROGRESSIVE                  0x00U

#define HDMI_TX_INVID0_DE_GEN_DISABLE   0x00U
#define HDMI_TX_INVID0_MAP_RGB888       0x01U /* color_format 0x01 */
#define HDMI_TX_INSTUFFING_ALL          0x07U /* B/G/R stuffing enable */

#define HDMI_VP_PR_CD_VAL       0x00U /* color depth default (24bpp), no
                                       * pixel-repeat factor */
#define HDMI_VP_STUFF_PR        0x01U
#define HDMI_VP_STUFF_IDEFAULT  (1u << 5)
#define HDMI_VP_STUFF_PP        0x02U
#define HDMI_VP_STUFF_YCC422    0x04U
#define HDMI_VP_CONF_BYPASS_EN  0x40U
#define HDMI_VP_CONF_BYPASS_SEL 0x04U
#define HDMI_VP_CONF_OUT_SEL_BYPASS 0x03U
#define HDMI_VP_REMAP_YCC422_16BIT  0x00U

#define HDMI_MC_CLKDIS_PIXELCLK   0x01U
#define HDMI_MC_CLKDIS_TMDSCLK    0x02U
#define HDMI_MC_FLOWCTRL_CSC_BYPASS 0x00U
#define HDMI_MC_SWRSTZ_TMDSSWRST_REQ 0x02U

#define HDMI_IH_MUTE_WAKEUP    0x02U
#define HDMI_IH_MUTE_ALL       0x01U
#define HDMI_PHY_I2CM_INT_DONE_POL 0x08U
#define HDMI_PHY_I2CM_CTLINT_NAC_POL 0x80U
#define HDMI_PHY_I2CM_CTLINT_ARB_POL 0x08U
#define HDMI_PHY_HPD           0x02U
#define HDMI_IH_PHY_STAT0_HPD  0x01U

/* ---- Allwinner analog HDMI PHY (sunxi_dw_hdmi.c "struct sunxi_hdmi_phy",
 * byte offsets from HDMI_PHY_BASE; all WORD (32-bit) registers) --------- */
#define PHY_POL      (HDMI_PHY_BASE + 0x00)
#define PHY_READ_EN  (HDMI_PHY_BASE + 0x10)
#define PHY_UNSCRAMBLE (HDMI_PHY_BASE + 0x14)
#define PHY_CTRL     (HDMI_PHY_BASE + 0x20)
#define PHY_UNK1     (HDMI_PHY_BASE + 0x24)
#define PHY_UNK2     (HDMI_PHY_BASE + 0x28)
#define PHY_PLL      (HDMI_PHY_BASE + 0x2C)
#define PHY_CLK      (HDMI_PHY_BASE + 0x30)
#define PHY_UNK3     (HDMI_PHY_BASE + 0x34)
#define PHY_STATUS   (HDMI_PHY_BASE + 0x38)

/* ==================================================================== *
 * Stage 1 -- CCU: PLL10 (fixed 432MHz DE2 functional clock), PLL_VIDEO0/
 * pll3 (search for our pixel clock), gate + deassert-reset every block on
 * the path, select clock parents.
 * ==================================================================== */

/* Faithful port of sunxi_dw_hdmi_pll_set() (sunxi_dw_hdmi.c:196-236): find
 * the (m, n, div) triple that reproduces clk_khz most closely via
 * 24MHz * n / m / div, m,n bounded by the PLL3 field widths, div by the
 * PHY's supported dividers (1,2,4,11 -- sunxi_dw_hdmi_get_divider()). */
static uint32_t pll_video_pick(uint32_t clk_khz, int *out_div)
{
	int div, m, n;
	int best_m = 1, best_n = 1, best_div = 1;
	long best_diff = 0x0FFFFFFF;

	for (div = 1; div <= 16; div++) {
		long target = (long)clk_khz * div;
		if (target < 192000 || target > 912000)
			continue;
		for (m = 1; m <= 16; m++) {
			n = (m * (int)target) / 24000;
			if (n < 1 || n > 128)
				continue;
			long value = (24000L * n) / m / div;
			long diff = (long)clk_khz - value;
			if (diff < 0)
				diff = -diff;
			if (diff < best_diff) {
				best_diff = diff;
				best_m = m;
				best_n = n;
				best_div = div;
			}
		}
	}

	*out_div = best_div;

	wr32(CCU_BASE + CCU_PLL_VIDEO0_CFG,
	     CCM_PLL3_EN | CCM_PLL3_INTEGER | CCM_PLL3_N((uint32_t)best_n) |
	     CCM_PLL3_M((uint32_t)best_m));

	if (!wait_bits(CCU_BASE + CCU_PLL_VIDEO0_CFG, CCM_PLL3_LOCK, CCM_PLL3_LOCK, 5000))
		bc_timeout(HDMI_STAGE_CLOCKS);

	/* Actual achieved PLL_VIDEO0 rate, Hz (24MHz * n / m). */
	return (24000000u / (uint32_t)best_m) * (uint32_t)best_n;
}

static int stage_clocks(uint32_t *out_pll3_hz)
{
	bc_stage(HDMI_STAGE_CLOCKS);

	/* A64: give DE2 access to the video SRAM aperture (sunxi_de2.c:41-44,
	 * CONFIG_MACH_SUN50I branch). */
	clr32(SRAMC_BASE + 0x04, 1u << 24);

	/* PLL10 -> fixed 432MHz, the DE2 mixer's own functional clock (NOT
	 * the pixel clock -- sunxi_de2.c:47 clock_set_pll10(432000000), "6
	 * MHz steps to allow higher frequency for DE2", clock_sun6i.c:285-303
	 * m=2 steps). N = 432MHz / (24MHz/2) = 36. */
	wr32(CCU_BASE + CCU_PLL10_CFG,
	     CCM_PLL10_EN | CCM_PLL10_INTEGER | CCM_PLL10_N(36) | CCM_PLL10_M(2));
	if (!wait_bits(CCU_BASE + CCU_PLL10_CFG, CCM_PLL10_LOCK, CCM_PLL10_LOCK, 5000))
		bc_timeout(HDMI_STAGE_CLOCKS);

	/* DE parent = PLL10 (sunxi_de2.c:50-51). */
	clrset32(CCU_BASE + CCU_DE_CLK_CFG, CCM_DE2_PLL_MASK, CCM_DE2_PLL10);
	/* DE ahb reset off + gate on (sunxi_de2.c:54-55). */
	set32(CCU_BASE + CCU_AHB_RESET1_CFG, 1u << AHB_RESET_OFFSET_DE);
	set32(CCU_BASE + CCU_AHB_GATE1, 1u << AHB_GATE_OFFSET_DE);
	/* DE module clock on (sunxi_de2.c:58). */
	set32(CCU_BASE + CCU_DE_CLK_CFG, CCM_DE2_GATE);

	/* PLL_VIDEO0/pll3 for our pixel clock (sunxi_dw_hdmi_pll_set() is
	 * normally called per-mode from phy_set(); we call it here up front
	 * since v1 has exactly one fixed mode -- the divider it picks is also
	 * what the PHY needs later, so stash it via *out_div through the
	 * caller's pll3_hz output for stage_tcon()/stage_phy() to reuse). */
	int phy_div;
	uint32_t pll3_hz = pll_video_pick(HDMI_MODE_PIXEL_CLOCK_HZ / 1000u, &phy_div);
	*out_pll3_hz = pll3_hz;

	/* HDMI module clock parent = pll3 (sunxi_dw_hdmi.c:344-345). */
	clrset32(CCU_BASE + CCU_HDMI_CLK_CFG, CCM_HDMI_PLL_MASK, CCM_HDMI_PLL3);
	/* HDMI ahb reset off: bit11 (controller) + bit10 (PHY, DT-referenced
	 * -- sunxi_dw_hdmi.c:348) + gate on. */
	set32(CCU_BASE + CCU_AHB_RESET1_CFG,
	      (1u << AHB_RESET_OFFSET_HDMI) | (1u << AHB_RESET_OFFSET_HDMI2));
	set32(CCU_BASE + CCU_AHB_GATE1, 1u << AHB_GATE_OFFSET_HDMI);
	set32(CCU_BASE + CCU_HDMI_CLK_CFG, CCM_HDMI_GATE);
	/* HDMI "slow" (DDC/I2C) clock gate -- harmless with no EDID read yet,
	 * kept on for parity with the known-good sequence. */
	set32(CCU_BASE + CCU_HDMI_SLOW_CLK, CCM_HDMI_SLOW_DDC_GATE);

	/* LCD1/TCON1 ahb reset off + gate on now; the module clock DIVIDER
	 * depends on pll3_hz and is programmed in stage_tcon() (mirrors
	 * sunxi_dw_hdmi_lcdc_init(), sunxi_dw_hdmi.c:256-266). */
	set32(CCU_BASE + CCU_AHB_RESET1_CFG, 1u << AHB_RESET_OFFSET_LCD1);
	set32(CCU_BASE + CCU_AHB_GATE1, 1u << AHB_GATE_OFFSET_LCD1);

	bc_clk_readback(rd32(CCU_BASE + CCU_PLL_VIDEO0_CFG));
	bc_stage(HDMI_STAGE_CLOCKS);
	return phy_div;
}

/* ==================================================================== *
 * Stage 2 -- DE2 mixer1: one RGB "UI" layer, full screen, pointed at the
 * fixed framebuffer. Faithful port of sunxi_de2_mode_set(), mux=1,
 * is_composite=false, bpp=32 (sunxi_de2.c:61-178).
 * ==================================================================== */
static void stage_de2(void)
{
	uint32_t size = DE2_WH(HDMI_MODE_HACTIVE, HDMI_MODE_VACTIVE);
	int ch;

	/* per-mux clock enable bits on the shared de_clk block (mux index 1
	 * -- sunxi_de2.c:87-94). */
	set32(DE_CLK_RST_CFG, 1u << 1);
	set32(DE_CLK_GATE_CFG, 1u << 1);
	set32(DE_CLK_BUS_CFG, 1u << 1);
	clr32(DE_CLK_SEL_CFG, 1u << 0);

	wr32(DE_GLB_CTL, DE2_MUX_GLB_CTL_EN);
	wr32(DE_GLB_STATUS, 0);
	wr32(DE_GLB_DBUFF, 1);
	wr32(DE_GLB_SIZE, size);

	/* zero all 4 channel register blocks (sunxi_de2.c:101-106). */
	for (ch = 0; ch < 4; ch++) {
		uintptr_t base = DE_CHAN_REGS_BASE + DE_CHAN_SZ * (uint32_t)ch;
		uint32_t off;
		for (off = 0; off < DE_CHAN_ZERO_SPAN; off += 4)
			wr32(base + off, 0);
	}
	/* zero the blender register block (sunxi_de2.c:107). */
	{
		uint32_t off;
		for (off = 0; off < 0x100; off += 4)
			wr32(DE_BLD_BASE + off, 0);
	}

	wr32(DE_BLD_FCOLOR_CTL, 0x00000101);
	wr32(DE_BLD_ROUTE, 1);
	wr32(DE_BLD_PREMULTIPLY, 0);
	wr32(DE_BLD_BKCOLOR, 0xff000000u);
	wr32(DE_BLD_MODE0, 0x03010301);
	wr32(DE_BLD_OUTPUT_SIZE, size);
	wr32(DE_BLD_OUT_CTL, 0); /* progressive, not interlaced */
	wr32(DE_BLD_CK_CTL, 0);
	wr32(DE_BLD_ATTR0_FCOLOR, 0xff000000u);
	wr32(DE_BLD_ATTR0_INSIZE, size);

	/* disable every other functional unit (sunxi_de2.c:127-136). */
	wr32(DE_VSU_REGS, 0);
	wr32(DE_GSU1_REGS, 0);
	wr32(DE_GSU2_REGS, 0);
	wr32(DE_GSU3_REGS, 0);
	wr32(DE_FCE_REGS, 0);
	wr32(DE_BWS_REGS, 0);
	wr32(DE_LTI_REGS, 0);
	wr32(DE_PEAK_REGS, 0);
	wr32(DE_ASE_REGS, 0);
	wr32(DE_FCC_REGS, 0);

	/* no CSC: source is already RGB, is_composite=false (sunxi_de2.c:156). */
	wr32(DE_DCSC_CTL, 0);

	/* the one UI layer we actually scan out (sunxi_de2.c:169-174). */
	wr32(DE_UI1_CFG0_ATTR, DE2_UI_ATTR_EN | DE2_UI_ATTR_FMT(DE2_FORMAT_XRGB8888));
	wr32(DE_UI1_CFG0_SIZE, size);
	wr32(DE_UI1_CFG0_COORD, 0);
	wr32(DE_UI1_CFG0_PITCH, 4u * HDMI_MODE_HACTIVE); /* bytes/line, 32bpp */
	wr32(DE_UI1_CFG0_TOP_LADDR, (uint32_t)HDMI_FB_BASE);
	wr32(DE_UI1_OVL_SIZE, size);

	/* apply (double-buffer flip register, sunxi_de2.c:177). */
	wr32(DE_GLB_DBUFF, 1);

	bc_clk_readback(rd32(CCU_BASE + CCU_DE_CLK_CFG));
	bc_stage(HDMI_STAGE_DE2);
}

/* ==================================================================== *
 * Stage 3 -- TCON1: program the mode timings and enable the TCON. Port of
 * lcdc_init() + lcdc_tcon1_mode_set(..., ext_hvsync=false, is_composite=
 * false) + lcdc_enable(), matching exactly how sunxi_dw_hdmi_lcdc_init()
 * calls them for the HDMI path (lcdc.c:30-40, 158-207, 43-65).
 * ==================================================================== */
static void stage_tcon(uint32_t pll3_hz)
{
	uint32_t div = (pll3_hz + HDMI_MODE_PIXEL_CLOCK_HZ - 1u) / HDMI_MODE_PIXEL_CLOCK_HZ;
	uint32_t clk_delay, bp, total, htot_v;

	if (div < 1)
		div = 1;
	if (div > 16)
		div = 16;

	/* LCD1/TCON1 module clock divider (sunxi_dw_hdmi.c:264-265). */
	wr32(CCU_BASE + CCU_LCD1_CLK_CFG, CCM_LCD1_GATE | CCM_LCD1_M(div));

	/* lcdc_init(): disable, mask interrupts, tristate everything, and
	 * (shared silicon) make sure TCON0's dot clock is off (lcdc.c:30-40). */
	wr32(TCON_CTRL, 0);
	wr32(TCON_INT0, 0);
	clr32(TCON0_DCLK, TCON0_DCLK_ENABLE_MASK);
	wr32(TCON0_IO_TRISTATE, 0xFFFFFFFFu);
	wr32(TCON1_IO_TRISTATE, 0xFFFFFFFFu);

	/* lcdc_tcon1_mode_set(): clk_delay = min(30, vfp+vsync+vbp - 2) for
	 * tcon==1 (lcdc.c:17-28). */
	clk_delay = HDMI_MODE_VFRONT_PORCH + HDMI_MODE_VSYNC_LEN + HDMI_MODE_VBACK_PORCH;
	clk_delay = (clk_delay >= 2) ? (clk_delay - 2) : 0;
	if (clk_delay > 30)
		clk_delay = 30;

	wr32(TCON1_CTRL, TCON1_CTRL_ENABLE | TCON1_CTRL_CLK_DELAY(clk_delay));

	wr32(TCON1_TIMING_SOURCE, LCDC_X(HDMI_MODE_HACTIVE) | LCDC_Y(HDMI_MODE_VACTIVE));
	wr32(TCON1_TIMING_SCALE, LCDC_X(HDMI_MODE_HACTIVE) | LCDC_Y(HDMI_MODE_VACTIVE));
	wr32(TCON1_TIMING_OUT, LCDC_X(HDMI_MODE_HACTIVE) | LCDC_Y(HDMI_MODE_VACTIVE));

	bp = HDMI_MODE_HSYNC_LEN + HDMI_MODE_HBACK_PORCH;
	total = HDMI_MODE_HACTIVE + HDMI_MODE_HFRONT_PORCH + bp;
	wr32(TCON1_TIMING_H, TCON1_H_TOTAL(total) | TCON1_H_BP(bp));

	bp = HDMI_MODE_VSYNC_LEN + HDMI_MODE_VBACK_PORCH;
	htot_v = HDMI_MODE_VACTIVE + HDMI_MODE_VFRONT_PORCH + bp;
	htot_v *= 2; /* progressive: lcdc.c:189-190 doubles the v-total */
	wr32(TCON1_TIMING_V, TCON1_V_TOTAL(htot_v) | TCON1_V_BP(bp));

	wr32(TCON1_TIMING_SYNC, LCDC_X(HDMI_MODE_HSYNC_LEN) | LCDC_Y(HDMI_MODE_VSYNC_LEN));

	/* ext_hvsync=false for the HDMI path (sunxi_dw_hdmi.c:269): no
	 * external TCON1 io_polarity/tristate pin twiddling -- HDMI consumes
	 * the internal timing signals, not the parallel LCD pins. */

	/* lcdc_enable(): turn the TCON itself on (lcdc.c:45). */
	set32(TCON_CTRL, TCON_CTRL_TCON_ENABLE);

	g_tcon1_div = div;

	bc_clk_readback(rd32(CCU_BASE + CCU_LCD1_CLK_CFG));
	bc_write(6, div);
	bc_stage(HDMI_STAGE_TCON);
}

/* ==================================================================== *
 * Stage 4 -- DW HDMI controller: interrupt masks + video timing/format
 * registers (dw_hdmi_init() + hdmi_av_composer() + hdmi_video_packetize()
 * + hdmi_video_sample(), dw_hdmi.c various).
 * ==================================================================== */
static void stage_hdmi_ctrl(void)
{
	uint32_t hbl, vbl;

	/* dw_hdmi_init(): mute all interrupts, unmask i2c-master done/error
	 * (dw_hdmi.c:1032-1063) -- we don't use interrupts in v1, this just
	 * matches the known-good register state U-Boot leaves the core in. */
	hwr8(HDMI_CTRL_BASE + HDMI_IH_MUTE, HDMI_IH_MUTE_WAKEUP | HDMI_IH_MUTE_ALL);
	hwr8(HDMI_CTRL_BASE + HDMI_I2CM_INT, (uint8_t)~0x04u);
	hwr8(HDMI_CTRL_BASE + HDMI_I2CM_CTLINT, (uint8_t)~0x44u);

	/* hdmi_av_composer() (dw_hdmi.c:458-524): FC_INVIDCONF + timing regs.
	 * Our mode is progressive, HDMI (not DVI), sync polarities both
	 * active-high per the 720p60 CEA timing, DE active-high. */
	{
		uint32_t inv = HDMI_FC_INVIDCONF_HDCP_KEEPOUT_INACTIVE;
		inv |= HDMI_MODE_VSYNC_ACTIVE_HIGH ? HDMI_FC_INVIDCONF_VSYNC_ACTIVE_HIGH : 0;
		inv |= HDMI_MODE_HSYNC_ACTIVE_HIGH ? HDMI_FC_INVIDCONF_HSYNC_ACTIVE_HIGH : 0;
		inv |= HDMI_FC_INVIDCONF_DE_ACTIVE_HIGH;
		inv |= HDMI_FC_INVIDCONF_DVI_MODEZ_HDMI;
		inv |= HDMI_FC_INVIDCONF_R_V_BLANK_OSC_ACTIVE_LOW;
		inv |= HDMI_FC_INVIDCONF_PROGRESSIVE;
		hwr8(HDMI_CTRL_BASE + HDMI_FC_INVIDCONF, (uint8_t)inv);
	}

	hwr8(HDMI_CTRL_BASE + HDMI_FC_INHACTV1, (uint8_t)(HDMI_MODE_HACTIVE >> 8));
	hwr8(HDMI_CTRL_BASE + HDMI_FC_INHACTV0, (uint8_t)HDMI_MODE_HACTIVE);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_INVACTV1, (uint8_t)(HDMI_MODE_VACTIVE >> 8));
	hwr8(HDMI_CTRL_BASE + HDMI_FC_INVACTV0, (uint8_t)HDMI_MODE_VACTIVE);

	hbl = HDMI_MODE_HBACK_PORCH + HDMI_MODE_HFRONT_PORCH + HDMI_MODE_HSYNC_LEN;
	vbl = HDMI_MODE_VBACK_PORCH + HDMI_MODE_VFRONT_PORCH + HDMI_MODE_VSYNC_LEN;
	hwr8(HDMI_CTRL_BASE + HDMI_FC_INHBLANK1, (uint8_t)(hbl >> 8));
	hwr8(HDMI_CTRL_BASE + HDMI_FC_INHBLANK0, (uint8_t)hbl);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_INVBLANK, (uint8_t)vbl);

	hwr8(HDMI_CTRL_BASE + HDMI_FC_HSYNCINDELAY1, (uint8_t)(HDMI_MODE_HFRONT_PORCH >> 8));
	hwr8(HDMI_CTRL_BASE + HDMI_FC_HSYNCINDELAY0, (uint8_t)HDMI_MODE_HFRONT_PORCH);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_VSYNCINDELAY, (uint8_t)HDMI_MODE_VFRONT_PORCH);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_HSYNCINWIDTH1, (uint8_t)(HDMI_MODE_HSYNC_LEN >> 8));
	hwr8(HDMI_CTRL_BASE + HDMI_FC_HSYNCINWIDTH0, (uint8_t)HDMI_MODE_HSYNC_LEN);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_VSYNCINWIDTH, (uint8_t)HDMI_MODE_VSYNC_LEN);

	/* hdmi_video_packetize() (dw_hdmi.c:253-297): bypass the pixel
	 * repeater/YCC422 packer, straight RGB888 passthrough. */
	hwr8(HDMI_CTRL_BASE + HDMI_VP_PR_CD, HDMI_VP_PR_CD_VAL);
	hwr8(HDMI_CTRL_BASE + HDMI_VP_STUFF, HDMI_VP_STUFF_PR);
	hwr8(HDMI_CTRL_BASE + HDMI_VP_CONF, 0); /* PR disable, bypass select */
	hwr8(HDMI_CTRL_BASE + HDMI_VP_STUFF, HDMI_VP_STUFF_PR | HDMI_VP_STUFF_IDEFAULT);
	hwr8(HDMI_CTRL_BASE + HDMI_VP_REMAP, HDMI_VP_REMAP_YCC422_16BIT);
	hwr8(HDMI_CTRL_BASE + HDMI_VP_CONF, HDMI_VP_CONF_BYPASS_EN);
	hwr8(HDMI_CTRL_BASE + HDMI_VP_STUFF,
	     HDMI_VP_STUFF_PR | HDMI_VP_STUFF_IDEFAULT | HDMI_VP_STUFF_PP | HDMI_VP_STUFF_YCC422);
	hwr8(HDMI_CTRL_BASE + HDMI_VP_CONF, HDMI_VP_CONF_BYPASS_EN | HDMI_VP_CONF_OUT_SEL_BYPASS);

	/* hdmi_video_sample() (dw_hdmi.c:186-251): RGB888 -> color_format
	 * 0x01, no tx-stuffing data (DE-gated, so stuffing values don't
	 * matter for a full-screen active layer, but zero them for parity). */
	hwr8(HDMI_CTRL_BASE + HDMI_TX_INVID0, HDMI_TX_INVID0_DE_GEN_DISABLE | HDMI_TX_INVID0_MAP_RGB888);
	hwr8(HDMI_CTRL_BASE + HDMI_TX_INSTUFFING, HDMI_TX_INSTUFFING_ALL);
	hwr8(HDMI_CTRL_BASE + HDMI_TX_GYDATA0, 0);
	hwr8(HDMI_CTRL_BASE + HDMI_TX_GYDATA1, 0);
	hwr8(HDMI_CTRL_BASE + HDMI_TX_RCRDATA0, 0);
	hwr8(HDMI_CTRL_BASE + HDMI_TX_RCRDATA1, 0);
	hwr8(HDMI_CTRL_BASE + HDMI_TX_BCBDATA0, 0);
	hwr8(HDMI_CTRL_BASE + HDMI_TX_BCBDATA1, 0);

	bc_clk_readback(hrd8(HDMI_CTRL_BASE + HDMI_FC_INVIDCONF));
	bc_stage(HDMI_STAGE_HDMICTRL);
}

/* ==================================================================== *
 * Stage 5 -- Allwinner analog HDMI PHY bring-up + video-path clock enable.
 * The PHY sequence is UNDOCUMENTED Allwinner BSP magic, ported byte-for-
 * byte from sunxi_dw_hdmi_phy_init()/_phy_set() (sunxi_dw_hdmi.c:64-194) --
 * see the honest caveats in the report / hdmi.h.
 * ==================================================================== */
static void phy_init(void)
{
	wr32(PHY_CTRL, 0);
	set32(PHY_CTRL, 1u << 0);
	timer_delay_us(5);
	set32(PHY_CTRL, 1u << 16);
	set32(PHY_CTRL, 1u << 1);
	timer_delay_us(10);
	set32(PHY_CTRL, 1u << 2);
	timer_delay_us(5);
	set32(PHY_CTRL, 1u << 3);
	timer_delay_us(40);
	set32(PHY_CTRL, 1u << 19);
	timer_delay_us(100);
	set32(PHY_CTRL, 1u << 18);
	set32(PHY_CTRL, 7u << 4);

	/* Allwinner's own code "doesn't fail in case of timeout" here either
	 * -- we record it (breadcrumb) but keep going regardless, exactly
	 * matching their tolerance for a PHY that isn't reporting ready yet
	 * (e.g. nothing plugged in). */
	if (!wait_bits(PHY_STATUS, 0x80u, 0x80u, 2000))
		bc_timeout(HDMI_STAGE_PHY);

	set32(PHY_CTRL, 0xFu << 8);
	set32(PHY_CTRL, 1u << 7);

	wr32(PHY_PLL, 0x39dc5040u);
	wr32(PHY_CLK, 0x80084343u);
	timer_delay_us(10000);
	wr32(PHY_UNK3, 1);
	set32(PHY_PLL, 1u << 25);
	timer_delay_us(100000);
	{
		uint32_t tmp = (rd32(PHY_STATUS) & 0x1f800u) >> 11;
		set32(PHY_PLL, (1u << 31) | (1u << 30));
		set32(PHY_PLL, tmp);
	}
	wr32(PHY_CTRL, 0x01FF0F7Fu);
	wr32(PHY_UNK1, 0x80639000u);
	wr32(PHY_UNK2, 0x0F81C405u);

	/* enable register-offset "descrambling" so the controller's normal
	 * (non-PHY) register space reads/writes correctly (sunxi_dw_hdmi.c:
	 * 115-118). */
	wr32(PHY_READ_EN, 0x54524545u);
	wr32(PHY_UNSCRAMBLE, 0x42494E47u);
}

/* Divider selection mirrors sunxi_dw_hdmi_get_divider() (sunxi_dw_hdmi.c:
 * 47-62): the PHY "div" buckets are <=27MHz->11, <=74.25MHz->4,
 * <=148.5MHz->2, else->1. Our fixed 148.5MHz pixel clock lands exactly on
 * the <=148.5MHz boundary -> div=2, which selects the `case 2:` PHY
 * register set below (the correct set for 1080p60 / TMDS 1.485 Gbps,
 * NOT the 720p `case 4:` values) -- ported verbatim from
 * sunxi_dw_hdmi_phy_set()'s case 2 branch (sunxi_dw_hdmi.c:151-164). */
static void phy_set(uint32_t pixelclock_hz, int phy_div)
{
	int div = (pixelclock_hz <= 27000000u) ? 11 :
	          (pixelclock_hz <= 74250000u) ? 4 :
	          (pixelclock_hz <= 148500000u) ? 2 : 1;
	uint32_t tmp;

	switch (div) {
	case 1:
		wr32(PHY_PLL, 0x30dc5fc0u);
		wr32(PHY_CLK, 0x800863C0u | (uint32_t)(phy_div - 1));
		timer_delay_us(10000);
		wr32(PHY_UNK3, 1);
		set32(PHY_PLL, 1u << 25);
		timer_delay_us(200000);
		tmp = (rd32(PHY_STATUS) & 0x1f800u) >> 11;
		set32(PHY_PLL, (1u << 31) | (1u << 30));
		set32(PHY_PLL, (tmp < 0x3d) ? (tmp + 2) : 0x3f);
		timer_delay_us(100000);
		wr32(PHY_CTRL, 0x01FFFF7Fu);
		wr32(PHY_UNK1, 0x8063b000u);
		wr32(PHY_UNK2, 0x0F8246B5u);
		break;
	case 2:
		wr32(PHY_PLL, 0x39dc5040u);
		wr32(PHY_CLK, 0x80084380u | (uint32_t)(phy_div - 1));
		timer_delay_us(10000);
		wr32(PHY_UNK3, 1);
		set32(PHY_PLL, 1u << 25);
		timer_delay_us(100000);
		tmp = (rd32(PHY_STATUS) & 0x1f800u) >> 11;
		set32(PHY_PLL, (1u << 31) | (1u << 30));
		set32(PHY_PLL, tmp);
		wr32(PHY_CTRL, 0x01FFFF7Fu);
		wr32(PHY_UNK1, 0x8063a800u);
		wr32(PHY_UNK2, 0x0F81C485u);
		break;
	case 4:
		wr32(PHY_PLL, 0x39dc5040u);
		wr32(PHY_CLK, 0x80084340u | (uint32_t)(phy_div - 1));
		timer_delay_us(10000);
		wr32(PHY_UNK3, 1);
		set32(PHY_PLL, 1u << 25);
		timer_delay_us(100000);
		tmp = (rd32(PHY_STATUS) & 0x1f800u) >> 11;
		set32(PHY_PLL, (1u << 31) | (1u << 30));
		set32(PHY_PLL, tmp);
		wr32(PHY_CTRL, 0x01FFFF7Fu);
		wr32(PHY_UNK1, 0x8063b000u);
		wr32(PHY_UNK2, 0x0F81C405u);
		break;
	case 11:
	default:
		wr32(PHY_PLL, 0x39dc5040u);
		wr32(PHY_CLK, 0x80084300u | (uint32_t)(phy_div - 1));
		timer_delay_us(10000);
		wr32(PHY_UNK3, 1);
		set32(PHY_PLL, 1u << 25);
		timer_delay_us(100000);
		tmp = (rd32(PHY_STATUS) & 0x1f800u) >> 11;
		set32(PHY_PLL, (1u << 31) | (1u << 30));
		set32(PHY_PLL, tmp);
		wr32(PHY_CTRL, 0x01FFFF7Fu);
		wr32(PHY_UNK1, 0x8063b000u);
		wr32(PHY_UNK2, 0x0F81C405u);
		break;
	}
}

static void stage_phy(int phy_div)
{
	uint32_t clkdis;

	phy_init();
	phy_set(HDMI_MODE_PIXEL_CLOCK_HZ, phy_div);

	/* hdmi_enable_video_path() (dw_hdmi.c:701-744): control-period
	 * durations, TMDS channel preambles, flow control = CSC bypass, then
	 * enable pixel clock + TMDS clock (no CSC, no audio in v1). */
	hwr8(HDMI_CTRL_BASE + HDMI_FC_CTRLDUR, 12);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_EXCTRLDUR, 32);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_EXCTRLSPAC, 1);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_CH0PREAM, 0x0b);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_CH1PREAM, 0x16);
	hwr8(HDMI_CTRL_BASE + HDMI_FC_CH2PREAM, 0x21);
	hwr8(HDMI_CTRL_BASE + HDMI_MC_FLOWCTRL, HDMI_MC_FLOWCTRL_CSC_BYPASS);

	clkdis = 0x7Fu;
	clkdis &= (uint32_t)~HDMI_MC_CLKDIS_PIXELCLK;
	hwr8(HDMI_CTRL_BASE + HDMI_MC_CLKDIS, (uint8_t)clkdis);
	clkdis &= (uint32_t)~HDMI_MC_CLKDIS_TMDSCLK;
	hwr8(HDMI_CTRL_BASE + HDMI_MC_CLKDIS, (uint8_t)clkdis);

	bc_phy_status(rd32(PHY_STATUS));
	bc_clk_readback(rd32(PHY_PLL));
	bc_stage(HDMI_STAGE_PHY);
}

/* ==================================================================== *
 * Stage 6 -- final scanout enable: clear any TMDS overflow, apply sync
 * polarity to the PHY (pol depends on our fixed mode's active-high sync,
 * both already high here so no bits set), re-apply the DE2 double-buffer
 * flip, and declare victory. Mirrors the tail of sunxi_dw_hdmi_enable()
 * (sunxi_dw_hdmi.c:296-327) and hdmi_clear_overflow() (dw_hdmi.c:747-758).
 * ==================================================================== */
static void stage_scanout(void)
{
	uint8_t inv;
	int i;

	/* hdmi_clear_overflow(): tmds soft reset, then rewrite FC_INVIDCONF a
	 * few times (documented Allwinner/DW workaround). */
	hwr8(HDMI_CTRL_BASE + HDMI_MC_SWRSTZ, (uint8_t)~HDMI_MC_SWRSTZ_TMDSSWRST_REQ);
	inv = hrd8(HDMI_CTRL_BASE + HDMI_FC_INVIDCONF);
	for (i = 0; i < 4; i++)
		hwr8(HDMI_CTRL_BASE + HDMI_FC_INVIDCONF, inv);

	/* our mode's sync polarities are both active-high, so phy->pol gets
	 * no bits set (the VSYNC_LOW/HSYNC_LOW branches in
	 * sunxi_dw_hdmi_enable() don't apply here -- sunxi_dw_hdmi.c:311-315). */
	set32(PHY_CTRL, 0xFu << 12);

	/* "last hdmi access before boot, so scramble addresses again"
	 * (sunxi_dw_hdmi.c:319-325): dummy readback then re-scramble. */
	(void)hrd8(HDMI_CTRL_BASE);
	wr32(PHY_UNSCRAMBLE, 0);

	/* re-apply the DE2 mixer double-buffer flip now that the whole
	 * downstream pipe is live. */
	wr32(DE_GLB_DBUFF, 1);

	bc_phy_status(rd32(PHY_STATUS));
	bc_stage(HDMI_STAGE_SCANOUT);
}

/* ==================================================================== *
 * Public API
 * ==================================================================== */
int hdmi_init(void)
{
	uint32_t pll3_hz = 0;

	g_timeout_latched = 0;
	bc_write(4, 0);
	bc_write(5, HDMI_MODE_PIXEL_CLOCK_HZ / 1000u);

	int phy_div = stage_clocks(&pll3_hz);
	g_phy_div = (uint32_t)phy_div;
	stage_de2();
	stage_tcon(pll3_hz);
	stage_hdmi_ctrl();
	stage_phy(phy_div);
	stage_scanout();

	return 0; /* always -- breadcrumb + word[4] is the real verdict */
}

/* ==================================================================== *
 * Post-boot PHY relock -- see hdmi.h for the full rationale (live-board-
 * confirmed: none of the CCU/DE2/TCON config registers below actually need
 * restoring, they never change; the analog PHY's OWN lock status is what
 * drops). The CCU/DE2/TCON re-assertions are SET-only (set32 = read | OR),
 * so even though this runs on CPU1 concurrently with the guest driving its
 * own clocks through the very same CCU_BASE register file, it can never
 * clear a bit the guest set elsewhere in the same 32-bit register -- it
 * only ever turns back on the exact few display-specific bits hdmi_init()
 * itself turned on, bits the guest DTB never references (every display
 * node is status="disabled").
 * ==================================================================== */
int hdmi_phy_locked(void)
{
	return (rd32(PHY_STATUS) & 0x80u) ? 1 : 0;
}

void hdmi_relock(void)
{
	/* CRITICAL (2026-07-25, live-confirmed root cause): re-enable the HDMI
	 * PHY's SUPPLY before anything else. A second into guest boot FreeBSD's
	 * axp8xx PMIC driver disables AXP803 dldo1 ("vcc-hdmi-dsi", the DTB
	 * hvcc-supply of the PHY) as "unused" — read live over RSB, REG 0x12 goes
	 * 0x88 -> 0x80, i.e. DLDO1 (bit3) cleared — which cuts PHY power, so
	 * re-running phy_init() alone re-locks NOTHING (no rail to lock onto).
	 * Reclaim the RSB bus (init re-does the whole sequence; the guest re-inits
	 * its side on its next transaction) and set DLDO1 back on. Confirmed live:
	 * this + the PHY re-init below brings PHY_STATUS bit7 back to 1, stable. */
	rsb_init();
	rsb_set_device_address(0x3a3u, 0x2du);   /* AXP803 hw=0x3a3, runtime=0x2d */
	{
		uint8_t v = 0;
		if (rsb_read(0x2du, 0x12u, &v) == 0)
			rsb_write(0x2du, 0x12u, (uint8_t)(v | 0x08u)); /* set DLDO1 en */
		else
			rsb_write(0x2du, 0x12u, 0x88u);   /* best-effort: DC1SW+DLDO1 */
	}

	/* Re-lock PLL_VIDEO0 next. FreeBSD's CCU init
	 * gates/disables PLL_VIDEO0 (the pixel-clock source) as "unused" once the
	 * guest boots, so the analog PHY has no TMDS clock to lock onto — re-running
	 * phy_init() alone (as an earlier version did) re-locked NOTHING because the
	 * clock underneath it was dead. pll_video_pick() re-programs CCU_PLL_VIDEO0_CFG
	 * (enable + N/M for our pixel clock) and waits for CCM_PLL3_LOCK, exactly as
	 * stage_clocks() does at boot. */
	{
		int pll_div;
		(void)pll_video_pick(HDMI_MODE_PIXEL_CLOCK_HZ / 1000u, &pll_div);
	}

	/* Belt-and-suspenders: re-OR exactly the display gate/reset/PLL-enable
	 * bits stage_clocks()/stage_de2()/stage_tcon() set. */
	set32(CCU_BASE + CCU_AHB_RESET1_CFG,
	      (1u << AHB_RESET_OFFSET_DE) | (1u << AHB_RESET_OFFSET_HDMI) |
	      (1u << AHB_RESET_OFFSET_HDMI2) | (1u << AHB_RESET_OFFSET_LCD1));
	set32(CCU_BASE + CCU_AHB_GATE1,
	      (1u << AHB_GATE_OFFSET_DE) | (1u << AHB_GATE_OFFSET_HDMI) |
	      (1u << AHB_GATE_OFFSET_LCD1));
	set32(CCU_BASE + CCU_DE_CLK_CFG, CCM_DE2_GATE);
	set32(CCU_BASE + CCU_HDMI_CLK_CFG, CCM_HDMI_GATE);
	set32(CCU_BASE + CCU_HDMI_SLOW_CLK, CCM_HDMI_SLOW_DDC_GATE);
	set32(CCU_BASE + CCU_LCD1_CLK_CFG, CCM_LCD1_GATE | CCM_LCD1_M(g_tcon1_div));
	set32(DE_CLK_RST_CFG, 1u << 1);
	set32(DE_CLK_GATE_CFG, 1u << 1);
	set32(DE_CLK_BUS_CFG, 1u << 1);
	set32(DE_GLB_CTL, DE2_MUX_GLB_CTL_EN);
	set32(TCON_CTRL, TCON_CTRL_TCON_ENABLE);
	set32(TCON1_CTRL, TCON1_CTRL_ENABLE);

	/* The operation that actually restores the signal: redrive the analog
	 * PHY bring-up sequence with the same divider computed at boot. */
	phy_init();
	phy_set(HDMI_MODE_PIXEL_CLOCK_HZ, (int)g_phy_div);

	/* Mirror stage_scanout()'s tail: clear any TMDS overflow, re-apply the
	 * DE2 double-buffer flip. */
	{
		uint8_t inv;
		hwr8(HDMI_CTRL_BASE + HDMI_MC_SWRSTZ, (uint8_t)~HDMI_MC_SWRSTZ_TMDSSWRST_REQ);
		inv = hrd8(HDMI_CTRL_BASE + HDMI_FC_INVIDCONF);
		hwr8(HDMI_CTRL_BASE + HDMI_FC_INVIDCONF, inv);
		wr32(DE_GLB_DBUFF, 1);
	}

	bc_phy_status(rd32(PHY_STATUS));
	bc_write(7, ++g_relock_count);
}

uint32_t *hdmi_fb(void)   { return (uint32_t *)HDMI_FB_BASE; }
int hdmi_width(void)      { return HDMI_MODE_HACTIVE; }
int hdmi_height(void)     { return HDMI_MODE_VACTIVE; }
int hdmi_stride(void)     { return HDMI_MODE_HACTIVE; }

/* ==================================================================== *
 * hdmi_demo() -- the HUD skeleton: dark-blue clear, title bar, a border
 * box (the future "guest window"), and a few fake register readouts drawn
 * with fb.h's text/hex primitives, using this module's own breadcrumb
 * values as the "registers" so the demo is simultaneously a real, live
 * readout of how the bring-up went.
 * ==================================================================== */
#include "fb.h"

void hdmi_demo(void)
{
	int w = hdmi_width();
	int h = hdmi_height();

	fb_init(hdmi_fb(), w, h, hdmi_stride());
	fb_clear(0xFF001030u); /* dark blue, ARGB */

	/* title bar */
	fb_fillrect(0, 0, w, 24, 0xFF102040u);
	fb_str(8, 4, "bzdOS HDMI/DE2 bring-up -- HUD skeleton v1", 0xFFFFFFFFu, 0xFF102040u);

	/* border box: stand-in for the future guest-OS window */
	fb_rect(40, 48, w - 80, h - 140, 0xFF40C0FFu);
	fb_str(56, 56, "guest window (not yet composited)", 0xFF80D0FFu, 0xFF001030u);

	/* sample "register readouts" -- pulled straight from our own
	 * breadcrumb, so this is a live self-report of the bring-up, not
	 * just decoration. */
	{
		int x = 56, y = 84;
		volatile uint32_t *bc = (volatile uint32_t *)BC_HDMI_BASE;

		fb_str(x, y, "HDMI_BC.stage   = 0x", 0xFFFFFF00u, 0xFF001030u);
		fb_hex(x + 168, y, bc[1], 8, 0xFFFFFF00u, 0xFF001030u);
		y += 16;
		fb_str(x, y, "HDMI_BC.clk     = 0x", 0xFFFFFF00u, 0xFF001030u);
		fb_hex(x + 168, y, bc[2], 8, 0xFFFFFF00u, 0xFF001030u);
		y += 16;
		fb_str(x, y, "HDMI_BC.phy     = 0x", 0xFFFFFF00u, 0xFF001030u);
		fb_hex(x + 168, y, bc[3], 8, 0xFFFFFF00u, 0xFF001030u);
		y += 16;
		fb_str(x, y, "HDMI_BC.timeout = 0x", 0xFFFFFF00u, 0xFF001030u);
		fb_hex(x + 168, y, bc[4], 8, 0xFFFFFF00u, 0xFF001030u);
		y += 16;
		fb_str(x, y, "mode: 1920x1080@60  pixclk=148.5MHz", 0xFFA0FFA0u, 0xFF001030u);
	}

	fb_flush();
}
