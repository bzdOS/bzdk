#!/usr/bin/env python3
"""Run shell commands in the booted FreeBSD guest over its console.

The guest reached a real root shell on 2026-07-30, which makes it its own
administration and build host -- no x86 dev VM, no offline UFS2 surgery over
the debug channel. This is the thin wrapper that makes that usable from here.

The console drops/interleaves bytes (long-documented), so output is framed
between two unique markers and everything outside them is discarded. The
markers are echoed by the shell itself, so a partially-corrupted line in the
middle is visible rather than silently swallowed.

Usage:
    python3 guest_sh.py 'uname -a'
    python3 guest_sh.py -f script.sh      # feed a file line by line
    import guest_sh; guest_sh.run("id")
"""
import os
import sys
import termios
import time

TTY = "/dev/ttyACM0"
PROMPT = b"# "


def _open():
    fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0
    a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


_echo_off = False


def _quiet_the_shell(fd):
    """Turn OFF the guest tty's own echo, once per session.

    With echo on, the shell repeats the whole command line -- including this
    module's frame markers -- so the marker search finds the echo instead of
    the output. Cheaper and far more reliable than trying to out-parse it."""
    global _echo_off
    if _echo_off:
        return
    _send(fd, "stty -echo\r")
    _drain_quiet(fd, quiet_for=0.4, cap=6.0)
    _echo_off = True


def _drain(fd, dur):
    t0 = time.time()
    buf = b""
    while time.time() - t0 < dur:
        try:
            c = os.read(fd, 4096)
            if c:
                buf += c
        except BlockingIOError:
            pass
        time.sleep(0.03)
    return buf


def _drain_quiet(fd, quiet_for=0.5, cap=15.0):
    """Read until the line goes quiet for `quiet_for`, or `cap` elapses.

    A fixed-duration drain is not enough here: this console replays a
    surprising amount of buffered backlog, and leftovers from a previous
    command land in the middle of the next one's frame."""
    t0 = time.time()
    last = time.time()
    buf = b""
    while time.time() - t0 < cap:
        try:
            c = os.read(fd, 4096)
        except BlockingIOError:
            c = b""
        if c:
            buf += c
            last = time.time()
        elif time.time() - last >= quiet_for:
            break
        time.sleep(0.03)
    return buf


def _send(fd, s):
    """Byte-at-a-time with a small gap: the CDC-ACM gadget drops input that
    arrives faster than the guest's tty layer consumes it."""
    for ch in s.encode():
        for _ in range(80):
            try:
                os.write(fd, bytes([ch]))
                break
            except BlockingIOError:
                time.sleep(0.02)
        time.sleep(0.012)


def run(cmd, timeout=25.0, fd=None, quiet=False):
    """Run one command, return its output as text (markers stripped)."""
    own = fd is None
    if own:
        fd = _open()
    try:
        _quiet_the_shell(fd)
        _drain_quiet(fd, quiet_for=0.4, cap=8.0)   # discard the backlog
        tag = "M%d" % (int(time.time() * 1000) % 1000000)
        beg, end = f"-{tag}-BEG-", f"-{tag}-END-"
        # printf rather than echo: no trailing-newline or escape surprises.
        _send(fd, f"printf '%s\\n' {beg}; {cmd}; printf '%s\\n' {end}\r")
        out = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            out += _drain(fd, 1.0)
            if end.encode() in out:
                break
        txt = out.decode("utf-8", "replace")
        i, j = txt.find(beg), txt.rfind(end)
        body = txt[i + len(beg):j] if (i >= 0 and j > i) else txt
        body = body.strip("\r\n")
        if not quiet:
            print(body, flush=True)
        return body
    finally:
        if own:
            os.close(fd)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "-f":
        fd = _open()
        try:
            for line in open(sys.argv[2]):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                print(f"\n=== {line} ===", flush=True)
                run(line, fd=fd)
        finally:
            os.close(fd)
    elif len(sys.argv) >= 2:
        run(" ".join(sys.argv[1:]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
