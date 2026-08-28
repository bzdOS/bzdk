#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
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

# The STABLE udev symlink, not /dev/ttyACM0. The board's CDC-ACM node renumbers
# whenever USB re-enumerates -- observed live 2026-08-19 landing on ttyACM1 while
# this module still opened ttyACM0 and failed with a bare
# `OSError: [Errno 5] Input/output error`, which reads like a dead board or a
# hardware fault rather than a wrong filename. loady_over_acm.py has always used
# B.CHIMP_TTY for exactly this reason; this file was the last one hardcoding it.
import bzd_board as _B
TTY = _B.CHIMP_TTY
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


def ensure_shell(fd, tries=3):
    """Get to a shell prompt, logging in as root if the console is at `login:`.

    Needed after every boot, so it belongs here rather than in each caller --
    the soak harness in particular re-enters this on every cycle. Root has no
    password on this image, so `login: root` lands straight in a shell."""
    global _echo_off
    for _ in range(tries):
        _send(fd, "\r")
        out = _drain_quiet(fd, quiet_for=0.5, cap=8.0).decode("utf-8", "replace")
        if "# " in out and "login:" not in out[-40:]:
            return True
        if "login:" in out:
            _send(fd, "root\r")
            # Marker-driven, not silence-driven [MEASURED 2026-08-27: two
            # reload-night failures had `reboot` land at the Password prompt
            # because a fixed quiet-gap elapsed while getty was still
            # printing the MOTD]. getty echoes "Password:" when it wants the
            # (empty) password; wait for THAT, not for a clock.
            pw = _drain_quiet(fd, quiet_for=0.8, cap=25.0)
            if b"Password:" not in pw:
                # Some images skip the password prompt entirely on empty
                # password; only treat silence as success if a prompt showed.
                if b"# " not in pw:
                    continue
            else:
                _send(fd, "\r")
            got = _drain_quiet(fd, quiet_for=0.8, cap=25.0).decode("utf-8", "replace")
            if "# " in got:
                _echo_off = False       # fresh shell: its echo is on again
                return True
    return False


def _abort_partial_line(fd):
    """Discard whatever the shell is still waiting to finish reading.

    This console drops AND inserts bytes, so a fair fraction of command lines
    arrive mangled. That is usually harmless (the command just fails), but when
    the damage lands inside a quote, sh switches to its continuation prompt and
    swallows every command sent afterwards as more of the same unterminated
    string. The channel then looks completely dead -- no markers, no echo, no
    prompt -- while the guest itself is perfectly healthy and its timer, block
    layer and flight recorder all keep running.

    That failure mode is indistinguishable from a wedged guest from the host
    side, and it cost most of a session's probes: `md5 /sbin/growfs` was
    declared hung (and its binary suspected corrupt) when the truth was that a
    mangled line three commands earlier had parked the shell in a quote. INTR
    is what discards a partial line, so send it before every command and start
    from a known-clean line editor."""
    for _ in range(2):
        _send(fd, "\x03")
        time.sleep(0.2)
    _drain_quiet(fd, quiet_for=0.3, cap=3.0)


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


def _run_once(cmd, timeout, fd):
    _abort_partial_line(fd)
    ensure_shell(fd)
    _quiet_the_shell(fd)
    _drain_quiet(fd, quiet_for=0.4, cap=8.0)   # discard the backlog
    tag = "M%d" % (int(time.time() * 1000) % 1000000)
    beg, end = f"-{tag}-BEG-", f"-{tag}-END-"
    # Send the markers SPLIT BY AN EMPTY QUOTE PAIR: sh concatenates the word
    # and prints "-tag-BEG-", but the line the tty echoes back still contains
    # "-tag-BE''G-", which does not match what we search for. So the frame can
    # only ever be found in real command OUTPUT, never in the echo.
    #
    # This replaces relying on `stty -echo`: that silently does nothing when its
    # own command line is one of the mangled ones, and the old code marked echo
    # as disabled regardless. The result framed the echo instead of the output
    # and returned the command text back as if it were the answer -- which is
    # far worse than no reply, because it looks like a successful call.
    beg_src = f"-{tag}-BE''G-"
    end_src = f"-{tag}-EN''D-"
    # printf rather than echo: no trailing-newline or escape surprises.
    _send(fd, f"printf '%s\\n' {beg_src}; {cmd}; printf '%s\\n' {end_src}\r")
    out = b""
    t0 = time.time()
    while time.time() - t0 < timeout:
        out += _drain(fd, 1.0)
        if end.encode() in out:
            break
    txt = out.decode("utf-8", "replace")
    i, j = txt.find(beg), txt.rfind(end)
    if i < 0 or j <= i:
        return None                            # frame never arrived intact
    return txt[i + len(beg):j].strip("\r\n")


def run(cmd, timeout=25.0, fd=None, quiet=False, tries=4):
    """Run one command, return its output as text (markers stripped).

    Retries on a missing frame rather than returning the partial garbage it
    scraped: with a channel this lossy a single attempt succeeds only about half
    the time, and every caller was otherwise obliged to build its own retry
    loop (several did, slightly differently, and one of them mistook the
    resulting silence for a hung guest). Returns "" only when the command
    genuinely produced no output; None if the channel never framed a reply."""
    own = fd is None
    if own:
        fd = _open()
    try:
        for _ in range(tries):
            body = _run_once(cmd, timeout, fd)
            if body is not None:
                if not quiet:
                    print(body, flush=True)
                return body
            time.sleep(0.5)
        if not quiet:
            print("!! no framed reply after %d tries" % tries, flush=True)
        return None
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
