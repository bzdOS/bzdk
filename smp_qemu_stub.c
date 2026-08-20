/* SPDX-License-Identifier: BSD-2-Clause */

/* smp_qemu_stub.c — inert stand-ins for the handful of real-hardware symbols
 * smp.c references UNCONDITIONALLY (i.e. the linker needs them resolved no
 * matter what any runtime flag is set to), so the REAL, UNMODIFIED smp.c can
 * be linked into a QEMU `virt` CI target without also dragging in musb.o/
 * emac.o/dbgmon.o/usbacm.o/el2_exc.o and their own transitive dependency
 * chains (board-only MUSB/EMAC/eMMC drivers, the full board debugger).
 *
 * This mirrors wdt_qemu_stub.c's existing precedent EXACTLY (see that file's
 * header): "the real X.o pokes board-only MMIO that doesn't exist under
 * QEMU; link a real, but inert, implementation instead". wdt_qemu_stub.c
 * does this for one symbol (wdt_pet(), needed by snapshot.c). This file does
 * the same thing for the wider set smp.c needs, needed by main_dual_qemu.c's
 * proof that smp_init()/smp_secondary_main() (the real PSCI CPU_ON bring-up)
 * work under QEMU.
 *
 * WHY THESE SYMBOLS ARE NEEDED AT ALL, GIVEN THEY'RE NEVER CALLED AT RUNTIME:
 * main_dual_qemu.c sets smp.c's own existing diagnostic flag
 * `dbg_core_enable = 0` before calling smp_init() (see that flag's comment in
 * smp.c: "ISOLATION TEST flag... skip EMAC/dbgmon on CPU1"), which is a
 * RUNTIME condition — smp_secondary_main()'s `if (cpu == SMP_DEBUG_CPU &&
 * dbg_core_enable)` block (wdt_debug_kick(), dbgmon_service(), musb_*(),
 * emac_link_watchdog(), usbacm_poll(), the &g_last_guest_frame reference)
 * never EXECUTES on this target. But the compiler still emits relocations
 * for every symbol that block references regardless of the runtime flag
 * value, so the LINKER still needs a definition for each one, or the link
 * fails with "undefined reference" before main_dual_qemu.c ever runs a
 * single instruction. Hence: real symbols, inert bodies, never invoked here.
 *
 * NOT a modification of smp.c (untouched) and NOT a reimplementation of any
 * board driver's real behavior — just enough of a definition to satisfy the
 * static linker. Freestanding: <stdint.h> + the real project headers whose
 * prototypes these bodies must match exactly (musb.h, emac.h, dbgmon.h,
 * usbacm.h, wdt.h, exceptions.h), no libc.
 */
#include <stdint.h>
#include "exceptions.h"
#include "musb.h"
#include "emac.h"
#include "dbgmon.h"
#include "usbacm.h"
#include "wdt.h"

/* --- el2_exc.c's shared guest-frame snapshot + debug-core flag -------------
 * Real definitions live in el2_exc.c (see its header: "The shared guest-frame
 * snapshot + debug-core flag live in el2_exc.c"), which this QEMU target does
 * not link (no guest is ever entered here — see main_dual_qemu.c). smp.c
 * takes `&g_last_guest_frame` (inside the dead dbg_core_enable branch, see
 * above) and calls el2_snapshot_guest_frame() the same way, so both need a
 * real definition somewhere. dbg_core_active is only ever SET by smp.c's own
 * dead branch and never read by anything linked into this target; kept at 0.
 */
struct el2_frame g_last_guest_frame;
volatile uint32_t dbg_core_active;

/* --- GDB-stub cross-core stop-state globals --------------------------------
 * smp.c's header: "The cross-core stop-state globals live in el2_exc.o
 * (always linked)." That statement is true of every REAL board target (they
 * all link el2_exc.o); this one doesn't. gdb_channel/gdbstub_poll/
 * gdbstub_on_debug_event are declared __attribute__((weak)) in smp.c
 * specifically so a build without gdbstub.o (like this one) can leave them
 * unresolved -> address 0 -> `if (&gdb_channel && gdb_channel)` false ->
 * dead branch. But gdb_stop_pending/gdb_stop_signal/gdb_resume_act are NOT
 * weak in smp.c (they're read/written unconditionally inside that same dead
 * branch), so the linker still needs them defined even though nothing here
 * ever sets gdb_channel != 0 to make the branch live. */
volatile uint32_t gdb_stop_pending;
volatile uint32_t gdb_stop_signal;
volatile uint32_t gdb_resume_act;

/* Plain word-copy, not `*out = g_last_guest_frame;` — deliberately avoids
 * relying on the compiler lowering a ~0x140-byte struct assignment to a
 * memcpy() call (this build links libmin.o for exactly that reason already,
 * but this file has no other reason to depend on it). Never actually called
 * on this target (see file banner), so correctness here is not
 * safety-critical, just cheap to get right anyway. */
void el2_snapshot_guest_frame(struct el2_frame *out)
{
	const uint64_t *src = (const uint64_t *)&g_last_guest_frame;
	uint64_t *dst = (uint64_t *)out;
	unsigned i;

	for (i = 0; i < sizeof(*out) / sizeof(uint64_t); i++)
		dst[i] = src[i];
}

/* --- wdt.c's debug-core watchdog owner -------------------------------------
 * Real wdt_debug_kick() pokes the Allwinner A64 WDOG MMIO (0x01c20cb0),
 * which does not exist under QEMU virt (same fact wdt_qemu_stub.c's header
 * documents for wdt_pet()). Never called here (dbg_core_enable=0), so a
 * plain no-op is correct, not just convenient. */
void wdt_debug_kick(void)
{
}

/* wdt_debug_hold used to need a stand-in definition here too (this file
 * links instead of the real wdt.o, which owned the only linked instance of
 * the symbol). It no longer does: wdt.h now #defines wdt_debug_hold as a
 * macro over a fixed hv-scratch address (HVMAP_WDT_DEBUG_HOLD, hv_addrmap.h)
 * rather than a plain global, so there is no linked symbol left to stand in
 * for -- every reference (including smp.c's, which is why this file exists)
 * resolves at compile time to a direct MMIO dereference, dead code on this
 * target exactly like wdt_debug_kick() above (dbg_core_enable=0 means
 * smp.c's debug-core branch that touches it never runs), and declaring
 * storage for it here now would be a compile error: the name is a macro
 * expanding to a pointer dereference, not an identifier a declaration can
 * bind to. */

/* --- dbgmon.c's board debugger tick-path service ---------------------------
 * Real dbgmon_service() answers the EMAC-attached `dbgmon.py`/hvdbg.py
 * protocol (gr/sr/w/wb/bp/...) against board MMIO. Never called here. */
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
