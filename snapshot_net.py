#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""snapshot_net.py — host-side pull/push for the bzdOS "snapshot over GbE"
transport (ROADMAP D1). Companion to snapshot_net.h/.c on the board.

WHY TWO CHANNELS (must match snapshot_net.h's design-rationale comment
exactly — read that first if this is your first time here):

  - The BULK image (up to ~1 GiB) rides a brand-new, bespoke, fire-and-forget
    EtherType 0x88B8 channel (own 16-byte header: magic/seq/len/crc32, no
    ACK per frame at all) — this is what actually gets throughput anywhere
    near the ROADMAP's ~20-30s/2GiB estimate; netcon's ACKed stop-and-wait
    style would need ~2.1M chunk round trips for a 1 GiB image (tens of
    minutes, two orders of magnitude too slow) even though its wire format
    could technically carry the byte count.
  - The MANIFEST (which chunks are non-zero / were actually sent) and any
    MISSING-CHUNK retry requests are small (~94 KiB and <=32 KiB respectively)
    and MUST be reliable, so those ride the EXISTING, UNCHANGED netcon.py/
    netcon.c transport (ethertype 0x88B6) — this script imports netcon.py
    directly rather than re-implementing an ACKed transport.

Both scripts must be in the same directory (this one does
sys.path.insert so `import netcon` works regardless of cwd).

Zero-skip: the store is scanned/reconstructed in fixed CHUNK-byte pieces;
an all-zero chunk is never put on the wire in either direction (the
receiving side always starts from an all-zero destination).

