/* SPDX-License-Identifier: BSD-2-Clause */

/* mmio_absorb.h — absorb every CPU3/Zephyr stage-2 MMIO fault with a
 * generic, always-succeeds emulation (dual-guest milestone). Linked ONLY
 * into the `dual` Makefile target.
 *
 * WHY THIS EXISTS: per stage2_zephyr.h's design, Zephyr gets ZERO real MMIO
 * passthrough -- IPA 0x00000000-0x7FFFFFFF is entirely invalid in its
 * stage-2 table (see stage2_zephyr.c: no UART/GIC/etc carve-out the way
 * FreeBSD's vconsole.c/vblk_emmc.c/vnet_emac.c get). Zephyr's board (see
 * zephyr-guest/boards/bzdos/bpi_m64_hv/bpi_m64_hv.dts) still describes a
 * UART0 and a GIC, and its drivers will probe them at boot regardless.
 * el2_exc.c's CPU3 routing (see that file's header) tries vconsole.c's
 * channel-1 UART emulation FIRST (the one real, meaningfully-emulated
 * device Zephyr's console needs); everything else that isn't the UART
 * lands here and is unconditionally "succeeded" so a driver probe never
 * hangs the guest waiting for a real response that will never come.
 *
 * This is deliberately dumb: it does not model any device's actual
 * register semantics, it just makes every access complete instead of
 * faulting forever. A future pass that wants Zephyr to actually see a
 * (virtual) GIC or other device would add a real emulator ahead of this
 * one in el2_exc.c's dispatch, the same way vconsole.c's channel 1 is
 * already ahead of it.
 *
 * Freestanding: <stdint.h> + exceptions.h only, no libc.
 */
#ifndef BZDOS_MMIO_ABSORB_H
#define BZDOS_MMIO_ABSORB_H

#include <stdint.h>
#include "exceptions.h"

/* Called from el2_exc.c for a CPU3 stage-2 data-abort fault that
 * vconsole_handle_fault(frame, 1) did not claim. Decodes ESR_EL2.ISS
 * (ISV/SAS/SRT/WnR), same field layout vconsole.c's ISV=0 fallback path
 * uses:
 *   - read (WnR==0) with ISV==1: writes 0 into the destination GPR named
 *     by SRT (skipped if SRT==31, i.e. XZR -- there is no x31 to write).
 *   - write (WnR==1): no effect (the value is simply discarded -- there is
 *     no real register behind this address for it to land in).
 *   - ISV==0 (undecodable syndrome): no register write is attempted
 *     (we don't know which one, if any), but the fault is still absorbed.
 * In EVERY case, frame->elr is advanced by 4 (skip the faulting
 * instruction) and this returns 1 (handled). By design for this milestone,
 * this function NEVER returns 0 -- every CPU3 MMIO access reaching it is
 * absorbed. The return value is still a genuine int (not void) so a future
 * pass that wants to distinguish "handled" from "not mine" (e.g. once a
 * real per-device emulator is added ahead of this catch-all) can do so
 * without an API change.
 */
int mmio_absorb_fault(struct el2_frame *frame);

#endif /* BZDOS_MMIO_ABSORB_H */
