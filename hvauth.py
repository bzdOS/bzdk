#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
hvauth.py -- host side of the HV debug-channel envelope (dbgauth.h).

Every host -> HV debug frame (dbgmon/gdb console, netcon, dbgtools, snapnet)
goes through Channel.frame()/Channel.send(): with a key it is wrapped in an
authenticated session envelope on ethertype 0x88B5; without one it is the
legacy unauthenticated frame on the channel's own ethertype.

Key: the same file the HV build embeds -- $BZDK_DBG_KEY_FILE, default
~/.config/bzdk/dbg.key (hex, 16..64 bytes; never committed). Missing file ->
legacy frames (an HV built with DBG_NO_AUTH=1, or an old image).
BZDK_DBG_NOAUTH=1 forces legacy frames even when the key exists.

Session: HELLO(Nh) -> CHAL(sess, Sb, proof). The proof is checked, so a
wrong key is reported as such. When the board does not answer the HELLO at
all (old image without dbgauth, or the board is down) the frame goes out
legacy-style and the handshake is retried on a later send -- an auth-only
board ignores those, so nothing is weakened, and a rollback to an older
image stays debuggable. BZDK_DBG_AUTH=require disables that fallback.
A NAK (the board forgot the session: reboot, or more than 16 newer
sessions) makes the next frame re-handshake first (every frame() checks
for one); callers that see no reply call check_nak() and resend at once.
"""
import hashlib
import hmac
import os
import secrets
import socket
import struct
import sys
import threading
import time

ETYPE_CONSOLE = 0x88B5
MAGIC = b"\x00BZK"
T_HELLO, T_CHAL, T_DATA, T_NAK = 1, 2, 3, 4
CH_CONSOLE, CH_NETCON, CH_DBGTOOLS, CH_SNAPNET = 0, 1, 2, 3
LEGACY_ETYPE = {CH_CONSOLE: 0x88B5, CH_NETCON: 0x88B6,
                CH_DBGTOOLS: 0x88B7, CH_SNAPNET: 0x88B8}
BCAST = b"\xff" * 6
BOARD_MAC = bytes.fromhex("02bd05000001")
MINPAY = 46
TAG_LEN = 16
DATA_HDR = 36

DEFAULT_KEY_FILE = os.path.expanduser("~/.config/bzdk/dbg.key")


def key_file():
    return os.environ.get("BZDK_DBG_KEY_FILE") or DEFAULT_KEY_FILE


def load_key(path=None):
    """The shared key as bytes, or None (no key file / BZDK_DBG_NOAUTH=1)."""
    if os.environ.get("BZDK_DBG_NOAUTH") == "1":
        return None
    path = path or key_file()
    try:
        txt = open(path).read().strip()
    except FileNotFoundError:
        return None
    k = bytes.fromhex(txt)
    if not 16 <= len(k) <= 64:
        raise ValueError(f"{path}: key must be 16..64 bytes of hex")
    return k


def _h(key, *parts):
    return hmac.new(key, b"".join(parts), hashlib.sha256).digest()


def data_payload(ks, sess, ctr, chan, inner):
    """DATA envelope (frame payload after the Ethernet header)."""
    hdr = MAGIC + bytes([T_DATA, chan]) + struct.pack(">IQH", sess, ctr,
                                                      len(inner))
    return hdr + _h(ks, hdr, inner)[:TAG_LEN] + inner


def _pad(p):
    return p + b"\x00" * (MINPAY - len(p)) if len(p) < MINPAY else p


class AuthError(Exception):
    pass


class Channel:
    """One debug session to the board. Thread-safe."""

    _warned = set()

    def __init__(self, iface="br0", key=None, board_mac=BOARD_MAC):
        if isinstance(key, str):
            key = bytes.fromhex(key)
        self.key = load_key() if key is None else key
        self.iface = iface
        self.board_mac = board_mac
        self.require = os.environ.get("BZDK_DBG_AUTH") == "require"
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                  socket.htons(ETYPE_CONSOLE))
        self.sock.bind((iface, ETYPE_CONSOLE))
        self.src_mac = self.sock.getsockname()[4]
        self.sess = None
        self.ks = None
        self.ctr = 0
        self.dst = BCAST
        self._next_try = 0.0
        self._lock = threading.RLock()

    @property
    def auth(self):
        return self.key is not None

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _warn(self, msg):
        if msg not in Channel._warned:
            Channel._warned.add(msg)
            print(f"[hvauth] {msg}", file=sys.stderr)

    # -- session ---------------------------------------------------------
    def handshake(self, tries=3, wait=0.25):
        """HELLO -> CHAL. True on a verified session."""
        with self._lock:
            nh = secrets.token_bytes(16)
            hello = _pad(MAGIC + bytes([T_HELLO, 0]) + nh)
            for _ in range(tries):
                self.sock.send(BCAST + self.src_mac +
                               struct.pack("!H", ETYPE_CONSOLE) + hello)
                t_end = time.time() + wait
                while time.time() < t_end:
                    self.sock.settimeout(max(0.005, t_end - time.time()))
                    try:
                        f = self.sock.recv(2048)
                    except (socket.timeout, OSError):
                        break
                    p = f[14:]
                    if (len(p) < 58 or p[:4] != MAGIC or p[4] != T_CHAL or
                            p[26:42] != nh):
                        continue
                    sess = struct.unpack(">I", p[6:10])[0]
                    sb = p[10:26]
                    lbl = struct.pack(">I", sess)
                    proof = _h(self.key, b"BZK1/chal", sb, lbl, nh)[:16]
                    if not hmac.compare_digest(proof, p[42:58]):
                        raise AuthError(
                            "board answered the handshake with a wrong proof:"
                            f" its key differs from {key_file()}")
                    self.sess = sess
                    self.ks = _h(self.key, b"BZK1/sess", sb, lbl, nh)
                    self.ctr = 0
                    self.dst = f[6:12]
                    return True
            return False

    def check_nak(self):
        """Drain pending control frames; True if the board NAKed our
        session (then the next send re-handshakes)."""
        naked = False
        with self._lock:
            self.sock.settimeout(0)
            while True:
                try:
                    f = self.sock.recv(2048)
                except (BlockingIOError, socket.timeout, OSError):
                    break
                p = f[14:]
                if (len(p) >= 10 and p[:4] == MAGIC and p[4] == T_NAK and
                        self.sess is not None and
                        struct.unpack(">I", p[6:10])[0] == self.sess):
                    self.sess = None
                    naked = True
        return naked

    def _ensure(self):
        if self.sess is not None:
            return True
        if not self.require and time.time() < self._next_try:
            return False
        if self.handshake():
            return True
        if self.require:
            raise AuthError("board did not answer the auth handshake")
        self._next_try = time.time() + 5.0
        self._warn("board did not answer the auth handshake; sending "
                   "unauthenticated frames (old image?)")
        return False

    # -- frames ------------------------------------------------------------
    def frame(self, chan, inner, dst=BCAST):
        """Complete Ethernet frame carrying `inner` on channel `chan`."""
        with self._lock:
            if self.key is not None and self.sess is not None:
                self.check_nak()        # a NAK for an earlier frame
            if self.key is not None and self._ensure():
                self.ctr += 1
                p = data_payload(self.ks, self.sess, self.ctr, chan, inner)
                return (self.dst + self.src_mac +
                        struct.pack("!H", ETYPE_CONSOLE) + _pad(p))
            return (dst + self.src_mac +
                    struct.pack("!H", LEGACY_ETYPE[chan]) + _pad(inner))

    def send(self, chan, inner, dst=BCAST):
        f = self.frame(chan, inner, dst)
        with self._lock:
            self.sock.send(f)
        return f


def board_stats(hv):
    """Parse dbgmon's `auth` reply into a dict (None if not understood)."""
    names = ("ok", "hello", "badtag", "replay", "nosess", "malformed",
             "legacy_ok", "legacy_drop", "naks", "evict", "verify_ticks",
             "verify_len")
    for _ in range(5):          # another console user's prompt can end
        r = hv.cmd("auth")      # our drain early: retry
        for line in r.splitlines():
            line = line.strip()
            parts = line.split()
            if line.startswith("auth=") and len(parts) == 1 + len(names):
                d = {"mode": parts[0][5:]}
                for n, v in zip(names, parts[1:]):
                    d[n] = int(v, 16)
                return d
    return None


def selftest():
    """Board-free: the DATA layout against the vector test_dbgauth.c checks
    on the C side, plus RFC 4231 test case 2."""
    assert data_payload(bytes(range(32)), 0x01020304, 5, 2, b"abc").hex() == (
        "00425a4b030201020304000000000000000500039ac08c87a00bb8ceacfcfbb170"
        "50929d616263")
    assert _h(b"Jefe", b"what do ya want ", b"for nothing?").hex() == (
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")
    assert _pad(b"x") == b"x" + bytes(45) and len(MAGIC) == 4
    print("hvauth selftest OK")


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="debug-channel auth check")
    ap.add_argument("--iface", default="br0")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        sys.exit(0)
    c = Channel(a.iface)
    if not c.auth:
        print(f"no key ({key_file()}): legacy mode")
        sys.exit(1)
    t0 = time.time()
    ok = c.handshake()
    print(f"handshake {'ok' if ok else 'FAILED'} in {time.time() - t0:.3f}s"
          f" sess={c.sess} board={c.dst.hex(':')}")
    sys.exit(0 if ok else 1)
