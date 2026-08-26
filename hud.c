/* SPDX-License-Identifier: BSD-2-Clause */

/* hud.c — hypervisor HUD compositor for the bzdOS EL2 hypervisor.
 * See hud.h for the API contract. This file owns ONLY pixels: it draws
 * static chrome once (hud_init) and refreshes dynamic value fields on
 * every call (hud_update), pulling live data from the guest's saved
 * el2_frame, EL1 sysregs (mrs — EL2 can read them directly), and the
 * fixed breadcrumb/MMIO windows already established elsewhere in this
 * tree (timer.c, gic_timer.c, musb.c, emac.c, gtrace.c, hdmi.c, el2_exc.c,
 * vconsole.c).
 *
 * The headline surface is the GUEST window: it renders the FreeBSD guest's
 * captured UART console text (from the vconsole capture ring) live, plus a
 * compact strip of the guest's saved registers. The right column stacks
 * TIMING/JITTER, GUEST-TRACE + EL2-FAULT, MEMORY/BREADCRUMBS, and a live
 * REGISTER MAP.
 *
 * Freestanding: only <stdint.h> (via headers), no libc, no floats,
 * -mgeneral-regs-only. Every panel is a fixed set of fields — the only
 * variable-length loop (console text) is hard-bounded by the 3 KiB capture
 * ring — so hud_update() is safe to call from IRQ/tick context.
 */
#include <stdint.h>
#include "hud.h"
#include "profiler.h"
#include "hdmi.h"
#include "fb.h"
#include "exceptions.h"
#include "hv_addrmap.h"
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

/* ------------------------------------------------------------------ *
 * Palette — dark background, cyan headers, green/amber values, red for
 * fault/anomaly indicators. All 0xAARRGGBB (alpha byte written but
 * ignored by the DE2 mixer's UI layer format).
 * ------------------------------------------------------------------ */
#define HUD_BG          0xFF0A0E14u /* overall dark background */
#define HUD_TITLE_BG    0xFF06141Au /* title bar background */
#define HUD_PANEL_BG    0xFF10161Eu /* panel interior background */
#define HUD_CON_BG      0xFF04080Cu /* guest console area background */
#define HUD_CON_FG      0xFFAFE8B0u /* guest console text (soft green) */
#define HUD_BORDER      0xFF1E8C99u /* panel border (dim teal) */
#define HUD_WIN_BORDER  0xFF00C2D1u /* GUEST window border (bright teal) */
#define HUD_HDR_FG      0xFF00E5FFu /* cyan — panel/section headers */
#define HUD_LABEL_FG    0xFF6E94A6u /* dim blue-gray — field labels */
#define HUD_VAL_GREEN   0xFF33FF77u /* nominal live values */
#define HUD_VAL_AMBER   0xFFFFB300u /* secondary/derived values */
#define HUD_VAL_RED     0xFFFF4433u /* fault / anomaly values */
#define HUD_VAL_DIM     0xFF556070u /* inactive / off flags */
#define HUD_TITLE_FG    0xFFE8F6F8u /* title bar text */

/* ------------------------------------------------------------------ *
 * Screen + layout constants — track the ACTUAL forced HDMI mode (hdmi.h)
 * instead of a hardcoded 1920x1080. The whole HUD layout below is derived
 * from these (title bar width, right column, bottom strip, regmap height),
 * so tying them to HDMI_MODE_H/VACTIVE makes the HUD fit whatever mode is
 * scanned out — at 720p the 1080p-positioned panels used to fall off the
 * 1280x720 framebuffer's right/bottom edge (clipped by fb.c), which read on
 * a real monitor as "the picture is bigger than the screen".
 * ------------------------------------------------------------------ */
#define SCR_W  HDMI_MODE_HACTIVE
#define SCR_H  HDMI_MODE_VACTIVE
#define MARGIN 8
#define TITLE_H 28
#define PANEL_GAP 8

/* Bottom band (below the GUEST window, spanning the left column): the new
 * SCHED-GANTT and PROFILE panels live here. The GUEST window is shortened
 * by BOTTOM_H + PANEL_GAP to make room; everything below is relative so the
 * console/register-strip geometry follows automatically. */
#define BOTTOM_H 200

/* GUEST window: left ~60% of the screen. */
#define GW_X (MARGIN)
#define GW_Y (TITLE_H + MARGIN)
#define GW_W 1150
#define GW_H (SCR_H - GW_Y - MARGIN - BOTTOM_H - PANEL_GAP)

/* Guest console text region (top of the GUEST window). */
#define CON_X (GW_X + 8)
#define CON_Y (GW_Y + 30)
#define CON_W (GW_W - 16)
#define CON_LINE_H 10
/* Compact guest-register strip lives at the bottom of the GUEST window. */
#define STRIP_H 148
#define STRIP_Y (GW_Y + GW_H - STRIP_H - 6)
#define STRIP_X (GW_X + 8)
#define STRIP_COLW 280
#define CON_H (STRIP_Y - CON_Y - 8)
#define CON_ROWS (CON_H / CON_LINE_H)   /* max text rows that fit */
#define CON_COLS (CON_W / FB_FONT_W)    /* max chars per row */

/* Right column: TIMING / TRACE+EXC / MEMORY / REGISTER MAP stacked. */
#define RC_X (GW_X + GW_W + MARGIN)
#define RC_W (SCR_W - RC_X - MARGIN)

/* The right column's width is whatever the mode leaves over after the fixed
 * 1150-px GUEST window, so a narrower mode silently squeezes it: 746 px at
 * 1920 but only 106 px at 1280, which is less than the panel titles need. That
 * is not a clip by fb.c -- the column really is that narrow, and it showed up as
 * "TIMING / JITTE", "GUEST TRACE /", "MEMORY / BREAK" cut off mid-word in a
 * screenshot taken from EL2 (screenshot.py). The guest window is
 * HDMI_GUESTWIN_W (1120) wide and has to fit inside the left panel, so the fix
 * is a wider mode, not a narrower column -- hence 1080p is the default build
 * (see the Makefile). This assert exists so the next mode change fails HERE,
 * at compile time, instead of quietly truncating the panels again. 300 px is
 * the measured minimum: the widest label plus its value column. */
_Static_assert(RC_W >= 300,
    "display mode too narrow for the HUD's right column -- widen the mode or "
    "shrink GW_W and HDMI_GUESTWIN_W together");

#define TIMING_Y GW_Y
#define TIMING_H 180

#define TRACE_Y  (TIMING_Y + TIMING_H + PANEL_GAP)
#define TRACE_H  220

#define MEMORY_Y (TRACE_Y + TRACE_H + PANEL_GAP)
#define MEMORY_H 330

#define REGMAP_Y (MEMORY_Y + MEMORY_H + PANEL_GAP)
#define REGMAP_H (SCR_H - REGMAP_Y - MARGIN)

/* Bottom band: SCHED GANTT (left) + PROFILE / HOT PCs (right), spanning the
 * full width of the (now slightly shorter) GUEST column, beneath it. */
