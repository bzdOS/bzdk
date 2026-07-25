#!/usr/bin/env python3
"""
gdb-bridge.py — host-side TCP <-> EMAC relay for the bzdOS GDB stub.

Turns `gdb ... target remote :1234` into raw-Ethernet 0x88B5 console frames the
board's gdbstub.c consumes byte-for-byte. The payload is RAW RSP (GDB's own
$...#cc framing IS the framing) so this process is a dumb, transparent byte
pipe: GDB's acks/retransmits work end-to-end.

Reuses hvdbg.py's AF_PACKET plumbing (ethertype 0x88B5, board MAC
02:bd:05:00:00:01, broadcast dst, 46-byte zero-pad, RX split on first NUL).

Usage (run as root — AF_PACKET needs CAP_NET_RAW):
    sudo ./gdb-bridge.py                 # listen :1234, relay to board over br0
    sudo ./gdb-bridge.py --iface br0 --port 1234
    sudo ./gdb-bridge.py --arm           # first send the dbgmon `gdb` hand-off
                                         #   line so the board flips the 0x88B5
                                         #   channel from text dbgmon to RSP
Then, on the host:
    aarch64-none-elf-gdb microkernel-dbg.elf
    (gdb) target remote :1234
    (gdb) info registers        # g-packet: x0..x30, sp, pc, cpsr
    (gdb) x/4i $pc              # m-packet through guest stage-1 (AT S1E1R)
    (gdb) break *0xffff000000abcd00 ; continue     # Z0 software breakpoint
    (gdb) hbreak *0x...        # Z1 hardware breakpoint (needs CPSR.D==0)
    (gdb) watch *(int*)0x...   # Z2 write watchpoint

See docs/gdbstub-design.md for the full protocol and the PSTATE.D / watchdog
caveats around when a live attach is viable.

DBG_AUTH (ROADMAP T5, optional — docs/security-notes.md option 1): if the
board was built with -DDBG_AUTH, pass the same key here so RSP frames are
accepted:
    sudo ./gdb-bridge.py --key a1b2c3...
Omitting --key (the default) sends byte-for-byte the same unauthenticated
frames as before -- nothing about the default path changed.
"""
import argparse
import hashlib
import hmac
import selectors
import socket
import struct
import sys
import time

ETYPE     = 0x88B5
BOARD_MAC = bytes.fromhex("02bd05000001")
BCAST     = b"\xff" * 6
CHUNK     = 256          # keep each frame's payload under one Ethernet frame
MINPAY    = 46           # Ethernet minimum payload; board pads/relies on this

# DBG_AUTH wire envelope (must match emac.c's dbg_auth_check() exactly):
#   [0:8]   nonce, 8 bytes big-endian uint64, strictly increasing
#   [8:40]  mac,   32 bytes HMAC-SHA256(key, nonce || cmd)
#   [40:]   cmd,   the frame's payload bytes, unchanged from the plain wire
DBG_AUTH_NONCE_LEN = 8
DBG_AUTH_MAC_LEN   = 32


