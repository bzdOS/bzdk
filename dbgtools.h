/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgtools.h — small, always-on host-observability primitives for the bzdOS
 * EL2 hypervisor. Added 2026-07-26 after a live RSP-debugging session hit
 * two concrete problems (see hv_addrmap.h's HVMAP_DBGTOOLS_* comment for the
 * blow-by-blow):
 *
 *   1. gdbstub's RSP channel went completely unresponsive for extended
 *      periods with no way to tell "CPU1 spinning but not answering THIS
 *      channel" from "CPU1 actually stopped". Fix: a heartbeat word bumped
 *      unconditionally every smp_secondary_main() debug-core loop iteration
 *      (smp.c), readable via a RAW EMAC request/reply (ETHERTYPE_DBGRAW,
 *      0x88B7) serviced directly inside emac_poll()'s RX demux — the exact
 *      same "call a module's _rx_frame() hook straight from emac_poll(),
 *      bypassing dbgmon_service()/gdbstub_poll() entirely" pattern netcon.c
 *      and snapshot_net.c already use for their own channels (see emac.c).
 *
 *      IMPORTANT LIMITATION: this only works because gdb_getc() (gdbstub.c)
 *      pumps emac_poll() itself on every call, including from deep inside
 *      gdbstub's own command_loop() while a session is stopped -- so the RAW
 *      peek keeps answering even while CPU1 is "stuck" servicing an active
 *      RSP conversation. If CPU1 stops calling emac_poll() at all (a genuine
 *      crash/hard-hang, not just "busy in the wrong branch"), the raw peek
 *      gets NO reply either, same as everything else -- there is no path
 *      lower than "the EMAC RX descriptor ring itself", and this codebase
 *      has no interrupt-driven RX (emac_poll() must be called by software).
 *      Three distinguishable states result (see hvdbg.py's heartbeat()):
 *        - heartbeat climbing + raw peek answers  -> CPU1 fully healthy.
 *        - heartbeat frozen + raw peek STILL answers (with the frozen value)
 *          -> CPU1 stuck deep in one iteration (e.g. gdbstub command_loop())
 *          but still pumping emac_poll() -- "busy, not dead".
 *        - raw peek gets no reply at all -> CPU1 genuinely wedged/crashed.
 *
 *   2. hvdbg.py's wdt_reset() resolved reboot_clean()'s address via `nm`
 *      against the microkernel-dbg.elf FILE ON DISK while the board was
 *      still running an OLDER build (rebuilt-since-last-cycle) -- the RPC
 *      silently jumped to garbage. Fix: a build-id string (-DBZDOS_BUILD_ID,
 *      Makefile) written once at HV startup to a fixed address; host tooling
 *      compares it (via the SAME `git rev-parse` invocation the Makefile
 *      used) before trusting any nm-resolved symbol address.
 *
 *   3. (main_dbg.c/main_gdb.c, not this file) an opt-in pause-before-entry
 *      gate so host tooling can plant breakpoints before the guest's first
 *      instruction, using the two fixed words below (HOLD/RELEASE) instead
 *      of a linked symbol a host would otherwise have to nm-resolve.
 *
 * All three pieces share ONE fixed DRAM lane (HVMAP_DBGTOOLS_* in
 * hv_addrmap.h) instead of a linked symbol, specifically so a host tool
 * never needs to trust `nm` against a possibly-stale on-disk ELF to reach
 * them -- the exact class of bug that motivated piece 2.
 *
 * Freestanding: <stdint.h> + hv_addrmap.h only, no libc.
 */
#ifndef BZDOS_DBGTOOLS_H
#define BZDOS_DBGTOOLS_H

#include <stdint.h>
#include "hv_addrmap.h"

/* Called once from main_dbg.c / main_gdb.c early in main() (after
 * emac_init(), so the raw peek below has something to answer immediately;
 * before the HOLD check, so a warm-reset-armed hold is honored). Idempotent
 * -- safe to call more than once, though nothing does.
 *
 * Cold-vs-warm-reset behavior (the one correctness property that matters
 * most here -- see the "Hard constraints" note in the task that added this):
 *   - Magic word ALREADY present (0x50021000 == HVMAP_DBGTOOLS_MAGIC): a
 *     WARM reset (hv.wdt_reset()-style) -- DRAM retained our marker, so
 *     HOLD/RELEASE are left exactly as a host tool may have armed them
 *     before triggering the reset.
 *   - Magic ABSENT: a true cold boot (fresh TFTP load, or the very first
 *     boot after a full power cycle) -- DRAM at this address is NOT
 *     guaranteed zero (nothing clears the hv-scratch window except the
 *     kernel/DTB TFTP load targets, which live elsewhere), so HOLD/RELEASE
 *     are forced to 0 here. This is what keeps the default (nobody armed
 *     anything) IDENTICAL to today's boot-straight-through behavior on every
 *     ordinary chimpd cold-load cycle.
 * The magic write itself always happens (stamps/re-stamps "DBGT" either
 * way) so the NEXT reset -- warm or cold -- can tell which kind it was.
 */
void dbgtools_init(void);

/* Durable (dc civac + dsb, reaches physical DRAM before returning) setters
 * for the entry-hold gate -- deliberately NOT exposed only via dbgmon's
 * generic `w <addr> <val>` command (cmd_write_word() in dbgmon.c is a plain
 * store, no cache maintenance): the whole point of this gate is to survive
 * a subsequent WARM RESET, so the write reaching physical DRAM before that
 * reset fires is exactly the property that must not be left to chance.
 * Wired to dbgmon.c's new `hold` / `release` verbs. */
void dbgtools_hold_set(void);      /* HOLD=1, RELEASE=0 */
void dbgtools_release_set(void);   /* RELEASE=1 (leaves HOLD as-is) */

static inline uint32_t dbgtools_hold_get(void)
{
	return *(volatile uint32_t *)HVMAP_DBGTOOLS_HOLD;
}

static inline uint32_t dbgtools_release_get(void)
{
	return *(volatile uint32_t *)HVMAP_DBGTOOLS_RELEASE;
}

/* ETHERTYPE_DBGRAW (0x88B7) RX hook -- called directly from emac.c's
 * emac_poll() RX demux for every frame on this ethertype, exactly like
 * netcon_rx_frame()/snapshot_net_rx_frame()/vnet_emac_rx_frame() (see
 * emac.c). `payload`/`len` are ignored (the request carries no fields --
 * any frame on this ethertype means "send the current snapshot"); the reply
 * is sent immediately, synchronously, from inside this call. Weak default
 * (no-op) lives in emac.c so builds that don't link dbgtools.o
 * (repl/fbsd/zephyr/hdmi/net/stage0) are completely unaffected. */
void dbgtools_rx_frame(const uint8_t *payload, uint16_t len);

#endif /* BZDOS_DBGTOOLS_H */
