/* SPDX-License-Identifier: BSD-2-Clause */

/* fb.h — pixel/rect/text drawing layer for the bzdOS microkernel/hypervisor
 * HUD, operating on a linear XRGB8888 (0xAARRGGBB, alpha ignored/opaque)
 * framebuffer. This is the foundation the future hypervisor HUD compositor
 * builds on: the guest OS shown in a window, surrounded by live diagnostic
 * overlays (register addresses/values, timings/jitter, memory).
 *
 * Deliberately generic: every call operates on whatever buffer/geometry
 * fb_init() was last configured with, so it works equally well on
 * hdmi_fb() (see hdmi.h) or any other caller-owned buffer/backing store —
 * no dependency on hdmi.c beyond hdmi_demo() (in hdmi.c) using both.
 *
 * Freestanding: only <stdint.h>, no libc (no memset/memcpy — every fill is
 * a manual word loop so nothing pulls in a libgcc/NEON helper under
 * -mgeneral-regs-only), no floats.
 */
#ifndef BZDOS_FB_H
#define BZDOS_FB_H
#include <stdint.h>

/* Configure the drawing surface. base: pixel (0,0), 32bpp XRGB8888/ARGB8888
 * (alpha byte is written but the display engine ignores it — DE2's UI
 * layer format is XRGB8888). w,h: dimensions in pixels. stride: pixels per
 * scanline (>= w; lets a caller draw into a sub-rect of a larger backing
 * buffer without the drawing code needing to know). If base is NULL or the
 * geometry is degenerate (w<=0, h<=0, stride<w) the surface is left
 * not-ready: fb_ready() returns 0 and every draw call below becomes a
 * bounds-safe no-op. */
void fb_init(uint32_t *base, int w, int h, int stride);

/* 1 once fb_init() has accepted a plausible buffer/geometry, 0 otherwise. */
int fb_ready(void);

/* Fill the whole configured surface with a 0xAARRGGBB color. */
void fb_clear(uint32_t argb);

/* Single pixel. Out-of-bounds (x,y) is a silent no-op (never touches
 * memory outside the buffer). */
void fb_pixel(int x, int y, uint32_t argb);

/* Filled rectangle. Clipped to the surface bounds; degenerate (w<=0 or
 * h<=0) is a no-op. */
void fb_fillrect(int x, int y, int w, int h, uint32_t argb);

/* Rectangle outline, 1px lines on all four edges (drawn via fb_fillrect,
 * so it shares the same clipping). */
void fb_rect(int x, int y, int w, int h, uint32_t argb);

/* ------------------------------------------------------------------ *
 * Text: 8x8 monospace bitmap font, ASCII 0x20..0x7E (embedded, public-
 * domain "PC/VGA 8x8" glyph shapes — see fb.c).
 * ------------------------------------------------------------------ */
#define FB_FONT_W 8
#define FB_FONT_H 8

/* One character cell at (x,y) (top-left corner), fg on bg. Characters
 * outside the printable range render as a solid bg-filled cell (silently —
 * this is a HUD overlay, not a terminal, so there's no cursor/scroll
 * state to get confused). */
void fb_char(int x, int y, char c, uint32_t fg, uint32_t bg);

/* NUL-terminated string, left-to-right, FB_FONT_W apart, no wrapping (the
 * HUD compositor owns layout — this is a primitive, not a console). */
void fb_str(int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* Hex dump helper: v printed as exactly `ndigits` hex nibbles (clamped to
 * 1..16), most-significant nibble first, no "0x" prefix — for register
 * addresses/values in the HUD. */
void fb_hex(int x, int y, uint64_t v, int ndigits, uint32_t fg, uint32_t bg);

/* Decimal dump helper: v printed as an unsigned decimal integer, no
 * padding (at most 20 digits for a 64-bit value) — for timings/counters/
 * jitter values in the HUD. Returns the number of characters drawn (so a
 * caller can position whatever comes next without re-measuring). */
int fb_dec(int x, int y, uint64_t v, uint32_t fg, uint32_t bg);

/* Ensure drawn pixels are visible to the display engine's DMA: cleans
 * (`dc civac`) every cache line touched since the last fb_flush() (or
 * fb_init()), then `dsb sy`. MUST be called after a batch of draw calls
 * and before relying on the monitor showing them — the D-cache is on
 * (MMU on, per house style) and the DE2 mixer reads straight from DRAM. */
void fb_flush(void);

#endif /* BZDOS_FB_H */
