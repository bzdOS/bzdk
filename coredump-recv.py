#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""coredump-recv.py — host-side reassembler for the bzdOS EL2 hypervisor's
bounded ELF-core streamer, ethertype 0x88B7. Companion to coredump.c/.h on
the board (coredump_send()) and to netcon.py/repl-client.py's raw-Ethernet
style for the other two board<->host channels (0x88B6, 0x88B5).

Wire format — MUST match coredump.h's header comment EXACTLY (that comment
is the single source of truth; this parses it byte-for-byte, nothing here
is guessed):

  Ethernet: dst=broadcast, src=board MAC 02:bd:05:00:00:01, ethertype 0x88B7.
  Each frame's Ethernet PAYLOAD is a 12-byte little-endian header + data:

    off  size  field
    0    4     magic  = 0x45524F43  ("CORE", LE bytes C,O,R,E)
    4    2     seq    = 0-based frame index
    6    2     total  = total number of frames in this dump
    8    2     flags  = bit0 (0x1) set on the LAST frame
    10   2     len    = number of DATA bytes that follow the header
    12   len   data   = raw bytes of the core.elf byte stream

The concatenation of every frame's `data`, in `seq` order, IS the complete
core.elf file. Frames may be dropped or arrive out of order under load; this
script reassembles by seq into a dict, tolerates gaps, and reports precisely
which seq numbers are missing if the LAST frame arrives (or the idle timeout
fires) before every seq in [0,total) has been seen.

Unlike netcon (0x88B6), this is receive-only, unacknowledged, best-effort —
coredump_send() on the board is a bounded, non-blocking, fire-and-forget
streamer (see coredump.h: "It can never hang the fault path"). There is no
ACK/retry handshake to mirror; this script's only job is to listen, collect,
and tell you honestly whether the result is complete.

CLI:
    coredump-recv.py [outfile] [iface] [idle_timeout_s]
        Listen for one coredump stream and write it to <outfile> (default
        core.elf) once the LAST-flagged frame has arrived AND every seq in
        [0,total) has been collected, or bail out after <idle_timeout_s>
        (default 5.0) seconds of silence — writing whatever was collected
        so far and reporting the missing seq ranges, rather than hanging
        forever on a dropped final frame.
    coredump-recv.py selftest
        Offline check of the reassembly logic AND the wire-level frame parser
        against hand-crafted fake frames — no network, no hardware, no files.
        Covers: in-order, out-of-order, a dropped-then-arriving-late frame, a
        genuinely incomplete stream, corrupt/truncated drops, header
        round-trip, duplicate (identical and conflicting) chunks, frames that
        disagree about `total`, the total-is-None fallbacks, parse_l2_frame()'s
        five reject paths, agreement with coredump.c's frame-count arithmetic
        (including the exact-multiple-of-1024 extra-empty-LAST-frame quirk),
        and that an assembled ELF blob is handed back byte-for-byte.
