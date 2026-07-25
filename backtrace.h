/* SPDX-License-Identifier: BSD-2-Clause */

/* backtrace.h — AArch64 frame-pointer chain walk for the bzdOS hypervisor.
 *
 * Given a starting (pc, fp=x29, lr=x30, sp) this walks the AAPCS64 frame
 * record chain: at each frame the pair {caller_fp, caller_lr} lives at
 * [fp+0] and [fp+8]. We follow it upward, collecting return addresses, until
 * fp becomes 0 / unaligned / non-monotonic / unreadable, or we hit the frame
 * cap. Full symbolization (arbitrary depth, every frame, DWARF-quality) is
 * still HOST-SIDE: `addr2line`/`nm`/kgdb against the real kernel.debug ELF
 * remains the authoritative tool — see coredump.c for the vmcore this HV
 * streams for exactly that. ROADMAP B3 ADDS a bounded, best-effort ON-BOARD
 * resolution for the FIRST few frames (backtrace_symbolize(), below) so an
 * operator watching a live incident over the network sees "symbol+0xoffset"
 * immediately, without first fetching the kernel.debug ELF and running
 * addr2line by hand — additive to the raw address list, never a replacement
 * for it (the raw BTR1 breadcrumb this file already writes is untouched).
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

/* ROADMAP B3: resolve the first few entries of a backtrace_walk() result to
 * "symbol+0xoffset" via ksym.c (which in turn consults the kernel ELF
 * kload.c already parsed) and mirror them into the "BTS1" breadcrumb @
 * 0x50000e00 — a NEW, separate window, deliberately NOT grown out of BTR1's
 * existing word layout (see backtrace.c's header comment for why: BTR1's
 * own base address already collides with an unrelated pre-existing window,
 * a landmine this change does not touch or make worse).
 *
 * Call with the SAME `frames`/n a backtrace_walk() call just produced (n is
 * its return value); frames[i] for i >= n is never read. Best-effort and
 * additive only — if no kernel has been kload_parse_elf()'d yet (e.g. this
 * fault happened before/without a FreeBSD guest ever loading, or the kernel
 * is stripped), every frame simply resolves to nothing and only the header
 * words of BTS1 are stamped; the raw BTR1 breadcrumb backtrace_walk() wrote
 * is completely unaffected either way. Never faults: ksym_resolve() only
 * ever reads DRAM ranges kload.c already validated as the resident kernel
 * ELF image. */
void backtrace_symbolize(const uint64_t *frames, int n);

#endif /* BZDOS_BACKTRACE_H */
