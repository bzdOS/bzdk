#!/usr/bin/env python3
"""netcon.py — host-side library + CLI for the bzdOS "netcon" reliable
datagram transport, ethertype 0x88B6, layered on top of emac.c/netcon.c on
the board. Companion to repl-client.py's console channel (0x88B5) — netcon
is a SECOND raw-Ethernet channel used for bulk, ACKed transfers (hot-reload
pushes host->board, reliable multi-frame readback board->host) where plain
console frames are too lossy for multi-frame data (frames drop under
bursts; single small frames usually get through, multi-frame ones don't).

Wire format — MUST match netcon.h/netcon.c on the board exactly:
  struct netcon_hdr (little-endian, 20 bytes, no padding):
    magic      u16   0x4E43 ("NC")
    type       u8    0=DATA, 1=ACK, 2=NAK
    flags      u8    bit0 = LAST chunk (NC_FLAG_LAST)
    seq        u16   chunk sequence number (0-based, per transfer)
    total_len  u32   total blob length in bytes (whole transfer)
    off        u32   byte offset of this chunk within the blob
    chunk_len  u16   payload bytes following the header (0 for ACK/NAK)
    crc32      u32   IEEE crc32 over the chunk payload (0 for ACK/NAK)
  followed by `chunk_len` bytes of raw payload.

Protocol is stop-and-wait: one DATA frame outstanding at a time, sender
waits (bounded) for the matching ACK before advancing, retransmitting on
timeout up to a bounded retry count. This is what makes multi-frame
transfers deterministic over the lossy 0x88B5-class link.

CLI:
    netcon.py push <file> <hexaddr> [iface]   — reliably deliver a file to
        the board. Pair with a REPL `nrx <hexaddr> <len>` command that
        calls netcon_recv() on the board FIRST (or concurrently — the
        board's bounded receive loop is already spinning when frames start
        arriving); this script only drives the host side of the handshake.
    netcon.py pull <outfile> [iface] [timeout_s] — reliably collect a
        board -> host reply (e.g. from a board-side netcon_send() readback)
        and write it to <outfile>.
    netcon.py selftest — offline check of header pack/unpack + the crc32
        test vector (no network, no hardware).
"""
import sys
import time
import socket
import struct
import binascii

IFACE = "br0"
NC_ETHERTYPE = 0x88B6
BOARD_MAC = bytes.fromhex("02bd05000001")
BCAST = b"\xff" * 6

NC_MAGIC = 0x4E43
NC_DATA, NC_ACK, NC_NAK = 0, 1, 2
NC_FLAG_LAST = 1

CHUNK = 512                  # payload bytes per DATA frame — matches the
                              # board's NETCON_MAX_CHUNK (netcon.h)
MAX_LEN = 2 * 1024 * 1024     # matches the board's NETCON_MAX_LEN
MAX_RETRIES = 8               # matches the board's NETCON_MAX_RETRIES
ACK_TIMEOUT = 0.25            # seconds to wait for one ACK before a retry
RECV_IDLE_TIMEOUT = 3.0       # seconds of silence before giving up on a pull
INTER_FRAME_DELAY = 0.002     # small pacing delay between chunks — together
                              # with the ACK handshake this is what keeps the
                              # board's 8-descriptor RX ring from overflowing
                              # (see module docstring / report: a chunk is
                              # never sent until the previous one is ACKed,
                              # so at most ~1 netcon frame is ever in flight
                              # at once, plus this delay gives emac_poll()
                              # room to drain the ring between bursts of any
                              # OTHER traffic sharing the link).

HDR_FMT = "<HBBHIIHI"   # magic,type,flags,seq,total_len,off,chunk_len,crc32
HDR_LEN = struct.calcsize(HDR_FMT)
assert HDR_LEN == 20, f"netcon header must be 20 bytes, got {HDR_LEN}"


def _crc32(data: bytes) -> int:
    """Standard IEEE 802.3 / zlib CRC32 (poly 0xEDB88320) — identical
    algorithm to the board's crc32_calc() in netcon.c, so both sides agree
    bit-for-bit."""
    return binascii.crc32(data) & 0xFFFFFFFF


# Known-answer self-check so a broken table/algorithm never silently
# disagrees with the board's implementation (same vector is documented
# next to crc32_calc() in netcon.c).
assert _crc32(b"123456789") == 0xCBF43926, "crc32 self-check failed"


def pack_hdr(type_, flags, seq, total_len, off, chunk_len, crc):
    return struct.pack(HDR_FMT, NC_MAGIC, type_, flags, seq,
                        total_len, off, chunk_len, crc)