"""
import sys
import time
import socket
import struct
import traceback

IFACE = "br0"
COREDUMP_ETHERTYPE = 0x88B7
BOARD_MAC = bytes.fromhex("02bd05000001")

COREDUMP_MAGIC = 0x45524F43   # "CORE" (LE bytes C,O,R,E)
FLAG_LAST = 0x1

# magic(u32) seq(u16) total(u16) flags(u16) len(u16) — 4+2+2+2+2 = 12 bytes,
# little-endian, exactly coredump.h's documented layout.
HDR_FMT = "<IHHHH"
HDR_LEN = struct.calcsize(HDR_FMT)
assert HDR_LEN == 12, f"coredump frame header must be 12 bytes, got {HDR_LEN}"

DEFAULT_OUTFILE = "core.elf"
DEFAULT_IDLE_TIMEOUT = 5.0

# Data bytes per frame on the sender side — coredump.h:51's COREDUMP_DATA_MAX.
# The receiver never needs this to reassemble (it trusts each frame's own `len`
# field), but the selftest uses it to check our completeness arithmetic against
# coredump.c's total-frame-count formula (coredump.c:314-316).
COREDUMP_DATA_MAX = 1024
# coredump.h:53's hard cap on streamed core bytes (384 KiB — itself an exact
# multiple of COREDUMP_DATA_MAX, so the quirk below is the *common* case for a
# dump that hits the cap).
COREDUMP_MAX_TOTAL = 384 * 1024


def unpack_hdr(buf):
    magic, seq, total, flags, length = struct.unpack(HDR_FMT, buf[:HDR_LEN])
    return dict(magic=magic, seq=seq, total=total, flags=flags, len=length)


def expected_frame_count(total_bytes):
    """Receiver-side mirror of the sender's frame-count arithmetic, verbatim
    from coredump.c:314-316:

        s.total = (total_bytes + COREDUMP_DATA_MAX - 1u) / COREDUMP_DATA_MAX;
        if (s.total == 0) s.total = 1;

    i.e. ceil(total_bytes/1024), with a floor of 1. Kept here (rather than only
    in the selftest) because it is the one piece of the sender's contract this
    script's completeness check depends on, and it is the thing that breaks
    silently if COREDUMP_DATA_MAX ever changes on the board.

    NOTE THE QUIRK it does NOT account for: when total_bytes is an exact
    multiple of COREDUMP_DATA_MAX, coredump.c streams the last full frame from
    stream_byte()'s `if (s->fill >= COREDUMP_DATA_MAX) frame_flush(s, 0)`
    (coredump.c:141-142) and then still runs the unconditional final
    `frame_flush(&s, 1)` at coredump.c:323 — emitting one EXTRA frame with
    len=0, flags=LAST and seq == total (one PAST the end of [0,total)). The
    receiver handles that correctly by construction: the empty frame lands in
    self.frames under an out-of-range seq, satisfies saw_last, contributes
    nothing to missing() (which only scans [0,total)) and nothing to assemble()
    (which only concatenates [0,total)). See the selftest case
    case_frame_count_math_matches_coredump_c().
    """
    n = (total_bytes + COREDUMP_DATA_MAX - 1) // COREDUMP_DATA_MAX
    return n if n else 1


class Reassembler:
    """Collects (seq -> data) frames for ONE coredump stream and reports
    completeness. Pure logic, no I/O — testable offline (see selftest)."""

    def __init__(self):
        self.frames = {}      # seq -> bytes
        self.total = None     # declared frame count (from any frame seen)
        self.saw_last = False

    def feed(self, hdr, data):
        """Record one parsed header + its data payload. Returns True if this
        frame completed the last-known requirement (i.e. worth checking
        is_complete() right away)."""
        if hdr["magic"] != COREDUMP_MAGIC:
            return False           # not ours / corrupt: silently drop
        if len(data) != hdr["len"]:
            return False           # truncated/malformed frame: drop
        if self.total is None:
            self.total = hdr["total"]
        # A well-formed stream reports the same `total` in every frame; if it
        # ever disagrees, trust the newest (still bounded -- a stream can't
        # un-send frames, so growing total just means "keep waiting").
        elif hdr["total"] != self.total:
            self.total = max(self.total, hdr["total"])
        self.frames[hdr["seq"]] = data
        if hdr["flags"] & FLAG_LAST:
            self.saw_last = True
        return True

    def missing(self):
        """Sorted list of seq numbers in [0,total) not yet collected. Empty
        list + saw_last True == a genuinely complete stream."""
        if self.total is None:
            return []
        return [i for i in range(self.total) if i not in self.frames]

    def is_complete(self):
        return self.saw_last and self.total is not None and not self.missing()

    def assemble(self):
        """Concatenate whatever frames were collected, in seq order. Missing
        seqs are simply skipped (NOT zero-filled) -- the caller should check
        is_complete()/missing() first and treat a non-empty missing() as an
        honestly-incomplete core, not silently pad a corrupt ELF."""
        if self.total is None:
            return b"".join(self.frames[s] for s in sorted(self.frames))
        return b"".join(self.frames[s] for s in range(self.total)
                         if s in self.frames)


def open_coredump_socket(iface=IFACE):
    """AF_PACKET raw socket bound to `iface`, filtered to the coredump
    ethertype (0x88B7) — mirrors netcon.py's open_netcon_socket() /
    repl-client.py's open_raw() for the other two raw-Ethernet channels."""
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                       socket.htons(COREDUMP_ETHERTYPE))
    s.bind((iface, COREDUMP_ETHERTYPE))
    return s


def parse_l2_frame(frame):
    """Pure byte-level parse of ONE raw L2 frame — exactly the bytes AF_PACKET
    hands back: 14-byte Ethernet header (dst6|src6|ethertype2) then the payload
    described in this file's header comment. Returns (hdr, data) for a frame we
    accept, or None for one we drop (too short / wrong ethertype / bad magic /
    truncated relative to its own `len` field).

    REFACTOR NOTE (behaviour-preserving, 2026-08): this function is _recv_one()'s
    per-frame check chain, moved out verbatim and unchanged — same five checks,
    same order, same accept/drop outcomes — so the wire parsing is testable with
    no socket at all (it was previously reachable only from a live AF_PACKET
    socket, i.e. only on the board's network). Each `continue` in the old loop
    is now a `return None` here, and _recv_one()'s loop does the `continue`
    instead, so a dropped frame still just keeps waiting out the remaining
    timeout. Nothing about the wire format or the socket setup changed."""
    if len(frame) < 14 + HDR_LEN:
        return None
    if struct.unpack("!H", frame[12:14])[0] != COREDUMP_ETHERTYPE:
        return None
    payload = frame[14:]
    hdr = unpack_hdr(payload)
    if hdr["magic"] != COREDUMP_MAGIC:
        return None
    data = payload[HDR_LEN:HDR_LEN + hdr["len"]]
    if len(data) < hdr["len"]:
        return None               # truncated: drop, nothing to retry
    return hdr, data


def _recv_one(sock, timeout):
    """Return (hdr, data) for one valid coredump frame from `sock`'s
    ethertype (0x88B7 only, already filtered by the socket binding), or None
    if nothing arrives within `timeout` seconds. Drops anything too short,
    wrong magic, or truncated (see parse_l2_frame()) and keeps waiting out the
    remaining timeout — exactly netcon.py's _recv_one() pattern, minus the
    CRC/ACK bits this fire-and-forget channel doesn't have."""
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
        got = parse_l2_frame(frame)
        if got is None:
            continue              # drop it, keep waiting out the timeout
        return got


