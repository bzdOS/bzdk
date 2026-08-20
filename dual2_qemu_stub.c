/* SPDX-License-Identifier: BSD-2-Clause */

/* dual2_qemu_stub.c — inert stand-ins for the handful of real-hardware
 * symbols smp.o still references UNCONDITIONALLY on the dual-guest Step 1
 * QEMU target (main_dual2_qemu.c / dual-qemu-ci.sh), mirroring
 * smp_qemu_stub.c's existing precedent (see that file's header for the full
 * "why these are needed at link time despite never executing" argument --
 * identical reasoning applies here unchanged).
 *
 * WHY THIS IS A SEPARATE FILE FROM smp_qemu_stub.c, NOT A REUSE OF IT:
 * This target ALSO links the REAL wdt.o (needed for vconsole.c's
 * wdt_note_progress(), which zguest_cpu3.c's call to vconsole_init_chan1()
 * pulls in transitively -- see ZEPHYR_QEMU_OBJS's own Makefile comment for
 * why wdt.o is safe and already-proven portable under QEMU: only
 * wdt_note_progress() is ever reachable, a plain CNTPCT timestamp write,
 * never wdt_pet()/wdt_init(), which are the parts that poke the real A64
 * WDOG). smp_qemu_stub.c ALSO defines wdt_debug_kick() -- linking both would
 * be a duplicate-symbol error. This file therefore provides every symbol
 * smp_qemu_stub.c does EXCEPT that one, which the REAL wdt.o already
 * supplies (dead code on this target either way, dbg_core_enable=0 -- see
 * main_dual2_qemu.c -- means CPU1's debug-core branch that would call
 * wdt_debug_kick() never runs). wdt_debug_hold itself is no longer a linked
 * symbol at all (wdt.h #defines it as a fixed hv-scratch-address macro, see
 * hv_addrmap.h's HVMAP_WDT_DEBUG_HOLD), so it was never a third thing either
 * file needed to provide.
 *
 * Freestanding: <stdint.h> + the real project headers whose prototypes
 * these bodies must match exactly (musb.h, emac.h, dbgmon.h, usbacm.h,
 * exceptions.h), no libc.
 */
#include <stdint.h>
#include "exceptions.h"
#include "musb.h"
#include "emac.h"
#include "dbgmon.h"
#include "usbacm.h"

/* --- el2_exc.c's shared guest-frame snapshot + debug-core flag -------------
 * Real definitions live in el2_exc.c, which this QEMU target does not link
 * (see el2_exc_dual2_qemu.c's own header for why: a new, minimal el2_trap()
 * is used instead). smp.c takes `&g_last_guest_frame` and calls
 * el2_snapshot_guest_frame() inside its dead CPU1-debug-core branch, so both
 * need a real definition somewhere -- same as smp_qemu_stub.c. */
struct el2_frame g_last_guest_frame;
volatile uint32_t dbg_core_active;

/* --- GDB-stub cross-core stop-state globals --------------------------------
 * Same rationale as smp_qemu_stub.c: gdb_channel/gdbstub_poll/
 * gdbstub_on_debug_event are weak in smp.c (unresolved here -> dead
 * branch), but gdb_stop_pending/gdb_stop_signal/gdb_resume_act are NOT weak,
 * so the linker still needs them defined even though nothing on this target
 * ever sets gdb_channel != 0 to make that branch live. */
volatile uint32_t gdb_stop_pending;
volatile uint32_t gdb_stop_signal;
volatile uint32_t gdb_resume_act;

/* Plain word-copy, matching smp_qemu_stub.c's own reasoning: never actually
 * called on this target (dbg_core_enable=0), correctness here is not
 * safety-critical, just cheap to get right anyway. */
void el2_snapshot_guest_frame(struct el2_frame *out)
{
	const uint64_t *src = (const uint64_t *)&g_last_guest_frame;
	uint64_t *dst = (uint64_t *)out;
	unsigned i;

	for (i = 0; i < sizeof(*out) / sizeof(uint64_t); i++)
		dst[i] = src[i];
}

/* --- dbgmon.c's board debugger tick-path service ---------------------------
 * Real dbgmon_service() answers the EMAC-attached dbgmon.py/hvdbg.py
 * protocol against board MMIO. Never called here. */
void dbgmon_service(struct el2_frame *guest_frame)
{
	(void)guest_frame;
}

/* --- musb.c's USB-OTG gadget console ---------------------------------------
 * Real musb_putc()/musb_puts()/musb_flush() drive the A64's MUSB controller
 * registers. Never called here (the same dead branch that would call
 * usbacm_poll(), which is what actually calls these). */
void musb_putc(int c)
{
	(void)c;
}

void musb_puts(const char *s)
{
	(void)s;
}

void musb_flush(void)
{
}

/* --- emac.c's link self-heal watchdog ---------------------------------------
 * Real emac_link_watchdog() pokes the A64's EMAC MMIO and returns whether it
 * gave up on ever seeing an RX frame. Never called here; 0 ("didn't give
 * up") is the inert answer. */
int emac_link_watchdog(void)
{
	return 0;
}

/* --- usbacm.c's CDC-ACM gadget poll ----------------------------------------
 * Real usbacm_poll() drives the MUSB gadget + vconsole bridge rings. Never
 * called here. */
void usbacm_poll(void)
{
}
