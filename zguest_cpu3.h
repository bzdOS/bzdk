/* SPDX-License-Identifier: BSD-2-Clause */

/* zguest_cpu3.h — the strong CPU3 hook that turns Zephyr into a genuinely
 * concurrent second guest, started/stopped independently over EMAC with no
 * reboot needed (dual-guest milestone). Linked ONLY into the `dual`
 * Makefile target; every other target links smp.c's weak
 * zephyr_cpu3_run() default (a plain WFI park) instead.
 *
 * LIFECYCLE
 * ---------
 *   1. `make dual` boots exactly like `make dbg` on CPU0/CPU1/CPU2 (FreeBSD,
 *      the debug core, async eMMC — all byte-for-byte unchanged). CPU3 comes
 *      up (smp_init(), unchanged) and immediately parks here in a wfe-poll
 *      loop, touching nothing Zephyr-related yet.
 *   2. An operator stages a raw Zephyr ELF image into physical DRAM at
 *      ZG3_ELF_STAGE_PA (see below) using EXISTING tooling — dbgmon's `w`/
 *      `wb` commands, or a bulk loader akin to loady_over_acm.py — over
 *      EMAC, with the hypervisor already up and FreeBSD already running.
 *      No such bulk-loader script is added by this pass; wiring one up is
 *      host-side tooling, out of scope here (board-free firmware work only
 *      — see the task that added this file).
 *   3. The operator sends dbgmon.c's new `zboot` command. This calls
 *      zguest_cpu3_start_set() (below), which sets a flag and `sev`s.
 *   4. This core's wfe-poll loop wakes, sees the flag, and runs the ONE-WAY
 *      boot sequence: zload2_parse_and_place() -> stage2_zephyr_init() +
 *      stage2_zephyr_enable() -> stage2_zephyr_isolation_selfcheck() (MUST
 *      pass, or halt) -> vconsole_init_chan1() -> guest_config() ->
 *      kload_enter(). From here CPU3 is Zephyr, running concurrently with
 *      CPU0's FreeBSD, until the next full board reload.
 *
 * NO WATCHDOG INVOLVEMENT: this core touches neither wdt_pet() (CPU0/guest-
 * progress-gated) nor wdt_debug_kick() (CPU1-only, unconditional) at any
 * point, matching the plan's design: CPU1 alone owns the hardware watchdog,
 * and a WFI/WFE-parked CPU3 doing nothing watchdog-related — before AND
 * after `zboot` — is correct, not an oversight.
 *
 * STAGING ADDRESSES: both live inside Zephyr's OWN 32 MiB private slice
 * (stage2_zephyr.h's ZSTAGE2_DRAM_BASE/ZSTAGE2_DRAM_SIZE), split into two
 * disjoint halves so the raw-ELF staging buffer and the relocated/placed
 * image never overlap during the copy:
 *   ZG3_PA_BASE       = ZSTAGE2_DRAM_BASE            (lower 16 MiB half —
 *                       zload2_parse_and_place()'s placement destination)
 *   ZG3_ELF_STAGE_PA  = ZSTAGE2_DRAM_BASE + 16 MiB    (upper 16 MiB half —
 *                       where the raw ELF bytes are staged before `zboot`)
 * Deliberately NOT anywhere in FreeBSD's low-DRAM gigabyte (unlike
 * main_zephyr.c's standalone Zephyr-only build, which stages at 0x44000000
 * — safe there ONLY because FreeBSD is never also running): reusing that
 * convention here, after FreeBSD has been running for a while and may have
 * allocated over it, would mean writing into memory the OTHER guest now
 * owns — exactly the cross-guest isolation violation this whole milestone
 * exists to prevent.
 *
 * Freestanding: <stdint.h> only, no libc.
 */
#ifndef BZDOS_ZGUEST_CPU3_H
#define BZDOS_ZGUEST_CPU3_H

#include <stdint.h>

/* The strong override of smp.c's weak zephyr_cpu3_run() (see smp.c). Called
 * from smp_secondary_main() when cpu==3, on CPU3 itself, and never returns
 * (parks in wfe forever after entering the guest, exactly like every other
 * per-core dispatch in smp_secondary_main()). */
void zephyr_cpu3_run(void);

/* Set by dbgmon.c's `zboot` command (any core; the flag itself is a plain
 * cross-core-visible word, SMPEN cache coherency, no dc civac needed since
 * it is live/never-persisted state, matching e.g. vconsole.c's RX-ring
 * head/tail convention) to wake CPU3's wfe-poll loop and start the one-way
 * Zephyr boot sequence described above. Idempotent: calling it again once
 * CPU3 has already left the poll loop has no effect (the flag is only ever
 * consulted there). */
void zguest_cpu3_start_set(void);

#endif /* BZDOS_ZGUEST_CPU3_H */
