/* netcon.c — reliable stop-and-wait datagram transport for the bzdOS
 * microkernel, implementing netcon.h on top of emac.c's raw-Ethernet TX
 * (emac_send_frame, ethertype 0x88B6) and RX demux hook (netcon_rx_frame(),
 * called from emac_poll() for every 0x88B6 frame — see emac.c/emac.h).
 *
 * Freestanding: -ffreestanding -nostdlib -mgeneral-regs-only, <stdint.h>
 * only, no libc. Header fields are read/written byte-by-byte (NOT via a
 * cast struct pointer) because the 20-byte netcon header sits right after
 * the 14-byte Ethernet header inside the RX/TX DMA buffers, so it is only
 * 2-byte aligned, not 4-byte — a cast `struct netcon_hdr *` would put
 * naturally-4-byte fields (total_len/off/crc32) at unaligned addresses.
 * Byte-wise little-endian accessors sidestep that entirely, and cost
 * nothing extra at -O2.
 */
#include <stdint.h>
#include "netcon.h"
#include "emac.h"
#include "wdt.h"

/* Ethertype for this channel — mirrors emac.c's ETHERTYPE_NETCON (0x88B6);
 * kept here too since emac.h intentionally doesn't expose the raw value
 * (only the send/RX-hook API), and netcon.c is the one caller that needs
 * to pass an ethertype to emac_send_frame(). */
#define NETCON_ETHERTYPE 0x88B6u

#define NC_MAGIC     0x4E43u   /* "NC" */
#define NC_HDR_LEN   20u       /* magic(2)+type(1)+flags(1)+seq(2)+
                                * total_len(4)+off(4)+chunk_len(2)+crc32(4) */

