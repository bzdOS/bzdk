/* SPDX-License-Identifier: BSD-2-Clause */

/* wdt.c — Allwinner A64 watchdog driver (see wdt.h). Bare-metal, MMIO via
 * absolute physical addresses (U-Boot leaves the MMU on with a flat device
 * mapping — same contract as musb.c).
 *
 * Register map (A64 WDOG):
 *   WDOG_CTRL @ 0x01C20CB0 : bit0 RSTART, bits[12:1] key=0x0A57 -> write
 *                            (0x0A57<<1)|1 = 0x14AF to (re)start the counter.
 *   WDOG_CFG  @ 0x01C20CB4 : [1:0] 1 = reset whole system on timeout.
 *   WDOG_MODE @ 0x01C20CB8 : bit0 EN, bits[7:4] interval index; 11 = ~16 s
 *                            (0xB1) — this is the HARDWARE MAXIMUM interval.
 *
 * ── Dead-man's switch with a MINUTES-level effective timeout ──────────────
 * The hardware maxes out at a 16 s interval, but we want to auto-reboot only
 * after MINUTES of no forward progress — a short 16 s HW timeout would kill
 * legitimate quiet boot phases (locore / device probing gaps). So we extend it
 * in software using the physical counter (CNTPCT_EL0) as a real-time clock:
 *
 *   - wdt_note_progress() is called whenever the guest makes OBSERVABLE
 *     progress (emits a console byte via vconsole THR write). It timestamps
 *     "last progress" from CNTPCT.
 *   - wdt_pet() is called from el2_trap() on every EL2 exception. It re-arms
 *     the 16 s HW timer ONLY IF the last progress was within WDT_TIMEOUT_S.
 *     Once progress has been stale for > WDT_TIMEOUT_S, wdt_pet() STOPS
 *     re-arming, so the HW watchdog fires within its remaining ≤16 s window
 *     and resets the board back to U-Boot (persistent chimpd reloads).
 *
 * Net behaviour:
 *   - Healthy boot (console output flowing): progress fresh → petted → resident.
 *   - Busy wedge (traps constantly but no console, e.g. a poll storm): el2_trap
 *     keeps calling wdt_pet, but progress is stale → not re-armed → reboots
 *     ~WDT_TIMEOUT_S after output stopped. (This is the case el2_trap-only
 *     petting got WRONG — it would keep a busy wedge alive forever.)
 *   - Total wedge (no traps at all): wdt_pet never runs → HW fires 16 s after
 *     the last pet. Bounded ≤16 s.
 *   - Effective progress timeout = WDT_TIMEOUT_S (+ up to 16 s HW slack).
 */
#include <stdint.h>
#include "wdt.h"
#include "cntpct.h"
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

#define WDOG_CTRL (*(volatile uint32_t *)SOC_A64_WDOG_CTRL)
#define WDOG_CFG  (*(volatile uint32_t *)SOC_A64_WDOG_CFG)
#define WDOG_MODE (*(volatile uint32_t *)SOC_A64_WDOG_MODE)

#define WDOG_CTRL_RESTART  (((uint32_t)0x0A57u << 1) | 1u)   /* 0x14AF */
#define WDOG_CFG_RESET_SYS 0x00000001u
#define WDOG_MODE_16S_EN   (((uint32_t)11u << 4) | 1u)       /* 0xB1 */

/* Minutes-level effective timeout: reboot after this many seconds without the
 * guest emitting a console byte. Tunable; 180 s = 3 min tolerates long quiet
 * boot phases while still recovering a real wedge autonomously. */
#define WDT_TIMEOUT_S      180u

static uint64_t wdt_last_progress;   /* CNTPCT at the last observed progress   */