#define BOTTOM_Y  (GW_Y + GW_H + PANEL_GAP)

#define GANTT_X   GW_X
#define GANTT_Y   BOTTOM_Y
#define GANTT_W   700
#define GANTT_H   BOTTOM_H

#define FLAME_X   (GANTT_X + GANTT_W + PANEL_GAP)
#define FLAME_Y   BOTTOM_Y
#define FLAME_W   (GW_X + GW_W - FLAME_X)
#define FLAME_H   BOTTOM_H

/* Fixed pixel width reserved for a decimal value field (12 chars). */
#define DEC_FIELD_W (12 * FB_FONT_W)

/* ------------------------------------------------------------------ *
 * Shared trace/profiler DRAM windows (FIXED formats produced by a sibling
 * track — read-only here; both are validated by magic and rendered as a dim
 * placeholder when absent, and every loop is hard-capped).
 * ------------------------------------------------------------------ */
/* From hv_addrmap.h. This was a local 0x50004000 copy, and PROF_BASE below
 * was a local 0x50004800 -- 2 KiB INSIDE this ring, not the profiler at
 * all, which is the entire reason the PROFILE panel never found its
 * magic. Same failure this file already had once with HDMI_BC_BASE. */
#define TRC_BASE     HVMAP_TRACE_RING
#define TRC_ENTRIES  0x50004040UL   /* 16-byte entries start here */
#define TRC_ENTSZ    16u
#define TRC_MAGIC    0x54524331u    /* "TRC1" */
#define TRC_MAXSCAN  2048u          /* cap events walked per update */

/* event types (word2 low byte) */
#define EV_CTX_SWITCH 1u

/* Profiler histogram: "PROF" magic, then buckets of {u32 pc, u32 count}.
 * The header size is NOT guessed here any more. It used to be "we assume a
 * 16-byte (4-word) header ... if the producer differs, adjust
 * PROF_BUCKET_OFF" -- the producer did differ (8 words), nobody adjusted it,
 * and because this panel had never been linked into a build the error was
 * invisible until the first live render showed CNTFRQ as the hottest PC.
 * PROF_HDR_BYTES now comes from profiler.h, so producer and reader cannot
 * disagree. */
#define PROF_BASE       HVMAP_PROF_HIST
#define PROF_MAGIC      0x50524F46u /* "PROF" (MSB-first, matching TRC1) */
#define PROF_BUCKET_OFF PROF_HDR_BYTES   /* from profiler.h -- was a
                                          * hardcoded 16, i.e. wrong */
#define PROF_MAXBKT     24u         /* buckets scanned */
#define PROF_TOPN       12u         /* bars rendered */

#define NCPU 4
/* Idle-task assumption: task id 0 is the idle/null task. Time a CPU spends
 * running task 0 counts as idle; everything else counts as busy for CPU%. */
#define GANTT_IDLE_TASK 0u

/* ------------------------------------------------------------------ *
 * EL1 sysreg reads — EL2 can read these directly, no trap involved.
 * ------------------------------------------------------------------ */
static inline uint64_t read_elr_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, elr_el1" : "=r"(v));
	return v;
}
static inline uint64_t read_esr_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, esr_el1" : "=r"(v));
	return v;
}
static inline uint64_t read_far_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, far_el1" : "=r"(v));
	return v;
}
static inline uint64_t read_sp_el1(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, sp_el1" : "=r"(v));
	return v;
}
static inline uint64_t read_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v));
	return v;
}
static inline uint64_t read_cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

/* ------------------------------------------------------------------ *
 * Breadcrumb / MMIO reads.
 * ------------------------------------------------------------------ */
static inline uint32_t bc_word(uint32_t base, int idx)
{
	return *(volatile uint32_t *)(uint64_t)(base + (uint32_t)idx * 4u);
}
static inline uint32_t mmio32(uint32_t addr)
{
	return *(volatile uint32_t *)(uint64_t)addr;
}

#define MUSB_BC_BASE 0x50000000UL
#define EMAC_BC_BASE 0x50000100UL
#define EXC_BC_BASE  0x50000400UL   /* "EXC1" — el2_exc.c */
#define TIMR_BC_BASE 0x50000500UL
#define GICT_BC_BASE 0x50000800UL
#define VCON_RING    0x50000f00UL   /* "UART" — vconsole.c */
_Static_assert(VCON_RING == HVMAP_LOW_VCONSOLE_HDR,
               "VCON_RING drifted from hv_addrmap.h -- the map owns this address");
#define VCON_BUF     0x50000f10UL   /* captured console bytes */
#define VCON_SIZE    3072u
#define GTRC_BC_BASE 0x50002000UL   /* "GTRC" — gtrace.c */
_Static_assert(GTRC_BC_BASE == HVMAP_LOW_GTRACE,
               "GTRC_BC_BASE drifted from hv_addrmap.h -- the map owns this address");
/* FIXED (found while adding zero-copy-scanout support, see
 * docs/zero-copy-scanout.md): hdmi.c's breadcrumb moved from 0x50003000 to
 * 0x50011800 on 2026-07-25 (hdmi.h's own "Relocated 2026-07-25" comment —
 * that old address sits inside the vconsole capture ring and was being
 * clobbered). This copy was never updated, so the HUD's "stage" readout
 * below (kv_addr_val at line ~677) had been silently reading a dead address
 * — whatever was last written there before the move, never hdmi.c's real,
 * live pipeline state — in every HV_HDMI build since. */
#define HDMI_BC_BASE 0x50011800UL
_Static_assert(HDMI_BC_BASE == HVMAP_LOW_HDMI_BC,
               "HDMI_BC_BASE drifted from hv_addrmap.h -- the map owns this address");

#define VCON_MAGIC   0x55415254u    /* "UART" */
#define EXC_MAGIC    0x45584331u    /* "EXC1" */

#define GICD_CTLR_ADDR SOC_A64_GICD_BASE
#define GICC_PMR_ADDR  (SOC_A64_GICC_BASE + 0x4)
#define MUSB_POWER_ADDR (SOC_A64_MUSB_BASE + 0x40)

/* ------------------------------------------------------------------ *
 * Small drawing helpers.
 * ------------------------------------------------------------------ */
static void
panel_chrome(int x, int y, int w, int h, const char *title, uint32_t border)
{
	fb_fillrect(x, y, w, h, HUD_PANEL_BG);
	fb_rect(x, y, w, h, border);
	fb_fillrect(x + 1, y + 1, w - 2, 18, HUD_TITLE_BG);
	fb_str(x + 6, y + 5, title, HUD_HDR_FG, HUD_TITLE_BG);
}

/* label: decimal value, fixed-width field cleared first (fb_dec() has no
 * padding, so a shrinking value must have its old tail blanked). */
