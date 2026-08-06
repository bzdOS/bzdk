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
import re
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


def chunk_range(seq: int, store_len: int = STORE_LEN):
    """Byte range of chunk `seq`. `store_len` exists ONLY so the offline
    selftest can exercise the same arithmetic over a tiny synthetic store
    instead of a 1 GiB one; it defaults to the real STORE_LEN, so every
    production call site (`_cli_pull`/`_cli_push`/`build_manifest`) behaves
    exactly as before this parameter was added."""
    off = seq * CHUNK
    end = min(off + CHUNK, store_len)
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


def build_manifest(blob: bytes, store_len: int = STORE_LEN,
                   total_chunks: int = TOTAL_CHUNKS,
                   manifest_bytes: int = MANIFEST_BYTES) -> tuple:
    """Scan `blob` (must be exactly `store_len` bytes) in CHUNK-byte pieces,
    returning (manifest_bitmap, {seq: bytes}) for every non-zero chunk.
    `piece != bytes(len(piece))` is a fast (C-level) all-zero check —
    `any(piece)` would iterate byte-by-byte in pure Python, far too slow
    for ~767K chunks.

    The three geometry parameters exist ONLY so the offline selftest can run
    this over a tiny synthetic store (a 1 GiB allocation in a unit test is
    not acceptable). They default to the real constants, so `_cli_push`'s
    single call site is unchanged in behaviour."""
    manifest = bytearray(manifest_bytes)
    chunks = {}
    for seq in range(total_chunks):
        off, end = chunk_range(seq, store_len)
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
            self.parse_frame(frame)

    def parse_frame(self, frame: bytes, store_len: int = STORE_LEN,
                     total_chunks: int = TOTAL_CHUNKS) -> bool:
        """Validate ONE raw Ethernet frame and, if it is good, record its
        chunk in self.received. Returns True if accepted, False if dropped.

        Split out of _run() (which used to inline this body verbatim) purely
        so the offline selftest can drive every accept/drop path with no
        socket at all. The checks, their order, and the wire format are
        byte-for-byte what _run() did before: nothing here knows about the
        socket, and _run() now does nothing but read bytes and call this.

        `store_len`/`total_chunks` exist ONLY so the offline selftest can
        drive this over the tiny synthetic store instead of the real 1 GiB
        one (same reasoning/pattern as chunk_range()/build_manifest()); every
        production call site (_run(), via the bare `self.parse_frame(frame)`
        in the socket-reading loop) uses the real STORE_LEN/TOTAL_CHUNKS
        defaults unchanged.

        FIXED (was the open issue `open_issue_host_trusts_frame_length`):
        this now cross-checks the frame's declared `length` against the
        length its `seq` implies -- mirroring snapshot_net.c's own
        `if (plen != expect_len) return;` (snapshot_net.c:304) -- and drops
        any mismatch before it can reach _cli_pull's
            out[off:end] = data
        reassembly step, where a length that disagrees with `end - off`
        does not partially write, it RESIZES the bytearray (shifting every
        later byte and silently corrupting the whole pulled image). The
        order mirrors the board's own validation ladder: bounds/truncation
        check first (so an out-of-range seq is never used to index
        chunk_range()), then the seq-range check, then the length-vs-seq
        check, then the payload CRC last."""
        if len(frame) < 14 + HDR_LEN:
            return False
        if struct.unpack("!H", frame[12:14])[0] != SNAPNET_ETHERTYPE:
            return False
        payload = frame[14:]
        magic, seq, length, crc = unpack_hdr(payload)
        if magic != MAGIC:
            return False
        available = len(payload) - HDR_LEN
        if length > available:
            return False          # truncated: drop, will show up as missing
        if seq >= total_chunks:
            return False          # bogus chunk index: drop
        off, end = chunk_range(seq, store_len)
        expect_len = end - off
        if length != expect_len:
            return False          # wrong length for this index: drop (mirrors
                                   # snapshot_net.c's own expect_len check)
        data = payload[HDR_LEN:HDR_LEN + length]
        if _crc32(data) != crc:
            return False          # corrupt: drop, will show up as missing
        self.received[seq] = bytes(data)
        return True


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
#                                                                       #
# STRICTLY OFFLINE: no AF_PACKET socket is ever opened, no board is      #
# touched, and nothing allocates anything near STORE_LEN (1 GiB). The    #
# geometry-dependent cases run over a tiny SYNTHETIC store that has the  #
# same SHAPE as the real one (N-1 full CHUNK-byte pieces plus one short  #
# tail), which is why chunk_range()/build_manifest() take optional       #
# geometry parameters — see their docstrings.                            #
#                                                                       #
# Companion: test_snapshot_fmt.c covers the BOARD side of the same wire  #
# format (snapshot_net_rx_frame()'s validation ladder, the store-header  #
# ABI, and the two C CRC32 implementations). This file covers the HOST   #
# side and the C-vs-Python constant agreement between the two.           #
# ------------------------------------------------------------------ #

