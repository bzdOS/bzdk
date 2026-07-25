/* main_repl.c — bzdOS microkernel entry for the RESIDENT interactive REPL.
 *
 * Analogue of main.c / main_net.c, but instead of a fixed heartbeat loop it
 * brings up the EMAC raw-Ethernet console and then hands off to repl_run()
 * (repl.c) — a live "hardware REPL" that stays resident forever, letting us
 * peek/poke registers, call functions, and re-run MUSB bring-up AT RUNTIME
 * over the net, collapsing the debug loop from a ~90 s reflash to milliseconds.
 *
 * I/O split (the whole point): the REPL's own command channel is EMAC, while
 * MUSB is the device-under-test we poke at. The console_* hooks below are the
 * REPL's only I/O and are thin wrappers over emac_*; nothing in the REPL path
 * touches musb_* except the explicit `mi`/`mp`/`mpN` test commands. So we can
 * drive MUSB into any (even wedged) state without ever losing our prompt.
 *
 * Watchdog contract (identical to main.c/main_net.c): arm the WDT BEFORE any
 * risky bring-up; the resident REPL loop pets it every iteration; if the EMAC
 * link never comes up we STOP petting so the proven ~16 s WDT reset returns us
 * to a fresh U-Boot and never bricks the session.
 *
 * Freestanding: only <stdint.h>, "emac.h", "wdt.h", "repl.h".
 */
#include <stdint.h>
#include "emac.h"
#include "wdt.h"
#include "repl.h"
#include "reboot.h"
#include "hdmi.h"
#include "hud.h"

/* ------------------------------------------------------------------ *
 * DRAM breadcrumb — REPL window.
 *
 * Same mechanism as main.c (write a fixed DRAM word window, cleaned to the
 * Point of Coherency so it survives the WDT reset with the D-cache on), but at
 * a DISTINCT base and with a DISTINCT magic so it never collides with the MUSB
 * breadcrumb (0x50000000) or the EMAC breadcrumb (0x50000100):
 *
 *   base 0x50000300, magic 0x4D4C5201 ("MLR\1" = MicrokerneL Repl v1)
 *   [0] 0x4D4C5201  magic (proves THIS payload, the REPL, wrote it)
 *   [1] stage       max checkpoint reached (BC_STAGE_* below)
 *   [2] link_wait   emac_link_up() poll iterations spent waiting for link
 * ------------------------------------------------------------------ */
#define BC_BASE   0x50000300UL
#define BC_MAGIC  0x4D4C5201u

#define BC_STAGE_ENTER    1u   /* entered main(), before wdt_arm */
#define BC_STAGE_WDT      2u   /* watchdog armed */
#define BC_STAGE_EMAC     3u   /* emac_init() returned */
#define BC_STAGE_LINKWAIT 4u   /* waiting (bounded) for link up */
#define BC_STAGE_LINKUP   5u   /* PHY link up */
#define BC_STAGE_BANNER   6u   /* ready banner emitted over EMAC */
#define BC_STAGE_REPL     7u   /* repl_run() entered (resident) */
#define BC_STAGE_NOLINK  99u   /* gave up: no link -> WDT reset */

/* Stand-in for dbgmon.c's global of the same name (see main_gdb.c's file
 * header for the full rationale). el2_exc.c's cmd_call() fault-recovery
 * path references this `extern`, unconditionally, from every build; the
 * REPL never links dbgmon.o (repl.c is its own separate command loop, with
 * no "call a guest function" command that would set this), so el2_exc.c's
 * guard is simply always false here -- a no-op, exactly like main_gdb.c and
 * main_fbsd.c's identical stand-in. */
volatile int dbgmon_call_active = 0;

static inline void bc_write(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}
#define BC(i, v) bc_write((i), (uint32_t)(v))

/* Bound the wait for link so a dead PHY ends in a clean WDT reset rather than a
 * pet-forever spin. Tuned like main.c's ENUM_GIVEUP against observed poll speed;
 * emac_init() already does its own bounded autoneg wait, this just catches a
 * slightly-late link. */
#define LINK_GIVEUP 60000000u

/* ------------------------------------------------------------------ *
 * Console hooks the REPL (repl.c) uses for ALL of its I/O. Thin wrappers over
 * the EMAC channel — keeping the REPL's command path entirely off MUSB, which
 * is the device-under-test.
 * ------------------------------------------------------------------ */
int  console_getc(void)   { return emac_getc(); }
void console_putc(int c)  { emac_putc(c); }
void console_poll(void)   { emac_poll(); }
void console_flush(void)  { emac_flush(); }
int  console_link_up(void){ return emac_link_up(); }

int main(void)
{
	uint32_t link_wait = 0;
	int rc;

	/* Prove we ran and lay down the magic before anything risky. */
	BC(0, BC_MAGIC);
	BC(1, BC_STAGE_ENTER);

	/* Clean-disconnect U-Boot's now-unserviced USB gadget on takeover so a
	 * later WDT reset re-enumerates cleanly (avoids the zombie). */
	usb_gadget_disconnect();

	/* Bring up HDMI + draw the HUD-skeleton demo. The DE2 scans the frame
	 * buffer out in hardware, so it stays on the monitor while the REPL loop
	 * runs — giving a persistent display that is ALSO network-controllable
	 * (reset/poke/reload over the net), so no more physical resets to swap it. */
	hdmi_init();
	hud_init();              /* draw the HUD chrome; repl_run() refreshes it live */

	/* Arm the watchdog BEFORE bring-up — a hang/fault now self-resets the
	 * board in ~16 s to a fresh U-Boot. The resident REPL loop pets it. */
	wdt_arm();
	BC(1, BC_STAGE_WDT);

	rc = emac_init();
	BC(1, BC_STAGE_EMAC);

	/* Wait (bounded) for the PHY link. emac_init() already does a bounded
	 * autoneg wait, but poll a little longer here in case link settles just
	 * after init. If it never comes up, STOP petting the WDT and spin so the
	 * proven ~16 s reset returns us to a fresh U-Boot (mirrors main.c). */
	BC(1, BC_STAGE_LINKWAIT);
	(void)rc;   /* emac_link_up() is the source of truth for link state */
	while (!emac_link_up()) {
		if (++link_wait >= LINK_GIVEUP) {
			BC(1, BC_STAGE_NOLINK);
			BC(2, link_wait);
			for (;;) {
				/* intentionally NO wdt_pet() — let the WDT reset us */
			}
		}
		emac_poll();
		wdt_pet();
		BC(2, link_wait);
	}
	BC(1, BC_STAGE_LINKUP);
	BC(2, link_wait);

	/* Link is up — announce readiness over EMAC, then go resident. */
	emac_puts("bzdOS microkernel REPL ready\r\n");
	emac_flush();
	BC(1, BC_STAGE_BANNER);

	BC(1, BC_STAGE_REPL);
	repl_run();   /* never returns; pets WDT + polls console every iteration */

	/* Unreachable, but keep main()'s type contract with start.S. */
	return 0;
}