static void
kv_dec(int x, int y, const char *label, uint64_t v, uint32_t fg)
{
	int vx = x + 17 * FB_FONT_W;

	fb_str(x, y, label, HUD_LABEL_FG, HUD_PANEL_BG);
	fb_fillrect(vx, y, DEC_FIELD_W, FB_FONT_H, HUD_PANEL_BG);
	fb_dec(vx, y, v, fg, HUD_PANEL_BG);
}

/* label: "addr = value" hex/hex row (both address AND value shown — the
 * educational breadcrumb/register-map format). */
static void
kv_addr_val(int x, int y, const char *label, uint32_t addr, uint32_t val, uint32_t fg)
{
	fb_str(x, y, label, HUD_LABEL_FG, HUD_PANEL_BG);
	fb_hex(x + 15 * FB_FONT_W, y, addr, 8, HUD_LABEL_FG, HUD_PANEL_BG);
	fb_str(x + 24 * FB_FONT_W, y, "=", HUD_LABEL_FG, HUD_PANEL_BG);
	fb_hex(x + 26 * FB_FONT_W, y, val, 8, fg, HUD_PANEL_BG);
}

/* Decode ESR_ELx exception-class (bits[31:26]) into a short label. */
static const char *
esr_ec_label(uint32_t ec)
{
	switch (ec) {
	case 0x00: return "Unknown  ";
	case 0x15: return "SVC      ";
	case 0x16: return "HVC      ";
	case 0x18: return "MSR/MRS  ";
	case 0x20: return "InstrAbrt"; /* lower EL */
	case 0x21: return "InstrAbrt"; /* same EL */
	case 0x24: return "DataAbort"; /* lower EL */
	case 0x25: return "DataAbort"; /* same EL */
	case 0x3C: return "BRK      ";
	default:   return "EC?      ";
	}
}

/* ------------------------------------------------------------------ *
 * hud_init() — static chrome, drawn exactly once.
 * ------------------------------------------------------------------ */
void
hud_init(void)
{
	fb_init(hdmi_fb(), hdmi_width(), hdmi_height(), hdmi_stride());

	fb_clear(HUD_BG);

	/* Title bar. */
	fb_fillrect(0, 0, SCR_W, TITLE_H, HUD_TITLE_BG);
	fb_rect(0, 0, SCR_W, TITLE_H, HUD_BORDER);
	/* "bzdk", not "bzdOS": bzdk is THIS -- the EL2 hypervisor. bzdOS is the
	 * operating system, a separate project. The title said "bzdOS
	 * hypervisor" and conflated the two, which matters because this line
	 * ends up in every screenshot. */
	fb_str(MARGIN, 10, "bzdk hypervisor -- Chimp BPI-M64", HUD_TITLE_FG, HUD_TITLE_BG);

	/* GUEST window frame + header. */
	fb_fillrect(GW_X, GW_Y, GW_W, GW_H, HUD_PANEL_BG);
	fb_rect(GW_X, GW_Y, GW_W, GW_H, HUD_WIN_BORDER);
	fb_rect(GW_X + 1, GW_Y + 1, GW_W - 2, GW_H - 2, HUD_WIN_BORDER);
	fb_fillrect(GW_X + 2, GW_Y + 2, GW_W - 4, 20, HUD_TITLE_BG);
	fb_str(GW_X + 8, GW_Y + 7, "GUEST  --  FreeBSD console (captured UART)",
	       HUD_HDR_FG, HUD_TITLE_BG);

	/* Console text background box. */
	fb_fillrect(CON_X, CON_Y, CON_W, CON_H, HUD_CON_BG);
	fb_rect(CON_X, CON_Y, CON_W, CON_H, HUD_BORDER);

	/* Register-strip divider + header (values filled by hud_update). */
	fb_fillrect(STRIP_X, STRIP_Y, GW_W - 16, 16, HUD_TITLE_BG);
	fb_str(STRIP_X + 4, STRIP_Y + 4, "-- GUEST REGISTERS (EL2 frame + EL1 sysregs) --",
	       HUD_HDR_FG, HUD_TITLE_BG);

	/* Right-column panels. */
	panel_chrome(RC_X, TIMING_Y, RC_W, TIMING_H, "TIMING / JITTER", HUD_BORDER);
	panel_chrome(RC_X, TRACE_Y, RC_W, TRACE_H, "GUEST TRACE / EL2 FAULT", HUD_BORDER);
	panel_chrome(RC_X, MEMORY_Y, RC_W, MEMORY_H, "MEMORY / BREADCRUMBS", HUD_BORDER);
	panel_chrome(RC_X, REGMAP_Y, RC_W, REGMAP_H, "REGISTER MAP (live)", HUD_BORDER);

	/* Bottom band: scheduler timeline + profiler histogram. */
	panel_chrome(GANTT_X, GANTT_Y, GANTT_W, GANTT_H,
		     "SCHED GANTT / CPU TIMELINE", HUD_BORDER);
	panel_chrome(FLAME_X, FLAME_Y, FLAME_W, FLAME_H,
		     "PROFILE / HOT PCs", HUD_BORDER);

	fb_flush();
}

/* ------------------------------------------------------------------ *
 * GUEST CONSOLE — render captured UART text from the vconsole ring.
 *
 * Ring: word[0] magic "UART", word[1] total_bytes, word[2] fault_count,
 * word[3] reserved, then a 3 KiB byte buffer at VCON_BUF that wraps at
 * total_bytes % 3072. We show the last CON_ROWS lines:
 *   - `valid` = min(total, 3072) bytes are live.
 *   - `start` = ring index of the OLDEST live byte (0 before wrap, else
 *     total % 3072).
 *   - Scan backward counting '\n' to find where the last CON_ROWS lines
 *     begin, then render forward, wrapping long lines at CON_COLS and
 *     stopping at CON_ROWS rows. Bounded by `valid` (<= 3072).
 * ------------------------------------------------------------------ */
static inline uint8_t
vcon_byte(uint32_t start, uint32_t linear_idx)
{
	uint32_t r = start + linear_idx;

	if (r >= VCON_SIZE)
		r -= VCON_SIZE;                 /* single wrap: start,idx both < SIZE */
	return *(volatile uint8_t *)(uint64_t)(VCON_BUF + r);
}