# Synthetic store used by the geometry/round-trip cases: 4 full chunks plus a
# 360-byte tail, deliberately the SAME tail length as the real store's last
# chunk (STORE_LEN - (TOTAL_CHUNKS-1)*CHUNK == 360), so the short-last-chunk
# path is exercised for real rather than approximated.
SYN_CHUNKS = 5
SYN_STORE_LEN = (SYN_CHUNKS - 1) * CHUNK + 360
SYN_MANIFEST_BYTES = (SYN_CHUNKS + 7) // 8

_HERE = os.path.dirname(os.path.abspath(__file__))

# `#define NAME expr` where NAME starts with a prefix we care about. Requires
# whitespace after NAME, so function-like macros (`#define FOO(x) ...`) never
# match — none of the constants below are function-like.
_C_DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+(\S.*)$")
# Integer literal with C suffixes: 0x40000000UL, 1400u, 8192u, ...
_C_SUFFIX_RE = re.compile(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b")
# Whitelist for the expressions we are willing to evaluate.
_C_SAFE_RE = re.compile(r"^[0-9A-Za-z_\s()+\-*/]+$")


def _parse_c_defines(filename, prefixes):
    """Scrape `#define <PREFIX...>_NAME <expr>` out of one C source/header.
    Returns {name: expr_string} with comments stripped. Deliberately dumb:
    it does not preprocess, so a constant hidden behind #if would be missed
    (none of the ones checked here are)."""
    out = {}
    with open(os.path.join(_HERE, filename), "r", encoding="utf-8") as f:
        for line in f:
            m = _C_DEFINE_RE.match(line)
            if not m:
                continue
            name, expr = m.group(1), m.group(2)
            if not any(name.startswith(p) for p in prefixes):
                continue
            expr = re.sub(r"/\*.*?\*/", " ", expr)
            expr = re.sub(r"/\*.*$", " ", expr)      # comment opens, runs on
            expr = re.sub(r"//.*$", " ", expr).strip()
            if not expr or expr.endswith("\\"):
                continue                              # multi-line macro: skip
            out.setdefault(name, expr)
    return out


def _eval_c_expr(expr, env):
    """Evaluate a simple C integer-constant expression. Strips u/U/l/L
    suffixes and turns C's `/` (integer division on unsigned operands, which
    is all these are) into Python's `//`."""
    e = _C_SUFFIX_RE.sub(r"\1", expr)
    e = e.replace("/", "//")
    if not _C_SAFE_RE.match(e):
        raise ValueError(f"refusing to evaluate {expr!r}")
    return eval(e, {"__builtins__": {}}, dict(env))   # noqa: S307 (whitelisted)


def _resolve_c_defines(raw):
    """Resolve a {name: expr} map into {name: int}, iterating so a macro
    defined in terms of another (SNAPNET_TOTAL_CHUNKS -> SNAPNET_STORE_LEN ->
    SNAP_META_SIZE + SNAP_DRAM_SIZE) resolves regardless of file order.
    Returns (resolved, unresolved_names)."""
    env, pending = {}, dict(raw)
    for _ in range(len(raw) + 2):
        if not pending:
            break
        progressed = False
        for name in list(pending):
            try:
                env[name] = _eval_c_expr(pending[name], env)
            except Exception:
                continue
            del pending[name]
            progressed = True
        if not progressed:
            break
    return env, sorted(pending)


def _st_header_roundtrip():
    """PRE-EXISTING case, unchanged: the 16-byte bulk header packs and
    unpacks losslessly."""
    hdr = pack_hdr(12345, 999, 0xDEADBEEF)
    magic, seq, length, crc = unpack_hdr(hdr)
    assert (magic, seq, length, crc) == (MAGIC, 12345, 999, 0xDEADBEEF), \
        f"header round-trip mismatch: {(magic, seq, length, crc)}"
    assert HDR_LEN == 16
    return "pack/unpack lossless, HDR_LEN=16"


def _st_bitmap_helpers():
    """PRE-EXISTING case, unchanged, plus the byte-level LSB-first check the
    original left implicit (the board's bit_set() at snapshot_net.c:124-125 is
    LSB-first within each byte; a transposed convention here would silently
    scramble every manifest)."""
    bm = bytearray(4)
    bit_set(bm, 0)
    bit_set(bm, 9)
    bit_set(bm, 31)
    assert bit_test(bm, 0) and bit_test(bm, 9) and bit_test(bm, 31)
    assert not bit_test(bm, 1) and not bit_test(bm, 8)
    # NEW: pin the byte image, not just the predicate.
    assert bytes(bm) == b"\x01\x02\x00\x80", bytes(bm).hex()
    return "bits 0/9/31 set, LSB-first byte image 01020080"


def _st_crc32_vector():
    """PRE-EXISTING case, unchanged, plus the empty-input identity. Both C
    implementations are checked against the same vector in
    test_snapshot_fmt.c."""
    assert _crc32(b"123456789") == 0xCBF43926
    assert _crc32(b"") == 0
    return "CRC32('123456789')==0xCBF43926, CRC32('')==0"


def _st_last_chunk_geometry():
    """PRE-EXISTING case, unchanged, plus the explicit 360-byte tail length
    and the ceiling-divide boundary."""
    off, end = chunk_range(TOTAL_CHUNKS - 1)
    assert end == STORE_LEN, f"last chunk must end exactly at STORE_LEN: {end} != {STORE_LEN}"
    # NEW: the tail is 360 bytes, not CHUNK, and one chunk fewer would not
    # cover the store.
    assert end - off == 360, end - off
    assert (TOTAL_CHUNKS - 1) * CHUNK < STORE_LEN <= TOTAL_CHUNKS * CHUNK
    assert MANIFEST_BYTES * 8 >= TOTAL_CHUNKS
    return (f"last chunk = [{off},{end}) = {end - off} B, ends exactly at "
            f"STORE_LEN={STORE_LEN}")


def _st_c_constant_drift():
    """NEW. snapshot_net.h/.c and this module each hardcode the same numbers,
    and NOTHING checked they agree — a drift silently corrupts every transfer
    (a CHUNK mismatch misaligns the whole image; a TOTAL_CHUNKS mismatch makes
    one side reject the other's last frame; a MISSING_CAP mismatch makes the
    board's fixed-length netcon_recv() drop every missing-list message).

    HONESTY, what is verified MECHANICALLY here: every constant listed in
    `pairs` below is scraped from the real C source and compared to this
    module's own copy, including the ones the C side computes as expressions
    (SNAPNET_STORE_LEN / _TOTAL_CHUNKS / _MANIFEST_BYTES are re-derived from
    the scraped SNAP_META_SIZE / SNAP_DRAM_SIZE / SNAPNET_CHUNK, so the
    comparison is against the C arithmetic, not against a literal).

    What is NOT verified mechanically, and why:
      - The timing/poll budgets. snapshot_net.c:32-40 counts POLL ITERATIONS
        (SNAPNET_BULK_IDLE_POLLS=500000, SNAPNET_ROUND_TIMEOUT_POLLS=2000000);
        this module counts SECONDS (MANIFEST_TIMEOUT_S / ROUND_TIMEOUT_S /
        SETTLE_S / BULK_READ_TIMEOUT_S). There is no calibration between the
        two anywhere in the tree, so no assertion is possible — only a live
        run can say whether the two ends' patience actually overlaps.
      - BOARD_MAC (imported from netcon.py) against emac.c's MAC. Not scraped;
        emac.c is a large file with several MAC-shaped constants and picking
        the right one by regex would be a guess.
      - Nothing checks that the SIZE of a struct snapshot_hdr matches what
        this module assumes, because this module makes NO assumption: it
        treats chunk 0 as opaque bytes and never decodes the store header at
        all (so `pull` cannot tell a valid snapshot from garbage, and `push`
        cannot validate what it is about to restore — a separate gap, noted
        in the report, not a drift).
      - SNAP_MAGIC / SNAP_DRAM_BASE / SNAP_STORE_BASE / SNAPSHOT_VERSION exist
        only on the C side; they are reported below as unmirrored rather than
        compared."""
    c = {}
    c.update(_parse_c_defines("snapshot.h", ("SNAP_", "SNAPSHOT_")))
    c.update(_parse_c_defines("snapshot_net.h", ("SNAPNET_",)))
    c.update(_parse_c_defines("snapshot_net.c", ("SNAPNET_",)))
    c.update(_parse_c_defines("netcon.h", ("NETCON_",)))
    c.update(_parse_c_defines("emac.c", ("ETHERTYPE_",)))

    env, unresolved = _resolve_c_defines(c)

    pairs = [
        ("SNAP_META_SIZE",        SNAP_META_SIZE),
        ("SNAP_DRAM_SIZE",        SNAP_DRAM_SIZE),
        ("SNAPNET_STORE_LEN",     STORE_LEN),
        ("SNAPNET_CHUNK",         CHUNK),
        ("SNAPNET_TOTAL_CHUNKS",  TOTAL_CHUNKS),
        ("SNAPNET_MANIFEST_BYTES", MANIFEST_BYTES),
        ("SNAPNET_MAX_ROUNDS",    MAX_ROUNDS),
        ("SNAPNET_MISSING_CAP",   MISSING_CAP),
        ("SNAPNET_SENTINEL",      SENTINEL),
        ("SNAPNET_MAGIC",         MAGIC),
        ("SNAPNET_HDR_LEN",       HDR_LEN),
        ("SNAPNET_ETHERTYPE",     SNAPNET_ETHERTYPE),
        # emac.c's own copy of the ethertype: the dispatch that actually
        # routes a frame to snapshot_net_rx_frame() (emac.c:192/1401).
        ("ETHERTYPE_SNAPNET",     SNAPNET_ETHERTYPE),
    ]
    for name, py_val in pairs:
        assert name in env, (f"could not scrape {name} out of the C sources "
                             f"(unresolved: {unresolved})")
        assert env[name] == py_val, \
            f"C-vs-Python DRIFT: {name}={env[name]} in C, {py_val} in snapshot_net.py"

    # The design rationale at snapshot_net.h:47-49 and :137-141 claims both
    # control messages fit inside netcon's existing 2 MiB cap "so netcon.h/.c
    # are NOT modified at all". That claim is load-bearing and mechanically
    # checkable on both sides.
    assert "NETCON_MAX_LEN" in env
    assert env["NETCON_MAX_LEN"] == netcon.MAX_LEN, \
        f"netcon.h NETCON_MAX_LEN={env['NETCON_MAX_LEN']} vs netcon.py MAX_LEN={netcon.MAX_LEN}"
    assert MANIFEST_BYTES <= env["NETCON_MAX_LEN"]
    assert MISSING_CAP * 4 <= env["NETCON_MAX_LEN"]

    # The three ethertypes must stay distinct or the emac dispatch collides.
    assert env["ETHERTYPE_SNAPNET"] != env["ETHERTYPE_NETCON"] != env["ETHERTYPE_CONSOLE"]
    assert env["ETHERTYPE_NETCON"] == netcon.NC_ETHERTYPE

    unmirrored = [n for n in ("SNAP_MAGIC", "SNAP_DRAM_BASE", "SNAP_STORE_BASE",
                              "SNAPSHOT_VERSION", "SNAP_DRAM_STORE")
                  if n in env]
    return (f"{len(pairs)} constants agree with the C sources; "
            f"netcon cap {env['NETCON_MAX_LEN']} >= manifest {MANIFEST_BYTES} "
            f"and missing-list {MISSING_CAP * 4}; "
            f"C-only (no Python mirror, not compared): {','.join(unmirrored)}")


def _st_build_manifest_zero_skip():
    """NEW. build_manifest() is the host twin of the board's chunk_is_zero()
    (snapshot_net.c:198-209): an all-zero chunk is never marked and never put
    on the wire, because the receiver pre-zeroes its destination. A false
    positive here silently drops real data with no error anywhere."""
    blob = bytearray(SYN_STORE_LEN)
    # chunk 0: fully non-zero
    blob[0:CHUNK] = bytes((i % 251) + 1 for i in range(CHUNK))
    # chunk 1: all zero  -> must NOT be marked
    # chunk 2: exactly ONE non-zero byte, at the very last position -> marked
    blob[3 * CHUNK - 1] = 0x01
    # chunk 3: all zero  -> must NOT be marked
    # chunk 4 (the short 360-byte tail): one non-zero byte at position 0
    blob[4 * CHUNK] = 0xFF

    manifest, chunks = build_manifest(bytes(blob), SYN_STORE_LEN,
                                      SYN_CHUNKS, SYN_MANIFEST_BYTES)

    marked = {s for s in range(SYN_CHUNKS) if bit_test(manifest, s)}
    assert marked == {0, 2, 4}, marked
    assert set(chunks.keys()) == marked
    assert len(manifest) == SYN_MANIFEST_BYTES
    # Bit indices line up with the byte image, LSB-first: 0b00010101 = 0x15.
    assert manifest == b"\x15", manifest.hex()
    # Lengths: full chunks are CHUNK, the tail is 360.
    assert len(chunks[0]) == CHUNK and len(chunks[2]) == CHUNK
    assert len(chunks[4]) == 360, len(chunks[4])
    # And the marked pieces really are the store's bytes at those offsets.
    for seq in marked:
        off, end = chunk_range(seq, SYN_STORE_LEN)
        assert chunks[seq] == bytes(blob[off:end])

    # A wholly-zero store marks nothing at all (the degenerate zero-skip).
    m0, c0 = build_manifest(bytes(SYN_STORE_LEN), SYN_STORE_LEN,
                            SYN_CHUNKS, SYN_MANIFEST_BYTES)
    assert m0 == b"\x00" and c0 == {}
    return "marked={0,2,4} (bitmap 0x15), zero chunks skipped, tail=360 B"


def _st_bulk_reader_parse_paths():
    """NEW. Drive _BulkReader.parse_frame() over every accept/drop path with
    no socket: a good frame, a runt, a wrong ethertype, a bad magic, an
    out-of-range seq, a bad payload CRC, and a truncated payload. This is the
    host counterpart of test_snapshot_fmt.c's rx_* tests.

    _BulkReader is constructed with sock=None: parse_frame() never touches
    self.sock, and the reader thread is created but never started, so nothing
    can block or open a descriptor."""
    reader = _BulkReader(None)
    src = b"\x02\xbd\x05\x00\x00\x02"

    def frame_for(seq, data, magic=MAGIC, crc=None, length=None, ethertype=True):
        hdr = struct.pack(HDR_FMT, magic, seq,
                          len(data) if length is None else length,
                          0, _crc32(data) if crc is None else crc)
        f = _eth_frame(src, BOARD_MAC, hdr + data)
        if not ethertype:
            f = f[:12] + struct.pack("!H", 0x88B6) + f[14:]
        return f

    good = bytes((i * 7) % 256 for i in range(CHUNK))

    # (1) good frame accepted
    assert reader.parse_frame(frame_for(0, good)) is True
    assert reader.received[0] == good
    # (2) runt frame (shorter than 14 + HDR_LEN)
    assert reader.parse_frame(b"\x00" * (14 + HDR_LEN - 1)) is False
    # (3) wrong ethertype
    assert reader.parse_frame(frame_for(1, good, ethertype=False)) is False
    # (4) bad magic
    assert reader.parse_frame(frame_for(1, good, magic=MAGIC ^ 1)) is False
    # (5) seq out of range
    assert reader.parse_frame(frame_for(TOTAL_CHUNKS, good)) is False
    assert reader.parse_frame(frame_for(0xFFFFFFFF, good)) is False
    # (6) bad payload CRC (both a wrong crc field and a flipped payload bit)
    assert reader.parse_frame(frame_for(1, good, crc=0xDEADBEEF)) is False
    bad = bytearray(good)
    bad[700] ^= 0x01
    assert reader.parse_frame(frame_for(1, bytes(bad), crc=_crc32(good))) is False
    # (7) truncated payload: header claims CHUNK, only 100 bytes follow
    assert reader.parse_frame(frame_for(1, good[:100], length=CHUNK,
                                        crc=_crc32(good))) is False

    # Every rejection left the reader untouched: only chunk 0 is present.
    assert set(reader.received.keys()) == {0}, set(reader.received.keys())
    assert reader.received[0] == good

    # The short 360-byte tail chunk is accepted at its own length.
    tail = bytes(range(256)) + bytes(104)
    assert len(tail) == 360
    assert reader.parse_frame(frame_for(TOTAL_CHUNKS - 1, tail)) is True
    assert reader.received[TOTAL_CHUNKS - 1] == tail
    return "1 accept + 7 distinct drop paths, plus the 360-B tail chunk"


def _st_missing_list_message_shape():
    """NEW. The retry-request message the two ends exchange must be EXACTLY
    MISSING_CAP*4 bytes, sentinel-padded, because the board's netcon_recv()
    pre-declares that exact length and netcon_rx_frame() drops any transfer
    whose total_len differs (snapshot_net.c:46-59). Built here with the same
    expression _cli_pull uses, so a change there breaks this."""
    subset = [0, 1, 7, 8, 4200, TOTAL_CHUNKS - 1]
    entries = subset + [SENTINEL] * (MISSING_CAP - len(subset))
    req_blob = b"".join(struct.pack("<I", e) for e in entries)

    assert len(req_blob) == MISSING_CAP * 4 == 32768, len(req_blob)
    # Board side (snapshot_net.c:396-404): read until the first sentinel.
    vals = struct.unpack(f"<{MISSING_CAP}I", req_blob)
    stop = vals.index(SENTINEL)
    assert list(vals[:stop]) == subset, vals[:stop]
    assert all(v == SENTINEL for v in vals[stop:])
    # The sentinel can never be mistaken for a real chunk index.
    assert SENTINEL >= TOTAL_CHUNKS
    # DOCUMENTED BOUND, not a bug: MAX_ROUNDS rounds of MISSING_CAP entries
    # cannot cover a whole manifest's worth of loss, so a transfer losing more
    # than MAX_ROUNDS*MISSING_CAP chunks fails by design (board returns -3).
    assert MAX_ROUNDS * MISSING_CAP < TOTAL_CHUNKS
    return (f"{MISSING_CAP * 4} B fixed message, {len(subset)} entries then "
            f"0xFFFFFFFF padding; retry ceiling = {MAX_ROUNDS * MISSING_CAP} chunks")


def _st_offline_round_trip():
    """NEW. A genuine push->loss->missing-list->resend->pull round trip driven
    entirely through the module's OWN pure functions, with zero sockets: the
    real build_manifest(), the real pack_hdr()/_eth_frame() framing, the real
    _BulkReader.parse_frame(), the real _cli_pull missing-list construction
    and the real _cli_push sentinel-terminated resend loop, over the synthetic
    5-chunk store. Ends by asserting the reassembled image is byte-identical
    to the original.

    What this does NOT do: no socket, so nothing about AF_PACKET framing,
    netcon's ACK handshake, timing, thread interleaving or real loss is
    covered. It proves the pure logic composes correctly, not that a transfer
    works."""
    blob = bytearray(SYN_STORE_LEN)
    blob[0:CHUNK] = bytes((i % 251) + 1 for i in range(CHUNK))       # chunk 0
    # chunk 1 stays all zero (zero-skipped, must reconstruct as zero)
    blob[2 * CHUNK:3 * CHUNK] = bytes((i % 97) + 3 for i in range(CHUNK))
    # chunk 3 stays all zero
    blob[4 * CHUNK:SYN_STORE_LEN] = bytes((i % 89) + 5 for i in range(360))
    blob = bytes(blob)

    manifest, chunks = build_manifest(blob, SYN_STORE_LEN, SYN_CHUNKS,
                                      SYN_MANIFEST_BYTES)
    expected = {s for s in range(SYN_CHUNKS) if bit_test(manifest, s)}
    assert expected == {0, 2, 4} == set(chunks.keys())

    reader = _BulkReader(None)
    src = b"\x02\xbd\x05\x00\x00\x02"

    def send(seq):
        data = chunks[seq]
        hdr = pack_hdr(seq, len(data), _crc32(data))     # same as _cli_push
        # Synthetic geometry, explicitly: parse_frame()'s length-vs-seq check
        # (see its own docstring) needs to know THIS store's shape, not the
        # real 1 GiB one, or it would reject chunk 4 (the real 360-B tail
        # length only matches seq TOTAL_CHUNKS-1 under the real geometry).
        return reader.parse_frame(_eth_frame(src, BOARD_MAC, hdr + data),
                                   SYN_STORE_LEN, SYN_CHUNKS)

    # Bulk pass with chunk 2 "lost" on the wire.
    assert send(0) is True
    assert send(4) is True
    missing = expected - set(reader.received.keys())
    assert missing == {2}, missing

    # --- host->board retry request, exactly as _cli_pull builds it ---
    subset = sorted(missing)[:MISSING_CAP]
    entries = subset + [SENTINEL] * (MISSING_CAP - len(subset))
    req_blob = b"".join(struct.pack("<I", e) for e in entries)
    assert len(req_blob) == MISSING_CAP * 4

    # --- sender side, exactly as _cli_push's round loop reads it ---
    count = len(req_blob) // 4
    seqs = struct.unpack(f"<{count}I", req_blob[:count * 4])
    resent = 0
    for s in seqs:
        if s == SENTINEL:
            break
        if s in chunks:
            assert send(s) is True
            resent += 1
    assert resent == 1

    assert expected - set(reader.received.keys()) == set()

    # --- reassembly, exactly as _cli_pull writes the output file ---
    out = bytearray(SYN_STORE_LEN)
    for seq, data in reader.received.items():
        off, end = chunk_range(seq, SYN_STORE_LEN)
        out[off:end] = data
    assert len(out) == SYN_STORE_LEN
    assert bytes(out) == blob, "round trip did not reproduce the store"
    # The two zero-skipped chunks were never on the wire and reconstructed
    # from the pre-zeroed destination alone.
    assert 1 not in reader.received and 3 not in reader.received
    assert out[CHUNK:2 * CHUNK] == bytes(CHUNK)
    return ("5-chunk store: 2 zero-skipped, 1 lost + resent, reassembled "
            "byte-identical")


def _st_frame_length_mismatch_is_rejected():
    """FIXED (was `open_issue_host_trusts_frame_length`, PINNED-NOT-FIXED).

    The board's RX path checks that a frame's `len` equals the length its
    `seq` implies (snapshot_net.c:301-305: expect_len = min(CHUNK,
    STORE_LEN-off), then `if (plen != expect_len) return;`). _BulkReader had
    NO such check: it accepted any length whose CRC happened to match. That
    mattered because _cli_pull then reassembles with

        out[off:end] = data

    on a bytearray. If len(data) != end-off, that is not a partial write — it
    RESIZES the bytearray, shifting every byte after `off` and changing the
    output file's length, so one wrong-length frame used to silently corrupt
    the entire pulled image.

    parse_frame() now cross-checks length-vs-seq (see its docstring) and
    rejects the mismatch outright, before self.received is ever touched —
    this test pins the FIXED behaviour: the short frame is dropped, never
    reaches reassembly, and a correctly-sized frame for the same chunk still
    works normally right after."""
    reader = _BulkReader(None)
    src = b"\x02\xbd\x05\x00\x00\x02"
    short = bytes((i % 13) + 1 for i in range(100))
    hdr = pack_hdr(0, len(short), _crc32(short))
    # Chunk 0 must be exactly CHUNK bytes; a 100-byte frame for it is now
    # rejected outright -- never reaches self.received, so the reassembly
    # bytearray-resize hazard is never reached either.
    assert reader.parse_frame(_eth_frame(src, BOARD_MAC, hdr + short)) is False
    assert 0 not in reader.received

    # A correctly-sized frame for the SAME chunk is still accepted normally.
    good = bytes((i * 3) % 256 for i in range(CHUNK))
    hdr2 = pack_hdr(0, len(good), _crc32(good))
    assert reader.parse_frame(_eth_frame(src, BOARD_MAC, hdr2 + good)) is True
    assert reader.received[0] == good

    return (f"a {len(short)}-B frame for a {CHUNK}-B chunk is now rejected "
            f"before reassembly (previously silently accepted and would "
            f"have shrunk the pulled image)")


_SELFTEST_CASES = [
    ("header_roundtrip",                   _st_header_roundtrip),
    ("bitmap_helpers",                     _st_bitmap_helpers),
    ("crc32_vector",                       _st_crc32_vector),
    ("last_chunk_geometry",                _st_last_chunk_geometry),
    ("c_constant_drift",                   _st_c_constant_drift),
    ("build_manifest_zero_skip",           _st_build_manifest_zero_skip),
    ("bulk_reader_parse_paths",            _st_bulk_reader_parse_paths),
    ("missing_list_message_shape",         _st_missing_list_message_shape),
    ("offline_round_trip",                 _st_offline_round_trip),
    ("frame_length_mismatch_is_rejected",   _st_frame_length_mismatch_is_rejected),
]


def _cli_selftest(_args):
    passed = failed = 0
    for name, fn in _SELFTEST_CASES:
        print(f"[ RUN ] {name}")
        try:
            detail = fn()
        except AssertionError as exc:
            failed += 1
            print(f"[FAIL ] {name}: {exc}")
            continue
        except Exception as exc:                      # noqa: BLE001
            failed += 1
            print(f"[ERROR] {name}: {type(exc).__name__}: {exc}")
            continue
        passed += 1
        print(f"[ OK  ] {name} -- {detail}")

    total = len(_SELFTEST_CASES)
    print(f"---- snapshot_net host selftest: {passed}/{total} passed ----")
    if failed:
        print(f"[snapnet] selftest FAILED ({failed} case(s))")
        return 1
    print(f"[snapnet] selftest OK (header round-trip, bitmap helpers, crc32 "
          f"vector, C-constant drift, zero-skip, bulk-reader parse paths, "
          f"missing-list shape, offline round trip, "
          f"STORE_LEN={STORE_LEN} TOTAL_CHUNKS={TOTAL_CHUNKS} "
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
