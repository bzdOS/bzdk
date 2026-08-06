/* SPDX-License-Identifier: BSD-2-Clause */

/* snapshot_net.h — stream a guest snapshot (see snapshot.h/snapshot.c) over
 * GbE to/from the host, instead of the DRAM-to-DRAM store snapshot.c already
 * implements. ROADMAP D1 v1 scope: freeze -> dump RAM over GbE -> restore.
 *
 * ============================================================================
 * TRANSPORT CHOICE — WHY A NEW BULK CHANNEL, NOT netcon's, PLUS netcon REUSED
 * FOR THE SMALL CONTROL MESSAGES
 * ============================================================================
 * Two existing raw-Ethernet precedents were evaluated (see the task's own
 * framing): netcon.c (0x88B6, ACKed stop-and-wait, NETCON_MAX_LEN=2 MiB) and
 * coredump.c (0x88B7, 12-byte header, fire-and-forget seq/total framing, no
 * ACK, capped at 384 KiB).
 *
 * NEITHER is a drop-in fit for a ~1 GiB store:
 *
 *   - netcon's NETCON_MAX_LEN=2 MiB is NOT a hard wire-format limit — total_len
 *     and off are already 32-bit fields, so the *framing* supports up to 4 GiB.
 *     Raising the constant would be a one-line change. The real blocker is
 *     netcon's *stop-and-wait* discipline: ONE chunk (<=512 B) in flight at a
 *     time, sender blocks for an ACK before sending the next. For a ~1 GiB
 *     image that is ~2.1M chunks; even an optimistic ~1 ms software-driven
 *     round trip (no NIC-level flow control, polled TX/RX rings) is ~35
 *     minutes of pure ACK-wait latency, two orders of magnitude past the
 *     ROADMAP's own ~20-30 s "2 GiB as-is" estimate. Raising NETCON_MAX_CHUNK
 *     to the ~1400 B MTU ceiling only gets this down to ~12 minutes worst
 *     case — still not close. Per-chunk ACKing is fundamentally the wrong
 *     shape for bulk transfer; extending netcon's *cap* doesn't fix that.
 *
 *   - coredump.c's fire-and-forget style (seq/total, no ACK) is the right
 *     shape for throughput, but its 16-bit seq/total (max 65536 frames) and
 *     384 KiB hard cap are sized for a small register+stack dump, not a GiB
 *     image, and it has NO retransmission story at all (a dropped frame is
 *     just a documented "incomplete core" — acceptable for a debug aid,
 *     not for a checkpoint you intend to restore from).
 *
 * CHOSEN DESIGN: a new bespoke bulk channel (this file), modeled on
 * coredump.c's fire-and-forget framing (own EtherType 0x88B8, no ACK per
 * frame — this is what actually gets line-rate-ish throughput), PLUS re-use
 * of netcon.c UNCHANGED for the two small, must-be-reliable control messages
 * that make the bulk channel's frame loss recoverable without stop-and-wait
 * overhead on the bulk data itself:
 *
 *   1. The SENDER of the bulk image builds a "manifest" bitmap while it scans
 *      (1 bit per chunk: was this chunk non-zero / actually transmitted?) and
 *      delivers that bitmap RELIABLY over netcon (~94 KiB for a full 1 GiB
 *      image at CHUNK=1400 B granularity — comfortably inside netcon's
 *      existing 2 MiB cap, so netcon.h/.c are NOT modified at all).
 *   2. The RECEIVER compares "manifest" (expected) against what it actually
 *      received (tracked in its own same-sized bitmap) and, if anything is
 *      missing, sends the list of missing chunk indices back over netcon
 *      (again comfortably inside the 2 MiB cap — capped per round at
 *      SNAPNET_MISSING_CAP entries besides). The original sender then
 *      re-transmits exactly those chunks (fire-and-forget again) and the
 *      round repeats, bounded to SNAPNET_MAX_ROUNDS.
 *
 * This gives bulk throughput close to what the fire-and-forget style allows
 * (no per-1400-byte-chunk round trip) while still being end-to-end reliable
 * against real packet loss, using netcon's ALREADY-VERIFIED ACKed transport
 * for exactly the parts that are small enough for stop-and-wait to be cheap
 * (a handful to a few hundred chunks' worth of control data, not two million).
 *
 * ============================================================================
 * ZERO-SKIP
 * ============================================================================
 * The store (logically: the SNAP_META_SIZE-byte header followed by the
 * SNAP_DRAM_SIZE-byte DRAM mirror — snapshot.h's SNAPNET_STORE_LEN bytes in
 * total; see snapshot.h's "Snapshot STORE region" for why these are, since
 * 2026-08, two DISJOINT physical regions rather than one contiguous window —
 * store_read()/store_write() in snapshot_net.c are what stitch them back into
 * one logical byte stream for everything below) is scanned in fixed
 * SNAPNET_CHUNK-byte pieces. An all-zero chunk is never
 * transmitted at all (no frame, seq number simply absent from the manifest);
 * the receiver pre-zeroes its destination before receiving, so an
 * intentionally-skipped chunk reconstructs correctly as zero with no wire
 * cost. This is the "at minimum, zero-skip" bar the task calls for; LZ4/
 * dirty-page tracking are explicitly out of scope for this pass (ROADMAP
 * names them as later, nice-to-have optimizations).
 *
 * ============================================================================
 * WHAT THIS DOES vs. WHAT snapshot.c ALREADY DOES
 * ============================================================================
 * snapshot_net_send() calls the EXISTING, unmodified snapshot_save() to
 * (re)populate the DRAM store, then streams that store out. snapshot_net_recv()
 * receives a store image into the SAME DRAM store region and, once it
 * validates (manifest fully accounted for + a whole-image CRC32 recheck
 * against the header's own crc32 field), calls the EXISTING, unmodified
 * snapshot_restore() to commit it into the live guest. Neither function in
 * snapshot.c/.h is touched by this file — this is pure addition.
 *
 * ============================================================================
 * KNOWN, UNVERIFIED-ON-HARDWARE RISK (see the report this shipped with)
 * ============================================================================
 * snapshot_net_send()/snapshot_net_recv() are long-running, fully synchronous
 * calls (no yielding) bounded only by wdt_pet()-fed loops and the round/poll
 * budgets below — this is a deliberate extension of the SAME trade-off
 * snapshot_save()/snapshot_restore() already make for their <1s local DRAM
 * copy, just for a much longer (network-bound, unmeasured) duration. Actual
 * transfer time, real-world packet loss behavior, and whether this wedges
 * anything else that shares CPU1's poll loop can only be confirmed live.
 *
 * Freestanding: <stdint.h> only, -mgeneral-regs-only, matches the rest of
 * the tree.
 */