static void
update_console(void)
{
	uint32_t magic = bc_word(VCON_RING, 0);
	uint32_t total = bc_word(VCON_RING, 1);
	uint32_t valid, start, begin, idx;
	int row, col, nl;

	/* The console area is allowed to clear+redraw its own rectangle each
	 * update (per brief) — the surrounding chrome is untouched, so there's
	 * no whole-screen flicker. */
	fb_fillrect(CON_X + 1, CON_Y + 1, CON_W - 2, CON_H - 2, HUD_CON_BG);

	if (magic != VCON_MAGIC) {
		fb_str(CON_X + 6, CON_Y + 6,
		       "(guest console ring not initialised)",
		       HUD_VAL_DIM, HUD_CON_BG);
		return;
	}
	if (total == 0) {
		fb_str(CON_X + 6, CON_Y + 6,
		       "(no guest console output captured yet)",
		       HUD_VAL_DIM, HUD_CON_BG);
		return;
	}

	valid = (total < VCON_SIZE) ? total : VCON_SIZE;
	start = (total < VCON_SIZE) ? 0u : (total % VCON_SIZE);

	/* Backward scan for the start of the last CON_ROWS lines. */
	begin = 0;
	nl = 0;
	for (uint32_t k = 0; k < valid; k++) {
		idx = valid - 1u - k;
		if (vcon_byte(start, idx) == '\n') {
			nl++;
			if (nl > CON_ROWS) {
				begin = idx + 1u;
				break;
			}
		}
	}

	/* Forward render, wrapping at CON_COLS, capped at CON_ROWS rows. */
	row = 0;
	col = 0;
	for (idx = begin; idx < valid && row < CON_ROWS; idx++) {
		uint8_t c = vcon_byte(start, idx);

		if (c == '\n') {
			row++;
			col = 0;
			continue;
		}
		if (c == '\r') {
			col = 0;
			continue;
		}
		if (c < 0x20 || c > 0x7e)
			c = ' ';
		if (col >= CON_COLS) {   /* soft-wrap an over-long line */
			row++;
			col = 0;
			if (row >= CON_ROWS)
				break;
		}
		fb_char(CON_X + 4 + col * FB_FONT_W,
			CON_Y + 4 + row * CON_LINE_H,
			(char)c, HUD_CON_FG, HUD_CON_BG);
		col++;
	}
}

/* ------------------------------------------------------------------ *
 * GUEST REGISTER STRIP — compact EL2-frame + EL1-sysreg readout at the
 * bottom of the GUEST window. Fixed-width fields (hex is bg-filled per
 * nibble; labels are constant text redrawn in place), so no clear needed
 * and no flicker.
 * ------------------------------------------------------------------ */
static void
strip_field(int col, int rrow, const char *label, uint64_t v, int ndig, uint32_t fg)
{
	int x = STRIP_X + col * STRIP_COLW;
	int y = STRIP_Y + 22 + rrow * 16;

	fb_str(x, y, label, HUD_LABEL_FG, HUD_PANEL_BG);
	fb_hex(x + 6 * FB_FONT_W, y, v, ndig, fg, HUD_PANEL_BG);
}

static void
update_reg_strip(const struct el2_frame *g)
{
	uint64_t elr1 = read_elr_el1();
	uint64_t esr1 = read_esr_el1();
	uint64_t far1 = read_far_el1();
	uint64_t sp1  = read_sp_el1();
	uint32_t esr2fg = (g && g->esr) ? HUD_VAL_RED : HUD_VAL_GREEN;
	uint32_t esr1fg = esr1 ? HUD_VAL_AMBER : HUD_VAL_GREEN;

	strip_field(0, 0, "PC :", g ? g->elr : 0, 16, HUD_VAL_GREEN);
	strip_field(1, 0, "SPS:", g ? g->spsr : 0, 8, HUD_VAL_GREEN);
	strip_field(2, 0, "ES2:", g ? g->esr : 0, 8, esr2fg);
	strip_field(3, 0, "FA2:", g ? g->far : 0, 16, HUD_VAL_GREEN);

	strip_field(0, 1, "x0 :", g ? g->x[0] : 0, 16, HUD_VAL_GREEN);
	strip_field(1, 1, "x1 :", g ? g->x[1] : 0, 16, HUD_VAL_GREEN);
	strip_field(2, 1, "x2 :", g ? g->x[2] : 0, 16, HUD_VAL_GREEN);
	strip_field(3, 1, "x3 :", g ? g->x[3] : 0, 16, HUD_VAL_GREEN);

	strip_field(0, 2, "x4 :", g ? g->x[4] : 0, 16, HUD_VAL_GREEN);
	strip_field(1, 2, "x5 :", g ? g->x[5] : 0, 16, HUD_VAL_GREEN);
	strip_field(2, 2, "x6 :", g ? g->x[6] : 0, 16, HUD_VAL_GREEN);
	strip_field(3, 2, "x7 :", g ? g->x[7] : 0, 16, HUD_VAL_GREEN);

	strip_field(0, 3, "x29:", g ? g->x[29] : 0, 16, HUD_VAL_GREEN);
	strip_field(1, 3, "x30:", g ? g->x[30] : 0, 16, HUD_VAL_GREEN);
	strip_field(2, 3, "SP :", g ? g->sp_at_entry : 0, 16, HUD_VAL_GREEN);

	strip_field(0, 4, "E1P:", elr1, 16, HUD_VAL_GREEN);   /* ELR_EL1 */
	strip_field(1, 4, "E1E:", esr1, 8, esr1fg);           /* ESR_EL1 */
	strip_field(2, 4, "E1F:", far1, 16, HUD_VAL_GREEN);   /* FAR_EL1 */
	strip_field(3, 4, "E1S:", sp1, 16, HUD_VAL_GREEN);    /* SP_EL1  */
}

/* ------------------------------------------------------------------ *
 * TIMING panel — TIMR (0x50000500) jitter + GICT (0x50000800) tick count,
 * ticks -> microseconds as integer tenths (no floats).
 * ------------------------------------------------------------------ */
static uint64_t
ticks_to_tenths_us(uint32_t ticks, uint32_t freq)
{
	if (freq == 0)
		return 0;
	return ((uint64_t)ticks * 10000000ull) / (uint64_t)freq;
}

static void
draw_us_tenths(int x, int y, uint32_t ticks, uint32_t freq, uint32_t fg)
{
	uint64_t tenths = ticks_to_tenths_us(ticks, freq);
	uint64_t whole = tenths / 10u;
	uint64_t frac = tenths % 10u;
	int n;

	fb_fillrect(x, y, DEC_FIELD_W, FB_FONT_H, HUD_PANEL_BG);
	n = fb_dec(x, y, whole, fg, HUD_PANEL_BG);
	fb_char(x + n * FB_FONT_W, y, '.', fg, HUD_PANEL_BG);
	fb_dec(x + (n + 1) * FB_FONT_W, y, frac, fg, HUD_PANEL_BG);
	fb_str(x + (n + 3) * FB_FONT_W, y, "us", HUD_LABEL_FG, HUD_PANEL_BG);
}