def unpack_hdr(buf):
    magic, type_, flags, seq, total_len, off, chunk_len, crc = \
        struct.unpack(HDR_FMT, buf[:HDR_LEN])
    return dict(magic=magic, type=type_, flags=flags, seq=seq,
                total_len=total_len, off=off, chunk_len=chunk_len, crc=crc)


def open_netcon_socket(iface=IFACE):
    """AF_PACKET raw socket bound to `iface`, filtered to the netcon
    ethertype (0x88B6) — mirrors repl-client.py's open_raw() for the
    console channel (0x88B5), just a different ethertype/purpose."""
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(NC_ETHERTYPE))
    s.bind((iface, NC_ETHERTYPE))
    return s, s.getsockname()[4]   # (socket, host MAC on that iface)


def _eth_frame(src_mac, dst_mac, payload):
    if len(payload) < 46:                    # Ethernet minimum payload
        payload = payload + b"\x00" * (46 - len(payload))
    return dst_mac + src_mac + struct.pack("!H", NC_ETHERTYPE) + payload


def _recv_one(sock, timeout):
    """Return one parsed+validated netcon frame (dict, plus 'chunk' bytes),
    or None if nothing matching arrives within `timeout` seconds. Drops
    (and keeps waiting out the remaining timeout for) anything too short,
    wrong ethertype/magic, truncated, or CRC-mismatched — the caller's
    bounded timeout is what prevents this from ever blocking forever."""
    deadline = time.time() + timeout
    while True:
        remain = deadline - time.time()
        if remain <= 0:
            return None
        sock.settimeout(remain)
        try:
            frame = sock.recv(2048)
        except (socket.timeout, OSError):
            return None
        if len(frame) < 14 + HDR_LEN:
            continue
        if struct.unpack("!H", frame[12:14])[0] != NC_ETHERTYPE:
            continue
        payload = frame[14:]
        h = unpack_hdr(payload)
        if h["magic"] != NC_MAGIC:
            continue
        chunk = payload[HDR_LEN:HDR_LEN + h["chunk_len"]]
        if len(chunk) < h["chunk_len"]:
            continue                          # truncated frame: drop
        if h["chunk_len"] and _crc32(chunk) != h["crc"]:
            continue                          # corrupt: drop, sender retries
        h["chunk"] = chunk
        return h


def send_blob(sock, blob, addr_hint=None, dst_mac=BOARD_MAC, verbose=True):
    """Reliably deliver `blob` to the board (host -> board), e.g. for
    hot-reload. Chunks it into <=CHUNK-byte DATA frames, stop-and-wait: send
    one, wait up to ACK_TIMEOUT for the matching ACK, retransmit on timeout
    up to MAX_RETRIES times before giving up. `addr_hint`, if given, is the
    destination memory address — informational only for progress messages;
    netcon frames carry no address field, so the board must already know
    where to land the bytes (set up out of band by a REPL `nrx <addr> <len>`
    command that calls netcon_recv(addr, len, ...) before/while this runs).
    Returns True iff every chunk was ACKed; False if any chunk exhausted its
    retry budget (the transfer is then incomplete on the board and it will
    have bailed out of its own bounded netcon_recv() timeout)."""
    if len(blob) > MAX_LEN:
        raise ValueError(f"blob too large: {len(blob)} > {MAX_LEN} (NETCON_MAX_LEN)")
    src = sock.getsockname()[4]
    total = len(blob)
    off = 0
    seq = 0

    if verbose:
        tgt = f" -> 0x{addr_hint:08x}" if addr_hint is not None else ""
        print(f"[netcon] pushing {total} bytes{tgt} in <={CHUNK}B chunks")

    while True:
        chunk = blob[off:off + CHUNK]
        is_last = (off + len(chunk)) >= total
        flags = NC_FLAG_LAST if is_last else 0
        hdr = pack_hdr(NC_DATA, flags, seq, total, off, len(chunk), _crc32(chunk))
        frame = _eth_frame(src, dst_mac, hdr + chunk)

        acked = False
        for _attempt in range(MAX_RETRIES):
            sock.send(frame)
            reply = _recv_one(sock, ACK_TIMEOUT)
            if reply and reply["seq"] == seq:
                if reply["type"] == NC_ACK:
                    acked = True
                    break
                if reply["type"] == NC_NAK:
                    continue        # immediate retransmit, skip the rest of
                                     # this timeout window
        if not acked:
            if verbose:
                print(f"\n[netcon] FAILED: chunk seq={seq} off={off} never "
                      f"ACKed after {MAX_RETRIES} attempts — aborting")
            return False
        if verbose:
            print(f"[netcon] chunk seq={seq} off={off}/{total} ACKed" + " " * 8,
                  end="\r")

        off += len(chunk)
        seq += 1
        if off >= total:
            break
        time.sleep(INTER_FRAME_DELAY)

    if verbose:
        print(f"\n[netcon] push complete: {total} bytes, {seq} chunk(s)")
    return True