CLI:
    snapshot_net.py pull <outfile> [iface] [--manifest-timeout S]
        Collects a board -> host bulk stream (board's snapshot_net_send(),
        triggered separately on the board) into <outfile> (always exactly
        SNAPNET_STORE_LEN bytes, zero-filled where zero-skipped). Run this
        FIRST (it starts listening before anything is sent), same convention
        netcon.py's own push/pull commands already use.
    snapshot_net.py push <infile> [iface]
        Streams a previously-pulled <infile> to the board (host -> board),
        for the board's snapshot_net_recv() (triggered separately) to
        receive + restore. <infile> MUST be exactly SNAPNET_STORE_LEN bytes
        (i.e. produced by this script's own `pull`).
    snapshot_net.py selftest
        Offline header/bitmap/crc32 sanity check, no network, no hardware.

HONESTY NOTE: none of this has been exercised against the real board/link —
see the report this shipped with for exactly what is reasoned-through vs.
what needs a live run to confirm (real transfer timing, real packet-loss
recovery behavior, whether the fixed-size-padding convention below actually
round-trips through netcon.c's `total_len` check on real hardware).
"""
import os
import sys
import time
import zlib
import struct
import socket
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netcon  # noqa: E402  (see module docstring: reused unmodified)

# ------------------------------------------------------------------ #
# Constants — MUST mirror snapshot_net.h / snapshot.h exactly (same
# "hardcode + cross-reference comment" convention coredump.py-equivalent
# constants already use elsewhere in this tree for DRAM_HI/STAGE2_DRAM_BASE).
# ------------------------------------------------------------------ #
IFACE = "br0"
SNAPNET_ETHERTYPE = 0x88B8
BOARD_MAC = netcon.BOARD_MAC

SNAP_META_SIZE = 0x00010000          # snapshot.h SNAP_META_SIZE
SNAP_DRAM_SIZE = 0x40000000          # snapshot.h SNAP_DRAM_SIZE (== stage2.h
                                      # STAGE2_DRAM_SIZE; 1 GiB on this board)
STORE_LEN      = SNAP_META_SIZE + SNAP_DRAM_SIZE   # snapshot_net.h SNAPNET_STORE_LEN

CHUNK = 1400                          # snapshot_net.h SNAPNET_CHUNK
TOTAL_CHUNKS = (STORE_LEN + CHUNK - 1) // CHUNK      # snapshot_net.h SNAPNET_TOTAL_CHUNKS
MANIFEST_BYTES = (TOTAL_CHUNKS + 7) // 8             # snapshot_net.h SNAPNET_MANIFEST_BYTES

MAX_ROUNDS = 5                         # snapshot_net.h SNAPNET_MAX_ROUNDS
MISSING_CAP = 8192                     # snapshot_net.h SNAPNET_MISSING_CAP
SENTINEL = 0xFFFFFFFF                  # snapshot_net.h SNAPNET_SENTINEL

MAGIC = 0x42504E53                     # snapshot_net.h SNAPNET_MAGIC ("SNPB" LE)
HDR_FMT = "<IIHHI"                     # magic,seq,len,reserved,crc32
HDR_LEN = struct.calcsize(HDR_FMT)
assert HDR_LEN == 16, f"snapnet header must be 16 bytes, got {HDR_LEN}"

MANIFEST_TIMEOUT_S = 20.0    # netcon idle timeout waiting for the manifest
ROUND_TIMEOUT_S     = 15.0   # netcon idle timeout waiting for a missing-list
SETTLE_S            = 1.0    # grace period after a control message before
                              # re-checking what the bulk reader has collected
BULK_READ_TIMEOUT_S = 0.5    # per-recv() timeout on the bulk socket (just a
                              # poll granularity, not a transfer timeout)


def _crc32(data: bytes) -> int:
    """Same standard IEEE crc32 netcon.py already uses/self-checks; kept
    here too so this module has no import-order dependency on that check."""
    return zlib.crc32(data) & 0xFFFFFFFF


def bit_test(bm: bytes, i: int) -> int:
    return (bm[i >> 3] >> (i & 7)) & 1


def bit_set(bm: bytearray, i: int) -> None:
    bm[i >> 3] |= (1 << (i & 7))


def pack_hdr(seq: int, length: int, crc: int) -> bytes:
    return struct.pack(HDR_FMT, MAGIC, seq, length, 0, crc)


def unpack_hdr(buf: bytes):
    magic, seq, length, _reserved, crc = struct.unpack(HDR_FMT, buf[:HDR_LEN])
    return magic, seq, length, crc


def chunk_range(seq: int):
    off = seq * CHUNK
    end = min(off + CHUNK, STORE_LEN)
    return off, end


def open_bulk_socket(iface=IFACE):
    """AF_PACKET raw socket bound to `iface`, filtered to the bulk
    snapshot-stream ethertype (0x88B8) — mirrors netcon.py's
    open_netcon_socket(), just a different ethertype/purpose."""
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(SNAPNET_ETHERTYPE))
    s.bind((iface, SNAPNET_ETHERTYPE))
    return s, s.getsockname()[4]


def _eth_frame(src_mac, dst_mac, payload):
    if len(payload) < 46:
        payload = payload + b"\x00" * (46 - len(payload))
    return dst_mac + src_mac + struct.pack("!H", SNAPNET_ETHERTYPE) + payload


def build_manifest(blob: bytes) -> tuple:
    """Scan `blob` (must be exactly STORE_LEN bytes) in CHUNK-byte pieces,
    returning (manifest_bitmap, {seq: bytes}) for every non-zero chunk.
    `piece != bytes(len(piece))` is a fast (C-level) all-zero check —
    `any(piece)` would iterate byte-by-byte in pure Python, far too slow
    for ~767K chunks."""
    manifest = bytearray(MANIFEST_BYTES)
    chunks = {}
    for seq in range(TOTAL_CHUNKS):
        off, end = chunk_range(seq)
        piece = blob[off:end]
        if piece != bytes(len(piece)):
            bit_set(manifest, seq)
            chunks[seq] = piece
    return bytes(manifest), chunks


# ------------------------------------------------------------------ #
# PULL: board (snapshot_net_send()) -> host                            #
# ------------------------------------------------------------------ #
class _BulkReader:
    """Background thread draining the bulk (0x88B8) socket into `received`
    (seq -> bytes) while the main thread drives the netcon control-plane
    handshake concurrently on its OWN socket/ethertype — the two channels
    never contend for the same fd."""

    def __init__(self, sock):
        self.sock = sock
        self.received = {}
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=2.0)

    def _run(self):
        while not self._stop.is_set():
            self.sock.settimeout(BULK_READ_TIMEOUT_S)
            try:
                frame = self.sock.recv(2048)
            except (socket.timeout, OSError):
                continue
            if len(frame) < 14 + HDR_LEN:
                continue
            if struct.unpack("!H", frame[12:14])[0] != SNAPNET_ETHERTYPE:
                continue
            payload = frame[14:]
            magic, seq, length, crc = unpack_hdr(payload)
            if magic != MAGIC or seq >= TOTAL_CHUNKS:
                continue
            data = payload[HDR_LEN:HDR_LEN + length]
            if len(data) < length:
                continue          # truncated: drop, will show up as missing
            if _crc32(data) != crc:
                continue          # corrupt: drop, will show up as missing
            self.received[seq] = bytes(data)


def _cli_pull(args):
    if len(args) < 1:
        print("usage: snapshot_net.py pull <outfile> [iface] [manifest_timeout_s]")
        return 2
    outfile = args[0]
    iface = args[1] if len(args) > 1 else IFACE
    manifest_timeout = float(args[2]) if len(args) > 2 else MANIFEST_TIMEOUT_S

    bulk_sock, _ = open_bulk_socket(iface)
    reader = _BulkReader(bulk_sock)
    reader.start()

    nc_sock, _ = netcon.open_netcon_socket(iface)
    print(f"[snapnet] listening on {iface} for the board's bulk pass + "
          f"manifest -- now trigger snapshot_net_send() on the board")

    manifest = netcon.recv_blob(nc_sock, timeout=manifest_timeout, max_len=MANIFEST_BYTES + 16)
    if len(manifest) != MANIFEST_BYTES:
        print(f"[snapnet] FAILED: never received a valid {MANIFEST_BYTES}-byte "
              f"manifest over netcon (got {len(manifest)} bytes) -- board "
              f"likely never reached the manifest step, or the link is down")
        reader.stop()
        bulk_sock.close()
        nc_sock.close()
        return 1

    time.sleep(SETTLE_S)
    expected = {seq for seq in range(TOTAL_CHUNKS) if bit_test(manifest, seq)}
    print(f"[snapnet] manifest received: {len(expected)}/{TOTAL_CHUNKS} chunks "
          f"expected ({len(expected) * CHUNK / (1024 * 1024):.1f} MiB non-zero)")

    rounds = 0
    while rounds < MAX_ROUNDS:
        missing = expected - set(reader.received.keys())
        if not missing:
            break
        subset = sorted(missing)[:MISSING_CAP]
        print(f"[snapnet] round {rounds}: requesting {len(subset)} of "
              f"{len(missing)} missing chunk(s)")
        entries = subset + [SENTINEL] * (MISSING_CAP - len(subset))
        req_blob = b"".join(struct.pack("<I", e) for e in entries)
        # Board's netcon_recv() pre-declares this EXACT length (see
        # snapshot_net.c "FIXED-SIZE CONTROL MESSAGES") -- req_blob is
        # always exactly MISSING_CAP*4 bytes, sentinel-padded.
        if not netcon.send_blob(nc_sock, req_blob, dst_mac=BOARD_MAC, verbose=False):
            print("[snapnet] missing-list delivery failed (board not "
                  "responding over netcon) -- giving up on further rounds")
            break
        time.sleep(SETTLE_S)
        rounds += 1

    reader.stop()
    bulk_sock.close()
    nc_sock.close()

    received = reader.received
    out = bytearray(STORE_LEN)
    for seq, data in received.items():
        off, end = chunk_range(seq)
        out[off:end] = data
    with open(outfile, "wb") as f:
        f.write(out)

    still_missing = expected - set(received.keys())
    print(f"[snapnet] wrote {STORE_LEN} bytes to {outfile} "
          f"({len(received)}/{len(expected)} expected chunks present)")
    if still_missing:
        print(f"[snapnet] INCOMPLETE: {len(still_missing)} chunk(s) never "
              f"arrived after {rounds} retry round(s) -- {outfile} is "
              f"CORRUPT/PARTIAL. Do not push it back; re-run pull.")
        return 1
    print("[snapnet] pull OK (all expected chunks accounted for)")
    return 0


# ------------------------------------------------------------------ #
# PUSH: host -> board (snapshot_net_recv())                            #
# ------------------------------------------------------------------ #
def _cli_push(args):
    if len(args) < 1:
        print("usage: snapshot_net.py push <infile> [iface]")
        return 2
    infile = args[0]
    iface = args[1] if len(args) > 1 else IFACE

    with open(infile, "rb") as f:
        blob = f.read()
    if len(blob) != STORE_LEN:
        print(f"[snapnet] FAILED: {infile} is {len(blob)} bytes, expected "
              f"exactly {STORE_LEN} (must be a file produced by this "
              f"script's own `pull`)")
        return 1

    manifest, chunks = build_manifest(blob)
    print(f"[snapnet] {infile}: {len(chunks)}/{TOTAL_CHUNKS} non-zero chunks "
          f"({len(chunks) * CHUNK / (1024 * 1024):.1f} MiB) to send")

    bulk_sock, src = open_bulk_socket(iface)
    nc_sock, _ = netcon.open_netcon_socket(iface)

    def send_chunk(seq):
        data = chunks[seq]
        hdr = pack_hdr(seq, len(data), _crc32(data))
        bulk_sock.send(_eth_frame(src, BOARD_MAC, hdr + data))

    print("[snapnet] streaming bulk pass -- make sure snapshot_net_recv() "
          "is already running (or about to run) on the board")
    for i, seq in enumerate(sorted(chunks.keys())):
        send_chunk(seq)
        if i % 64 == 0:
            time.sleep(0.001)   # light pacing so the board's RX ring can drain

    print(f"[snapnet] bulk pass done ({len(chunks)} chunks); delivering "
          f"manifest over netcon")
    if not netcon.send_blob(nc_sock, manifest, dst_mac=BOARD_MAC, verbose=False):
        print("[snapnet] FAILED: manifest was never ACKed over netcon -- "
              "board not listening / link down")
        bulk_sock.close()
        nc_sock.close()
        return 1

    rounds = 0
    while rounds < MAX_ROUNDS:
        req = netcon.recv_blob(nc_sock, timeout=ROUND_TIMEOUT_S, max_len=MISSING_CAP * 4 + 16)
        if len(req) < 4:
            break   # board asked for nothing more within the timeout
        count = len(req) // 4
        seqs = struct.unpack(f"<{count}I", req[:count * 4])
        resent = 0
        for s in seqs:
            if s == SENTINEL:
                break
            if s in chunks:
                send_chunk(s)
                resent += 1
        print(f"[snapnet] round {rounds}: board requested a resend, "
              f"{resent} chunk(s) sent")
        rounds += 1
        if resent == 0:
            break

    bulk_sock.close()
    nc_sock.close()
    print("[snapnet] push complete (best-effort) -- the board's own console "
          "reports the definitive restore result (snapshot_net_recv()'s "
          "return value); this script cannot see that from here")
    return 0


# ------------------------------------------------------------------ #
# selftest                                                              #
# ------------------------------------------------------------------ #
def _cli_selftest(_args):
    hdr = pack_hdr(12345, 999, 0xDEADBEEF)
    magic, seq, length, crc = unpack_hdr(hdr)
    assert (magic, seq, length, crc) == (MAGIC, 12345, 999, 0xDEADBEEF), \
        f"header round-trip mismatch: {(magic, seq, length, crc)}"
    assert HDR_LEN == 16

    bm = bytearray(4)
    bit_set(bm, 0)
    bit_set(bm, 9)
    bit_set(bm, 31)
    assert bit_test(bm, 0) and bit_test(bm, 9) and bit_test(bm, 31)
    assert not bit_test(bm, 1) and not bit_test(bm, 8)

    assert _crc32(b"123456789") == 0xCBF43926

    off, end = chunk_range(TOTAL_CHUNKS - 1)
    assert end == STORE_LEN, f"last chunk must end exactly at STORE_LEN: {end} != {STORE_LEN}"

    print(f"[snapnet] selftest OK (header round-trip, bitmap helpers, crc32 "
          f"vector, STORE_LEN={STORE_LEN} TOTAL_CHUNKS={TOTAL_CHUNKS} "
          f"MANIFEST_BYTES={MANIFEST_BYTES})")
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    if cmd == "pull":
        return _cli_pull(rest)
    if cmd == "push":
        return _cli_push(rest)
    if cmd == "selftest":
        return _cli_selftest(rest)
    print(f"unknown command: {cmd}")
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
