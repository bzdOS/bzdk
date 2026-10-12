#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""uboot_nc.py -- U-Boot's prompt over netconsole (UDP 6666), no USB, no UART.

The eMMC U-Boot (u-boot-nc.itb) has CONFIG_NETCONSOLE and its env routes
stdin/stdout to `nc` (ncip = this host). With bootdelay >= 1 (open the
window with `uboot_maint.py open`), a key arriving over netconsole during
the countdown stops autoboot. This tool floods Ctrl-C at the board's
netconsole port until the prompt shows, then runs commands one by one,
each answered by the next prompt, logging everything it hears.

    uboot_nc.py listen [--secs N]                 just log what U-Boot says
    uboot_nc.py run FILE [--catch-secs N] [--log F] [--tail-secs N]

FILE: one U-Boot command per line (`#` comments). A line starting with `!`
is sent without waiting for a prompt (for `boot`/`run bzdk_boot` that do
not come back); the tool then logs for --tail-secs. Nothing here saves the
environment unless a line says `saveenv`.

Start `run` BEFORE the board resets (e.g. then `ssh guest shutdown -r now`):
the countdown is only bootdelay seconds. The host's own 6666 is used both
ways, so U-Boot's replies match the conntrack entry our Ctrl-C flood
creates and pass the host firewall.
"""
import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzd_board  # noqa: E402

PORT = 6666
PROMPT = b"=> "


class NC:
    def __init__(self, log=None):
        self.board = (bzd_board.UBOOT_IP, PORT)   # U-Boot: static ipaddr
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("", PORT))
        self.log = open(log, "ab") if log else None
        self.buf = b""

    def _out(self, d):
        sys.stdout.buffer.write(d)
        sys.stdout.flush()
        if self.log:
            self.log.write(d)
            self.log.flush()

    def recv(self, timeout):
        self.s.settimeout(max(0.001, timeout))
        try:
            d, src = self.s.recvfrom(4096)
        except (socket.timeout, OSError):
            return b""
        if src[0] != self.board[0]:
            return b""
        self._out(d)
        self.buf += d
        return d

    def send(self, data):
        # netconsole's input buffer is CONFIG_NETCONSOLE_BUFFER_SIZE (512)
        # bytes: feed long lines in small pieces, giving the shell time.
        for i in range(0, len(data), 128):
            self.s.sendto(data[i:i + 128], self.board)
            if len(data) > 128:
                self.recv(0.03)

    def catch(self, secs):
        """Flood Ctrl-C until a prompt is seen; True if caught."""
        t_end = time.time() + secs
        self.buf = b""
        while time.time() < t_end:
            self.send(b"\x03")
            self.recv(0.05)
            if PROMPT in self.buf[-64:] and b"autoboot" not in self.buf[-16:]:
                # make sure it is a quiet prompt, not the countdown line
                self.buf = b""
                self.send(b"\n")
                t1 = time.time() + 2
                while time.time() < t1:
                    self.recv(0.2)
                    if self.buf.endswith(PROMPT):
                        return True
        return False

    def cmd(self, line, timeout=60):
        self.buf = b""
        self.send(line.encode() + b"\n")
        t_end = time.time() + timeout
        while time.time() < t_end:
            self.recv(0.5)
            if self.buf.endswith(PROMPT):
                return self.buf
        return None

    def listen(self, secs):
        t_end = time.time() + secs
        while time.time() < t_end:
            self.recv(0.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    l = sub.add_parser("listen")
    l.add_argument("--secs", type=float, default=120)
    l.add_argument("--log")
    r = sub.add_parser("run")
    r.add_argument("file")
    r.add_argument("--catch-secs", type=float, default=300)
    r.add_argument("--tail-secs", type=float, default=60)
    r.add_argument("--cmd-timeout", type=float, default=90)
    r.add_argument("--log")
    a = ap.parse_args()

    nc = NC(a.log)
    if a.cmd == "listen":
        nc.listen(a.secs)
        return
    lines = [x.rstrip("\n") for x in open(a.file)]
    lines = [x for x in lines if x.strip() and not x.lstrip().startswith("#")]
    if not nc.catch(a.catch_secs):
        sys.exit("\n[uboot_nc] no prompt caught")
    print("\n[uboot_nc] prompt caught", flush=True)
    for line in lines:
        if line.startswith("!"):
            nc.send(line[1:].encode() + b"\n")
            nc.listen(a.tail_secs)
            return
        if nc.cmd(line, a.cmd_timeout) is None:
            sys.exit(f"\n[uboot_nc] no prompt after: {line}")
    print("\n[uboot_nc] script done, prompt left open (BOOT_RETRY reboots "
          "it after 120 s idle)", flush=True)


if __name__ == "__main__":
    main()