#ifndef BZDOS_SNAPSHOT_NET_H
#define BZDOS_SNAPSHOT_NET_H

#include <stdint.h>
#include "exceptions.h"   /* struct el2_frame */
#include "snapshot.h"     /* SNAP_STORE_BASE / SNAP_META_SIZE / SNAP_DRAM_SIZE */

/* EtherType for the bulk snapshot-data channel. 0x88B5=console, 0x88B6=netcon,
 * 0x88B7=coredump; 0x88B8 is the next free local-experimental slot, so this
 * traffic never collides with any existing channel. */
#define SNAPNET_ETHERTYPE   0x88B8u

/* Payload bytes per bulk DATA frame AND the zero-skip scan granularity (the
 * two are deliberately the same value — see file header comment: this keeps
 * "chunk index" <-> "byte offset" a trivial multiply with no separate
 * off-list to keep in sync). Chosen to clear standard (non-jumbo) Ethernet
 * MTU: 14 (eth hdr) + SNAPNET_HDR_LEN (16) + 1400 = 1430 bytes, comfortably
 * under 1500. */
#define SNAPNET_CHUNK       1400u

/* Total store length this module streams: the same struct snapshot_hdr
 * region PLUS the DRAM copy that snapshot_save()/snapshot_restore() already
 * define (see snapshot.h SNAP_META_SIZE / SNAP_DRAM_SIZE). Kept as a macro
 * expression (not a hand-computed literal) so it can never drift from
 * snapshot.h if SNAP_DRAM_SIZE is ever widened for a bigger-DRAM board. */
#define SNAPNET_STORE_LEN   (SNAP_META_SIZE + SNAP_DRAM_SIZE)

/* Total chunk count and manifest/received-bitmap size, both derived purely
 * from the above so the board (C) and host (Python) side can never disagree
 * as long as both mirror SNAP_META_SIZE/SNAP_DRAM_SIZE/SNAPNET_CHUNK (exactly
 * the same "hardcode + cross-reference comment" convention coredump.h/.py
 * already use for DRAM_HI/STAGE2_DRAM_BASE). For the current 1 GiB
 * SNAP_DRAM_SIZE this is 767,006 chunks / ~93.6 KiB of bitmap. */
#define SNAPNET_TOTAL_CHUNKS  ((SNAPNET_STORE_LEN + SNAPNET_CHUNK - 1u) / SNAPNET_CHUNK)
#define SNAPNET_MANIFEST_BYTES ((SNAPNET_TOTAL_CHUNKS + 7u) / 8u)

/* Bounded retransmission-round budget (see file header "CHOSEN DESIGN") and
 * the per-round cap on how many missing chunk indices one netcon control
 * message carries (8192 entries * 4 bytes = 32 KiB, comfortably under both
 * netcon's 2 MiB cap and SNAPNET_MANIFEST_BYTES, so one shared scratch
 * buffer sized to the bigger of the two covers both uses). */
#define SNAPNET_MAX_ROUNDS   5
#define SNAPNET_MISSING_CAP  8192u

