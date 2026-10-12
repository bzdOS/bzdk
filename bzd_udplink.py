#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""bzd_udplink.py -- carry the HV debug channel over UDP/IPv4 (routed).

The host tools (hvdbg, triage, bzdctl, netcon, gdb-bridge, coredump-recv,
fbdump_recv, snapshot_net, ...) speak raw Ethernet frames on ethertypes
0x88B5..0x88B9 through AF_PACKET sockets. A router does not forward those.
The HV also accepts the very same frames as UDP datagrams to its own IPv4
address (dbgip.h). This daemon joins the two: it owns a persistent tap
interface (board.conf `tap`, default bzd0) and

  tap -> board   every debug-ethertype frame a tool writes on the tap is sent,
                 whole, as one datagram to board_ip:dbg_port;
  board -> tap   every datagram from board_ip is written into the tap, where
                 all the tools' raw sockets see it, exactly as on the wire.

With `transport = udp` in ~/.config/bzdk/board.conf, bzd_board.IFACE names
the tap, so the tools need no transport code of their own. The daemon binds
UDP dbg_port on this host too: before any host has spoken in a boot, the HV
sends its broadcast output (boot log, panic text) to U-Boot's `serverip` on
that port.

Nothing here is trusted: every frame is authenticated end to end by dbgauth
(the HV ignores anything else), the daemon only moves bytes.

    bzd_udplink.py [--conf FILE] [-v]         (normally: bzd-udplink.service)
    bzd_udplink.py --check                    one HELLO through the tap
"""
import argparse
import fcntl
import os
import select
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzd_board  # noqa: E402

TUNSETIFF = 0x400454CA
TUNSETPERSIST = 0x400454CB
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000
DBG_ETYPES = range(0x88B5, 0x88BA)
INNER_MAX = 1472          # one datagram, no fragmentation at a 1500 MTU


def open_tap(name):
    fd = os.open("/dev/net/tun", os.O_RDWR)
    fcntl.ioctl(fd, TUNSETIFF, struct.pack("16sH", name.encode(),
                                           IFF_TAP | IFF_NO_PI))
    fcntl.ioctl(fd, TUNSETPERSIST, 1)
    # No addresses, no IPv6 chatter: the tap only carries debug frames.
    subprocess.run(["sysctl", "-q", "-w",
                    f"net.ipv6.conf.{name}.disable_ipv6=1"], check=False)
    subprocess.run(["ip", "link", "set", name, "up"], check=True)
    return fd


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--conf", help="board.conf to use (default: bzd_board's)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="one auth handshake through the tap, then exit")
    a = ap.parse_args()
    if a.conf:
        os.environ["BZDK_BOARD_CONF"] = a.conf
    c = bzd_board.conf()
    board = (c["board_ip"], int(c["dbg_port"]))
    tap = c["tap"]

    if a.check:
        import hvauth
        ch = hvauth.Channel(tap)
        t0 = time.time()
        ok = ch.handshake(tries=5, wait=0.5)
        print(f"udplink check via {tap} -> {board[0]}:{board[1]}: "
              f"{'ok' if ok else 'NO ANSWER'} in {time.time() - t0:.3f}s")
        sys.exit(0 if ok else 1)

    fd = open_tap(tap)
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    us.bind(("", board[1]))
    print(f"bzd_udplink: {tap} <-> udp {board[0]}:{board[1]}", flush=True)

    n_up = n_down = n_big = n_foreign = 0
    t_stat = time.time()
    while True:
        r, _, _ = select.select([fd, us], [], [], 60)
        if fd in r:
            f = os.read(fd, 65536)
            if len(f) >= 14 and int.from_bytes(f[12:14], "big") in DBG_ETYPES:
                if len(f) > INNER_MAX:
                    n_big += 1
                else:
                    try:
                        us.sendto(f, board)
                        n_up += 1
                    except OSError as e:          # no route yet, etc.
                        if a.verbose:
                            print(f"send: {e}", flush=True)
        if us in r:
            try:
                d, src = us.recvfrom(65536)
            except OSError:
                d, src = b"", None
            if src and src[0] == board[0] and len(d) >= 14:
                os.write(fd, d)
                n_down += 1
            elif src:
                n_foreign += 1
        if a.verbose and time.time() - t_stat > 60:
            t_stat = time.time()
            print(f"up={n_up} down={n_down} too_big={n_big} "
                  f"foreign={n_foreign}", flush=True)


if __name__ == "__main__":
    main()