static void
update_timing_panel(void)
{
	int x = RC_X + 8;
	int y = TIMING_Y + 24;

	uint32_t freq = bc_word(TIMR_BC_BASE, 1);
	uint32_t n = bc_word(TIMR_BC_BASE, 2);
	uint32_t last_delta = bc_word(TIMR_BC_BASE, 3);
	uint32_t min_delta = bc_word(TIMR_BC_BASE, 4);
	uint32_t max_delta = bc_word(TIMR_BC_BASE, 5);
	uint32_t last_dev = bc_word(TIMR_BC_BASE, 6);
	uint32_t max_dev = bc_word(TIMR_BC_BASE, 7);

	uint32_t gict_lo = bc_word(GICT_BC_BASE, 1);
	uint32_t gict_hi = bc_word(GICT_BC_BASE, 2);
	uint64_t gict_ticks = ((uint64_t)gict_hi << 32) | gict_lo;

	uint32_t dev_fg = (last_dev > (freq / 1000u > 0 ? freq / 1000u : 1u))
			  ? HUD_VAL_AMBER : HUD_VAL_GREEN;

	kv_dec(x, y, "tick freq (Hz):", freq, HUD_VAL_GREEN);
	y += 16;
	kv_dec(x, y, "samples (n):", n, HUD_VAL_GREEN);
	y += 16;

	fb_str(x, y, "last period:", HUD_LABEL_FG, HUD_PANEL_BG);
	draw_us_tenths(x + 17 * FB_FONT_W, y, last_delta, freq, HUD_VAL_GREEN);
	y += 16;
	fb_str(x, y, "min / max:", HUD_LABEL_FG, HUD_PANEL_BG);
	draw_us_tenths(x + 17 * FB_FONT_W, y, min_delta, freq, HUD_VAL_GREEN);
	draw_us_tenths(x + 27 * FB_FONT_W, y, max_delta, freq, HUD_VAL_GREEN);
	y += 16;
	fb_str(x, y, "jitter (+/-):", HUD_LABEL_FG, HUD_PANEL_BG);
	draw_us_tenths(x + 17 * FB_FONT_W, y, last_dev, freq, dev_fg);
	y += 16;
	fb_str(x, y, "max jitter:", HUD_LABEL_FG, HUD_PANEL_BG);
	draw_us_tenths(x + 17 * FB_FONT_W, y, max_dev, freq, HUD_VAL_AMBER);
	y += 20;

	kv_dec(x, y, "GICT ticks:", gict_ticks, HUD_VAL_GREEN);
}

/* ------------------------------------------------------------------ *
 * GUEST TRACE + EL2 FAULT panel.
 *  - GTRC (0x50002000): event_count, last_sctlr (decode M/C/I bits),
 *    fault_count.
 *  - EXC1 (0x50000400): count, kind, esr (decode EC), elr, far — a guest
 *    fault shows its cause + address on screen in red.
 * ------------------------------------------------------------------ */
static void
draw_flag(int x, int y, const char *name, int on)
{
	/* name + fixed 3-char state "ON " / "off" so widths never leave
	 * stale pixels between updates. */
	fb_str(x, y, name, HUD_LABEL_FG, HUD_PANEL_BG);
	fb_str(x + 5 * FB_FONT_W, y, on ? "ON " : "off",
	       on ? HUD_VAL_GREEN : HUD_VAL_DIM, HUD_PANEL_BG);
}

static void
update_trace_exc_panel(void)
{
	int x = RC_X + 8;
	int y = TRACE_Y + 24;

	uint32_t g_events = bc_word(GTRC_BC_BASE, 1);
	uint32_t g_sctlr = bc_word(GTRC_BC_BASE, 2);
	uint32_t g_faults = bc_word(GTRC_BC_BASE, 3);

	int m_on = (g_sctlr >> 0) & 1;   /* SCTLR.M  = MMU enable   */
	int c_on = (g_sctlr >> 2) & 1;   /* SCTLR.C  = D-cache      */
	int i_on = (g_sctlr >> 12) & 1;  /* SCTLR.I  = I-cache      */

	fb_str(x, y, "guest SCTLR_EL1:", HUD_LABEL_FG, HUD_PANEL_BG);
	fb_hex(x + 17 * FB_FONT_W, y, g_sctlr, 8, HUD_VAL_AMBER, HUD_PANEL_BG);
	y += 16;
	draw_flag(x,                    y, "MMU:", m_on);
	draw_flag(x + 12 * FB_FONT_W,   y, "Dc:", c_on);
	draw_flag(x + 22 * FB_FONT_W,   y, "Ic:", i_on);
	y += 16;
	kv_dec(x, y, "trace events:", g_events, HUD_VAL_GREEN);
	y += 16;
	kv_dec(x, y, "guest faults:", g_faults, g_faults ? HUD_VAL_RED : HUD_VAL_GREEN);
	y += 22;

	/* --- EL2 fault breadcrumb (EXC1) --- */
	{
		uint32_t magic = bc_word(EXC_BC_BASE, 0);
		uint32_t count = bc_word(EXC_BC_BASE, 1);
		/* word[2] = kind (last vector index) — not surfaced here; the
		 * ESR exception-class decode below is the human-facing cause. */
		uint32_t esr   = bc_word(EXC_BC_BASE, 3);
		uint32_t elr_l = bc_word(EXC_BC_BASE, 4);
		uint32_t elr_h = bc_word(EXC_BC_BASE, 5);
		uint32_t far_l = bc_word(EXC_BC_BASE, 6);
		uint32_t far_h = bc_word(EXC_BC_BASE, 7);
		uint32_t ec = (esr >> 26) & 0x3fu;
		int have = (magic == EXC_MAGIC && count != 0);
		uint32_t fg = have ? HUD_VAL_RED : HUD_VAL_DIM;

		fb_str(x, y, "-- last EL2 fault (EXC1) --", HUD_HDR_FG, HUD_PANEL_BG);
		y += 16;
		fb_str(x, y, "cause:", HUD_LABEL_FG, HUD_PANEL_BG);
		/* fixed-width field: clear then draw the decoded EC label. */
		fb_fillrect(x + 7 * FB_FONT_W, y, 10 * FB_FONT_W, FB_FONT_H, HUD_PANEL_BG);
		fb_str(x + 7 * FB_FONT_W, y, have ? esr_ec_label(ec) : "(none)   ",
		       fg, HUD_PANEL_BG);
		fb_str(x + 18 * FB_FONT_W, y, "EC=", HUD_LABEL_FG, HUD_PANEL_BG);
		fb_hex(x + 21 * FB_FONT_W, y, have ? ec : 0, 2, fg, HUD_PANEL_BG);
		y += 16;
		fb_str(x, y, "ESR:", HUD_LABEL_FG, HUD_PANEL_BG);
		fb_hex(x + 5 * FB_FONT_W, y, esr, 8, fg, HUD_PANEL_BG);
		fb_str(x + 15 * FB_FONT_W, y, "cnt:", HUD_LABEL_FG, HUD_PANEL_BG);
		fb_fillrect(x + 19 * FB_FONT_W, y, DEC_FIELD_W, FB_FONT_H, HUD_PANEL_BG);
		fb_dec(x + 19 * FB_FONT_W, y, count, fg, HUD_PANEL_BG);
		y += 16;
		fb_str(x, y, "ELR:", HUD_LABEL_FG, HUD_PANEL_BG);
		fb_hex(x + 5 * FB_FONT_W, y, ((uint64_t)elr_h << 32) | elr_l, 16, fg, HUD_PANEL_BG);
		y += 16;
		fb_str(x, y, "FAR:", HUD_LABEL_FG, HUD_PANEL_BG);
		fb_hex(x + 5 * FB_FONT_W, y, ((uint64_t)far_h << 32) | far_l, 16, fg, HUD_PANEL_BG);
	}
}