def recv_coredump(sock, idle_timeout=DEFAULT_IDLE_TIMEOUT, verbose=True):
    """Listen until one coredump stream is fully collected (LAST frame seen
    AND every seq in [0,total) present) or `idle_timeout` seconds pass with
    no new frame. Returns a Reassembler (check .is_complete()/.missing())."""
    r = Reassembler()
    deadline = time.time() + idle_timeout
    while time.time() < deadline:
        remain = max(0.01, min(0.2, deadline - time.time()))
        got = _recv_one(sock, remain)
        if got is None:
            continue
        hdr, data = got
        r.feed(hdr, data)
        deadline = time.time() + idle_timeout   # progress: reset idle clock
        if verbose:
            print(f"[coredump] frame seq={hdr['seq']}/{hdr['total']} "
                  f"len={hdr['len']}"
                  + (" LAST" if hdr["flags"] & FLAG_LAST else "") + " " * 8,
                  end="\r")
        if r.is_complete():
            break
    if verbose:
        print()
    return r


def _cli_recv(args):
    outfile = args[0] if len(args) > 0 else DEFAULT_OUTFILE
    iface = args[1] if len(args) > 1 else IFACE
    idle_timeout = float(args[2]) if len(args) > 2 else DEFAULT_IDLE_TIMEOUT

    print(f"[coredump] listening on {iface} for ethertype "
          f"0x{COREDUMP_ETHERTYPE:04x} (idle timeout {idle_timeout}s)...")
    sock = open_coredump_socket(iface)
    try:
        r = recv_coredump(sock, idle_timeout=idle_timeout)
    finally:
        sock.close()

    blob = r.assemble()
    with open(outfile, "wb") as f:
        f.write(blob)

    missing = r.missing()
    if r.is_complete():
        print(f"[coredump] COMPLETE: {len(blob)} bytes, "
              f"{r.total} frame(s) -> {outfile}")
        print(f"[coredump] load with: aarch64-linux-gnu-gdb kernel {outfile}")
        return 0
    if r.total is None:
        print("[coredump] FAILED: no coredump frames seen at all "
              f"(timed out after {idle_timeout}s)")
        return 1
    print(f"[coredump] INCOMPLETE: wrote {len(blob)} bytes to {outfile} but "
          f"{len(missing)}/{r.total} frame(s) missing "
          f"(saw_last={r.saw_last}); missing seq: {missing[:20]}"
          + (" ..." if len(missing) > 20 else ""))
    return 1


# ====================================================================
# selftest — pure, offline. No sockets, no files, no hardware.
# ====================================================================
# Every case below is a plain function that raises AssertionError on failure;
# _cli_selftest() drives them, prints a RUN/OK trace, and exits non-zero if any
# case fails. Cases 1-6 are the original selftest body, unchanged.

CHUNKS = [b"AAAA", b"BBBB", b"CCCC"]


def _st_hdr(seq, total, flags=0, length=None, magic=COREDUMP_MAGIC):
    """Build a parsed-header dict exactly as unpack_hdr() would return one."""
    return dict(magic=magic, seq=seq, total=total, flags=flags, len=length)


def _st_payload(seq, total, flags, data, len_field=None, magic=COREDUMP_MAGIC):
    """Serialize one coredump Ethernet PAYLOAD (12-byte LE header + data) using
    the same struct format the receiver parses with. `len_field` defaults to
    len(data); pass a different value to forge a length/data mismatch."""
    if len_field is None:
        len_field = len(data)
    return struct.pack(HDR_FMT, magic, seq, total, flags, len_field) + data


def _st_l2(payload, ethertype=COREDUMP_ETHERTYPE, pad_to=0):
    """Wrap a payload in the 14-byte Ethernet header AF_PACKET hands us
    (dst6|src6|ethertype2), broadcast dst / board-MAC src like emac_send_frame()
    does. `pad_to` appends trailing zero padding, which is what a real NIC does
    to reach the 60-byte Ethernet minimum for our small frames."""
    frame = b"\xff" * 6 + BOARD_MAC + struct.pack("!H", ethertype) + payload
    if pad_to > len(frame):
        frame += b"\x00" * (pad_to - len(frame))
    return frame


def _st_simulate_sender(blob, cap=COREDUMP_MAX_TOTAL):
    """Mirror of coredump.c's byte-into-frames streamer, so a whole synthetic
    dump can be pushed through the receiver offline. Transcribed from:
      - coredump.c:314-316  total = ceil(total_bytes/DATA_MAX), floored to 1
      - coredump.c:134-143  stream_byte(): drop past the cap, else buffer one
                            byte and flush a NON-last frame at fill==DATA_MAX
      - coredump.c:323      coredump_send()'s UNCONDITIONAL final
                            frame_flush(&s, 1) -- runs even when fill == 0,
                            which is the source of the extra-empty-LAST-frame
                            quirk documented on expected_frame_count()
    Returns (declared_total, [(hdr, data), ...]) in send order."""
    truncated = blob[:cap]
    total = expected_frame_count(len(truncated))
    out = []
    seq = 0
    fill = []
    for byte in truncated:
        fill.append(byte)
        if len(fill) >= COREDUMP_DATA_MAX:
            data = bytes(fill)
            out.append((_st_hdr(seq, total, 0, len(data)), data))
            seq += 1
            fill = []
    data = bytes(fill)                       # may legitimately be empty
    out.append((_st_hdr(seq, total, FLAG_LAST, len(data)), data))
    return total, out