/* ── Reachability gate ────────────────────────────────────────────────────
 * Every pet path used to keep the board resident on a proof of LIFE: CPU1's
 * tick kicked unconditionally, CPU0 kicked while the guest made progress.
 * Neither says anything about whether anyone can still REACH the board. With
 * the EMAC dark and the USB gadget dead, a perfectly alive hypervisor fed its
 * watchdog for ever and was invisible on every channel -- the same shape as
 * U-Boot's parked prompt (HANDOFF 2026-09-26), one level up. Nothing but the
 * power switch could end it.
 *
 * So every pet now also requires that the board was reachable within
 * WDT_UNREACH_S: an EMAC frame received (emac.c calls wdt_note_reachable()),
 * or the USB host still clocking the bus -- the MUSB frame number advances
 * with every SOF only while a host holds the port enabled; a wedged gadget
 * that the host gave up on stops it. Past that, nobody pets, the 16 s WDOG
 * fires, and the board comes back through U-Boot.
 *
 * This does not replace the EMAC-dark rule: with the USB console alive the
 * board is reachable and a dark EMAC is left to that rule. Only "no channel
 * at all" trips this.
 *
 * wdt_unreach_test != 0 ignores both sources (hardware test of this gate:
 * poke it over EMAC, the board must reset ~WDT_UNREACH_S + 16 s later). */
#define WDT_UNREACH_S      900u
#define MUSB_FRAME_REG (*(volatile uint16_t *)(SOC_A64_MUSB_BASE + 0x54u)) /* sunxi layout */
static uint64_t wdt_unreach_ticks;
static volatile uint64_t wdt_last_reach;
static uint16_t wdt_last_frame;
volatile uint32_t wdt_unreach_test;
static uint64_t wdt_window_ticks;    /* WDT_TIMEOUT_S expressed in counter ticks */

static inline uint64_t rd_cntpct(void)
{
    uint64_t v;
    v = cntpct_read();   /* cntpct.h: Allwinner counter erratum */
    return v;
}

static inline uint64_t rd_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

void
wdt_arm(void)
{
    uint64_t f = rd_cntfrq();
    if (f == 0)
        f = 24000000ull;              /* A64 arch timer default 24 MHz */
    wdt_window_ticks  = f * (uint64_t)WDT_TIMEOUT_S;
    wdt_last_progress = rd_cntpct();   /* treat arm time as fresh progress */
    wdt_unreach_ticks = f * (uint64_t)WDT_UNREACH_S;
    wdt_last_reach    = wdt_last_progress;  /* boot grace: one full window */
    wdt_last_frame    = MUSB_FRAME_REG;
    wdt_unreach_test  = 0u;

    /* CLEAR THE HOLD FLAG. It lives at a FIXED ADDRESS in hv-scratch DRAM
     * (HVMAP_WDT_DEBUG_HOLD), and DRAM survives the warm reset the WDOG
     * performs -- so a hold, whose entire purpose is "stop petting so the
     * watchdog fires once", used to survive the very reset it asked for. Every
     * generation after it then read hold=1, never petted, and died at the 16 s
     * window: a self-sustaining reset loop that no reload could break, because
     * nothing in any build cleared this word.
     *
     * That is not hypothetical -- el2_exc.c's reboot_clean/dbg_clean_reset paths
     * SET this flag deliberately on their way out, so an ordinary
     * reliable_load.py cycle armed the trap. Observed 2026-08-20: after one such
     * reload the board reset every 15-16 s for over an hour, across four
     * different images, looking exactly like a hypervisor regression.
     *
     * A hold is a statement about the CURRENTLY RUNNING generation, so honouring
     * it past the reset it caused is wrong on its own terms. Set it again over
     * the net (bmc `wdt hold`) whenever a fresh one is wanted. */
    wdt_debug_hold = 0u;

    WDOG_CFG  = WDOG_CFG_RESET_SYS;
    WDOG_MODE = WDOG_MODE_16S_EN;
    WDOG_CTRL = WDOG_CTRL_RESTART;      /* start counting from full 16 s interval */
}

/* Guest made observable progress (emitted a console byte). Refresh the window. */
void
wdt_note_progress(void)
{
    wdt_last_progress = rd_cntpct();
}

void
wdt_note_reachable(void)
{
    if (!wdt_unreach_test)
        wdt_last_reach = rd_cntpct();
}

