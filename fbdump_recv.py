#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""fbdump_recv.py — host-side receiver for the bzdOS EL2 hypervisor's raw
framebuffer streamer (fbdump.c/fbdump.h, ethertype 0x88B9). Companion to
screenshot.py, which is the normal caller of fetch() below; this module also
stands alone as a CLI for dumping any DRAM region.

WHY A SEPARATE FIRE-AND-FORGET CHANNEL (see fbdump.h for the board-side half
of this rationale)

dbgmon's hex `d`/`r` commands and netcon.py's ACKed transport both pay one
full round trip per chunk, which is fine for a breadcrumb and useless for a
framebuffer (minutes for 1920x1080x4). fbdump.c instead fires every 1024-byte
chunk onto the wire without waiting for anything, and ALL the retry logic
lives here on the host: reassemble what arrived, compute the gaps, and
re-request just those byte ranges by issuing `fbdump` again with a narrower
(addr, len). This mirrors snapshot_net.py's bulk/control split (see that
file's docstring) rather than inventing a new pattern.

ORDERING IS THE WHOLE RELIABILITY STORY

The raw socket MUST be open and its reader thread MUST already be draining
it before the `fbdump` command is sent. fbdump_send() on the board starts
writing frames to the wire immediately and does not wait for a receiver to
be ready -- there is no handshake, no SYN, nothing. Any frame that arrives
before this process is listening is gone forever, no different from a UDP
packet sent to a socket that hasn't bound yet. Every call path below opens
the socket and starts the reader thread first, then sends the command.

WIRE FORMAT (must match fbdump.h exactly -- do not "fix" this to network
order; fbdump.c copies a native little-endian C struct straight onto the
wire, so `<IIHH` -- little-endian, no padding -- is correct on this
little-endian aarch64 board and MUST NOT become `>IIHH` if someone
"corrects" it later):

    u32 magic ('FBDP' = 0x50424446 as a little-endian word)
    u32 offset   (byte offset from the START of the REQUESTED region, i.e.
                  relative to whatever `addr` was just sent on the CURRENT
                  `fbdump` command -- not an absolute address, and not
                  relative to the overall multi-round fetch())
    u16 len      (payload bytes in this frame, <= FBDUMP_DATA_MAX)
    u16 flags    (bit 0 = FBDUMP_FLAG_LAST: final frame of this pass)
    data[len]

THROUGHPUT -- EXPECTED, NOT YET MEASURED ON THIS EXACT PATH

The board's EMAC link has been measured at ~915 KB/s on real hardware after
the forced-duplex-mismatch fix (project memory: emac-duplex-mismatch-forced-
link -- that was inbound host->board traffic, not this outbound board->host
path, but it is the same link and the same fixed duplex setting). This
module's own transfer has not been clocked on the board yet. Treating that
915 KB/s figure as an expected ceiling: a 1120x276x4 guest window (~1.2 MiB)
should take on the order of ~1.5 s, and a full 1920x1080x4 HUD frame (~8 MiB)
on the order of ~9 s -- versus multiple minutes over hvdbg.read_words(). Do
not quote these as measured until someone runs this against the board.

RETRY DESIGN

