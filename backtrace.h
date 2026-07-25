/* SPDX-License-Identifier: BSD-2-Clause */

/* backtrace.h — AArch64 frame-pointer chain walk for the bzdOS hypervisor.
 *
 * Given a starting (pc, fp=x29, lr=x30, sp) this walks the AAPCS64 frame
 * record chain: at each frame the pair {caller_fp, caller_lr} lives at
 * [fp+0] and [fp+8]. We follow it upward, collecting return addresses, until
 * fp becomes 0 / unaligned / non-monotonic / unreadable, or we hit the frame
 * cap. Symbolization is HOST-SIDE: emit the raw address list and resolve with
 * `addr2line`/`nm` against /opt/bzdos/tftpboot/kernel.
 *
 * Defensiveness is the whole point — the walker must NEVER itself fault:
 *   - every fp is alignment/range/monotonic checked before use;
 *   - every dereference goes through bt_readable(), which validates the VA via
 *     the SAME AT S1E1R guest-stage-1 translation dbgmon's `gva` uses (with a
 *     direct-PA fallback for MMU-off / EL2 addresses) before touching it.
 *
 * Works for both the guest (high KVA once FreeBSD's MMU is up, or flat PA
 * early) and our own EL2 kernel (linked at 0x42000000, stack in DRAM). */
#ifndef BZDOS_BACKTRACE_H
#define BZDOS_BACKTRACE_H

#include <stdint.h>

/* Walk the frame chain starting from (pc, fp, lr). Writes up to `max` return
 * addresses into out[]: out[0]=pc, out[1]=lr (if nonzero), then each caller's
 * saved lr up the chain. Returns the number of addresses written. Also mirrors
 * the result into the "BTR1" breadcrumb @ 0x50000700 for post-mortem md.l. */
int backtrace_walk(uint64_t pc, uint64_t fp, uint64_t lr, uint64_t *out, int max);

#endif /* BZDOS_BACKTRACE_H */