class EmacLink:
    """Raw-Ethernet 0x88B5 link to the board, mirroring hvdbg.py."""

    def __init__(self, iface, key=None):
        """`key`: optional hex string (see module docstring). None (the
        default) is byte-for-byte the pre-DBG_AUTH behavior."""
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                  socket.htons(ETYPE))
        self.sock.bind((iface, ETYPE))
        self.src_mac = self.sock.getsockname()[4]
        self.sock.setblocking(False)
        self._auth_key = bytes.fromhex(key) if key else None
        self._nonce = time.time_ns()

    def fileno(self):
        return self.sock.fileno()

    def _sign(self, data):
        """Prepend the DBG_AUTH envelope to one frame's worth of `data`.
        Only called when self._auth_key is set."""
        self._nonce += 1
        nonce = self._nonce.to_bytes(DBG_AUTH_NONCE_LEN, "big")
        mac = hmac.new(self._auth_key, nonce + data, hashlib.sha256).digest()
        assert len(mac) == DBG_AUTH_MAC_LEN
        return nonce + mac + data

    def send_bytes(self, data):
        """Ship `data` to the board as one or more 0x88B5 frames. RSP text has
        no embedded NUL, so zero-padding to the 46-byte minimum is transparent
        (the board's emac_getc stops at the first NUL)."""
        for i in range(0, len(data), CHUNK):
            p = data[i:i + CHUNK]
            if self._auth_key is not None:
                p = self._sign(p)
            if len(p) < MINPAY:
                p = p + b"\x00" * (MINPAY - len(p))
            self.sock.send(BCAST + self.src_mac + struct.pack("!H", ETYPE) + p)

    def recv_bytes(self):
        """Drain all queued RX frames from the board; return concatenated RSP
        payload bytes (trailing NUL padding stripped per frame)."""
        out = b""
        while True:
            try:
                f = self.sock.recv(2048)
            except (BlockingIOError, OSError):
                break
            if len(f) < 14 or struct.unpack("!H", f[12:14])[0] != ETYPE:
                continue
            if f[6:12] != BOARD_MAC:
                continue
            out += f[14:].split(b"\x00", 1)[0]   # strip NUL padding
        return out

    def send_text_line(self, line):
        """Send a dbgmon-style text command line (CR-terminated, NUL-padded) —
        used only for the optional `--arm` hand-off before RSP takes over."""
        p = (line + "\r").encode("latin1", "replace")
        if self._auth_key is not None:
            p = self._sign(p)
        if len(p) < MINPAY:
            p = p + b"\x00" * (MINPAY - len(p))
        self.sock.send(BCAST + self.src_mac + struct.pack("!H", ETYPE) + p)


def main():
    ap = argparse.ArgumentParser(description="TCP<->EMAC bridge for the bzdOS GDB stub")
    ap.add_argument("--iface", default="br0", help="raw-Ethernet interface (default br0)")
    ap.add_argument("--port", type=int, default=1234, help="TCP port for gdb (default 1234)")
    ap.add_argument("--arm", action="store_true",
                    help="send the dbgmon `gdb` hand-off line before relaying "
                         "(flips the 0x88B5 channel from text dbgmon to RSP)")
    ap.add_argument("--key", default=None,
                    help="hex-encoded DBG_AUTH key (ROADMAP T5, optional) -- "
                         "only needed against a board built with -DDBG_AUTH; "
                         "omit for the default unauthenticated protocol")
    args = ap.parse_args()

    link = EmacLink(args.iface, key=args.key)

    if args.arm:
        # Tell the running dbgmon to hand the console channel to the stub. The
        # board's dbgmon `gdb` command (see docs/gdbstub-integration.md) sets a
        # latch; after this, the tick path routes bytes to gdbstub_poll() and
        # prints no more `dbg> ` prompts that would corrupt RSP.
        link.send_text_line("gdb")
        time.sleep(0.2)
        # Discard whatever text (final prompt/banner) is still in flight.
        link.recv_bytes()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print("gdb-bridge: waiting for `target remote :%d` ..." % args.port, flush=True)

    sel = selectors.DefaultSelector()

    while True:
        tcp, peer = srv.accept()
        print("gdb-bridge: gdb connected from %s:%d" % peer, flush=True)
        tcp.setblocking(False)
        link.recv_bytes()               # flush any stale board bytes

        sel.register(tcp, selectors.EVENT_READ, "tcp")
        sel.register(link, selectors.EVENT_READ, "emac")
        try:
            while True:
                for key, _ in sel.select(timeout=1.0):
                    if key.data == "tcp":
                        try:
                            d = tcp.recv(4096)
                        except BlockingIOError:
                            continue
                        if not d:
                            raise ConnectionError("gdb disconnected")
                        link.send_bytes(d)          # raw RSP -> board
                    else:  # "emac"
                        pl = link.recv_bytes()
                        if pl:
                            tcp.sendall(pl)          # board RSP -> gdb
        except (ConnectionError, BrokenPipeError, OSError) as e:
            print("gdb-bridge: session ended (%s)" % e, flush=True)
        finally:
            sel.unregister(tcp)
            sel.unregister(link)
            try:
                tcp.close()
            except OSError:
                pass


if __name__ == "__main__":
    try:
        main()
    except PermissionError:
        sys.exit("gdb-bridge: AF_PACKET needs root (CAP_NET_RAW) — run with sudo")
    except KeyboardInterrupt:
        pass
