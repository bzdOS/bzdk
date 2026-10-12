/* SPDX-License-Identifier: BSD-2-Clause */

/* hmac_sha256.h -- compact freestanding SHA-256 / HMAC-SHA256, no libc.
 *
 * Part of ROADMAP.md T5 / docs/security-notes.md: the keyed envelope that
 * dbgauth.c puts on every LAN-facing HV debug channel. Built only under
 * -DDBG_AUTH (on by default for `make dbg`; `DBG_NO_AUTH=1` opts out).
 *
 * No malloc, no floating point, no libc string.h dependency -- every helper
 * below is self-contained arithmetic over fixed-size local buffers, safe
 * under -ffreestanding -mgeneral-regs-only same as the rest of the tree.
 */
#ifndef HMAC_SHA256_H
#define HMAC_SHA256_H

#include <stdint.h>

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN  64

/* One-shot HMAC-SHA256: out[] must have room for SHA256_DIGEST_LEN bytes.
 * key/msg may be any length (key longer than the block size is hashed down
 * first, per RFC 2104). Pure function, no static state retained. */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                  const uint8_t *msg, uint32_t msg_len,
                  uint8_t out[SHA256_DIGEST_LEN]);

/* As hmac_sha256(), over the concatenation a | b. */
void hmac_sha256_2(const uint8_t *key, uint32_t key_len,
                   const uint8_t *a, uint32_t alen,
                   const uint8_t *b, uint32_t blen,
                   uint8_t out[SHA256_DIGEST_LEN]);

/* Constant-time-ish compare of two 32-byte MACs (no early-out branch on the
 * first mismatching byte, to avoid a naive timing side channel over the LAN
 * link this protocol rides on). Returns 1 if equal, 0 otherwise. */
int hmac_sha256_equal(const uint8_t a[SHA256_DIGEST_LEN],
                       const uint8_t b[SHA256_DIGEST_LEN]);

#endif /* HMAC_SHA256_H */