/* ------------------------------------------------------------------ */
/* Little-endian byte accessors (see file header comment for why not a    */
/* cast struct).                                                          */
/* ------------------------------------------------------------------ */
static inline uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff); }

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3 / zlib polynomial 0xEDB88320) — table built lazily on */
/* first use; identical algorithm to netcon.py's binascii.crc32 so both    */
/* sides agree bit-for-bit. Known test vector: crc32("123456789") ==       */
/* 0xCBF43926 (checked again in netcon.py at import time).                */
/* ------------------------------------------------------------------ */
static uint32_t crc_table[256];
static int      crc_table_ready;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    if (!crc_table_ready)
        crc32_init();
    for (uint32_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* Cache maintenance for received DRAM payload — same coherent-store        */
/* pattern used throughout emac.c/musb.c (D-cache is on; DMA'd descriptors  */
/* and any DRAM that must survive a reset are hand-flushed). Duplicated     */
/* here (rather than exported from emac.c) to keep netcon.c link-           */
/* independent of emac.c internals — only emac.h's public API is used.     */
/* ------------------------------------------------------------------ */
static inline void nc_cache_clean(uintptr_t addr, uint32_t size)
{
    uintptr_t p = addr & ~63UL, end = addr + size;
    for (; p < end; p += 64)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* Receive-side state (host -> board bulk transfer, e.g. `nrx`)            */
/* ------------------------------------------------------------------ */
static int      s_recv_active;      /* a netcon_recv() call is in progress */
static uint32_t s_recv_addr;        /* destination base address */
static uint32_t s_recv_total;       /* expected total length */
static uint32_t s_recv_got;         /* contiguous bytes received so far */
static uint16_t s_recv_expect_seq;  /* next in-order chunk sequence number */

/* ------------------------------------------------------------------ */
/* Send-side state (board -> host reliable reply, e.g. breadcrumb readback)*/
/* ------------------------------------------------------------------ */
static int      s_send_waiting;     /* netcon_send() is waiting on an ACK */
static uint16_t s_send_wait_seq;    /* the seq we're waiting to see ACKed */
static volatile int s_send_acked;   /* set by netcon_rx_frame() on match */

/* Send one ACK or NAK for `seq` (chunk_len 0, no payload). Best-effort —
 * if the link is briefly down or the TX ring is full, emac_send_frame()
 * drops it and the host's own retransmit timeout recovers. */
static void send_ack_nak(uint8_t type, uint16_t seq, uint32_t total_len)
{
    uint8_t hdr[NC_HDR_LEN];
    wr16(hdr + 0, NC_MAGIC);
    hdr[2] = type;
    hdr[3] = 0;
    wr16(hdr + 4, seq);
    wr32(hdr + 6, total_len);
    wr32(hdr + 10, 0);
    wr16(hdr + 14, 0);
    wr32(hdr + 16, 0);
    (void)emac_send_frame(NETCON_ETHERTYPE, hdr, (uint16_t)NC_HDR_LEN);
}

/* ------------------------------------------------------------------ */
/* RX hook — called from emac_poll() for every ethertype-0x88B6 frame.     */
/* Never blocks; does at most one small copy + one ACK/NAK send.          */
/* ------------------------------------------------------------------ */
void netcon_rx_frame(const uint8_t *payload, uint16_t len)
{
    if (len < NC_HDR_LEN)
        return;                                  /* too short: drop */
    if (rd16(payload + 0) != NC_MAGIC)
        return;                                  /* not our header: drop */

    uint8_t  type      = payload[2];
    uint8_t  flags     = payload[3];
    uint16_t seq       = rd16(payload + 4);
    uint32_t total_len = rd32(payload + 6);
    uint32_t off       = rd32(payload + 10);
    uint16_t chunk_len = rd16(payload + 14);
    uint32_t crc       = rd32(payload + 16);
    (void)flags;

    /* Defensive: never trust chunk_len past what actually arrived. */
    if ((uint32_t)chunk_len > (uint32_t)len - NC_HDR_LEN)
        return;                                  /* corrupt header: drop */
    const uint8_t *chunk = payload + NC_HDR_LEN;

    if (type == NC_ACK) {
        if (s_send_waiting && seq == s_send_wait_seq)
            s_send_acked = 1;
        return;
    }
    if (type == NC_NAK)
        return;   /* board never needs to act on an inbound NAK today */

    if (type != NC_DATA)
        return;                                  /* unknown type: drop */

    if (!s_recv_active || total_len != s_recv_total)
        return;   /* no matching transfer in progress: drop, host retries */

    /* CRC check over the chunk payload only. */
    if (crc32_calc(chunk, chunk_len) != crc) {
        send_ack_nak(NC_NAK, seq, s_recv_total);
        return;
    }

    if (seq == s_recv_expect_seq && off == s_recv_got) {
        /* New in-order chunk. Bounds-check against the receive buffer
         * before touching memory. */
        if ((uint64_t)off + chunk_len <= (uint64_t)s_recv_total) {
            uint8_t *dst = (uint8_t *)(uintptr_t)(s_recv_addr + off);
            for (uint16_t i = 0; i < chunk_len; i++)
                dst[i] = chunk[i];
            if (chunk_len)
                nc_cache_clean((uintptr_t)dst, chunk_len);
            s_recv_got += chunk_len;
            s_recv_expect_seq++;
        }
        send_ack_nak(NC_ACK, seq, s_recv_total);
    } else if (seq < s_recv_expect_seq) {
        /* Duplicate of an already-delivered chunk (host retransmitted
         * because our ACK was lost) — idempotent: just re-ACK, no copy. */
        send_ack_nak(NC_ACK, seq, s_recv_total);
    }
    /* else: a chunk arrived out of the strict in-order sequence (shouldn't
     * happen under stop-and-wait); silently drop and let the host's
     * per-chunk timeout retransmit the one we actually still need. */
}

/* ------------------------------------------------------------------ */
/* netcon_recv() — host -> board reliable bulk receive                     */
/* ------------------------------------------------------------------ */
int netcon_recv(uint32_t addr, uint32_t len, uint32_t timeout_polls)
{
    if (len == 0)
        return 0;
    if (len > NETCON_MAX_LEN)
        return 0;                 /* reject oversized request outright */

    s_recv_addr       = addr;
    s_recv_total       = len;
    s_recv_got         = 0;
    s_recv_expect_seq  = 0;
    s_recv_active      = 1;

    uint32_t idle = 0;
    while (s_recv_got < s_recv_total) {
        uint32_t before = s_recv_got;
        emac_poll();               /* drives netcon_rx_frame() for us */
        wdt_pet();
        if (s_recv_got != before)
            idle = 0;               /* forward progress: reset the idle clock */
        else if (++idle >= timeout_polls)
            break;                  /* bounded abort: lost tail, don't wedge */
    }

    s_recv_active = 0;
    return (int)s_recv_got;
}

/* ------------------------------------------------------------------ */
/* netcon_send() — board -> host reliable reply                           */
/* ------------------------------------------------------------------ */
/* Bounded spin count per ACK-wait attempt (not calibrated wall time — this
 * is a freestanding build with no timer dependency here; emac_poll() itself
 * is cheap/non-blocking, so this just needs to be "long enough" for a
 * round trip on a 100 Mbit LAN many times over while staying bounded). */
#define NETCON_ACK_WAIT_POLLS 50000u

int netcon_send(const uint8_t *data, uint32_t len)
{
    if (len > NETCON_MAX_LEN)
        return 0;

    uint32_t off = 0;
    uint16_t seq = 0;
    uint8_t  frame[NC_HDR_LEN + NETCON_MAX_CHUNK];

    if (len == 0) {
        /* Degenerate zero-length reply: send a single empty, flagged-last
         * chunk so the host still gets a completion signal. */
        wr16(frame + 0, NC_MAGIC);
        frame[2] = NC_DATA;
        frame[3] = NC_FLAG_LAST;
        wr16(frame + 4, 0);
        wr32(frame + 6, 0);
        wr32(frame + 10, 0);
        wr16(frame + 14, 0);
        wr32(frame + 16, crc32_calc(data, 0));

        for (int attempt = 0; attempt < NETCON_MAX_RETRIES; attempt++) {
            s_send_wait_seq = 0;
            s_send_acked    = 0;
            s_send_waiting  = 1;
            (void)emac_send_frame(NETCON_ETHERTYPE, frame, (uint16_t)NC_HDR_LEN);
            for (uint32_t t = 0; t < NETCON_ACK_WAIT_POLLS && !s_send_acked; t++) {
                emac_poll();
                wdt_pet();
            }
            s_send_waiting = 0;
            if (s_send_acked)
                return 1;
        }
        return 0;
    }

    while (off < len) {
        uint32_t chunk_len = len - off;
        if (chunk_len > NETCON_MAX_CHUNK)
            chunk_len = NETCON_MAX_CHUNK;
        int is_last = (off + chunk_len >= len);

        wr16(frame + 0, NC_MAGIC);
        frame[2] = NC_DATA;
        frame[3] = (uint8_t)(is_last ? NC_FLAG_LAST : 0);
        wr16(frame + 4, seq);
        wr32(frame + 6, len);
        wr32(frame + 10, off);
        wr16(frame + 14, (uint16_t)chunk_len);
        wr32(frame + 16, crc32_calc(data + off, chunk_len));
        for (uint32_t i = 0; i < chunk_len; i++)
            frame[NC_HDR_LEN + i] = data[off + i];

        int acked = 0;
        for (int attempt = 0; attempt < NETCON_MAX_RETRIES && !acked; attempt++) {
            s_send_wait_seq = seq;
            s_send_acked    = 0;
            s_send_waiting  = 1;
            (void)emac_send_frame(NETCON_ETHERTYPE, frame,
                                   (uint16_t)(NC_HDR_LEN + chunk_len));
            for (uint32_t t = 0; t < NETCON_ACK_WAIT_POLLS && !s_send_acked; t++) {
                emac_poll();
                wdt_pet();
            }
            s_send_waiting = 0;
            if (s_send_acked)
                acked = 1;
        }
        if (!acked)
            return 0;              /* exhausted retry budget: abort cleanly */

        off += chunk_len;
        seq++;
    }
    return 1;
}