Round 0 requests the whole region. Whatever bytes did not arrive (tracked as
gaps in a merged interval list, not a per-byte bitmap -- cheap for an 8 MiB
region since frame count tops out around 8000) get re-requested individually
in round 1, then any still-missing gaps in round 2, and so on, capped at
`max_rounds` (default 4). fetch() returns honestly: the reassembled bytes
(zero-filled where nothing ever arrived -- bytearray's own default) AND the
list of byte ranges still missing after giving up, so a caller can decide
whether to trust, retry itself, or just say so. Nothing here silently
pretends a gap is real data.
"""
import argparse
import os
import queue
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzd_board as B

IFACE = B.IFACE

FBDUMP_ETHERTYPE = 0x88B9
FBDUMP_MAGIC     = 0x50424446          # 'FBDP' little-endian, see fbdump.h
FBDUMP_DATA_MAX  = 1024                # fbdump.h FBDUMP_DATA_MAX
FBDUMP_FLAG_LAST = 0x0001              # fbdump.h FBDUMP_FLAG_LAST
FBDUMP_MAX_LEN   = 16 * 1024 * 1024    # fbdump.h FBDUMP_MAX_LEN

# Same DRAM window fbdump.c itself refuses to read outside of (fbdump.c's
# DRAM_LO/DRAM_HI) -- checked here too so a bad request fails instantly
# instead of costing a live round trip to be told the same thing.
DRAM_LO = 0x40000000
DRAM_HI = 0x80000000

HDR_FMT = "<IIHH"                      # magic, offset, len, flags -- see the
HDR_LEN = struct.calcsize(HDR_FMT)     # module docstring's byte-order note
assert HDR_LEN == 12, "fbdump header must be 12 bytes, got %d" % HDR_LEN

# Expected (not measured on this exact path -- see module docstring).
EXPECTED_BYTES_PER_SEC = 915_000


def cmd_wait_budget(nbytes):
    """How long to let hv.cmd('fbdump ...') block waiting for the console
    reply. fbdump_send() on the board is synchronous on CPU1: the "frames=N"
    reply line is only sent AFTER every frame has already gone out on the
    wire, so this has to cover the whole transfer, not one round trip. 1.5x
    margin over the expected rate plus a flat floor for small requests where
    command/console overhead dominates."""
    return max(3.0, (nbytes / EXPECTED_BYTES_PER_SEC) * 1.5 + 2.0)


# ---------------------------------------------------------------------- #
# Pure helpers: frame parsing and interval bookkeeping. No socket, no
# threading -- these are what the offline self-test drives directly.
# ---------------------------------------------------------------------- #
def parse_fbdump_frame(frame):
    """Validate one raw Ethernet frame (14-byte dst+src+ethertype header,
    then the fbdump payload). Returns a dict with offset/len/flags/data/last
    on success, or None if the frame is not a valid fbdump frame -- wrong
    ethertype, bad magic, truncated, or a declared length that doesn't fit
    what actually arrived. A dropped frame here just shows up as a gap;
    there is no separate error path because a gap and a corrupt frame are
    handled identically by the retry round."""
    if len(frame) < 14 + HDR_LEN:
        return None
    if struct.unpack("!H", frame[12:14])[0] != FBDUMP_ETHERTYPE:
        return None
    payload = frame[14:]
    if len(payload) < HDR_LEN:
        return None
    magic, offset, length, flags = struct.unpack(HDR_FMT, payload[:HDR_LEN])
    if magic != FBDUMP_MAGIC:
        return None
    if length > FBDUMP_DATA_MAX:
        return None                     # cannot come from fbdump_send(); drop
    available = len(payload) - HDR_LEN
    if length > available:
        return None                     # truncated on the wire: drop
    data = payload[HDR_LEN:HDR_LEN + length]
    return {
        "offset": offset,
        "len": length,
        "flags": flags,
        "data": data,
        "last": bool(flags & FBDUMP_FLAG_LAST),
    }


def add_range(intervals, start, end):
    """Insert half-open [start, end) into `intervals` (a list of disjoint,
    sorted half-open ranges), merging anything overlapping or touching.
    Returns the new list; does not mutate the input."""
    if start >= end:
        return intervals
    ivs = sorted(intervals + [(start, end)])
    merged = [ivs[0]]
    for s, e in ivs[1:]:
        ls, le = merged[-1]
        if s <= le:
            merged[-1] = (ls, max(le, e))
        else:
            merged.append((s, e))
    return merged


def find_gaps(intervals, total_len):
    """Complement of the merged `intervals` within [0, total_len). Returns a
    sorted list of half-open (start, end) ranges that are NOT covered."""
    gaps = []
    pos = 0
    for s, e in sorted(intervals):
        if s > pos:
            gaps.append((pos, s))
        pos = max(pos, e)
    if pos < total_len:
        gaps.append((pos, total_len))
    return gaps


def apply_frames(buf, covered, base, req_len, frames):
    """Write each frame's data into `buf` at (base + frame offset), and fold
    the byte range it covered into `covered`. `req_len` is the length of the
    region THIS round asked for (not `len(buf) - base` -- a gap re-request
    in the middle of the buffer has req_len far smaller than the distance to
    the end of buf, and using the latter would let a stray frame with a
    too-large offset overwrite already-good bytes further along). Split out
    of fetch() so the offline self-test can drive the exact same reassembly
    code with synthetic frames and no socket at all -- one implementation,
    two callers."""
    for f in frames:
        if f["offset"] + f["len"] > req_len:
            continue   # stale/out-of-range for the region this round asked
                       # for -- see module docstring: frames are fire-and-
                       # forget with no request id, so a straggler from an
                       # earlier, larger request must be bounds-checked away
        astart = base + f["offset"]
        aend = astart + f["len"]
        buf[astart:aend] = f["data"]
        covered = add_range(covered, astart, aend)
    return covered


# ---------------------------------------------------------------------- #
# Socket plumbing
# ---------------------------------------------------------------------- #
def open_fbdump_socket(iface=IFACE):
    """AF_PACKET raw socket bound to `iface`, filtered to the fbdump
    ethertype (0x88B9) at the kernel level -- mirrors chimpd.py's NetCon and
    snapshot_net.py's open_bulk_socket()/netcon.py's open_netcon_socket(),
    just this module's own ethertype."""
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                      socket.htons(FBDUMP_ETHERTYPE))
    s.bind((iface, FBDUMP_ETHERTYPE))
    return s