/* Samples the USB frame number (cheap: one MMIO read) and answers whether
 * any channel reached the board within WDT_UNREACH_S. Several cores call
 * this; the shared stamp is a single aligned 64-bit store, and a stamp
 * another core wrote a moment "later" than our own `now` counts as fresh
 * rather than as a huge unsigned age. */
static int
wdt_reach_fresh(uint64_t now)
{
    uint64_t last = wdt_last_reach;

    if (last >= now)
        return 1;
    return (now - last) < wdt_unreach_ticks;
}

int
wdt_reach_ok(void)
{
    return wdt_reach_fresh(rd_cntpct());
}

static int
wdt_reachable(void)
{
    uint64_t now = rd_cntpct();

    if (!wdt_unreach_test) {
        uint16_t fr = MUSB_FRAME_REG;
        if (fr != wdt_last_frame) {
            wdt_last_frame = fr;
            wdt_last_reach = now;
        }
    }
    return wdt_reach_fresh(now);
}

/* Called on every EL2 exception. Re-arm the HW timer only while progress is
 * fresh; once stale > WDT_TIMEOUT_S, stop feeding it so it fires. */
void
wdt_pet(void)
{
    /* Honor wdt_debug_hold here too (previously only wdt_debug_kick() did).
     * Without this, CPU0's guest-progress-gated pet kept the HW WDOG fed any
     * time the guest was alive and chatty — exactly the "EMAC dead, guest
     * alive" case — so setting wdt_debug_hold could NOT force the documented
     * remote reset. See emac-flakiness-analysis (hypothesis #5). */
    if (wdt_debug_hold)
        return;
    /* CPU0, every EL2 exception: compare the stamp only -- the MMIO sample
     * happens on CPU1's tick (wdt_debug_kick), not hundreds of times a
     * second here. If CPU1 is dead nobody samples, the stamp ages, and the
     * board resets after WDT_UNREACH_S: with CPU1 gone the debug channel is
     * gone too, so that is the right answer. */
    uint64_t now = rd_cntpct();

    if (!wdt_reach_fresh(now))
        return;
    if (now - wdt_last_progress < wdt_window_ticks)
        WDOG_CTRL = WDOG_CTRL_RESTART;
    /* else: stale — do NOT re-arm; the HW watchdog will fire and reset. */
}

void
wdt_disarm(void)
{
    WDOG_MODE = 0;                     /* clear enable */
}

/* ── SMP debug-core watchdog ownership ────────────────────────────────────
 * When the dedicated debug core (CPU1) is up it pets the HW WDOG here on every
 * poll iteration — UNCONDITIONALLY, not progress-gated: as long as CPU1 is
 * alive the board is inspectable over EMAC, so we keep it resident even when
 * the guest (CPU0) idles/wedges with no console output. This deliberately
 * supersedes the guest-progress gate (wdt_pet): a wedged-but-inspectable board
 * is MORE useful than an auto-rebooting one, and I trigger resets myself over
 * EMAC. To do that, set wdt_debug_hold != 0 (a single memory write over the
 * net): CPU1 then stops petting and the HW WDOG fires within its ≤16 s window,
 * resetting to U-Boot (chimpd reloads). If CPU1 itself ever dies, petting stops
 * on its own and the same ≤16 s HW fire recovers the board — so the catastrophic
 * case is still covered automatically.
 *
 * wdt_debug_hold is a FIXED-ADDRESS flag (HVMAP_WDT_DEBUG_HOLD, hv_addrmap.h),
 * #defined as a macro in wdt.h — not a plain BSS global anymore. That is what
 * makes "a single memory write over the net" literally true: a host tool can
 * `w <addr> <val>` it directly, with no `nm`-resolved symbol address and
 * therefore no risk of writing to where a stale on-disk ELF THINKS the symbol
 * lives while a different build is actually running (see hv_addrmap.h's
 * HVMAP_WDT_DEBUG_HOLD comment and hvdbg.py's wdt_reset()). */

void
wdt_debug_kick(void)
{
    if (!wdt_debug_hold && wdt_reachable())
        WDOG_CTRL = WDOG_CTRL_RESTART;
}