# ---- case 1-6: the original selftest body, verbatim --------------------

def case_in_order_complete():
    r = Reassembler()
    for i, c in enumerate(CHUNKS):
        hdr = dict(magic=COREDUMP_MAGIC, seq=i, total=len(CHUNKS),
                    flags=(FLAG_LAST if i == len(CHUNKS) - 1 else 0),
                    len=len(c))
        assert r.feed(hdr, c)
    assert r.is_complete(), "in-order stream should be complete"
    assert r.assemble() == b"AAAABBBBCCCC"
    assert r.missing() == []


def case_out_of_order_complete():
    r = Reassembler()
    order = [2, 0, 1]
    for i in order:
        c = CHUNKS[i]
        hdr = dict(magic=COREDUMP_MAGIC, seq=i, total=len(CHUNKS),
                    flags=(FLAG_LAST if i == len(CHUNKS) - 1 else 0),
                    len=len(c))
        r.feed(hdr, c)
    assert r.is_complete(), "out-of-order stream should still be complete"
    assert r.assemble() == b"AAAABBBBCCCC"


def case_dropped_then_late_arrival():
    """A frame dropped, then arrives late (e.g. a retransmit-free network
    reorder) -- exercise LAST arriving before every seq is present."""
    r = Reassembler()
    hdr0 = dict(magic=COREDUMP_MAGIC, seq=0, total=3, flags=0, len=4)
    hdr2 = dict(magic=COREDUMP_MAGIC, seq=2, total=3, flags=FLAG_LAST, len=4)
    r.feed(hdr0, b"AAAA")
    r.feed(hdr2, b"CCCC")           # LAST seen, but seq=1 still missing
    assert not r.is_complete(), "gap at seq=1 must NOT be reported complete"
    assert r.missing() == [1]
    hdr1 = dict(magic=COREDUMP_MAGIC, seq=1, total=3, flags=0, len=4)
    r.feed(hdr1, b"BBBB")            # the missing frame finally lands
    assert r.is_complete()
    assert r.assemble() == b"AAAABBBBCCCC"


def case_genuinely_incomplete_skips_gap():
    """LAST seen, a gap that never fills (simulates the idle-timeout give-up
    path in recv_coredump())."""
    r = Reassembler()
    r.feed(dict(magic=COREDUMP_MAGIC, seq=0, total=4, flags=0, len=4), b"AAAA")
    r.feed(dict(magic=COREDUMP_MAGIC, seq=3, total=4, flags=FLAG_LAST, len=4),
           b"DDDD")
    assert not r.is_complete()
    assert r.missing() == [1, 2]
    assert r.assemble() == b"AAAA" + b"DDDD"   # skips the gap, no zero-pad


def case_corrupt_frames_dropped():
    """Wrong magic / truncated data must be silently dropped, not crash."""
    r = Reassembler()
    assert not r.feed(dict(magic=0xDEADBEEF, seq=0, total=1, flags=1, len=4),
                       b"AAAA")
    assert not r.feed(dict(magic=COREDUMP_MAGIC, seq=0, total=1, flags=1,
                            len=8), b"AAAA")   # len says 8, only 4 given
    assert r.total is None and r.frames == {}


def case_header_round_trip():
    """Header pack/unpack round-trip against the exact byte layout."""
    raw = struct.pack(HDR_FMT, COREDUMP_MAGIC, 7, 42, FLAG_LAST, 100)
    h = unpack_hdr(raw)
    assert h == dict(magic=COREDUMP_MAGIC, seq=7, total=42, flags=FLAG_LAST,
                      len=100)
    assert HDR_LEN == 12


# ---- case 7: duplicate chunks (feed()'s last-wins overwrite) -----------