class _FrameQueue:
    """Background thread draining the fbdump socket into a thread-safe
    queue of decoded frames (see parse_fbdump_frame). Must be constructed
    and start()-ed BEFORE the `fbdump` command is sent -- see the module
    docstring's ORDERING section."""

    def __init__(self, sock):
        self.sock = sock
        self.q = queue.Queue()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=2.0)

    def _run(self):
        while not self._stop.is_set():
            self.sock.settimeout(0.2)
            try:
                frame = self.sock.recv(2048)
            except (socket.timeout, OSError):
                continue
            f = parse_fbdump_frame(frame)
            if f is not None:
                self.q.put(f)

    def drain(self):
        """Discard anything already queued. Called right before issuing a
        new sub-request so a straggler from the PREVIOUS (usually larger)
        request's fire-and-forget stream cannot be mis-attributed to the new
        one's offset space -- apply_frames()'s bounds check catches most of
        that anyway, but starting each sub-request from an empty queue keeps
        the accounting honest and avoids processing frames twice."""
        try:
            while True:
                self.q.get_nowait()
        except queue.Empty:
            pass

    def collect(self, req_len, timeout, idle_timeout):
        """Collect frames for up to `timeout` seconds total, exiting early
        either on a FBDUMP_FLAG_LAST frame or after `idle_timeout` seconds
        with nothing new arriving. Exiting on LAST is a latency win, not a
        correctness requirement -- if the LAST frame itself is the one that
        got lost, the idle timeout is what saves this from hanging, and the
        resulting gap just gets picked up by the next round."""
        out = []
        t0 = time.time()
        t_last = t0
        while True:
            now = time.time()
            if now - t0 > timeout or now - t_last > idle_timeout:
                break
            remaining = min(idle_timeout - (now - t_last), timeout - (now - t0))
            try:
                f = self.q.get(timeout=max(0.01, remaining))
            except queue.Empty:
                continue
            t_last = time.time()
            out.append(f)
            if f["last"]:
                break
        return out


# ---------------------------------------------------------------------- #
# Public API
# ---------------------------------------------------------------------- #
def fetch(pa, length, hv=None, iface=IFACE, max_rounds=4,
          idle_timeout=1.5):
    """Fetch DRAM [pa, pa+length) via fbdump, retrying gaps up to
    `max_rounds` times. Returns (bytes_of_length_`length`, missing_ranges)
    where missing_ranges is a list of (start, end) byte offsets (relative to
    `pa`) that never arrived -- empty if the fetch was complete. Bytes in a
    missing range are zero in the returned buffer (bytearray's own default),
    but the honest answer is the second element of the tuple, not "all
    zero == background colour".

    `hv`: an existing hvdbg.HV() to issue the `fbdump` command on. Pass the
    caller's own instance (screenshot.py does) to reuse one debug-channel
    socket instead of opening a second one; if omitted, a private HV() is
    created and closed here.

    Raises ValueError for a request outside guest DRAM or larger than
    FBDUMP_MAX_LEN -- the same limits fbdump.c itself enforces, checked here
    first so a bad request fails instantly instead of costing a live round
    trip to be told the same thing by the board.
    """
    if length <= 0:
        return b"", []
    if pa < DRAM_LO or pa >= DRAM_HI or (pa + length) > DRAM_HI:
        raise ValueError("fbdump_recv.fetch: 0x%x..0x%x outside guest DRAM "
                         "[0x%x, 0x%x)" % (pa, pa + length, DRAM_LO, DRAM_HI))
    if length > FBDUMP_MAX_LEN:
        raise ValueError("fbdump_recv.fetch: length 0x%x exceeds "
                         "FBDUMP_MAX_LEN 0x%x" % (length, FBDUMP_MAX_LEN))

    own_hv = hv is None
    if own_hv:
        import hvdbg
        hv = hvdbg.HV(iface=iface)

    sock = open_fbdump_socket(iface)
    reader = _FrameQueue(sock)
    reader.start()   # listening BEFORE the first command -- see docstring
    try:
        buf = bytearray(length)
        covered = []
        reqs = [(0, length)]
        for _round in range(max_rounds):
            if not reqs:
                break
            for rstart, rlen in reqs:
                reader.drain()
                reply = hv.cmd("fbdump 0x%x 0x%x" % (pa + rstart, rlen),
                              wait=cmd_wait_budget(rlen))
                if ("outside guest DRAM" in reply or "too large" in reply or
                        "zero length" in reply):
                    continue   # board refused this range; retrying won't help
                frames = reader.collect(rlen, timeout=cmd_wait_budget(rlen),
                                        idle_timeout=idle_timeout)
                covered = apply_frames(buf, covered, rstart, rlen, frames)
            reqs = find_gaps(covered, length)
        return bytes(buf), find_gaps(covered, length)
    finally:
        reader.stop()
        sock.close()
        if own_hv:
            hv.close()


