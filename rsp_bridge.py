#!/usr/bin/env python3
"""rsp_bridge.py -- TCP<->board-console bridge for RSP (GDB remote) sessions.

The board's gdbstub speaks RSP over the SAME EMAC console byte channel that
dbgmon uses; the `gdb` dbgmon command flips the shared channel to the stub.
This bridge makes that usable for a real debugger:

    gdb-multiarch -ex 'target remote :12345' kernel

    TCP(client gdb) <-> :12345 <-> this bridge <-> EMAC 0x88B5 console bytes

RSP bytes are forwarded 1:1 (including 0x03 Ctrl-C interrupts). dbgmon's
own prompt/output only exists while gdb_channel==0; once the `gdb` command
is issued, the channel belongs to the stub, so the bridge must NOT inject
anything dbgmon-side except the initial `gdb` switch line.

Usage: python3 rsp_bridge.py [--port 12345] [--switch-gdb]
  --switch-gdb: send the dbgmon `gdb` command at startup (flips the channel).
"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hvdbg import HV, BCAST, BOARD_MAC, ETYPE  # noqa: E402

PORT = 12345


def main():
    port = PORT
    switch = "--switch-gdb" in sys.argv
    hv = HV()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print("RSP bridge listening on 127.0.0.1:%d" % port, flush=True)
    print("board: %s iface=%s" % (hv.__class__.__name__, hv.iface), flush=True)

    if switch:
        hv.cmd("gdb")
        print("sent dbgmon `gdb` -- console channel now belongs to the stub",
              flush=True)

    conn, _ = srv.accept()
    conn.settimeout(0.05)
    print("gdb connected", flush=True)

    hv.sock.settimeout(0.02)
    last_rx = time.time()
    while True:
        # gdb -> board
        try:
            data = conn.recv(4096)
            if not data:
                print("gdb closed", flush=True)
                break
            if data:
                for off in range(0, len(data), 64):
                    chunk = data[off:off + 64]
                    if len(chunk) < 46:
                        chunk = chunk + b"\x00" * (46 - len(chunk))
                    hv.sock.send(BCAST + hv.src_mac +
                                 struct.pack("!H", ETYPE) + chunk)
        except socket.timeout:
            pass

        # board -> gdb
        got = False
        while True:
            try:
                f = hv.sock.recv(2048)
            except socket.timeout:
                break
            except OSError:
                break
            if len(f) > 14 and struct.unpack("!H", f[12:14])[0] == ETYPE \
               and f[6:12] == BOARD_MAC:
                payload = f[14:].split(b"\x00")[0]
                if payload:
                    conn.sendall(payload)
                    got = True
        if got:
            last_rx = time.time()


if __name__ == "__main__":
    main()