/* On-wire bulk DATA frame header — 16 bytes, byte-wise accessed (same reason
 * as netcon.c: this sits right after a 14-byte Ethernet header inside the
 * RX/TX DMA buffer, so it is only 2-byte aligned, not 4-byte).
 *
 *   off  size  field
 *   0    4     magic     0x42504E53 ("SNPB" LE bytes S,N,P,B)
 *   4    4     seq       chunk index, 0-based; off = seq*SNAPNET_CHUNK,
 *                        len = min(SNAPNET_CHUNK, SNAPNET_STORE_LEN - off) —
 *                        NEITHER off NOR total is sent on the wire; both are
 *                        derived identically on both ends from seq alone
 *                        (see file header "off" elimination rationale).
 *   8    2     len       payload bytes following the header (<=SNAPNET_CHUNK;
 *                        only the last chunk in the store is shorter)
 *   10   2     reserved  0
 *   12   4     crc32     IEEE crc32 over the len payload bytes
 *   16   len   payload
 *
 * There is no ACK/NAK/DONE frame type on THIS channel at all — reliability
 * is entirely the netcon-carried manifest/missing-list handshake described
 * in the file header. A frame that fails its magic/bounds/crc check is
 * simply dropped; the affected chunk shows up as "missing" in the next
 * manifest-diff round, same effect as if it were skipped-as-zero, EXCEPT the
 * manifest correctly says it was expected, so it gets a resend request. */
#define SNAPNET_HDR_LEN     16u
#define SNAPNET_MAGIC       0x42504E53u   /* "SNPB" LE */

/* RX hook, called from emac_poll() (via emac.c's weak-symbol fallback) for
 * every received ethertype-0x88B8 frame. `payload`/`len` are the Ethernet
 * payload bytes AFTER the 14-byte Ethernet header (same convention as
 * netcon_rx_frame()). Never blocks. Frames are only acted on while a
 * snapshot_net_recv() call has armed the receive state (see snapshot_net.c);
 * otherwise dropped, same idle-drop convention as netcon_rx_frame(). */
void snapshot_net_rx_frame(const uint8_t *payload, uint16_t len);

/* Freshen the DRAM store (snapshot_save(frame) — the EXISTING, unmodified
 * checkpoint path) and stream it to the host: one fire-and-forget pass over
 * all SNAPNET_TOTAL_CHUNKS chunks (skipping all-zero ones), then the
 * manifest bitmap delivered reliably over netcon, then up to
 * SNAPNET_MAX_ROUNDS bounded rounds servicing the host's missing-chunk
 * requests (also carried over netcon). `frame` is passed straight through to
 * snapshot_save() — see its own precondition (quiesced guest, no in-flight
 * device DMA).
 *
 * Returns:
 *    0  everything streamed, manifest delivered, and no further missing-
 *       chunk request arrived within the round budget (this does NOT prove
 *       the host actually reconstructed a complete image — see the report
 *       this shipped with: the host's pull script is the authority on final
 *       success and must report incompleteness itself if its own missing set
 *       is still non-empty after all rounds).
 *   -1  snapshot_save(frame) itself failed.
 *   -2  the manifest bitmap was never ACKed by the host over netcon
 *       (netcon_send() exhausted its own retry budget) — the host almost
 *       certainly wasn't listening / link down; nothing useful was likely
 *       delivered. */
int snapshot_net_send(const struct el2_frame *frame);

/* Receive a store image from the host and, once validated, restore it into
 * the live guest via the EXISTING, unmodified snapshot_restore(). Sequence:
 * zero the store, collect the host's fire-and-forget bulk pass (bounded idle
 * timeout ends the collection window), reliably receive the host's manifest
 * over netcon, then up to SNAPNET_MAX_ROUNDS bounded rounds requesting (over
 * netcon) and collecting any still-missing chunks. If the manifest is fully
 * accounted for, recomputes CRC32 over the received DRAM region and checks
 * it against the received header's own crc32 field (the same field
 * snapshot_save() already computes/relies on) BEFORE calling
 * snapshot_restore() — a corrupt-but-"complete" image is refused rather than
 * silently restored into the live guest.
 *
 * Returns:
 *    >=0  snapshot_restore()'s own return value (0 = restored).
 *    -1   `frame` was NULL.
 *    -2   never received a valid MANIFEST_BYTES-sized manifest over netcon
 *         within the timeout (host likely never got far enough to send one).
 *    -3   still missing chunks (per the manifest) after all rounds — refused
 *         to restore from a known-incomplete image.
 *    -4   the received image failed its whole-DRAM CRC32 recheck against the
 *         store header's own crc32 — refused to restore from possibly-
 *         corrupt data even though the manifest looked complete. */
int snapshot_net_recv(struct el2_frame *frame);

#endif /* BZDOS_SNAPSHOT_NET_H */
