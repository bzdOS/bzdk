/* SPDX-License-Identifier: BSD-2-Clause */

/* zstage.h — the bulk loader for a second guest's raw ELF image.
 *
 * THE PROBLEM THIS SOLVES
 * -----------------------
 * The dual-guest milestone (docs/dual-guest.md) proved CPU3 can run a second
 * guest concurrently with FreeBSD on real hardware, but only with a stand-in
 * payload: zguest_cpu3.h's own header comment flagged from the start that
 * *getting a real image into DRAM* was out of scope, and the hardware test
 * confirmed why. Staging it the only way then available — dbgmon's single-word
 * `w` command over the EMAC debug channel — works for the 144 genuinely
 * meaningful bytes of a 4-instruction test payload and does not scale by two
 * orders of magnitude to a real Zephyr image.
 *
 * THE FIX, AND WHY IT IS THIS ONE
 * -------------------------------
 * chimpd.py already TFTPs two files from U-Boot before jumping into the
 * hypervisor (the FreeBSD kernel -> 0x44000000 and the DTB -> 0x4a000000) and
 * `loady`s a third (the HV ELF -> 0x48000000). A guest image is just a fourth
 * file through machinery that already exists and is already reliable, instead
 * of a new bulk-write extension to the debug protocol.
 *
 * The obvious version of that — TFTP straight into ZG3_ELF_STAGE_PA
 * (0xBF000000, inside Zephyr's own high-GiB slice) — was considered and
 * rejected: U-Boot relocates its own code, stack and heap to the top of usable
 * DRAM, which on this 2 GiB board is exactly that neighbourhood. Rather than
 * gamble on U-Boot's ram_top, the image lands in a low-DRAM window and the
 * hypervisor moves it.
 *
 * WHY THE COPY IS SAFE (this is the whole argument, read it before moving the
 * call site): ZSTAGE_LOW_PA is inside the FreeBSD guest's own gigabyte
 * (STAGE2_DRAM_BASE..+STAGE2_DRAM_SIZE), memory FreeBSD is free to allocate
 * over the moment it runs. The copy is therefore performed by CPU0, from
 * main_dbg.c, BEFORE smp_init() and BEFORE kload_enter() — i.e. before any
 * guest on any core has executed a single instruction. Doing it later, or from
 * CPU3 after smp_init(), would be a real race: CPU0 could enter FreeBSD while
 * CPU3 is still copying. main_zephyr.c relies on the same "low DRAM is free
 * while FreeBSD is not running" property for its own 0x44000000 staging, but
 * only because in that build FreeBSD never runs at all; here it does, so the
 * ordering is load-bearing rather than incidental.
 *
 * THE LANDING WINDOW
 * ------------------
 * [0x4E000000, 0x50000000) — 32 MiB, verified unclaimed by a full-tree grep.
 * It is the first round boundary above the HDMI framebuffer, which hdmi.h:60
 * already names as such ("the next round 0x01000000 boundary (0x4e000000)"),
 * and it sits below the hv-scratch reserve at 0x50000000. Everything else in
 * the low-DRAM map is well clear of it: HV image 0x42000000, raw kernel ELF
 * 0x44000000 (+16 MiB), placed kernel 0x46000000, U-Boot loady staging
 * 0x48000000, DTB 0x4a000000, modinfo 0x4a100000, guest SP_EL1 0x4c000000,
 * framebuffer 0x4D000000 (+8 MiB).
 *
 * Freestanding: <stdint.h> only, no libc.
 */
#ifndef BZDOS_ZSTAGE_H
#define BZDOS_ZSTAGE_H

#include <stdint.h>

/* Where U-Boot TFTPs the raw guest ELF (chimpd.py's ZADDR). */
#define ZSTAGE_LOW_PA     0x4E000000UL
#define ZSTAGE_LOW_SIZE   0x02000000UL   /* 32 MiB — the whole free window */

