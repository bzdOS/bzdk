/* SPDX-License-Identifier: BSD-2-Clause */

/* fbdump.h — stream a raw memory region (in practice: a framebuffer) out over
 * raw Ethernet, fire-and-forget.
 *
 * WHY A SEPARATE PATH AND NOT THE EXISTING ONES
 *
 * There were three candidates and each is wrong for this:
 *
 *   - dbgmon's `d`/read-words command hex-encodes 512 words per round trip.
 *     At this channel's ~60 ms RTT that is ~33 KB/s: a 1920x1080x4 frame takes
 *     minutes, and the 1120x276 guest window still takes ~40 s. Fine for
 *     reading a breadcrumb, useless for pixels.
 *   - netcon_send() waits for an ACK per chunk, so it is also one round trip
 *     per chunk -- reliable, and no faster.
 *   - coredump.c already streams regions, but wraps them in ET_CORE and caps
 *     the total at 384 KiB (COREDUMP_MAX_TOTAL). A frame is 20x that.
 *
 * So: fire every frame without waiting, and let the HOST notice gaps and
 * re-request just those byte ranges by issuing the command again with a
 * narrower (addr, len). That keeps all the retry logic on the host, where
 * complexity is cheap, and keeps this file a loop.
 *
 * ON WIRE
 *
 *   ethertype 0x88B9 (0x88B5 console, 0x88B6 netcon, 0x88B7 coredump,
 *                     0x88B8 snapshot_net are taken)
 *   payload:  u32 magic 'FBDP' | u32 offset | u16 len | u16 flags | data[len]
 *
 * `offset` is the byte offset from the START OF THE REQUESTED REGION, not an
 * absolute address: the receiver reassembles into a buffer it sized itself and
 * never has to know where the region lives. FBDUMP_FLAG_LAST marks the final
 * frame of a pass so the receiver can stop waiting instead of idling out.
 *
 * WATCHDOG
 *
 * This runs on CPU1, inside dbgmon's command handler, which is the same core
 * that pets the hardware watchdog. A multi-second dump that never returns to
 * the poll loop would let the WDOG fire mid-transfer, so the loop kicks it
 * itself every FBDUMP_KICK_FRAMES frames. That is not defensive
 * over-engineering: the window is 16 s and a full 1080p frame is ~6000 frames.
 */
#ifndef BZDOS_FBDUMP_H
#define BZDOS_FBDUMP_H

#include <stdint.h>

#define FBDUMP_ETHERTYPE   0x88B9u
#define FBDUMP_MAGIC       0x50424446u   /* 'FBDP' little-endian */
#define FBDUMP_DATA_MAX    1024u         /* data bytes per frame */
#define FBDUMP_FLAG_LAST   0x0001u
#define FBDUMP_KICK_FRAMES 64u           /* pet the watchdog this often */

/* Hard cap on one request, so a typo cannot ask for the whole address space:
 * 1920*1080*4 = 8100 KiB, so 16 MiB covers any frame this display can produce
 * with room to spare. */
#define FBDUMP_MAX_LEN     (16u * 1024u * 1024u)

/* Stream [pa, pa+len) as above. Refuses anything outside guest DRAM or larger
 * than FBDUMP_MAX_LEN. Returns the number of frames sent, or a negative value:
 * -1 out-of-range, -2 too large, -3 zero length. */
int fbdump_send(uint32_t pa, uint32_t len);

#endif /* BZDOS_FBDUMP_H */
