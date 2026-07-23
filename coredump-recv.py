#!/usr/bin/env python3
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
        Offline check of the reassembly logic against hand-crafted fake
        frames (in-order, out-of-order, a dropped-then-arriving-late frame,
        and a genuinely incomplete stream) — no network, no hardware.
"""
import sys
import time
import socket
import struct

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


def unpack_hdr(buf):
    magic, seq, total, flags, length = struct.unpack(HDR_FMT, buf[:HDR_LEN])
    return dict(magic=magic, seq=seq, total=total, flags=flags, len=length)


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


def _recv_one(sock, timeout):
    """Return (hdr, data) for one valid coredump frame from `sock`'s
    ethertype (0x88B7 only, already filtered by the socket binding), or None
    if nothing arrives within `timeout` seconds. Drops anything too short,
    wrong magic, or truncated and keeps waiting out the remaining timeout —
    exactly netcon.py's _recv_one() pattern, minus the CRC/ACK bits this
    fire-and-forget channel doesn't have."""
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
        if struct.unpack("!H", frame[12:14])[0] != COREDUMP_ETHERTYPE:
            continue
        payload = frame[14:]
        hdr = unpack_hdr(payload)
        if hdr["magic"] != COREDUMP_MAGIC:
            continue
        data = payload[HDR_LEN:HDR_LEN + hdr["len"]]
        if len(data) < hdr["len"]:
            continue                  # truncated: drop, nothing to retry
        return hdr, data


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


def _cli_selftest(_args):
    """Offline check of the reassembly logic against hand-crafted fake
    frames — no network, no hardware."""
    # 1) In-order, complete.
    r = Reassembler()
    chunks = [b"AAAA", b"BBBB", b"CCCC"]
    for i, c in enumerate(chunks):
        hdr = dict(magic=COREDUMP_MAGIC, seq=i, total=len(chunks),
                    flags=(FLAG_LAST if i == len(chunks) - 1 else 0),
                    len=len(c))
        assert r.feed(hdr, c)
    assert r.is_complete(), "in-order stream should be complete"
    assert r.assemble() == b"AAAABBBBCCCC"
    assert r.missing() == []

    # 2) Out-of-order arrival, still complete.
    r = Reassembler()
    order = [2, 0, 1]
    for i in order:
        c = chunks[i]
        hdr = dict(magic=COREDUMP_MAGIC, seq=i, total=len(chunks),
                    flags=(FLAG_LAST if i == len(chunks) - 1 else 0),
                    len=len(c))
        r.feed(hdr, c)
    assert r.is_complete(), "out-of-order stream should still be complete"
    assert r.assemble() == b"AAAABBBBCCCC"

    # 3) A frame dropped, then arrives late (e.g. a retransmit-free network
    #    reorder) -- exercise LAST arriving before every seq is present.
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

    # 4) Genuinely incomplete: LAST seen, a gap that never fills (simulates
    #    the idle-timeout give-up path in recv_coredump()).
    r = Reassembler()
    r.feed(dict(magic=COREDUMP_MAGIC, seq=0, total=4, flags=0, len=4), b"AAAA")
    r.feed(dict(magic=COREDUMP_MAGIC, seq=3, total=4, flags=FLAG_LAST, len=4),
           b"DDDD")
    assert not r.is_complete()
    assert r.missing() == [1, 2]
    assert r.assemble() == b"AAAA" + b"DDDD"   # skips the gap, no zero-pad

    # 5) Wrong magic / truncated data must be silently dropped, not crash.
    r = Reassembler()
    assert not r.feed(dict(magic=0xDEADBEEF, seq=0, total=1, flags=1, len=4),
                       b"AAAA")
    assert not r.feed(dict(magic=COREDUMP_MAGIC, seq=0, total=1, flags=1,
                            len=8), b"AAAA")   # len says 8, only 4 given
    assert r.total is None and r.frames == {}

    # 6) Header pack/unpack round-trip against the exact byte layout.
    raw = struct.pack(HDR_FMT, COREDUMP_MAGIC, 7, 42, FLAG_LAST, 100)
    h = unpack_hdr(raw)
    assert h == dict(magic=COREDUMP_MAGIC, seq=7, total=42, flags=FLAG_LAST,
                      len=100)
    assert HDR_LEN == 12

    print("[coredump] selftest OK (in-order, out-of-order, late-arrival, "
          "incomplete-stream, drop-on-corrupt, header round-trip)")
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "selftest":
        return _cli_selftest(argv[1:])
    return _cli_recv(argv)


if __name__ == "__main__":
    sys.exit(main())
