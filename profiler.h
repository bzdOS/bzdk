/* SPDX-License-Identifier: BSD-2-Clause */

/* profiler.h — non-halting sampling profiler for the bzdOS EL2 hypervisor.
 *
 * On every timer tick the tick path samples the interrupted PC (guest ELR, or
 * our own EL2 PC if the tick was taken from EL2) and folds it into a compact
 * PC histogram in a fixed cache-coherent DRAM window, AND emits a
 * TRACE_PROFILE event into the trace ring (trace.h). A host tool aggregates the
 * histogram to symbols with addr2line against the guest kernel
 * (/opt/bzdos/tftpboot/kernel) or our own ELFs. The sampler is wait-free and
 * bounded — it adds negligible latency to the tick handler.
 *
 * Histogram window (fixed): base 0x50006800, magic "PROF" (0x50524F46).
 *   NOTE: the trace ring at 0x50004000 with 512 slots spans
 *   0x50004000..0x50006040, so the historically-suggested 0x50004800 for PROF
 *   would sit INSIDE the ring. We place PROF at 0x50006800 (cache-line
 *   aligned, clear of the ring end). With PROF_BUCKETS=1024 the table spans
 *   0x50006800..0x50008820.
 *
 *   Header (8 words @ 0x50006800):
 *     [0] magic          0x50524F46 ("PROF")
 *     [1] nbuckets       PROF_BUCKETS (1024)
 *     [2] total_samples  every profiler_sample() call
 *     [3] guest_samples  samples with is_guest != 0
 *     [4] kernel_samples samples with is_guest == 0
 *     [5] dropped        samples that found no free bucket within the probe
 *                        bound (table saturated for that hash chain)
 *     [6] timer_freq_lo  timer_freq() low 32 bits
 *     [7] pc_mask_lo     low 32 bits of the bucketing mask (~0xF)
 *   Buckets start at 0x50006820. Bucket = 8 bytes/2 words:
 *     word0 = pc    (u32 low of the bucketed PC == sampled_pc & ~0xF; 0 = free)
 *     word1 = count (sample hits in this bucket)
 */
#ifndef BZDOS_PROFILER_H
#define BZDOS_PROFILER_H

#include <stdint.h>

/* Initialize (zero + stamp header) the PROF histogram. Call once, early. */
/* Byte offset of bucket 0 from PROF_BASE == the header size (8 words).
 * Exported because hud.c needs it: it used to hardcode 16 (a 4-word header),
 * so its PROFILE panel read words 6 and 7 of the HEADER as its first bucket
 * and displayed CNTFRQ (0x016E3600) as the hottest "PC" with a count of
 * 0xFFFFFFF0. The panel had never been live, so the mismatch had never been
 * visible. One definition now. */
#define PROF_HDR_BYTES  0x20u

void profiler_init(void);

/* Record one PC sample: fold into the histogram and emit a TRACE_PROFILE
 * event. is_guest != 0 => sampled the guest (EL1/EL0); 0 => our EL2 kernel.
 * Wait-free and bounded (fixed-length hash probe, atomic bucket claim/count).
 * Safe from tick/IRQ context. */
void profiler_sample(uint64_t pc, int is_guest);

#endif /* BZDOS_PROFILER_H */
