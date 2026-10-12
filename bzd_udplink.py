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
that port, and while no host is talking to it, an ANNOUNCE every 30 s and on
every address change (dbgip.h).

`board_ip = auto` (the HV takes its address by DHCP): the board address is
learned from the datagrams themselves. One from an address that is not the
current board's is a candidate -- never trusted as such: the daemon sends a
HELLO to it and adopts the address only when the CHAL that comes back
carries a valid proof of the shared key (hvauth.check_chal()), at most one
try per candidate every VERIFY_EVERY_S. The adopted address is written to
~/.config/bzdk/board.ip (bzd_board.board_ip() reads it) and used from then
on; a later lease change is followed the same way. Until the first address
is known, the last recorded one, else board.conf's uboot_ip, is used.

Nothing here is trusted: every frame is authenticated end to end by dbgauth
(the HV ignores anything else), the daemon only moves bytes; the address
check above only decides where to send them.

    bzd_udplink.py [--conf FILE] [-v]         (normally: bzd-udplink.service)
    bzd_udplink.py --check                    one HELLO through the tap
    bzd_udplink.py --selftest                 board-free test of the above
"""
import argparse
import fcntl
import os
import secrets
import select
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzd_board  # noqa: E402
import hvauth  # noqa: E402

TUNSETIFF = 0x400454CA
TUNSETPERSIST = 0x400454CB
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000
DBG_ETYPES = range(0x88B5, 0x88BA)
INNER_MAX = 1472          # one datagram, no fragmentation at a 1500 MTU
VERIFY_EVERY_S = 10.0     # one HELLO per candidate address per this long
VERIFY_WAIT_S = 3.0       # how long a HELLO's CHAL is waited for
SWEEP_EVERY_S = 60.0     # auto: re-sweep the subnet this often until found


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


class Link:
    """The forwarding decisions, without the tap or the socket.

    sendto(data, (ip, port)) and tap_write(frame) do the I/O; from_tap()
    and from_udp() take what arrived. `auto`: learn the board address as
    described in the module docstring (needs `key`)."""

    def __init__(self, sendto, tap_write, board_ip, port, auto=False,
                 key=None, src_mac=b"\x02\x00\x00\x00\x00\x01",
                 remember=None, log=None, now=time.time):
        self.sendto, self.tap_write = sendto, tap_write
        self.board_ip, self.port = board_ip, port
        self.auto, self.key, self.src_mac = auto, key, bytes(src_mac)
        self.remember = remember or (lambda ip: None)
        self.log = log or (lambda m: None)
        self.now = now
        self.verified = False        # board_ip proved by a handshake
        self.pending = {}            # candidate ip -> (nonce, deadline)
        self.last_try = {}           # candidate ip -> time of last HELLO
        self.n = dict(up=0, down=0, too_big=0, foreign=0, no_board=0,
                      verify_sent=0, adopted=0, bad_proof=0)
        if auto and key is None:
            self.log("board_ip = auto needs the debug key "
                     f"({hvauth.key_file()}): will not learn addresses")

    def from_tap(self, f):
        if len(f) < 14 or int.from_bytes(f[12:14], "big") not in DBG_ETYPES:
            return
        if len(f) > INNER_MAX:
            self.n["too_big"] += 1
        elif not self.board_ip:
            self.n["no_board"] += 1
        else:
            try:
                self.sendto(f, (self.board_ip, self.port))
                self.n["up"] += 1
            except OSError as e:          # no route yet, etc.
                self.log(f"send: {e}")

    def from_udp(self, d, src):
        ip = src[0]
        if ip == self.board_ip and not (ip in self.pending and
                                        not self.verified):
            if len(d) >= 14:
                self.tap_write(d)
                self.n["down"] += 1
            return
        self.n["foreign"] += 1
        if not self.auto or self.key is None or len(d) < 14:
            return
        p = self.pending.get(ip)
        if p and self.now() < p[1]:
            try:
                r = hvauth.check_chal(self.key, d, p[0])
            except hvauth.AuthError as e:
                self.n["bad_proof"] += 1
                self.pending.pop(ip, None)
                self.log(f"{ip}: {e} -- not adopted")
                return
            if r is not None:
                self.pending.pop(ip, None)
                self.adopt(ip)
                return
        ann = hvauth.parse_announce(d)
        t = self.now()
        if t - self.last_try.get(ip, -1e9) < VERIFY_EVERY_S:
            return
        self.last_try[ip] = t
        nh = secrets.token_bytes(16)
        self.pending[ip] = (nh, t + VERIFY_WAIT_S)
        what = (f"announce (ip {ann['ip']}, {ann['mode']}, guest "
                f"{ann['guest_ip']})" if ann else "a datagram")
        self.log(f"{what} from {ip}: verifying with a handshake")
        try:
            self.sendto(hvauth.hello_frame(self.src_mac, nh), (ip, self.port))
            self.n["verify_sent"] += 1
        except OSError as e:
            self.log(f"send to {ip}: {e}")

    def sweep(self, candidates):
        """Ask every candidate with a HELLO; the one whose CHAL carries a
        valid proof is adopted by from_udp(). Announces cannot replace
        this: a router that only lets replies through from the board's
        subnet (stateful firewall, NAT) drops them, while the CHAL to our
        own HELLO is a reply and passes."""
        t = self.now()
        self.n["sweeps"] = self.n.get("sweeps", 0) + 1
        for ip in candidates:
            nh = secrets.token_bytes(16)
            self.pending[ip] = (nh, t + VERIFY_WAIT_S)
            self.last_try[ip] = t
            try:
                self.sendto(hvauth.hello_frame(self.src_mac, nh),
                            (ip, self.port))
                self.n["verify_sent"] += 1
            except OSError as e:
                self.log(f"send to {ip}: {e}")

    def adopt(self, ip):
        old, self.board_ip = self.board_ip, ip
        self.verified = True
        self.n["adopted"] += 1
        self.remember(ip)
        self.log(f"board address {old} -> {ip} (handshake verified)")


def sweep_candidates(c, first=None):
    """The last known address first, then every host of the board's subnet
    (board.conf gateway/netmask), at most a /22."""
    import ipaddress
    out = [first] if first else []
    try:
        net = ipaddress.ip_network(f"{c['gateway']}/{c['netmask']}",
                                   strict=False)
    except (KeyError, ValueError):
        return out
    if net.num_addresses > 1024:
        return out
    return out + [str(h) for h in net.hosts() if str(h) != first]


def _selftest():
    """Board-free: a fake HV (a real dbgauth CHAL computed here with a
    throw-away key) behind a fake socket."""
    key = secrets.token_bytes(32)
    sent, tapped, logs, learned = [], [], [], []
    clock = [1000.0]

    def chal_for(hello, k=key):
        nh = hello[14 + 6:14 + 22]
        sb, sess = secrets.token_bytes(16), 0x1234
        proof = hvauth._h(k, b"BZK1/chal", sb, struct.pack(">I", sess), nh)
        p = (hvauth.MAGIC + bytes([hvauth.T_CHAL, 0]) +
             struct.pack(">I", sess) + sb + nh + proof[:16])
        return b"\xff" * 6 + b"\x02\xba\xfb\x3b\xf5\x8c" + b"\x88\xb5" + p

    def announce(ip):
        p = (hvauth.MAGIC + bytes([hvauth.T_ANNOUNCE, 1]) +
             socket.inet_aton(ip) + socket.inet_aton("255.255.255.0") +
             socket.inet_aton("192.0.2.1") + bytes([3, 1]) +
             socket.inet_aton("192.0.2.50") + bytes(6))
        return b"\xff" * 6 + b"\x02\xba\xfb\x3b\xf5\x8c" + b"\x88\xb5" + p.ljust(46, b"\0")

    L = Link(lambda d, a: sent.append((d, a)), tapped.append, "192.0.2.7",
             34997, auto=True, key=key, remember=learned.append,
             log=logs.append, now=lambda: clock[0])
    dbg = b"\xff" * 6 + b"\x02" * 6 + b"\x88\xb5" + b"x" * 50
    # explicit board: both directions
    L.from_tap(dbg)
    assert sent[-1] == (dbg, ("192.0.2.7", 34997))
    L.from_tap(b"\xff" * 12 + b"\x08\x00" + b"x" * 50)        # not debug
    assert len(sent) == 1
    L.from_udp(dbg, ("192.0.2.7", 34997))
    assert tapped == [dbg]
    # an announce from a new address: not forwarded, a HELLO goes there
    L.from_udp(announce("192.0.2.23"), ("192.0.2.23", 34997))
    assert len(tapped) == 1 and L.board_ip == "192.0.2.7"
    hello, dst = sent[-1]
    assert dst == ("192.0.2.23", 34997) and hello[14 + 4] == hvauth.T_HELLO
    # a second datagram within VERIFY_EVERY_S does not send another HELLO
    L.from_udp(dbg, ("192.0.2.23", 34997))
    assert sent[-1][0] is hello and L.board_ip == "192.0.2.7"
    # a CHAL for another nonce, or from another address: nothing
    L.from_udp(chal_for(b"\0" * 14 + bytes(6) + bytes(16)), ("192.0.2.23", 34997))
    L.from_udp(chal_for(hello), ("192.0.2.99", 34997))
    assert L.board_ip == "192.0.2.7" and not learned
    # a CHAL with a wrong key: refused, logged
    L.from_udp(chal_for(hello, k=b"k" * 32), ("192.0.2.23", 34997))
    assert L.board_ip == "192.0.2.7" and L.n["bad_proof"] == 1
    # the next try after VERIFY_EVERY_S, answered properly: adopted
    clock[0] += VERIFY_EVERY_S + 1
    L.from_udp(announce("192.0.2.23"), ("192.0.2.23", 34997))
    hello = sent[-1][0]
    L.from_udp(chal_for(hello), ("192.0.2.23", 34997))
    assert L.board_ip == "192.0.2.23" and learned == ["192.0.2.23"]
    L.from_tap(dbg)
    assert sent[-1] == (dbg, ("192.0.2.23", 34997))
    L.from_udp(dbg, ("192.0.2.23", 34997))
    assert tapped[-1] == dbg and len(tapped) == 2
    # the old address is foreign now
    L.from_udp(dbg, ("192.0.2.7", 34997))
    assert len(tapped) == 2
    # a CHAL that arrives after the wait: not adopted
    clock[0] += VERIFY_EVERY_S + 1
    L.from_udp(dbg, ("192.0.2.40", 34997))
    hello = sent[-1][0]
    clock[0] += VERIFY_WAIT_S + 0.1
    L.from_udp(chal_for(hello), ("192.0.2.40", 34997))
    assert L.board_ip == "192.0.2.23"
    # explicit mode never learns
    E = Link(lambda d, a: sent.append((d, a)), tapped.append, "192.0.2.7",
             34997, auto=False, key=key, now=lambda: clock[0])
    n = len(sent)
    E.from_udp(announce("192.0.2.23"), ("192.0.2.23", 34997))
    assert len(sent) == n and E.board_ip == "192.0.2.7"
    # auto without a key: never learns
    N = Link(lambda d, a: sent.append((d, a)), tapped.append, None, 34997,
             auto=True, key=None, now=lambda: clock[0])
    N.from_udp(announce("192.0.2.23"), ("192.0.2.23", 34997))
    N.from_tap(dbg)
    assert len(sent) == n and N.board_ip is None and N.n["no_board"] == 1
    # announce parsing
    a = hvauth.parse_announce(announce("192.0.2.23"))
    assert a["ip"] == "192.0.2.23" and a["mode"] == "dhcp" and a["guest_ip"] == "192.0.2.50"

    # the same over real sockets on loopback: a fake HV at 127.0.0.2
    hs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    hs.bind(("127.0.0.2", 0))
    hv_port = hs.getsockname()[1]
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.bind(("127.0.0.1", 0))
    host = us.getsockname()
    got = []
    R = Link(us.sendto, got.append, None, hv_port, auto=True, key=key,
             remember=learned.append)
    hs.sendto(announce("127.0.0.2"), host)
    d, src = us.recvfrom(2048)
    R.from_udp(d, src)
    hs.settimeout(2)
    hello, back = hs.recvfrom(2048)
    assert hello[18] == hvauth.T_HELLO and back == host
    hs.sendto(chal_for(hello), host)
    d, src = us.recvfrom(2048)
    R.from_udp(d, src)
    assert R.board_ip == "127.0.0.2" and learned[-1] == "127.0.0.2"
    R.from_tap(dbg)
    assert hs.recvfrom(2048)[0] == dbg
    hs.sendto(dbg, host)
    R.from_udp(*us.recvfrom(2048))
    assert got == [dbg]
    hs.close()
    us.close()
    print("bzd_udplink selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--conf", help="board.conf to use (default: bzd_board's)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="one auth handshake through the tap, then exit")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        _selftest()
        return
    if a.conf:
        os.environ["BZDK_BOARD_CONF"] = a.conf
    c = bzd_board.conf()
    auto = c["board_ip"] == "auto"
    port = int(c["dbg_port"])
    tap = c["tap"]

    if a.check:
        ch = hvauth.Channel(tap)
        t0 = time.time()
        ok = ch.handshake(tries=5, wait=0.5)
        print(f"udplink check via {tap} -> {bzd_board.board_ip()}:{port}: "
              f"{'ok' if ok else 'NO ANSWER'} in {time.time() - t0:.3f}s")
        sys.exit(0 if ok else 1)

    fd = open_tap(tap)
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    us.bind(("", port))
    src_mac = open(f"/sys/class/net/{tap}/address").read().strip()
    link = Link(us.sendto, lambda f: os.write(fd, f), bzd_board.board_ip(),
                port, auto=auto, key=hvauth.load_key() if auto else None,
                src_mac=bytes.fromhex(src_mac.replace(":", "")),
                remember=bzd_board.remember_board_ip,
                log=lambda m: print(f"bzd_udplink: {m}", flush=True))
    print(f"bzd_udplink: {tap} <-> udp {link.board_ip}:{port}"
          f"{' (auto: learned from verified datagrams)' if auto else ''}",
          flush=True)

    t_stat = time.time()
    t_sweep, down_seen, up_seen = 0.0, 0, 0
    while True:
        # board_ip = auto: find the board by asking, not by waiting for its
        # announce (see Link.sweep). Sweep until a handshake proved an
        # address, and again when frames go up and nothing comes back.
        if auto and time.time() - t_sweep > SWEEP_EVERY_S:
            if link.verified and link.n["up"] > up_seen and \
                    link.n["down"] == down_seen:
                link.verified = False
                link.log(f"{link.board_ip} stopped answering: sweeping")
            up_seen, down_seen = link.n["up"], link.n["down"]
            if not link.verified:
                t_sweep = time.time()
                link.sweep(sweep_candidates(c, link.board_ip))
        r, _, _ = select.select([fd, us], [], [], 5)
        if fd in r:
            link.from_tap(os.read(fd, 65536))
        if us in r:
            try:
                d, src = us.recvfrom(65536)
            except OSError:
                d, src = b"", None
            if src:
                link.from_udp(d, src)
        if a.verbose and time.time() - t_stat > 60:
            t_stat = time.time()
            print(" ".join(f"{k}={v}" for k, v in link.n.items()), flush=True)


if __name__ == "__main__":
    main()