def case_duplicate_chunks_last_wins():
    """Covers the bare `self.frames[hdr["seq"]] = data` assignment in feed():
    a repeated seq OVERWRITES, it is not rejected and not deduplicated.

    WHERE DUPLICATES ACTUALLY COME FROM: never from the board retransmitting.
    coredump.c's frame_flush() (coredump.c:121-127) loops only until
    emac_send_frame() reports the frame QUEUED and then breaks, so each seq is
    handed to the TX ring exactly once; there is no ACK/retry on this channel at
    all. A duplicate on the wire therefore means L2 duplication (a bridge
    flooding a broadcast frame down two paths, a capture tap, us bound to a
    bridge and one of its ports), which duplicates frames BYTE-FOR-BYTE. So the
    identical-duplicate case below is the realistic one, and it is benign.

    A CONFLICTING duplicate (same seq, different bytes) cannot be produced by a
    correct sender; it would mean genuine corruption that still passed magic +
    length checks. Last-wins is what the code DOES, and it is measured and
    pinned here -- but note it is *current behaviour, not a specified contract*:
    neither coredump.h nor this script documents a duplicate policy, and
    first-wins would be an equally defensible choice (arguably better: the first
    copy of a frame is the one least likely to have been mangled in transit).
    This assertion exists so that changing it has to be a deliberate act."""
    # (a) benign identical duplicate.
    r = Reassembler()
    h0 = _st_hdr(0, 2, 0, 4)
    h1 = _st_hdr(1, 2, FLAG_LAST, 4)
    assert r.feed(h0, b"AAAA")
    assert r.feed(h0, b"AAAA"), "a duplicate is ACCEPTED (returns True), not rejected"
    assert r.frames[0] == b"AAAA"
    assert len(r.frames) == 1, "duplicate must not create a second entry"
    assert r.feed(h1, b"BBBB")
    assert r.is_complete()
    assert r.assemble() == b"AAAABBBB", "identical duplicate must not double up"

    # (b) duplicate of the LAST frame keeps saw_last latched (it is a sticky
    #     flag; a duplicate can only re-set it, never clear it).
    assert r.feed(h1, b"BBBB")
    assert r.saw_last
    assert r.is_complete()
    assert r.assemble() == b"AAAABBBB"

    # (c) CONFLICTING duplicate: same seq, different bytes. MEASURED behaviour
    #     is last-wins -- the newest payload silently replaces the older one,
    #     with no warning anywhere.
    r = Reassembler()
    assert r.feed(_st_hdr(0, 2, 0, 4), b"AAAA")
    assert r.feed(_st_hdr(0, 2, 0, 4), b"ZZZZ")   # same seq, different data
    assert r.frames[0] == b"ZZZZ", "measured: LAST write wins"
    assert len(r.frames) == 1
    r.feed(_st_hdr(1, 2, FLAG_LAST, 4), b"BBBB")
    assert r.is_complete(), "a conflict does not make the stream incomplete"
    assert r.assemble() == b"ZZZZBBBB"

    # (d) a conflicting duplicate may even change the LENGTH of a frame, which
    #     silently changes the assembled blob's size. Nothing detects this.
    r = Reassembler()
    r.feed(_st_hdr(0, 1, FLAG_LAST, 4), b"AAAA")
    assert r.assemble() == b"AAAA"
    r.feed(_st_hdr(0, 1, FLAG_LAST, 8), b"AAAAAAAA")
    assert r.assemble() == b"AAAAAAAA"
    assert r.is_complete()


# ---- case 8: frames disagreeing about `total` ---------------------------

def case_disagreeing_total_takes_max():
    """Covers `self.total = max(self.total, hdr["total"])` in feed() -- the
    else-branch that no other case reaches (every other case sends a consistent
    `total`, so self.total is only ever set by the `is None` branch).

    The source's own comment justifies max() as "trust the newest (still
    bounded -- a stream can't un-send frames, so growing total just means keep
    waiting)". Note what max() actually implements: a LARGER total is adopted, a
    SMALLER one is IGNORED. Both directions are pinned below.

    Real coredump.c never does this: s.total is computed once (coredump.c:314)
    before any frame is streamed and stamped into every frame by frame_flush()
    (coredump.c:117). So this whole branch is defence against corruption or
    against two overlapping dumps on the wire, not against normal operation."""
    # Growing total is adopted -> more frames are required.
    r = Reassembler()
    r.feed(_st_hdr(0, 2, 0, 4), b"AAAA")
    assert r.total == 2
    r.feed(_st_hdr(1, 4, 0, 4), b"BBBB")          # disagrees: says 4
    assert r.total == 4, "larger total must be adopted"
    assert r.missing() == [2, 3]
    assert not r.is_complete()

    # Shrinking total is ignored (max() keeps the old, larger value), so a
    # stream that already promised 4 frames can never be satisfied by a later
    # frame claiming only 2 exist.
    r = Reassembler()
    r.feed(_st_hdr(0, 4, 0, 4), b"AAAA")
    assert r.total == 4
    r.feed(_st_hdr(1, 2, FLAG_LAST, 4), b"BBBB")  # disagrees downwards
    assert r.total == 4, "smaller total must be IGNORED (max keeps 4)"
    assert r.saw_last
    assert r.missing() == [2, 3]
    assert not r.is_complete()

    # An equal total takes the `elif` comparison but not the assignment; state
    # must be untouched.
    r = Reassembler()
    r.feed(_st_hdr(0, 2, 0, 4), b"AAAA")
    r.feed(_st_hdr(1, 2, FLAG_LAST, 4), b"BBBB")
    assert r.total == 2 and r.is_complete()


# ---- case 9: the total-is-None fallbacks -------------------------------

