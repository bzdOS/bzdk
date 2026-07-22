/* flightrec.h — "flight recorder": a generalized ring buffer of the last N
 * thousand events (traps, IRQ injects, virtio ops, console bytes, ...) in
 * reserved RAM, for post-mortem timeline reconstruction after any crash.
 * See ROADMAP.md B4.
 *
 * This is the FIRST generalized instrument in this tree — every breadcrumb
 * before it (EXC1, BTR1, SST1, FF1V, VBK1, ...) is a bespoke one-off ring
 * hardcoded to one subsystem. flightrec_log() is meant to be the ONE cheap
 * call any module reaches for instead of inventing its own ring: a plain
 * (kind, a0, a1) tuple, no per-subsystem struct, no separate breadcrumb
 * window to allocate/document.
 *
 * Freestanding: <stdint.h> only. No libc, no allocation, no recursion — safe
 * to call from EL2 trap/tick context, exactly like every other breadcrumb
 * writer in this tree.
 *
 * -------------------------------------------------------------------------
 * BREADCRUMB WINDOW
 * -------------------------------------------------------------------------
 * DRAM 0x50012000, magic "FLTR". Chosen clear of every window in smp.h's map
 * AND past vconsole's 64 KiB postmortem capture ring (0x50000f10..0x50010f10)
 * AND past el2_ncmap's NCM1 window (0x50011000, tiny) — see el2_ncmap.c's own
 * comment for why "past the vconsole ring" is the load-bearing constraint:
 * every low window (PANC/WCET/KSY1/KTMR/FF1V/BMC/PROF/ONEBP/...) nominally
 * documented in the 0x50000000-0x50008000 range actually sits INSIDE that
 * ring and gets clobbered by captured console bytes. 0x50012000 is comfortably
 * past it (ring ends 0x50010f10) and comfortably CLEAR of the DTB-reserved
 * hv-scratch carve-out that vblk_emmc.c/emmc_bio.c use starting at 0x50020000
 * (VBK1 lock/EBIO/HS-testbuf occupy 0x50020000..~0x50020500) — the free gap
 * between the two is 0x50011100..0x50020000, about 61 KiB, of which this ring
 * uses the first ~40 KiB:
 *
 *   Header (8 words @ 0x50012000):
 *     [0] magic FLTR_MAGIC (0x464C5452, "FLTR")
 *     [1] total events logged this boot (monotonic uint32, wraps at 2^32)
 *     [2] head        next slot index to write (0..FLTR_SLOTS-1, wraps)
 *     [3] capacity    FLTR_SLOTS, so a reader never has to hardcode it
 *     [4] slot_words  FLTR_SLOT_WORDS (stride), ditto
 *     [5..7] reserved
 *
 *   Slot i (5 words @ 0x50012000 + (8 + i*5)*4):
 *     [0] kind   (caller-defined small integer, see FLTR_K_* below)
 *     [1] a0 lo  [2] a0 hi
 *     [3] a1 lo  [4] a1 hi
 *
 * FLTR_SLOTS=2048 -> ring occupies 0x50012000..0x5001C020 (~40 KiB), holding
 * the last 2048 events — comfortably "N thousand" per the ROADMAP wording.
 * Wraps: once `head` reaches FLTR_SLOTS it wraps to 0 and starts overwriting
 * the oldest entries, exactly like every other ring in this tree (SST1, BTR1).
 * There is no separate reset-on-crash step: a crash's WDT warm-reboot leaves
 * the ring exactly as it was at the moment of the fault (DRAM survives the
 * reset, same trick as every other breadcrumb), so "post-mortem" is simply
 * reading it after the reboot — no explicit "auto-reset on crash" logic is
 * needed or implemented.
 *
 * -------------------------------------------------------------------------
 * COST
 * -------------------------------------------------------------------------
 * Each flightrec_log() call writes 5(+ up to 3 header) 32-bit words with
 * `dc civac` per word (so the ring survives a WDT reset even if the fault
 * that triggered the log IS the reason for the reset) but only ONE `dsb sy`
 * at the end of the whole call — not one per word like the older single-shot
 * breadcrumbs (EXC1/BTR1/etc, which fire rarely). flightrec_log() is meant to
 * be called far more often (per-trap, per console byte, ...), so batching the
 * barrier is the difference between "cheap" and "a dsb storm".
 *
 * -------------------------------------------------------------------------
 * CONFIG-TABLE GENERALIZATION (stretch goal, NOT implemented here)
 * -------------------------------------------------------------------------
 * The ROADMAP's "natural generalization" is a table of (PA-range -> logger)
 * so any MMIO trap dispatch (vconsole's UART0 trap today) can be
 * auto-instrumented without a recompile. That needs a registration API
 * (flightrec_register(pa_lo, pa_hi, kind)) and el2_trap threading every
 * lower-EL data-abort's FAR through a table lookup before/after each
 * device's own fault handler — a bigger, more invasive change than this
 * file. flightrec_log() is deliberately usable standalone in the meantime:
 * every call site below is a plain, explicit function call, not a table
 * lookup.
 */
#ifndef BZDOS_FLIGHTREC_H
#define BZDOS_FLIGHTREC_H

#include <stdint.h>

#define FLTR_BC_BASE     0x50012000UL
#define FLTR_MAGIC       0x464C5452u   /* "FLTR" */
#define FLTR_SLOTS       2048u
#define FLTR_HDR         8u            /* header words before the slot array */
#define FLTR_SLOT_WORDS  5u            /* kind, a0 lo/hi, a1 lo/hi */

/* Event kinds. Small, open-ended enum — callers may use their own values;
 * these are just the ones this tree's call sites use today. Keep values
 * stable once used elsewhere (a host-side reader keys off them). */
enum {
	FLTR_K_FAULT   = 1,   /* an el2_trap() fault recorded for post-mortem   */
	FLTR_K_TRAP    = 2,   /* reserved: generic guest trap (not yet logged) */
	FLTR_K_IRQ     = 3,   /* reserved: IRQ injected into the guest         */
	FLTR_K_VIRTIO  = 4,   /* reserved: virtio-mmio op (vblk, future net)   */
	FLTR_K_CONSOLE = 5,   /* reserved: a captured console byte             */
};

/* Log one (kind, a0, a1) event into the ring. Never fails, never blocks,
 * never faults — a plain wrapping-ring write, safe to call from EL2
 * trap/tick context (same defensiveness contract as every other breadcrumb
 * writer in this tree). */
void flightrec_log(uint32_t kind, uint64_t a0, uint64_t a1);

#endif /* BZDOS_FLIGHTREC_H */
