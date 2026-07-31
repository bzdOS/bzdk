#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
gdbstub_client.py — reusable host-side RSP client helpers for talking to the
bzdOS EL2 hypervisor's GDB stub (gdbstub.c) directly, WITHOUT running a real
`gdb` binary.

Why this exists: this session re-implemented the same hand-crafted RSP
client pattern from scratch in several one-off scratch scripts while chasing
the resolve()/hwop_run() bugs (see project memory
`gdbstub-breakpoint-resolve-broken.md` / `gdbstub-hwbp-wrong-core.md`) —
build a framed `$payload#checksum` packet, send a leading `+`/wait for one,
wait for a full `$...#cc` reply, and decode the g-packet register blob to
pull out PC. That pattern is extracted here ONCE so future live-debugging
sessions reuse it instead of re-deriving RSP framing by hand each time.

Layers on gdb-bridge.py's `EmacLink` (same raw-Ethernet 0x88B5 transport,
same board MAC / broadcast / 46-byte-minimum-frame conventions) rather than
duplicating it — `EmacLink` is loaded dynamically here since "gdb-bridge.py"
is not a valid Python module name (the hyphen), and renaming that file would
break its existing `sudo ./gdb-bridge.py` invocation. Importing this module
does NOT open any socket or touch the board — that only happens once a
`GdbStubClient` is actually instantiated. Verify with:

    python3 -c "import gdbstub_client"

RSP wire format (standard gdbserver protocol, see gdbstub.c's `gdb_recv_body`/
`tx_raw_and_ack`/`gdb_send` for the board-side implementation):
    request/reply packet:  '$' + payload + '#' + 2-hex-digit checksum
    checksum:              (sum of payload bytes) mod 256, hex
    ack:                   '+' (good checksum) / '-' (bad, please resend)
    async notifications (Ctrl-C stop, stop-reply after continue/step) look
    like any other packet on the wire; the only way to tell "a stop-reply
    arrived" from "a bare ack arrived" is inspecting whether what you read
    off the wire was a single ack byte or a full $...# packet.