/* ------------------------------------------------------------------ *
 * MEMORY / BREADCRUMB panel — labeled address+value hex dumps.
 * ------------------------------------------------------------------ */
static void
update_memory_panel(void)
{
	int x = RC_X + 8;
	int y = MEMORY_Y + 24;
	uint32_t link_up;

	fb_str(x, y, "MUSB (console gadget):", HUD_HDR_FG, HUD_PANEL_BG);
	y += 16;
	kv_addr_val(x, y, "stage", (uint32_t)(MUSB_BC_BASE + 4), bc_word(MUSB_BC_BASE, 1), HUD_VAL_GREEN);
	y += 16;
	kv_addr_val(x, y, "poll_count", (uint32_t)(MUSB_BC_BASE + 8), bc_word(MUSB_BC_BASE, 2), HUD_VAL_GREEN);
	y += 16;
	kv_addr_val(x, y, "usb_ready", (uint32_t)(MUSB_BC_BASE + 16), bc_word(MUSB_BC_BASE, 4), HUD_VAL_GREEN);
	y += 20;

	fb_str(x, y, "EMAC (link):", HUD_HDR_FG, HUD_PANEL_BG);
	y += 16;
	link_up = bc_word(EMAC_BC_BASE, 4);
	kv_addr_val(x, y, "link_up", (uint32_t)(EMAC_BC_BASE + 16), link_up,
		    link_up ? HUD_VAL_GREEN : HUD_VAL_RED);
	y += 16;
	kv_addr_val(x, y, "speed", (uint32_t)(EMAC_BC_BASE + 20), bc_word(EMAC_BC_BASE, 5), HUD_VAL_GREEN);
	y += 16;
	kv_addr_val(x, y, "tx_count", (uint32_t)(EMAC_BC_BASE + 24), bc_word(EMAC_BC_BASE, 6), HUD_VAL_GREEN);
	y += 16;
	kv_addr_val(x, y, "rx_count", (uint32_t)(EMAC_BC_BASE + 28), bc_word(EMAC_BC_BASE, 7), HUD_VAL_GREEN);
	y += 20;

	fb_str(x, y, "GTRC (guest sysreg trace):", HUD_HDR_FG, HUD_PANEL_BG);
	y += 16;
	kv_addr_val(x, y, "event_count", (uint32_t)(GTRC_BC_BASE + 4), bc_word(GTRC_BC_BASE, 1), HUD_VAL_GREEN);
	y += 16;
	kv_addr_val(x, y, "last_sctlr", (uint32_t)(GTRC_BC_BASE + 8), bc_word(GTRC_BC_BASE, 2), HUD_VAL_AMBER);
	y += 16;
	{
		uint32_t fault_count = bc_word(GTRC_BC_BASE, 3);

		kv_addr_val(x, y, "fault_count", (uint32_t)(GTRC_BC_BASE + 12), fault_count,
			    fault_count ? HUD_VAL_RED : HUD_VAL_GREEN);
	}
	y += 20;

	fb_str(x, y, "VCON (guest console):", HUD_HDR_FG, HUD_PANEL_BG);
	y += 16;
	kv_addr_val(x, y, "total_bytes", (uint32_t)(VCON_RING + 4), bc_word(VCON_RING, 1), HUD_VAL_GREEN);
	y += 16;
	kv_addr_val(x, y, "fault_count", (uint32_t)(VCON_RING + 8), bc_word(VCON_RING, 2), HUD_VAL_AMBER);
	y += 20;

	fb_str(x, y, "HDMI (display pipeline):", HUD_HDR_FG, HUD_PANEL_BG);
	y += 16;
	kv_addr_val(x, y, "stage", (uint32_t)(HDMI_BC_BASE + 4), bc_word(HDMI_BC_BASE, 1), HUD_VAL_GREEN);
}

/* ------------------------------------------------------------------ *
 * REGISTER MAP panel — named A64 registers, address + live value.
 * ------------------------------------------------------------------ */
static void
update_regmap_panel(void)
{
	int x = RC_X + 8;
	int y = REGMAP_Y + 24;

	fb_str(x, y, "CNTPCT_EL0 (sysreg):", HUD_LABEL_FG, HUD_PANEL_BG);
	y += 16;
	fb_hex(x + 4 * FB_FONT_W, y, read_cntpct(), 16, HUD_VAL_GREEN, HUD_PANEL_BG);
	y += 20;

	fb_str(x, y, "CNTFRQ_EL0 (sysreg):", HUD_LABEL_FG, HUD_PANEL_BG);
	y += 16;
	fb_hex(x + 4 * FB_FONT_W, y, read_cntfrq(), 16, HUD_VAL_GREEN, HUD_PANEL_BG);
	y += 20;

	kv_addr_val(x, y, "GICD_CTLR", (uint32_t)GICD_CTLR_ADDR, mmio32(GICD_CTLR_ADDR), HUD_VAL_GREEN);
	y += 16;
	kv_addr_val(x, y, "GICC_PMR", (uint32_t)GICC_PMR_ADDR, mmio32(GICC_PMR_ADDR), HUD_VAL_GREEN);
	y += 20;

	kv_addr_val(x, y, "MUSB.POWER", (uint32_t)MUSB_POWER_ADDR, mmio32(MUSB_POWER_ADDR), HUD_VAL_AMBER);
}

/* ================================================================== *
 * SCHED GANTT / CPU TIMELINE panel.
 *
 * Reads the event-trace ring (HVMAP_TRACE_RING, "TRC1") and, for each of the four
 * CPUs, draws a horizontal lane showing which task ran when over the most
 * recent window of events. CTX_SWITCH events (type 1) carry the new task id
 * (word2 arg16) and CPU (word2 bits[15:8]); each switch starts a colored
 * segment for that task until the next switch on the same CPU (or the right
 * edge = newest timestamp). The time window is the timestamp span of the
 * scanned events (oldest..newest), mapped linearly to lane width.
 *
 * Task->color: fixed 8-entry palette; idle task (id 0) is dim gray, other
 * ids map to pal[1 + (id-1) % 7] so distinct ids get distinct hues. CPU%
 * busy = fraction of a lane's covered time NOT running the idle task.
 * ================================================================== */
static inline uint32_t
ev_word(uint32_t slot, int w)
{
	return *(volatile uint32_t *)(uint64_t)
	       (TRC_ENTRIES + slot * TRC_ENTSZ + (uint32_t)w * 4u);
}
static inline uint64_t
ev_ts(uint32_t slot)
{
	uint64_t lo = ev_word(slot, 0);
	uint64_t hi = ev_word(slot, 1);
	return (hi << 32) | lo;
}

static uint32_t
task_color(uint32_t id)
{
	static const uint32_t pal[8] = {
		0xFF556070u, /* idle / gray  */
		0xFF33FF77u, /* green  */
		0xFF00C2D1u, /* teal   */
		0xFFFFB300u, /* amber  */
		0xFFB060FFu, /* purple */
		0xFFFF4433u, /* red    */
		0xFF4488FFu, /* blue   */
		0xFFFF66CCu, /* pink   */
	};

	if (id == GANTT_IDLE_TASK)
		return pal[0];
	return pal[1u + (id - 1u) % 7u];
}