/* Compute how many bytes of an ELF64/AArch64 image actually need to be moved:
 * the header, the program-header table, and every PT_LOAD segment's file
 * content — nothing past the last of those. Section headers and the symbol
 * table are deliberately NOT included: zload2_parse_and_place() never reads
 * them (see zload2.c), and a Zephyr ELF's debug sections can easily dwarf its
 * loadable content, so copying to e_shoff would move megabytes of bytes
 * nobody will ever look at.
 *
 * This is a pure function of the bytes at `elf` — no MMIO, no breadcrumbs, no
 * assumptions about where it is called from — specifically so it can be
 * exercised by a hosted unit test (test_zstage.c) rather than only on the
 * board. `avail` is the size of the readable window at `elf`; every field is
 * bounds-checked against it before use, so a garbage/absent image is rejected
 * rather than walked off the end of.
 *
 * Returns 1 and writes *span_out on success; returns 0 and leaves *span_out
 * untouched if the header is not a valid ELF64/little-endian/AArch64
 * EXEC-or-DYN image, if it has no PT_LOAD segments, or if any offset it
 * describes falls outside `avail`. */
int zstage_span(const void *elf, uint64_t avail, uint64_t *span_out);

/* Move the image from ZSTAGE_LOW_PA to `dest_pa`, copying exactly
 * zstage_span() bytes, cleaning it out to the point of coherency so the core
 * that will parse it sees the real bytes and not a stale line.
 *
 * `dest_limit` is the size of the destination window; the copy is refused
 * outright if the image would not fit. Returns the number of bytes copied, or
 * 0 if nothing was copied — which is the ordinary, expected result when no
 * image was staged at all, and is deliberately NOT treated as an error here: a
 * `dual` build with no guest image is exactly the `dual` build that has been
 * running on the board all along. The failure surfaces later, loudly, when
 * `zboot` makes zload2_parse_and_place() reject the empty buffer (breadcrumb
 * 0xBAD1), which is the existing, already hardware-exercised path.
 *
 * CRITICALLY, when nothing is copied the destination's header region is ZEROED
 * rather than left alone. A warm reset preserves DRAM, so the previous boot's
 * image would otherwise still be sitting there and `zboot` would silently boot
 * a ghost — this was observed live on 2026-08-10 and is written up in full at
 * zstage_invalidate_dest() in zstage.c. Read that before "optimising" the
 * memset away.
 *
 * Breadcrumbs land in HVMAP_ZSTAGE_BC (see hv_addrmap.h). */
uint64_t zstage_copy_to(uint64_t dest_pa, uint64_t dest_limit);

/* dbgmon.c's `zstage` command: re-run the copy-in on demand, at any time, from
 * whichever core is servicing the debug channel.
 *
 * WHY THIS EXISTS. `zunhalt` returns a failed CPU3 to its parked state, but on
 * its own that is not enough to be useful: the operator also needs a way to get
 * a corrected image to where zload2 will read it, and the boot-time copy-in has
 * long since run. Without this command the only route left is writing straight
 * to ZG3_ELF_STAGE_PA over the debug channel -- which is precisely the
 * destination-only route that zstage_invalidate_dest() made unsupported. So the
 * supported retry workflow is: put the image in the LANDING window (debug-channel
 * writes to ZSTAGE_LOW_PA, or it may still be there from this boot's TFTP),
 * `zstage`, `zunhalt`, `zboot`.
 *
 * Returns the number of bytes copied, 0 if nothing usable was staged (in which
 * case the destination is wiped, exactly as at boot -- so a `zstage` that finds
 * garbage leaves nothing bootable behind rather than half-updating).
 *
 * SAFE BUT NOT ALWAYS USEFUL LATE IN A BOOT: the landing window lives inside the
 * FreeBSD guest's own gigabyte, so once FreeBSD is running it may have allocated
 * over it. Reading it can therefore return garbage -- which is refused and
 * reported (breadcrumb state 2), not acted on. The destination it writes is
 * inside the second guest's private slice, so the write itself can never disturb
 * FreeBSD. */
uint64_t zstage_restage(void);

/* main_dbg.c's call site. Weakly defined there as a no-op, strongly overridden
 * by zstage.c, which is linked only into the `dual` target — the same
 * weak/strong linkage pattern smp.c already uses for zephyr_cpu3_run() and
 * vblk_async_cpu2_run(), so `dbg`/`gdb`/`fbsd`/`zephyr` are provably
 * unchanged. Resolves the destination from zguest_cpu3.h's staging address so
 * the two files cannot drift apart. */
void zguest_stage_copyin(void);

#endif /* BZDOS_ZSTAGE_H */