# ---------------------------------------------------------------------- #
# Offline self-test: reassembly + gap detection driven by synthetic frames.
# No socket, no board -- this is what "make dbg" clean plus this test can
# verify without hardware access.
# ---------------------------------------------------------------------- #
def _eth(payload, ethertype=FBDUMP_ETHERTYPE):
    dst = b"\x00" * 6
    src = b"\x00" * 6
    return dst + src + struct.pack("!H", ethertype) + payload


def _mkframe(offset, data, last=False, ethertype=FBDUMP_ETHERTYPE,
            magic=FBDUMP_MAGIC):
    flags = FBDUMP_FLAG_LAST if last else 0
    hdr = struct.pack(HDR_FMT, magic, offset, len(data), flags)
    return _eth(hdr + data, ethertype=ethertype)


def _selftest():
    import random

    # 1. parse_fbdump_frame: accept a good frame, reject the obvious ways a
    #    bad one can show up on the wire.
    good = _mkframe(0, b"ABCD", last=True)
    f = parse_fbdump_frame(good)
    assert f is not None and f["offset"] == 0 and f["data"] == b"ABCD" \
        and f["last"] is True
    assert parse_fbdump_frame(good[:-1]) is None       # declared len=4 but
                                                        # only 3 bytes present
                                                        # -- truncated, drop
    assert parse_fbdump_frame(_mkframe(0, b"AB", ethertype=0x88B5)) is None
    assert parse_fbdump_frame(_mkframe(0, b"AB", magic=0x11111111)) is None
    assert parse_fbdump_frame(b"\x00" * 10) is None    # too short even for
                                                        # the eth header
    hdr_only = _eth(struct.pack(HDR_FMT, FBDUMP_MAGIC, 0, 4, 0))  # len=4,
    assert parse_fbdump_frame(hdr_only) is None                   # no data
    print("parse_fbdump_frame: ok")

    # 2. add_range / find_gaps.
    ivs = []
    ivs = add_range(ivs, 100, 200)
    ivs = add_range(ivs, 300, 400)
    ivs = add_range(ivs, 200, 300)      # touches both -- must merge into one
    assert ivs == [(100, 400)], ivs
    gaps = find_gaps(ivs, 500)
    assert gaps == [(0, 100), (400, 500)], gaps
    print("add_range/find_gaps: ok")

    # 3. Full reassembly simulation: a synthetic region, chunked exactly the
    #    way fbdump_send() chunks it, with some frames dropped to simulate
    #    loss, reassembled over two rounds the same way fetch() does it.
    random.seed(1234)
    length = FBDUMP_DATA_MAX * 9 + 137     # 9 full chunks + one partial
    region = bytes(random.randrange(256) for _ in range(length))

    def chunk_frames(base_region, total_len):
        out = []
        off = 0
        while off < total_len:
            n = min(FBDUMP_DATA_MAX, total_len - off)
            out.append(_mkframe(off, base_region[off:off + n],
                                last=(off + n >= total_len)))
            off += n
        return out

    frames_r0 = chunk_frames(region, length)
    # Drop chunk indices 2 and 5, and drop the actual LAST frame too -- the
    # idle-timeout exit path (not the LAST-flag path) has to be what a real
    # collect() would fall back on; here we just skip straight to feeding
    # the survivors into apply_frames(), since that's the piece under test.
    dropped = {2, 5, 9}
    kept_r0 = [f for i, f in enumerate(frames_r0) if i not in dropped]

    buf = bytearray(length)
    covered = []
    parsed = [parse_fbdump_frame(fr) for fr in kept_r0]
    covered = apply_frames(buf, covered, 0, length, parsed)
    gaps = find_gaps(covered, length)
    expected_gaps = []
    off = 0
    for i in range(10):
        n = min(FBDUMP_DATA_MAX, length - off)
        if i in dropped:
            expected_gaps.append((off, off + n))
        off += n
    # Adjacent dropped chunks would have merged into one gap; none are
    # adjacent here (2, 5, 9 with kept chunks between), so no merge expected.
    assert gaps == expected_gaps, (gaps, expected_gaps)
    print("round 0 gaps match dropped chunks: ok (%r)" % (gaps,))

    # Round 1: re-request exactly the gaps, this time with nothing dropped.
    for (gstart, gend) in gaps:
        glen = gend - gstart
        sub_region = region[gstart:gend]
        sub_frames = chunk_frames(sub_region, glen)
        parsed_sub = [parse_fbdump_frame(fr) for fr in sub_frames]
        covered = apply_frames(buf, covered, gstart, glen, parsed_sub)
    final_gaps = find_gaps(covered, length)
    assert final_gaps == [], final_gaps
    assert bytes(buf) == region, "reassembled region does not match source"
    print("round 1 fills every gap, reassembled bytes match source: ok")

    # 4. Regression test for the req_len-vs-len(buf)-base bug: a stray frame
    #    whose offset is valid for a request STARTING at `base` but runs past
    #    the actual (smaller) region that request asked for must be dropped,
    #    not written -- even though it would still land inside `buf` overall.
    #    This is exactly what a late straggler from an earlier, larger
    #    request looks like once apply_frames() is called for a small,
    #    middle-of-the-buffer gap.
    probe = bytearray(1000)
    probe_covered = []
    canary = bytes(probe[500:1000])   # untouched region beyond the gap
    stray = parse_fbdump_frame(_mkframe(50, b"X" * 100))  # offset 50, len
                                                            # 100 -> 50..150,
                                                            # fine against
                                                            # req_len=100 at
                                                            # base=400 only if
                                                            # bounds-checked
                                                            # against req_len,
                                                            # not len(buf)-base
    in_bounds = parse_fbdump_frame(_mkframe(10, b"Y" * 20))   # 10..30, fits
    probe_covered = apply_frames(probe, probe_covered, 400, 100,
                                 [in_bounds, stray])
    assert probe[410:430] == b"Y" * 20
    assert bytes(probe[500:1000]) == canary, \
        "stray frame overran into bytes outside its own request's region"
    assert probe_covered == [(410, 430)], probe_covered
    print("apply_frames bounds check uses req_len, not len(buf)-base: ok")

    print("fbdump_recv selftest: ALL PASS")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--addr", help="physical address, hex, e.g. 0x46000000")
    ap.add_argument("--len", dest="length",
                    help="byte length, hex, e.g. 0x100000")
    ap.add_argument("--out", default=None,
                    help="output path for raw bytes (default: /tmp/bzdos-"
                         "fbdump-<addr>-<len>.bin)")
    ap.add_argument("--iface", default=IFACE)
    ap.add_argument("--max-rounds", type=int, default=4)
    ap.add_argument("--selftest", action="store_true",
                    help="run the offline reassembly/gap-detection test "
                         "(no board, no network) and exit")
    a = ap.parse_args()

    if a.selftest:
        return _selftest()

    if not a.addr or not a.length:
        ap.error("--addr and --len are required (or pass --selftest)")

    pa = int(a.addr, 16)
    length = int(a.length, 16)
    out = a.out or ("/tmp/bzdos-fbdump-%#x-%#x.bin" % (pa, length))

    import hvdbg
    hv = hvdbg.HV(iface=a.iface)
    t0 = time.time()
    data, missing = fetch(pa, length, hv=hv, iface=a.iface,
                          max_rounds=a.max_rounds)
    dt = time.time() - t0
    with open(out, "wb") as fh:
        fh.write(data)
    if missing:
        mb = sum(e - s for s, e in missing)
        print("fbdump_recv: wrote %s (%d bytes total, %d MISSING across "
              "%d gaps) in %.1fs" % (out, length, mb, len(missing), dt))
        return 1
    print("fbdump_recv: wrote %s (%d bytes, complete) in %.1fs (%.0f KB/s)" %
          (out, length, dt, length / 1024.0 / max(dt, 0.001)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