def recv_blob(sock, timeout=RECV_IDLE_TIMEOUT, max_len=MAX_LEN, dst_mac=BOARD_MAC):
    """Reliably collect a board -> host reply (the board side calling
    netcon_send()). ACKs every valid chunk as it lands (idempotent — a
    duplicate is just re-ACKed, matching the board's own idempotent
    handling), reassembles by offset, and stops on the LAST-flagged chunk or
    after `timeout` seconds of silence (a lost tail returns whatever arrived
    so far rather than hanging). Returns the reassembled bytes."""
    src = sock.getsockname()[4]
    buf = bytearray()
    seen = set()
    total_len = None
    deadline = time.time() + timeout

    while time.time() < deadline:
        h = _recv_one(sock, max(0.01, min(0.2, deadline - time.time())))
        if h is None or h["type"] != NC_DATA:
            continue
        total_len = h["total_len"]
        if total_len > max_len:
            break                      # refuse an absurd/garbage total_len

        ack = pack_hdr(NC_ACK, 0, h["seq"], total_len, 0, 0, 0)
        sock.send(_eth_frame(src, dst_mac, ack))

        if h["seq"] not in seen:
            seen.add(h["seq"])
            end = h["off"] + h["chunk_len"]
            if end > len(buf):
                buf.extend(b"\x00" * (end - len(buf)))
            buf[h["off"]:end] = h["chunk"]
            deadline = time.time() + timeout   # progress: reset idle clock

        if h["flags"] & NC_FLAG_LAST:
            break

    return bytes(buf[:total_len]) if total_len is not None else bytes(buf)


# ------------------------------------------------------------------ #
# CLI                                                                  #
# ------------------------------------------------------------------ #
def _cli_push(args):
    if len(args) < 2:
        print("usage: netcon.py push <file> <hexaddr> [iface]")
        return 2
    path, hexaddr = args[0], args[1]
    iface = args[2] if len(args) > 2 else IFACE
    addr = int(hexaddr, 16)
    with open(path, "rb") as f:
        blob = f.read()
    print(f"[netcon] {path}: {len(blob)} bytes -> board addr 0x{addr:08x} via {iface}")
    print(f"[netcon] make sure the REPL has issued: nrx 0x{addr:x} {len(blob)}")
    sock, _ = open_netcon_socket(iface)
    try:
        ok = send_blob(sock, blob, addr_hint=addr)
    finally:
        sock.close()
    return 0 if ok else 1


def _cli_pull(args):
    if len(args) < 1:
        print("usage: netcon.py pull <outfile> [iface] [timeout_s]")
        return 2
    path = args[0]
    iface = args[1] if len(args) > 1 else IFACE
    timeout = float(args[2]) if len(args) > 2 else RECV_IDLE_TIMEOUT
    sock, _ = open_netcon_socket(iface)
    print(f"[netcon] listening for a board reply on {iface} (timeout {timeout}s)...")
    try:
        data = recv_blob(sock, timeout=timeout)
    finally:
        sock.close()
    with open(path, "wb") as f:
        f.write(data)
    print(f"[netcon] wrote {len(data)} bytes to {path}")
    return 0


def _cli_selftest(_args):
    """Offline check of header pack/unpack + the crc32 vector — no network,
    no hardware."""
    hdr = pack_hdr(NC_DATA, NC_FLAG_LAST, 7, 1000, 500, 256, 0xDEADBEEF)
    h = unpack_hdr(hdr)
    expect = dict(magic=NC_MAGIC, type=NC_DATA, flags=NC_FLAG_LAST, seq=7,
                  total_len=1000, off=500, chunk_len=256, crc=0xDEADBEEF)
    assert h == expect, f"header round-trip mismatch: {h} != {expect}"
    assert _crc32(b"123456789") == 0xCBF43926
    assert HDR_LEN == 20
    print("[netcon] selftest OK (header pack/unpack round-trip, crc32 vector, "
          f"header size {HDR_LEN}B)")
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    if cmd == "push":
        return _cli_push(rest)
    if cmd == "pull":
        return _cli_pull(rest)
    if cmd == "selftest":
        return _cli_selftest(rest)
    print(f"unknown command: {cmd}")
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
