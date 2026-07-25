/* SPDX-License-Identifier: BSD-2-Clause */

/* coredump.h — bounded, non-blocking ELF (ET_CORE) coredump streamed over raw
 * Ethernet for the bzdOS EL2 microkernel.
 *
 * On a fatal fault (or on demand from dbgmon) coredump_send() emits a valid
 * AArch64 ELF core image — ELF header + PT_NOTE(NT_PRSTATUS with the GPRs, pc,
 * sp, pstate) + PT_LOAD segments for a bounded set of memory regions — chopped
 * into small framed chunks and sent as raw Ethernet frames. A ~30-line Python
 * receiver reassembles the frames into core.elf, loadable with:
 *
 *     aarch64-linux-gnu-gdb kernel core.elf
 *
 * EVERYTHING here is bounded and non-blocking: total payload is capped
 * (CORE_MAX_TOTAL), every per-region length is capped, and the TX path only
 * ever spins a bounded number of poll iterations before dropping a frame. It
 * can never hang the fault path.
 *
 * -------------------------------------------------------------------------
 * ON-WIRE FRAMING
 * -------------------------------------------------------------------------
 * EtherType 0x88B7 (0x88B5 = console text, 0x88B6 = netcon; 0x88B7 is ours,
 * so coredump traffic never collides with the console). dst = broadcast,
 * src = our MAC (02:bd:05:00:00:01), via emac_send_frame().
 *
 * Each frame's Ethernet PAYLOAD is a 12-byte little-endian header + data:
 *
 *     off  size  field
 *     0    4     magic  = 0x45524F43  ("CORE", LE bytes C,O,R,E)
 *     4    2     seq    = 0-based frame index
 *     6    2     total  = total number of frames in this dump
 *     8    2     flags  = bit0 (0x1) set on the LAST frame
 *     10   2     len    = number of DATA bytes that follow the header
 *     12   len   data   = raw bytes of the core.elf byte stream
 *
 * The concatenation of every frame's `data`, in `seq` order, IS the complete
 * core.elf file. The receiver: filter EtherType 0x88B7, check magic, index by
 * seq, append data, stop when the bit0 flag is seen and all seq in [0,total)
 * are present, write core.elf. (Frames may be dropped under load — the
 * receiver detects a gap via seq/total and can report an incomplete core.)
 */
#ifndef BZDOS_COREDUMP_H
#define BZDOS_COREDUMP_H

#include <stdint.h>

struct el2_frame;   /* exceptions.h */

#define COREDUMP_ETHERTYPE  0x88B7u
#define COREDUMP_MAGIC      0x45524F43u   /* "CORE" */
#define COREDUMP_DATA_MAX   1024u         /* data bytes per frame            */
#define COREDUMP_REGION_MAX 8u            /* max PT_LOAD segments            */
#define COREDUMP_MAX_TOTAL  (384u * 1024u)/* hard cap on streamed core bytes */

/* Stream an ELF core to the host. `frame` supplies the register state (GPRs,
 * pc, pstate). The current GUEST stack (a bounded window around the guest's
 * own SP_EL1, read directly — NOT frame->sp_at_entry, which is EL2's own
 * exception stack pointer and was a bug fixed 2026-07-25: see coredump.c's
 * gva_to_pa()) is captured automatically as one PT_LOAD. `regions` is an
 * optional caller-supplied array of (addr,len) pairs (2*nregions uint64_t
 * words) added as additional PT_LOAD segments; pass NULL / 0 for none.
 * Out-of-DRAM or oversized regions are clamped/skipped. Bounded and
 * non-blocking. */
void coredump_send(struct el2_frame *frame, uint64_t *regions, int nregions);

#endif /* BZDOS_COREDUMP_H */