g-packet register layout: gdbstub.c documents this at gdbstub.c:364
("Register access — GDB g-packet order: x0..x30, sp, pc, cpsr(32b)") and
implements it via `REG_SP`/`REG_PC`/`REG_CPSR`/`REG_MAX` (gdbstub.c:367-370)
and the 'g'/'G' handlers (gdbstub.c:636-655): 31 general registers (x0..x30),
then sp, then pc — each as 8 bytes little-endian hex (16 hex chars) — then
cpsr as 4 bytes little-endian hex (8 hex chars). Total payload length for a
'g' reply is therefore 33*16 + 8 = 536 hex chars.
"""
import importlib.util
import os
import time

# ---------------------------------------------------------------------------
# Dynamically load gdb-bridge.py's EmacLink class (see module docstring for
# why this can't be a plain `from gdb_bridge import EmacLink`). Only DEFINES
# things at import time — gdb-bridge.py's own `if __name__ == "__main__":`
# guard means loading it this way (under a different synthetic module name)
# never runs its `main()` / never opens a socket.
# ---------------------------------------------------------------------------
def _load_emac_link_class():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gdb-bridge.py")
    spec = importlib.util.spec_from_file_location("_gdbstub_client_bridge_internal", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.EmacLink


EmacLink = _load_emac_link_class()

# GDB stop-reply packets always start with one of these (see the GDB Remote
# Serial Protocol spec's "Stop Reply Packets" section) — used to distinguish
# an async stop notification from any other packet arriving off the wire.
STOP_REPLY_PREFIXES = ("S", "T", "W", "X", "O")

# g-packet layout constants (gdbstub.c:367-370/636-655).
REG_GPR_COUNT = 31          # x0..x30
REG_ORDER = ["x%d" % i for i in range(REG_GPR_COUNT)] + ["sp", "pc", "cpsr"]
_GPR_HEXLEN = 16            # 8 bytes, little-endian hex
_CPSR_HEXLEN = 8            # 4 bytes, little-endian hex


def rsp_checksum(payload_bytes):
    """RSP checksum: sum of payload bytes mod 256."""
    return sum(payload_bytes) & 0xff


def rsp_frame(payload):
    """Build a full `$payload#cc` RSP packet (bytes) from a str or bytes
    payload (no leading '$', no trailing '#cc' — those are added here)."""
    if isinstance(payload, str):
        payload = payload.encode("latin1", "replace")
    return b"$" + payload + b"#" + ("%02x" % rsp_checksum(payload)).encode("ascii")


def read_le_hex(hexstr, nbytes):
    """Decode `nbytes` bytes of little-endian hex (2*nbytes hex chars) from
    the front of `hexstr`, mirroring gdbstub.c's read_le() (gdbstub.c:413-424)
    used board-side for the inverse direction ('G'/'P' register writes)."""
    v = 0
    for i in range(nbytes):
        v |= int(hexstr[i * 2:i * 2 + 2], 16) << (i * 8)
    return v


def decode_g_reply(payload):
    """Decode a raw 'g'-packet reply payload (hex string, no '$'/'#') into a
    dict {'x0':.., ..., 'x30':.., 'sp':.., 'pc':.., 'cpsr':..} per the g-packet
    order gdbstub.c documents (gdbstub.c:364/636-643): x0..x30 (8 bytes each),
    sp (8 bytes), pc (8 bytes), cpsr (4 bytes) — all little-endian hex."""
    regs = {}
    off = 0
    for i in range(REG_GPR_COUNT):
        regs["x%d" % i] = read_le_hex(payload[off:off + _GPR_HEXLEN], 8)
        off += _GPR_HEXLEN
    regs["sp"] = read_le_hex(payload[off:off + _GPR_HEXLEN], 8); off += _GPR_HEXLEN
    regs["pc"] = read_le_hex(payload[off:off + _GPR_HEXLEN], 8); off += _GPR_HEXLEN
    regs["cpsr"] = read_le_hex(payload[off:off + _CPSR_HEXLEN], 4); off += _CPSR_HEXLEN
    return regs


def decode_pc(payload):
    """Pull just the PC field out of a raw 'g'-packet reply payload, without
    decoding the full register set (see decode_g_reply() for the layout)."""
    off = REG_GPR_COUNT * _GPR_HEXLEN + _GPR_HEXLEN   # skip x0..x30, sp
    return read_le_hex(payload[off:off + _GPR_HEXLEN], 8)


class GdbStubClient:
    """A minimal, dependency-free RSP client talking directly to gdbstub.c
    over the board's raw-Ethernet 0x88B5 debug channel — for scripted
    probing/verification without launching a real `gdb` + gdb-bridge.py pair.

    NOTE: constructing this class opens a raw AF_PACKET socket (via
    EmacLink), which needs CAP_NET_RAW (root) and a live board — this is
    host-tooling plumbing only, never exercised by `make test`/`./ci.sh`.
    """

    def __init__(self, iface="br0", key=None, arm=True):
        """`iface`: raw-Ethernet interface the board is reachable on (same
        default as gdb-bridge.py/hvdbg.py, br0). `key`: optional DBG_AUTH hex
        key, passed straight through to EmacLink (see gdb-bridge.py's module
        docstring) — None means the default unauthenticated protocol. `arm`:
        if True (default), immediately calls arm_gdb_mode() to flip the
        board's console channel from text dbgmon into RSP mode."""
        self.link = EmacLink(iface, key=key)
        self._rx_buf = b""
        if arm:
            self.arm_gdb_mode()

    def close(self):
        try:
            self.link.sock.close()
        except Exception:
            pass

    # ── channel mode ────────────────────────────────────────────────────
    def arm_gdb_mode(self, settle=0.2):
        """Flip the board's 0x88B5 channel from text dbgmon to RSP mode by
        sending dbgmon's `gdb` hand-off command line — mirrors gdb-bridge.py's
        `--arm` option exactly (same text line, same settle-and-discard
        pattern) so this client and gdb-bridge.py agree on how the hand-off
        happens."""
        self.link.send_text_line("gdb")
        time.sleep(settle)
        self.link.recv_bytes()   # discard whatever text (banner/prompt) is in flight

    # ── low-level framing ──────────────────────────────────────────────
    def _pump(self, timeout=0.05):
        """Pull any bytes currently queued on the link into the internal
        buffer. Returns True if anything was read."""
        t0 = time.time()
        got_any = False
        while time.time() - t0 < timeout:
            d = self.link.recv_bytes()
            if d:
                self._rx_buf += d
                got_any = True
            elif got_any:
                break
            else:
                time.sleep(0.005)
        return got_any

    def _extract_one(self):
        """Pop ONE complete unit off the front of the internal buffer:
        ('ack', '+'|'-'), ('ctrlc', None), ('pkt', payload_str), or None if
        nothing complete is buffered yet (caller should pump more and
        retry). A well-formed packet is ACKed ('+') automatically here (bad
        checksum gets a '-' so a real board would resend, matching standard
        RSP client behavior) — callers never need to ack manually."""
        buf = self._rx_buf
        if not buf:
            return None
        head = buf[0:1]
        if head in (b"+", b"-"):
            self._rx_buf = buf[1:]
            return ("ack", head.decode("ascii"))
        if head == b"\x03":
            self._rx_buf = buf[1:]
            return ("ctrlc", None)
        start = buf.find(b"$")
        if start < 0:
            self._rx_buf = b""   # no packet start in sight -- drop stray junk
            return None
        if start > 0:
            buf = buf[start:]
            self._rx_buf = buf
        hashpos = buf.find(b"#")
        if hashpos < 0 or len(buf) < hashpos + 3:
            return None          # packet still incomplete -- wait for more bytes
        payload = buf[1:hashpos]
        cs_hex = buf[hashpos + 1:hashpos + 3]
        self._rx_buf = buf[hashpos + 3:]
        try:
            given = int(cs_hex, 16)
        except ValueError:
            given = -1
        ok = (given == rsp_checksum(payload))
        self.link.send_bytes(b"+" if ok else b"-")
        if not ok:
            return None
        return ("pkt", payload.decode("latin1", "replace"))

    # ── public RSP operations ───────────────────────────────────────────
    def send_raw(self, payload):
        """Frame and transmit `payload` as one RSP packet. Does not wait for
        an ack or reply -- use send_command() for the common synchronous
        round-trip, or pair this with wait_for_reply()/wait_for_stop_reply()
        for more control (e.g. sending 'c'/continue and then separately
        awaiting the eventual stop-reply)."""
        self.link.send_bytes(rsp_frame(payload))

    def wait_for_reply(self, timeout=2.0):
        """Wait (bounded) for the next full `$...#cc` packet, returning its
        decoded payload string — distinct from wait_for_stop_reply() only in
        that this returns the FIRST packet seen, whatever its shape (bare
        acks are consumed silently along the way, not returned). Returns
        None on timeout."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            self._pump(0.05)
            while True:
                item = self._extract_one()
                if item is None:
                    break
                kind, val = item
                if kind == "pkt":
                    return val
        return None

    def send_command(self, cmd, timeout=2.0):
        """Send one RSP command (bare payload, e.g. 'g', 'm4020,10',
        'Z0,ffff...,4') and return the reply packet's payload string. Raises
        TimeoutError if no ack+reply arrives within `timeout` seconds."""
        self.send_raw(cmd)
        t0 = time.time()
        while time.time() - t0 < timeout:
            self._pump(0.05)
            while True:
                item = self._extract_one()
                if item is None:
                    break
                kind, val = item
                if kind == "pkt":
                    return val
                # bare acks ('+'/'-') and stray Ctrl-C bytes are consumed and
                # ignored here -- the reply packet is what send_command()
                # actually waits for.
        raise TimeoutError("no reply to RSP command %r within %.1fs" % (cmd, timeout))

    def wait_for_stop_reply(self, timeout=5.0):
        """Wait (bounded) for an asynchronous stop-reply packet — the kind
        gdbstub.c sends unprompted after a breakpoint/watchpoint fires
        following a 'c'/'s' (continue/step), as opposed to a synchronous
        reply to a command this client just sent. Distinguishes a real
        stop-reply packet (payload starting with one of STOP_REPLY_PREFIXES)
        from a bare ack byte or any other packet shape. Returns the payload
        string, or None on timeout."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            self._pump(0.1)
            while True:
                item = self._extract_one()
                if item is None:
                    break
                kind, val = item
                if kind == "pkt" and val[:1] in STOP_REPLY_PREFIXES:
                    return val
        return None

    # ── register convenience wrappers ───────────────────────────────────
    def read_registers(self, timeout=2.0):
        """Send 'g' and decode the full register set. Returns the
        decode_g_reply() dict."""
        return decode_g_reply(self.send_command("g", timeout=timeout))

    def read_pc(self, timeout=2.0):
        """Send 'g' and return just the PC field (see decode_pc())."""
        return decode_pc(self.send_command("g", timeout=timeout))