def case_total_none_fallbacks():
    """Covers the `if self.total is None` branches in missing() and assemble().

    HONEST NOTE ON REACHABILITY: feed() sets self.total on the FIRST accepted
    frame, so "total is None with frames present" is UNREACHABLE through feed().
    Part (a)/(b) below reach the fallbacks the only way real code can (no frame
    accepted yet -- a fresh receiver, or one that has seen nothing but corrupt
    frames, i.e. exactly the `r.total is None` -> "no coredump frames seen at
    all" path in _cli_recv()). Part (c) constructs the impossible state DIRECTLY
    to pin assemble()'s sorted() ordering; that assertion documents dead code,
    and is labelled as such rather than pretending the state can occur."""
    # (a) fresh receiver: nothing fed at all.
    r = Reassembler()
    assert r.total is None
    assert r.missing() == [], "missing() with no total known must be empty"
    assert r.assemble() == b"", "assemble() with nothing collected is empty"
    assert not r.is_complete()

    # (b) only rejected frames fed (bad magic, then a len/data mismatch): still
    #     no total, so both fallbacks are exercised through feed() alone.
    r = Reassembler()
    assert not r.feed(_st_hdr(0, 3, FLAG_LAST, 4, magic=0xDEADBEEF), b"AAAA")
    assert not r.feed(_st_hdr(1, 3, 0, 99), b"BBBB")
    assert r.total is None and r.frames == {}
    assert r.missing() == []
    assert r.assemble() == b""
    assert not r.is_complete()
    # is_complete() must stay False even if a LAST-flagged frame was among the
    # rejects -- feed() returns before touching saw_last for a rejected frame.
    assert not r.saw_last

    # (c) DEAD-CODE PIN: total None *with* frames present. Unreachable via
    #     feed() (see the note above), constructed here only to pin that
    #     assemble()'s fallback concatenates in ascending seq order rather than
    #     dict-insertion order.
    r = Reassembler()
    r.frames[2] = b"CCCC"
    r.frames[0] = b"AAAA"
    r.frames[1] = b"BBBB"
    assert r.total is None
    assert r.assemble() == b"AAAABBBBCCCC", "fallback must sort by seq"
    # ...and that missing() reports nothing in this state even though it plainly
    # cannot know whether anything is missing -- it has no total to compare to.
    assert r.missing() == []
    assert not r.is_complete(), "no saw_last, no total: never complete"


# ---- case 10: parse_l2_frame() wire-level parsing ----------------------

def case_parse_l2_frame_wire_level():
    """Covers parse_l2_frame() -- the byte-level half of _recv_one(), which
    before the extraction was reachable only with a live AF_PACKET socket.
    One case per accept/reject path, in the order the function checks them."""
    good_payload = _st_payload(3, 9, 0, b"payload!")

    # (a) accept: a well-formed frame parses to the exact header + data.
    got = parse_l2_frame(_st_l2(good_payload))
    assert got is not None, "a well-formed frame must be accepted"
    hdr, data = got
    assert hdr == dict(magic=COREDUMP_MAGIC, seq=3, total=9, flags=0, len=8)
    assert data == b"payload!"

    # (b) accept + LAST flag survives the round trip.
    hdr, data = parse_l2_frame(_st_l2(_st_payload(4, 5, FLAG_LAST, b"z")))
    assert hdr["flags"] & FLAG_LAST and hdr["seq"] == 4 and data == b"z"

    # (c) accept: a zero-length data frame is legal (this is exactly the extra
    #     final frame coredump.c emits at an exact-multiple total -- see
    #     case_frame_count_math_matches_coredump_c()). Its frame is 26 bytes,
    #     which is >= 14+HDR_LEN, so the short-frame check must not eat it.
    hdr, data = parse_l2_frame(_st_l2(_st_payload(2, 2, FLAG_LAST, b"")))
    assert hdr["len"] == 0 and data == b""

    # (d) accept: trailing NIC padding to the 60-byte Ethernet minimum must be
    #     ignored, NOT appended to the data (the slice is bounded by hdr[len]).
    #     Every small coredump frame on real wire is padded like this.
    padded = _st_l2(_st_payload(0, 1, FLAG_LAST, b"ABCD"), pad_to=60)
    assert len(padded) == 60
    hdr, data = parse_l2_frame(padded)
    assert data == b"ABCD", "Ethernet pad bytes must not leak into the data"

    # (e) reject: frame shorter than 14 + HDR_LEN (a runt / another protocol).
    assert parse_l2_frame(b"") is None
    assert parse_l2_frame(_st_l2(good_payload)[:14 + HDR_LEN - 1]) is None
    #     ...and exactly 14+HDR_LEN with a valid header is the boundary that
    #     must still be ACCEPTED (len=0), proving the check is not off by one.
    boundary = _st_l2(_st_payload(0, 1, FLAG_LAST, b""))
    assert len(boundary) == 14 + HDR_LEN
    assert parse_l2_frame(boundary) is not None

    # (f) reject: wrong ethertype. 0x88B5 is this project's console channel and
    #     0x88B6 netcon -- both share br0, so this check is load-bearing even
    #     though the socket binding also filters.
    assert parse_l2_frame(_st_l2(good_payload, ethertype=0x88B5)) is None
    assert parse_l2_frame(_st_l2(good_payload, ethertype=0x0800)) is None

    # (g) reject: bad magic (right ethertype, wrong/corrupt payload).
    assert parse_l2_frame(_st_l2(_st_payload(0, 1, 0, b"AAAA",
                                             magic=0xDEADBEEF))) is None

    # (h) reject: truncated -- the len field promises more data than the frame
    #     carries. Note the frame is still long enough to hold a header, so
    #     only the final length check catches this.
    short = _st_l2(_st_payload(0, 1, 0, b"AAAA", len_field=64))
    assert parse_l2_frame(short) is None
    #     One byte missing is enough to reject.
    off_by_one = _st_l2(_st_payload(0, 1, 0, b"AAAA", len_field=5))
    assert parse_l2_frame(off_by_one) is None

    # (i) end-to-end: parse a whole synthetic dump off "the wire" and reassemble
    #     it, i.e. parse_l2_frame() -> Reassembler.feed() composed exactly as
    #     _recv_one()/recv_coredump() compose them, with no socket involved.
    blob = bytes((i * 7 + 11) & 0xFF for i in range(2600))
    _total, frames = _st_simulate_sender(blob)
    r = Reassembler()
    for hdr, data in frames:
        parsed = parse_l2_frame(_st_l2(_st_payload(hdr["seq"], hdr["total"],
                                                   hdr["flags"], data)))
        assert parsed is not None
        assert r.feed(parsed[0], parsed[1])
    assert r.is_complete()
    assert r.assemble() == blob