/* Map a [t0,t1] tick interval to an x-span within the lane track and fill. */
static void
gantt_seg(int trk_x, int trk_w, int y, int seg_h,
	  uint64_t tstart, uint64_t span, uint64_t t0, uint64_t t1, uint32_t task)
{
	int x0, x1;

	if (t0 < tstart) t0 = tstart;
	if (t1 < t0)     t1 = t0;
	x0 = trk_x + (int)(((t0 - tstart) * (uint64_t)trk_w) / span);
	x1 = trk_x + (int)(((t1 - tstart) * (uint64_t)trk_w) / span);
	if (x0 < trk_x)          x0 = trk_x;
	if (x1 > trk_x + trk_w)  x1 = trk_x + trk_w;
	if (x1 <= x0)            x1 = x0 + 1;
	if (x1 > trk_x + trk_w)  x1 = trk_x + trk_w;
	if (x1 > x0)
		fb_fillrect(x0, y, x1 - x0, seg_h, task_color(task));
}

static void
update_gantt_panel(void)
{
	int ix = GANTT_X + 6;
	int iy = GANTT_Y + 22;
	int iw = GANTT_W - 12;
	int ih = GANTT_H - 28;
	uint32_t magic, total, head, cap, freq, nvalid, first;
	uint64_t t_start, t_end, span;
	int lw, trk_x, trk_w, lane_top, lane_h, seg_h;
	uint64_t last_t[NCPU], busy[NCPU], seen[NCPU];
	uint32_t last_task[NCPU];
	int have[NCPU];
	uint32_t ctx_count = 0;
	int cpu;

	/* Clear this panel's interior only (chrome + title bar stay put). */
	fb_fillrect(GANTT_X + 1, GANTT_Y + 19, GANTT_W - 2, GANTT_H - 20, HUD_PANEL_BG);

	magic = *(volatile uint32_t *)TRC_BASE;
	if (magic != TRC_MAGIC) {
		fb_str(ix, iy, "(event-trace ring absent -- \"TRC1\" magic not found)",
		       HUD_VAL_DIM, HUD_PANEL_BG);
		return;
	}
	total = *(volatile uint32_t *)(TRC_BASE + 4);
	head  = *(volatile uint32_t *)(TRC_BASE + 8);
	cap   = *(volatile uint32_t *)(TRC_BASE + 12);
	freq  = *(volatile uint32_t *)(TRC_BASE + 16);
	if (cap == 0 || cap > 65536u) {
		fb_str(ix, iy, "(trace ring capacity invalid)", HUD_VAL_DIM, HUD_PANEL_BG);
		return;
	}
	if (total == 0) {
		fb_str(ix, iy, "(no trace events recorded yet)", HUD_VAL_DIM, HUD_PANEL_BG);
		return;
	}
	head %= cap;
	nvalid = (total < cap) ? total : cap;
	if (nvalid > TRC_MAXSCAN)
		nvalid = TRC_MAXSCAN;
	first = (head + cap - nvalid) % cap;

	t_start = ev_ts(first);
	t_end   = ev_ts((first + nvalid - 1u) % cap);
	span    = (t_end > t_start) ? (t_end - t_start) : 1u;

	/* Geometry: label gutter, then the timeline track; 4 stacked lanes. */
	lw = 8 * FB_FONT_W;               /* "CPUn" + "nnn%" gutter */
	trk_x = ix + lw;
	trk_w = iw - lw;
	if (trk_w < 16) trk_w = 16;
	lane_top = iy + 12;
	lane_h = (ih - 12) / NCPU;
	if (lane_h < 10) lane_h = 10;
	seg_h = lane_h - 6;
	if (seg_h < 4) seg_h = 4;

	for (cpu = 0; cpu < NCPU; cpu++) {
		have[cpu] = 0;
		busy[cpu] = 0;
		seen[cpu] = 0;
		last_t[cpu] = 0;
		last_task[cpu] = 0;
	}

	/* Lane backgrounds + CPU labels. */
	for (cpu = 0; cpu < NCPU; cpu++) {
		int ly = lane_top + cpu * lane_h;
		char lab[5] = { 'C', 'P', 'U', (char)('0' + cpu), 0 };

		fb_str(ix, ly, lab, HUD_LABEL_FG, HUD_PANEL_BG);
		fb_fillrect(trk_x, ly, trk_w, seg_h, 0xFF05090Eu);
	}

	/* Single time-ordered pass: draw each task's segment on its CPU lane. */
	for (uint32_t k = 0; k < nvalid; k++) {
		uint32_t slot = (first + k) % cap;
		uint32_t w2 = ev_word(slot, 2);
		uint32_t type = w2 & 0xffu;
		uint32_t c = (w2 >> 8) & 0xffu;
		uint32_t newtask;
		uint64_t t;

		if (type != EV_CTX_SWITCH || c >= (uint32_t)NCPU)
			continue;
		newtask = (w2 >> 16) & 0xffffu;
		t = ev_ts(slot);
		ctx_count++;

		if (have[c]) {
			int ly = lane_top + (int)c * lane_h;

			gantt_seg(trk_x, trk_w, ly, seg_h, t_start, span,
				  last_t[c], t, last_task[c]);
			seen[c] += (t - last_t[c]);
			if (last_task[c] != GANTT_IDLE_TASK)
				busy[c] += (t - last_t[c]);
		}
		last_t[c] = t;
		last_task[c] = newtask;
		have[c] = 1;
	}

	/* An empty lane set has two very different causes, and saying which one
	 * is the whole value of this branch. "TRC1 magic not found" (above) means
	 * the ring is not being produced at all. Reaching here with events in the
	 * ring but zero CTX_SWITCH means the ring IS live and EL2 simply never
	 * multiplexes tasks on this build -- which is the normal, correct state
	 * for the FreeBSD guest target, where the guest owns CPU0 outright and
	 * EL2's scheduler is the no-op fallback. Without this line the panel looks
	 * identical to a broken one. */
	/* Final segment on each CPU: from its last switch to the window end. */
	for (cpu = 0; cpu < NCPU; cpu++) {
		if (have[cpu]) {
			int ly = lane_top + cpu * lane_h;

			gantt_seg(trk_x, trk_w, ly, seg_h, t_start, span,
				  last_t[cpu], t_end, last_task[cpu]);
			seen[cpu] += (t_end - last_t[cpu]);
			if (last_task[cpu] != GANTT_IDLE_TASK)
				busy[cpu] += (t_end - last_t[cpu]);
		}
	}

	/* Per-CPU busy% under each label. */
	for (cpu = 0; cpu < NCPU; cpu++) {
		int ly = lane_top + cpu * lane_h;
		uint64_t pct = seen[cpu] ? (busy[cpu] * 100u) / seen[cpu] : 0;
		uint32_t fg = have[cpu] ? (pct >= 50 ? HUD_VAL_AMBER : HUD_VAL_GREEN)
					: HUD_VAL_DIM;
		int n;

		fb_fillrect(ix, ly + FB_FONT_H + 1, 6 * FB_FONT_W, FB_FONT_H, HUD_PANEL_BG);
		n = fb_dec(ix, ly + FB_FONT_H + 1, pct, fg, HUD_PANEL_BG);
		fb_char(ix + n * FB_FONT_W, ly + FB_FONT_H + 1, '%', fg, HUD_PANEL_BG);
	}

	/* Summary line: context-switch rate + window duration (ms). */
	{
		uint64_t rate = (span && freq)
			      ? ((uint64_t)ctx_count * (uint64_t)freq) / span : 0;
		uint64_t win_ms = freq ? (span * 1000u) / (uint64_t)freq : 0;
		int n;

		fb_fillrect(ix, iy, iw, FB_FONT_H, HUD_PANEL_BG);
		fb_str(ix, iy, "ctx/s:", HUD_LABEL_FG, HUD_PANEL_BG);
		n = fb_dec(ix + 7 * FB_FONT_W, iy, rate, HUD_VAL_GREEN, HUD_PANEL_BG);
		fb_str(ix + (7 + n + 2) * FB_FONT_W, iy, "win(ms):",
		       HUD_LABEL_FG, HUD_PANEL_BG);
		fb_dec(ix + (7 + n + 2 + 8) * FB_FONT_W, iy, win_ms,
		       HUD_VAL_AMBER, HUD_PANEL_BG);

		/* Why the lanes are empty, on the same row and AFTER the
		 * fb_fillrect above -- drawn earlier it was erased by it, and
		 * drawn two rows down it landed on top of the CPU0 label. An
		 * empty lane set from "ring not produced" (handled far above,
		 * where the magic check bails out) and from "ring fine, EL2
		 * simply never switches tasks on this build" look identical
		 * otherwise, and only the second one is normal. */
		if (ctx_count == 0u)
			fb_str(ix + (7 + n + 2 + 8 + 4) * FB_FONT_W, iy,
			       "(ring live, 0 ctx switches: EL2 runs no tasks "
			       "on this build)", HUD_VAL_DIM, HUD_PANEL_BG);
	}
}

