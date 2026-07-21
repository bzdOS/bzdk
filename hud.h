/* hud.h — hypervisor HUD compositor for the bzdOS EL2 hypervisor
 * (Allwinner A64 / Banana Pi M64). Draws the "hacker dashboard": the guest
 * OS shown live in a bordered window, surrounded by diagnostic overlay
 * panels (guest register state, RTOS-style tick/jitter timing, breadcrumb
 * hex dumps, and a small educational live register map) — all rendered
 * straight to the HDMI scanout framebuffer via fb.h's primitives.
 *
 * The headline surface is the GUEST window: it renders the FreeBSD guest's
 * captured UART console text (vconsole ring at 0x50000f00) live, so the
 * guest kernel's console output appears ON the monitor, plus a compact
 * strip of the guest's saved registers. The right column stacks
 * TIMING/JITTER, GUEST-TRACE + EL2-FAULT (with SCTLR MMU/cache and ESR
 * exception-class decodes), MEMORY/BREADCRUMBS, and a live REGISTER MAP.
 *
 * Ownership: this file draws pixels only. It does not bring up HDMI
 * (hdmi.c/hdmi_init()), does not own the main loop or IRQ wiring, and does
 * not touch the Makefile — the integrator calls hud_init() once after
 * hdmi_init(), then hud_update() periodically (tick handler or REPL loop).
 *
 * Freestanding: only <stdint.h> (via the headers it includes), no libc,
 * no floats, -mgeneral-regs-only. Every draw is over a fixed set of
 * panels/fields (no unbounded loops), so hud_update() is safe to call from
 * IRQ/tick context.
 */
#ifndef BZDOS_HUD_H
#define BZDOS_HUD_H

#include <stdint.h>
#include "exceptions.h"

/* One-time setup: fb_init()s against the live HDMI framebuffer (hdmi_fb()/
 * hdmi_width()/hdmi_height()/hdmi_stride()) and draws all STATIC chrome —
 * background, title bar, the GUEST window border, and every panel's
 * border + header labels. Call this exactly once, after hdmi_init() has
 * brought the pipeline up (so hdmi_fb() is valid) and before the first
 * hud_update(). Safe to call even if the HDMI pipeline never scanned out
 * (it still draws into DRAM); it just won't be visible.
 *
 * Does NOT call fb_flush() internally at the very end beyond what's needed
 * to make the static chrome visible — it does flush once so the initial
 * screen shows up without waiting for the first hud_update().
 */
void hud_init(void);

/* Periodic refresh: overwrites ONLY the dynamic value fields in each panel
 * (guest registers, timing/jitter, breadcrumb hex values, live register
 * map) — never re-clears/redraws the static chrome, so there is no
 * full-screen flicker. `guest` is the most recently saved EL2 trap frame
 * (e.g. from the tick IRQ, or the last vmexit) and may be NULL (the GUEST
 * panel then shows its fields as zero/dashes rather than dereferencing).
 *
 * Reads, live, every call:
 *   - The vconsole capture ring (0x50000f00 "UART": total_bytes at word[1],
 *     then the 3 KiB byte buffer at 0x50000f10 wrapping at total%3072) —
 *     rendered as the guest console text (last screenful of lines) in the
 *     GUEST window.
 *   - guest->x[0..7], x[29], x[30], guest->elr/spsr/esr/far/sp_at_entry
 *     (from the passed-in frame — the last-saved EL2 view of the guest),
 *     plus ELR_EL1/ESR_EL1/FAR_EL1/SP_EL1 via `mrs` (EL2 reads EL1 sysregs
 *     directly), in the compact register strip.
 *   - GTRC (0x50002000): event_count, last_sctlr (SCTLR.M/C/I decoded as
 *     MMU/D-cache/I-cache ON/off), fault_count — the guest-trace panel.
 *   - EXC1 (0x50000400): count/kind/esr/elr/far — the last EL2 fault, with
 *     ESR exception-class decoded (DataAbort/InstrAbort/HVC/BRK/...), shown
 *     in red when a fault has been recorded.
 *   - CNTPCT_EL0/CNTFRQ_EL0 via `mrs`, and the TIMR (0x50000500) and GICT
 *     (0x50000800) breadcrumb windows, for the timing/jitter panel.
 *   - MUSB (0x50000000), EMAC (0x50000100), GTRC, VCON and HDMI
 *     (0x50003000) breadcrumb windows, for the memory/breadcrumb panel.
 *   - GICD_CTLR/GICC_PMR (0x01c81000/0x01c82004 — status reads only, no
 *     side effects) and the MUSB POWER register (0x01c19040), for the
 *     educational live register-map panel.
 *   - Event-trace ring (0x50004000 "TRC1": total_events/head_slot/capacity/
 *     timer_freq_hz header, 16-byte entries at 0x50004040) — walked (capped)
 *     for CTX_SWITCH events to draw the SCHED GANTT / CPU TIMELINE panel: a
 *     lane per CPU with colored per-task segments, a context-switch rate, and
 *     a per-CPU busy% (time not in the idle task, assumed task id 0).
 *   - Profiler histogram (0x50004800 "PROF": {u32 pc, u32 count} buckets) —
 *     top samples drawn as hot->cold horizontal bars in the PROFILE / HOT PCs
 *     panel (raw PC hex + count; symbol resolution is host-side).
 * Both new sources are magic-validated and render a dim placeholder when
 * absent/empty; every walk is hard-capped so a bad ring cannot hang.
 *
 * Bounded and non-blocking: a fixed number of panels/fields; the only
 * variable-length loop (console text) is hard-capped by the 3 KiB capture
 * ring. No allocation. Ends with fb_flush() so the DE2 mixer's next scanout
 * shows the new values. Safe to call from IRQ/tick context.
 */
void hud_update(const struct el2_frame *guest);

#endif /* BZDOS_HUD_H */
