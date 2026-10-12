/* SPDX-License-Identifier: BSD-2-Clause */

/* dbgauth.h -- authenticated envelope for every LAN-facing HV debug channel.
 *
 * All host -> HV debug traffic (console/dbgmon/gdbstub, netcon, dbgtools,
 * snapnet) rides ONE envelope on ethertype 0x88B5, with the channel named
 * inside the authenticated header. Frame payload (bytes after the 14-byte
 * Ethernet header), all integers big-endian:
 *
 *   HELLO  host->HV   00 'B' 'Z' 'K' | 01 | 00 | Nh[16]
 *   CHAL   HV->host   00 'B' 'Z' 'K' | 02 | 00 | sess[4] | Sb[16] | Nh[16] |
 *                     proof[16]
 *   DATA   host->HV   00 'B' 'Z' 'K' | 03 | chan | sess[4] | ctr[8] | len[2] |
 *                     tag[16] | inner[len]
 *   NAK    HV->host   00 'B' 'Z' 'K' | 04 | chan | sess[4]   (unknown session
 *                     or bad tag; to the frame's source, at most 1 per 20 ms)
 *
 *   Sb    per-boot board nonce, derived on the first HELLO of a boot from
 *         timing entropy (CNTPCT at every received frame since boot);
 *   sess  session number: starts at a random per-boot value, +1 per HELLO;
 *   Ks    = HMAC(K, "BZK1/sess" | Sb | sess | Nh)       session key
 *   proof = HMAC(K, "BZK1/chal" | Sb | sess | Nh)[:16]  board proves K
 *   tag   = HMAC(Ks, payload[0..19] | inner)[:16]
 *   ctr   strictly increasing within a session.
 *
 * Replay: a frame from an earlier boot names a session key built from an
 * earlier Sb, a replayed HELLO yields a NEW sess (and so a new Ks), and
 * within a session ctr must grow. Nothing is persisted and nothing resets
 * to a guessable state at boot.
 *
 * The leading 0x00 keeps every envelope invisible to the text-console
 * readers on the host (they cut each frame at the first NUL) and to the
 * legacy console parser in a DBG_AUTH_COMPAT image.
 */
#ifndef DBGAUTH_H
#define DBGAUTH_H

#include <stdint.h>

#define DBGAUTH_CHAN_CONSOLE   0u
#define DBGAUTH_CHAN_NETCON    1u
#define DBGAUTH_CHAN_DBGTOOLS  2u
#define DBGAUTH_CHAN_SNAPNET   3u

#define DBGAUTH_NOT_ENVELOPE   0   /* not ours: caller decides (legacy/drop) */
#define DBGAUTH_ACCEPT         1   /* authenticated DATA: dispatch *inner */
#define DBGAUTH_CONSUMED       2   /* control frame or rejected: done */

/* Counters, readable with the dbgmon `auth` command. */
enum {
	DBGAUTH_ST_OK = 0,        /* DATA frames accepted                    */
	DBGAUTH_ST_HELLO,         /* sessions opened                         */
	DBGAUTH_ST_BADTAG,        /* DATA with a wrong tag                   */
	DBGAUTH_ST_REPLAY,        /* DATA with a valid tag, ctr not above last */
	DBGAUTH_ST_NOSESS,        /* DATA naming an unknown/evicted session  */
	DBGAUTH_ST_MALFORMED,     /* envelope magic but bad length/type      */
	DBGAUTH_ST_LEGACY_OK,     /* unauthenticated frames ACCEPTED (compat)*/
	DBGAUTH_ST_LEGACY_DROP,   /* unauthenticated frames DROPPED (strict) */
	DBGAUTH_ST_NAKS,          /* NAKs sent                               */
	DBGAUTH_ST_EVICT,         /* live sessions evicted by a newer HELLO  */
	DBGAUTH_ST_VERIFY_TICKS,  /* CNTPCT ticks the last DATA check took   */
	DBGAUTH_ST_VERIFY_LEN,    /* ...and that frame's inner length        */
	DBGAUTH_ST_N
};

/* Mix a timing sample into the board-nonce entropy pool (cheap; called for
 * every received frame and once at EMAC init). */
void dbgauth_entropy(uint64_t v);

/* Parse one received 0x88B5 frame (`frame` = full Ethernet frame, `len`
 * bytes incl. whatever padding/FCS the DMA delivered). On DBGAUTH_ACCEPT,
 * chan, inner and inner_len describe the authenticated payload. HELLOs are
 * answered (CHAL) and unknown sessions NAKed from in here, unicast to the
 * frame's source MAC. */
int dbgauth_rx(const uint8_t *frame, uint32_t len, uint8_t *chan,
	       const uint8_t **inner, uint32_t *inner_len);

void dbgauth_count(uint32_t which);
const volatile uint32_t *dbgauth_stats(void);

/* Reply destination: the source MAC of the last accepted debug frame, or
 * NULL before any (callers then broadcast). */
const uint8_t *dbgauth_peer(void);
void dbgauth_set_peer(const uint8_t mac[6]);

#endif /* DBGAUTH_H */