# ---- case 11: frame-count math vs coredump.c --------------------------

def case_frame_count_math_matches_coredump_c():
    """Pins expected_frame_count() == ceil(total_bytes/COREDUMP_DATA_MAX) with a
    floor of 1 (coredump.c:314-316), and then pins the EXACT-MULTIPLE QUIRK
    end-to-end through the receiver.

    THE QUIRK, verified by reading coredump.c rather than assumed:
      - stream_byte() (coredump.c:134-143) flushes a NON-last frame the moment
        fill reaches COREDUMP_DATA_MAX;
      - coredump_send() then ALWAYS calls frame_flush(&s, 1) (coredump.c:323),
        with no `if (s.fill)` guard;
      => when total_bytes is an exact multiple of COREDUMP_DATA_MAX the last
         full frame was already flushed with fill reset to 0, so that final
         call emits one extra frame with len=0, flags=LAST and seq == total,
         i.e. ONE PAST the end of the [0,total) range the header declares.
    The receiver must still report COMPLETE: the empty frame satisfies
    saw_last, missing() only scans [0,total), and assemble() only concatenates
    [0,total), so the out-of-range entry is inert. That is asserted below."""
    # Plain arithmetic agreement.
    assert expected_frame_count(0) == 1, "floored to 1 (coredump.c:315-316)"
    assert expected_frame_count(1) == 1
    assert expected_frame_count(1023) == 1
    assert expected_frame_count(1024) == 1
    assert expected_frame_count(1025) == 2
    assert expected_frame_count(2047) == 2
    assert expected_frame_count(2048) == 2
    assert expected_frame_count(2049) == 3
    assert expected_frame_count(COREDUMP_MAX_TOTAL) == 384
    for n in (1, 500, 1023, 1024, 1025, 4096, 100000, COREDUMP_MAX_TOTAL):
        assert expected_frame_count(n) == -(-n // COREDUMP_DATA_MAX)

    # Non-multiple: no extra frame, highest seq == total-1.
    blob = bytes(range(256)) * 6              # 1536 bytes = 1.5 frames
    total, frames = _st_simulate_sender(blob)
    assert total == 2 and len(frames) == 2
    assert [h["seq"] for h, _ in frames] == [0, 1]
    assert frames[-1][0]["flags"] & FLAG_LAST
    assert frames[-1][0]["len"] == 1536 - COREDUMP_DATA_MAX
    r = Reassembler()
    for hdr, data in frames:
        assert r.feed(hdr, data)
    assert r.is_complete() and r.assemble() == blob

    # EXACT multiple: total==2 but THREE frames are sent, the third being the
    # empty LAST one at seq==2, outside [0,2).
    blob = bytes((i * 3) & 0xFF for i in range(2 * COREDUMP_DATA_MAX))
    total, frames = _st_simulate_sender(blob)
    assert total == expected_frame_count(len(blob)) == 2
    assert len(frames) == 3, "the extra empty LAST frame really is emitted"
    assert [h["seq"] for h, _ in frames] == [0, 1, 2]
    assert frames[2][0]["len"] == 0 and frames[2][1] == b""
    assert frames[2][0]["flags"] & FLAG_LAST
    assert frames[2][0]["seq"] == total, "seq == total: one PAST [0,total)"
    assert not (frames[1][0]["flags"] & FLAG_LAST), \
        "the last FULL frame is flushed non-last by stream_byte()"

    r = Reassembler()
    for hdr, data in frames:
        assert r.feed(hdr, data)
    assert r.total == 2
    assert 2 in r.frames, "the out-of-range seq is stored, harmlessly"
    assert r.missing() == [], "missing() only scans [0,total)"
    assert r.is_complete(), "receiver must still report COMPLETE (the quirk)"
    assert r.assemble() == blob, "assemble() must ignore the out-of-range seq"

    # The quirk is only harmless because assemble() iterates range(total)
    # rather than every collected seq. If an out-of-range frame ever carried
    # DATA (a corrupt or spoofed seq, not something coredump.c produces), it
    # must be excluded too -- splicing it in would silently append garbage to
    # an otherwise-COMPLETE core.
    r.feed(_st_hdr(5, 2, 0, 7), b"GARBAGE")
    assert 5 in r.frames
    assert r.missing() == []
    assert r.is_complete()
    assert r.assemble() == blob, \
        "assemble() must iterate [0,total), not every collected seq"
    assert b"GARBAGE" not in r.assemble()

    # Same shape at 1 frame exactly (blob == COREDUMP_DATA_MAX): total==1, two
    # frames sent, seq 1 is the empty LAST one.
    blob = b"E" * COREDUMP_DATA_MAX
    total, frames = _st_simulate_sender(blob)
    assert total == 1 and len(frames) == 2 and frames[1][0]["seq"] == 1
    r = Reassembler()
    for hdr, data in frames:
        r.feed(hdr, data)
    assert r.is_complete() and r.assemble() == blob

    # And the same at the hard cap (COREDUMP_MAX_TOTAL is itself an exact
    # multiple of COREDUMP_DATA_MAX, so a capped dump ALWAYS hits the quirk).
    assert COREDUMP_MAX_TOTAL % COREDUMP_DATA_MAX == 0
    blob = b"\xa5" * (COREDUMP_MAX_TOTAL + 777)     # oversized: gets truncated
    total, frames = _st_simulate_sender(blob)
    assert total == 384 and len(frames) == 385
    assert frames[-1][0]["len"] == 0
    r = Reassembler()
    for hdr, data in frames:
        r.feed(hdr, data)
    assert r.is_complete()
    assert len(r.assemble()) == COREDUMP_MAX_TOTAL, "cap truncates, honestly"


# ---- case 12: the assembled blob is handed back untouched --------------

def case_assembled_elf_blob_is_verbatim():
    """The receiver is a byte pipe: whatever it reassembles, it returns. It must
    not sniff, validate, byte-swap, pad or rewrite the ELF at all -- a
    half-corrupt core is still exactly the bytes that arrived, which is what
    makes `missing()` the only honest completeness signal.

    Uses a real ELF64/AArch64/ET_CORE e_ident+prefix laid out the way
    coredump.c:245-261 writes it, so the blob is representative rather than
    arbitrary, and is deliberately sized to STRADDLE a frame boundary (the ELF
    magic lands in seq 0, the tail in seq 1)."""
    ehdr = bytearray(64)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4] = 2                                   # ELFCLASS64
    ehdr[5] = 1                                   # ELFDATA2LSB
    ehdr[6] = 1                                   # EV_CURRENT
    ehdr[16:18] = struct.pack("<H", 4)            # e_type = ET_CORE
    ehdr[18:20] = struct.pack("<H", 183)          # e_machine = EM_AARCH64
    blob = bytes(ehdr) + bytes((i * 31 + 5) & 0xFF for i in range(1500))
    assert len(blob) > COREDUMP_DATA_MAX          # must straddle a boundary

    total, frames = _st_simulate_sender(blob)
    assert total == 2
    r = Reassembler()
    for hdr, data in frames:
        assert r.feed(hdr, data)
    assert r.is_complete()

    out = r.assemble()
    assert out == blob, "assembled bytes must be identical to what was sent"
    assert out[:4] == b"\x7fELF", "ELF magic survives the frame boundary split"
    assert out[4] == 2 and out[5] == 1 and out[6] == 1
    assert struct.unpack("<H", out[16:18])[0] == 4      # still ET_CORE
    assert struct.unpack("<H", out[18:20])[0] == 183    # still EM_AARCH64
    assert isinstance(out, bytes)
    assert len(out) == len(blob)

    # Out-of-order delivery of the very same frames must produce the identical
    # blob (assemble() is ordered by seq, not by arrival).
    r2 = Reassembler()
    for hdr, data in reversed(frames):
        r2.feed(hdr, data)
    assert r2.is_complete()
    assert r2.assemble() == blob

    # An INCOMPLETE assembly is also returned verbatim -- gap skipped, nothing
    # zero-padded, nothing "repaired". The caller must rely on missing().
    r3 = Reassembler()
    r3.feed(frames[1][0], frames[1][1])          # only the tail (LAST)
    assert not r3.is_complete()
    assert r3.missing() == [0]
    assert r3.assemble() == frames[1][1]
    assert not r3.assemble().startswith(b"\x7fELF")   # honestly broken