/* ================================================================== *
 * PROFILE / HOT PCs panel (flamegraph-ish Top-functions histogram).
 *
 * Reads the profiler histogram (HVMAP_PROF_HIST, "PROF"): buckets of {pc,count}.
 * We scan up to PROF_MAXBKT buckets, select the top PROF_TOPN by count, and
 * draw a labeled horizontal bar per PC (length proportional to count),
 * colored hot->cold by rank. Symbol resolution is host-side, so we show the
 * raw PC hex + the sample count.
 * ================================================================== */
static uint32_t
flame_color(int rank, int total)
{
	static const uint32_t ramp[6] = {
		0xFFFF3322u, /* hottest — red    */
		0xFFFF7722u, /* orange           */
		0xFFFFB300u, /* amber            */
		0xFFCFE030u, /* yellow-green     */
		0xFF55C0E0u, /* cyan             */
		0xFF4488FFu, /* coldest — blue   */
	};
	int idx = (total > 1) ? (rank * 5) / (total - 1) : 0;

	if (idx < 0) idx = 0;
	if (idx > 5) idx = 5;
	return ramp[idx];
}

static void
update_flame_panel(void)
{
	int ix = FLAME_X + 6;
	int iy = FLAME_Y + 22;
	int iw = FLAME_W - 12;
	int ih = FLAME_H - 28;
	uint32_t magic, pcs[PROF_MAXBKT], cnts[PROF_MAXBKT];
	int n = 0, top, a, rowh, bx, bw, cntx;
	uint32_t maxc;

	fb_fillrect(FLAME_X + 1, FLAME_Y + 19, FLAME_W - 2, FLAME_H - 20, HUD_PANEL_BG);

	magic = *(volatile uint32_t *)PROF_BASE;
	if (magic != PROF_MAGIC) {
		fb_str(ix, iy, "(profiler histogram absent -- \"PROF\" magic not found)",
		       HUD_VAL_DIM, HUD_PANEL_BG);
		return;
	}

	for (uint32_t i = 0; i < PROF_MAXBKT; i++) {
		uint32_t pc = *(volatile uint32_t *)
			      (uint64_t)(PROF_BASE + PROF_BUCKET_OFF + i * 8u);
		uint32_t c  = *(volatile uint32_t *)
			      (uint64_t)(PROF_BASE + PROF_BUCKET_OFF + i * 8u + 4u);

		if (c == 0)
			continue;
		pcs[n] = pc;
		cnts[n] = c;
		n++;
	}
	if (n == 0) {
		fb_str(ix, iy, "(no profile samples recorded yet)",
		       HUD_VAL_DIM, HUD_PANEL_BG);
		return;
	}

	top = (n < (int)PROF_TOPN) ? n : (int)PROF_TOPN;

	/* Partial selection sort: bring the top `top` counts to the front. */
	for (a = 0; a < top; a++) {
		int best = a, b;

		for (b = a + 1; b < n; b++)
			if (cnts[b] > cnts[best])
				best = b;
		if (best != a) {
			uint32_t tp = pcs[a], tc = cnts[a];

			pcs[a] = pcs[best];   cnts[a] = cnts[best];
			pcs[best] = tp;       cnts[best] = tc;
		}
	}

	maxc = cnts[0] ? cnts[0] : 1u;
	rowh = ih / (int)PROF_TOPN;
	if (rowh < 9) rowh = 9;
	bx = ix + 9 * FB_FONT_W;                  /* after "pc" hex */
	cntx = ix + iw - 8 * FB_FONT_W;           /* count column   */
	bw = cntx - bx - FB_FONT_W;
	if (bw < 16) bw = 16;

	for (a = 0; a < top; a++) {
		int y = iy + a * rowh;
		int len = (int)(((uint64_t)cnts[a] * (uint64_t)bw) / maxc);

		if (len < 1) len = 1;
		if (len > bw) len = bw;
		fb_hex(ix, y, pcs[a], 8, HUD_LABEL_FG, HUD_PANEL_BG);
		fb_fillrect(bx, y, len, rowh - 3, flame_color(a, top));
		fb_dec(cntx, y, cnts[a], HUD_VAL_GREEN, HUD_PANEL_BG);
	}
}

/* ------------------------------------------------------------------ *
 * hud_update() — refresh dynamic fields only, then flush.
 * ------------------------------------------------------------------ */
void
hud_update(const struct el2_frame *guest)
{
	if (!fb_ready())
		return;

	update_console();
	update_reg_strip(guest);
	update_timing_panel();
	update_trace_exc_panel();
	update_memory_panel();
	update_regmap_panel();
	update_gantt_panel();
	update_flame_panel();

	fb_flush();
}
