/* SPDX-License-Identifier: BSD-2-Clause */

/* netcon.h — reliable stop-and-wait datagram transport for the bzdOS
 * microkernel debug link, riding on top of emac.c's raw-Ethernet TX/RX
 * (ethertype 0x88B6 — see emac.h). The console (0x88B5, emac_getc/putc) is a
 * lossy best-effort byte stream; netcon adds ACK + bounded retransmit so
 * multi-frame transfers (hot-reload blobs pushed host->board, multi-line
 * breadcrumb/readback replies board->host) are deterministic at the link
 * level even though individual Ethernet frames still drop occasionally.
 *
 * Wire format (little-endian throughout), one struct per frame, immediately
 * followed by up to NETCON_MAX_CHUNK bytes of chunk payload:
 *
 *   struct netcon_hdr {
 *       uint16_t magic;      0x4E43 ("NC")
 *       uint8_t  type;       NC_DATA=0, NC_ACK=1, NC_NAK=2
 *       uint8_t  flags;      bit0 = NC_FLAG_LAST (this is the final chunk)
 *       uint16_t seq;        chunk sequence number, 0-based, per transfer
 *       uint32_t total_len;  total blob length in bytes (whole transfer)
 *       uint32_t off;        byte offset of this chunk within the blob
 *       uint16_t chunk_len;  bytes of real payload following the header
 *                            (0 for ACK/NAK — they carry no payload)
 *       uint32_t crc32;      IEEE CRC32 over the chunk_len payload bytes
 *                            (0 for ACK/NAK)
 *   };                       -- 20 bytes, no compiler padding (all fields
 *                                naturally aligned at their own size and the
 *                                struct is packed explicitly in netcon.c)
 *
 * ACK/NAK frames set seq to the chunk being acknowledged/rejected and
 * chunk_len=0 (no payload follows). A duplicate DATA chunk (one already
 * fully received and ACKed) is idempotent: it is simply re-ACKed, not
 * re-delivered to the destination buffer or double-counted.
 *
 * Everything here is bounded: netcon_recv()'s polling loop is a single
 * caller-supplied timeout (in emac_poll() call counts, not wall time — this
 * is a freestanding build with no calibrated timer dependency here), and
 * netcon_send()'s per-chunk retry loop gives up after NETCON_MAX_RETRIES.
 * Both pet the watchdog every spin so a lost tail aborts cleanly instead of
 * wedging the board.
 */
#ifndef BZDOS_NETCON_H
#define BZDOS_NETCON_H
#include <stdint.h>

/* Ethertype is owned by emac.c/emac.h (0x88B6); not redefined here to avoid
 * two sources of truth — netcon.c uses emac_send_frame() directly. */

/* Frame type byte (netcon_hdr.type). */
enum {
    NC_DATA = 0,
    NC_ACK  = 1,
    NC_NAK  = 2,
};

/* netcon_hdr.flags bits. */
#define NC_FLAG_LAST (1u << 0)   /* this DATA chunk is the last one */

/* Max chunk PAYLOAD bytes per DATA frame (excludes the 20-byte header).
 * Conservative so header+chunk+Ethernet header comfortably clears the
 * 60-byte minimum with room to spare and stays well under ETH_BUFSIZE;
 * matches the host side's pacing target (netcon.py CHUNK). */
#define NETCON_MAX_CHUNK   512u

/* Reject any requested transfer bigger than this (2 MiB cap per the spec —
 * a hot-reload blob or breadcrumb dump should never legitimately be larger,
 * and an unbounded/garbage total_len must never be allowed to walk memory
 * off the end of a caller-supplied buffer). */
#define NETCON_MAX_LEN     (2u * 1024u * 1024u)

/* Bounded retry budget for netcon_send()'s per-chunk ACK wait. */
#define NETCON_MAX_RETRIES 8

/* RX hook called from emac_poll() (via emac.c's weak-symbol fallback) for
 * every received ethertype-0x88B6 frame. `payload`/`len` are the Ethernet
 * payload bytes AFTER the 14-byte Ethernet header (len may include trailing
 * zero-pad up to the 60-byte Ethernet minimum — this function reads only
 * what the header's chunk_len says is real and ignores the rest).
 * Never blocks; never called by application code directly. */
void netcon_rx_frame(const uint8_t *payload, uint16_t len);

/* Reliably receive `len` bytes from the host into memory at `addr`
 * (physical/flat-mapped address — same MMU contract as the rest of the
 * bare-metal code). Arms the receive state machine, then spins calling
 * emac_poll() (which drives netcon_rx_frame()) + wdt_pet() until either:
 *   - all `len` bytes have arrived (contiguous, chunk 0..N-1, each ACKed
 *     as it lands) -> returns `len`, or
 *   - `timeout_polls` consecutive emac_poll() calls pass with NO forward
 *     progress (no new chunk accepted) -> returns however many bytes had
 *     arrived so far (may be 0), i.e. a lost tail aborts cleanly rather
 *     than wedging.
 * Rejects (returns 0 immediately) if len > NETCON_MAX_LEN. This is what a
 * REPL `nrx <addr> <len>` command should call directly. */
int netcon_recv(uint32_t addr, uint32_t len, uint32_t timeout_polls);

/* Reliably send `len` bytes from `data` to the host as a netcon reply
 * (e.g. breadcrumb/readback output). Chunks the data, sends each DATA
 * frame, and spins (bounded: emac_poll() + wdt_pet(), up to
 * NETCON_MAX_RETRIES retransmits per chunk on ACK timeout) waiting for the
 * matching ACK before moving to the next chunk. Returns 1 if every chunk
 * was ACKed, 0 if any chunk exhausted its retry budget (transfer aborted;
 * the host will see a short/partial blob and can re-request). */
int netcon_send(const uint8_t *data, uint32_t len);

#endif /* BZDOS_NETCON_H */