_ST_CASES = [
    ("in_order_complete",                  case_in_order_complete),
    ("out_of_order_complete",              case_out_of_order_complete),
    ("dropped_then_late_arrival",          case_dropped_then_late_arrival),
    ("genuinely_incomplete_skips_gap",     case_genuinely_incomplete_skips_gap),
    ("corrupt_frames_dropped",             case_corrupt_frames_dropped),
    ("header_round_trip",                  case_header_round_trip),
    ("duplicate_chunks_last_wins",         case_duplicate_chunks_last_wins),
    ("disagreeing_total_takes_max",        case_disagreeing_total_takes_max),
    ("total_none_fallbacks",               case_total_none_fallbacks),
    ("parse_l2_frame_wire_level",          case_parse_l2_frame_wire_level),
    ("frame_count_math_matches_coredump_c",
     case_frame_count_math_matches_coredump_c),
    ("assembled_elf_blob_is_verbatim",     case_assembled_elf_blob_is_verbatim),
]


def _cli_selftest(_args):
    """Offline check of the reassembly logic and the wire-level frame parser
    against hand-crafted fake frames — no network, no hardware, no files."""
    passed = failed = 0
    for name, fn in _ST_CASES:
        print(f"[ RUN ] {name}")
        try:
            fn()
        except Exception:        # AssertionError normally; anything else is
            failed += 1          # also a failure, not a reason to abort the run
            print(f"[FAIL ] {name}")
            traceback.print_exc()
            continue
        passed += 1
        print(f"[ OK  ] {name}")
    n = passed + failed
    print(f"---- coredump-recv selftest: {passed}/{n} passed ----")
    return 0 if failed == 0 else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "selftest":
        return _cli_selftest(argv[1:])
    return _cli_recv(argv)


if __name__ == "__main__":
    sys.exit(main())
