#!/usr/bin/env python3
"""
loady_over_acm.py -- host-side loader for the bzdOS microkernel over U-Boot's
USB-OTG CDC-ACM console (/dev/ttyACM0), no board access needed to run this
file itself -- it drives the existing U-Boot on the Banana Pi M64.

Flow (see /opt/bzdos/microkernel/PROJECT.md):
  1. Wait for /dev/ttyACM0, fix the "fake regular file" ttyACM quirk, set
     115200 raw -echo.
  2. Catch the U-Boot `=>` prompt (Ctrl-C flood + newline probe).
  3. Send `loady <addr>`, wait for U-Boot's YMODEM 'C' poll byte (0x43).
  4. Hand the tty to `sb` (lrzsz) to send the payload via YMODEM. Python
     stops reading the tty while `sb` owns it.
  5. Send `go <addr>`.
  6. The payload re-enumerates the USB gadget (U-Boot's console gadget drops,
     the microkernel brings its own CDC-ACM up). Wait up to ~15s for
     /dev/ttyACM0 to reappear, reopen it, and stream everything to stdout
     and --log. Success = seeing the `BZDOS-MK-ALIVE` marker.

Usage:
    loady_over_acm.py <payload.bin> [--addr 0x42000000] [--log FILE]

Exit code 0 = ENUMERATED+ALIVE, non-zero for any other verdict.

This module's tty helpers (ensure_chardev/open_tty/rd/wr/catch_uboot) are
deliberate copies of the patterns already proven in
/opt/bzdos/devstand/supervisor.py (the 24/7 dev-stand supervisor) -- same
bug-fixes (e.g. the fake-chardev mknod fix), same style. Kept standalone
here (no import of supervisor.py) so this script also works stand-alone,
off the dev-stand box, for manual iteration.
"""
import os, sys, time, select, re, stat as _st, argparse, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bzd_board as B

TTY = B.CHIMP_TTY   # stable udev symlink → board's ttyACMn (survives renumbering)
_CSI = re.compile(rb'\x1b\[[0-9;?]*[a-zA-Z]')
ALIVE_MARK = b"BZDOS-MK-ALIVE"
SB_BIN = "/usr/bin/sb"
DEFAULT_ADDR = 0x42000000

# ---------------------------------------------------------------- port lock
# 2026-07-14: two closely-spaced scripts (a diagnostic probe, then dbg-boot.py
# run #1 whose tftpboot failed slowly, then dbg-boot.py run #2) each opened
# and hammered \r\x03 into the ACM port within seconds of each other. U-Boot's
# CDC-ACM gadget on this board is documented-fragile to exactly this kind of
# open/interrupt race (see chimp-m64-uboot-reset.md) -- the board vanished
# from lsusb entirely afterward. An flock serializes ALL open_tty() callers
# across processes: if a previous script is still mid-command, the next one
# blocks here instead of racing the port. flock auto-releases if the holder
# dies/crashes, so a stuck lock can't itself wedge future runs.
_PORT_LOCK_PATH = "/tmp/chimp-acm.lock"
_port_lock_fd = None

def acquire_port_lock(settle=0.5):
    """Block until we're the only process touching the ACM port, then wait
    `settle` seconds before returning (lets the USB stack settle if the
    previous holder just closed the fd) -- call this before open_tty().
    Idempotent WITHIN a process: flock is per-open-file-description, so
    re-opening a fresh lock fd on a second open_tty() call in the SAME
    process (e.g. the sb-handoff close/reopen pattern) would self-deadlock
    against our own still-held first lock -- skip re-acquiring if we
    already hold it."""
    global _port_lock_fd
    if _port_lock_fd is not None:
        return   # already held by this process
    import fcntl
    _port_lock_fd = os.open(_PORT_LOCK_PATH, os.O_CREAT | os.O_RDWR, 0o666)
    fcntl.flock(_port_lock_fd, fcntl.LOCK_EX)
    time.sleep(settle)

def release_port_lock():
    global _port_lock_fd
    if _port_lock_fd is not None:
        import fcntl
        fcntl.flock(_port_lock_fd, fcntl.LOCK_UN)
        os.close(_port_lock_fd)
        _port_lock_fd = None


def tftp_preflight(host=B.SRV_IP, port=69, tftp_root="/opt/bzdos/tftpboot", autostart=True):
    """Verify (or start) a TFTP server on the host BEFORE U-Boot's `tftpboot`
    is attempted. Rationale (2026-07-14 incident): a tftpboot against a dead
    server makes U-Boot retry internally for an unpredictable duration; if a
    script's own read-loop times out first while U-Boot is still retrying,
    the NEXT script's open+interrupt-spam races the still-busy CLI parser.
    Returns True if a TFTP server is confirmed listening (existing or
    freshly started via dnsmasq)."""
    import socket
    def listening():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind((host, port))
            s.close()
            return False   # we could bind -> nobody was listening
        except OSError:
            s.close()
            return True    # already bound -> a server IS listening
    if listening():
        return True
    if not autostart:
        return False
    log = open("/tmp/dnsmasq-tftp.log", "a")
    subprocess.Popen(
        ["dnsmasq", "--port=0", f"--interface={B.IFACE}", "--bind-interfaces",
         "--enable-tftp", f"--tftp-root={tftp_root}",
         "--no-daemon", "--log-facility=-"],
        stdout=log, stderr=subprocess.STDOUT,
    )
    for _ in range(25):
        if listening():
            return True
        time.sleep(0.2)
    return False

# ---------------------------------------------------------------- tty basics
# (patterns matched to supervisor.py: ensure_chardev/open_tty/rd/wr/catch_uboot)

def ensure_chardev():
    """Work around the board sometimes presenting /dev/ttyACM0 as a stale
    regular file instead of a character device -- recreate the chardev node
    from sysfs' `dev` major:minor. Same fix as supervisor.py."""
    try:
        if not _st.S_ISCHR(os.stat(TTY).st_mode):
            os.remove(TTY)
            mm = open(f"/sys/class/tty/{os.path.basename(TTY)}/dev").read().strip().split(":")
            os.mknod(TTY, _st.S_IFCHR | 0o660, os.makedev(int(mm[0]), int(mm[1])))
    except FileNotFoundError:
        pass
    except Exception as e:
        sys.stderr.write(f"[ensure_chardev] {e}\n")


def open_tty(nonblock=True):
    """Open TTY raw/-echo @115200. nonblock=False gives sb a plain blocking
    fd (sb does its own read()/write() and should not see EAGAIN).
    Serialized via acquire_port_lock() so overlapping script invocations
    can't race the ACM port (see the lock's docstring above)."""
    acquire_port_lock()
    ensure_chardev()
    import termios, fcntl
    # Open O_NONBLOCK first (never blocks on a dead/no-carrier gadget), then set
    # raw+CLOCAL via termios (NOT stty, which reopens blocking and hangs on a
    # zombie gadget). Clear O_NONBLOCK afterwards only if a blocking fd was asked.
    fd = os.open(TTY, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] &= ~(termios.IGNBRK|termios.BRKINT|termios.PARMRK|termios.ISTRIP|termios.INLCR|termios.IGNCR|termios.ICRNL|termios.IXON)
    a[1] &= ~termios.OPOST
    a[3] &= ~(termios.ECHO|termios.ECHONL|termios.ICANON|termios.ISIG|termios.IEXTEN)
    a[2] &= ~(termios.CSIZE|termios.PARENB); a[2] |= termios.CS8|termios.CLOCAL|termios.CREAD
    a[4] = termios.B115200; a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    if not nonblock:
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)
    return fd


def rd(fd, t):
    """Read for up to t seconds. Returns (bytes, dead) where dead=True means
    the fd errored (device probably went away)."""
    b = b""
    e = time.time() + t
    while time.time() < e:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                d = os.read(fd, 65536)
                if d:
                    b += d
            except OSError:
                return b, True
    return b, False


def wr(fd, d):
    for _ in range(5):
        try:
            os.write(fd, d)
            return
        except OSError:
            time.sleep(0.05)


def wait_present(timeout=None):
    t = time.time()
    while not os.path.exists(TTY):
        if timeout and time.time() - t > timeout:
            return False
        time.sleep(0.05)
    return True


def catch_uboot(fd, secs=8):
    """Ctrl-C flood + newline probe until we see '=>' (CSI-stripped)."""
    buf = b""
    t = time.time()
    while time.time() - t < secs:
        try:
            os.write(fd, b"\x03")
        except OSError:
            return False, buf
        b, dead = rd(fd, 0.05)
        buf += b
        if dead:
            return False, buf
        if b"=>" in _CSI.sub(b'', buf)[-80:]:
            os.write(fd, b"\r\n")
            b, _ = rd(fd, 1.0)
            buf += b
            if b"=>" in _CSI.sub(b'', buf)[-80:]:
                return True, buf
    return False, buf


# ---------------------------------------------------------------- core flow

def do_loady_and_go(tfd, save, payload, addr=DEFAULT_ADDR,
                     c_wait=10.0, sb_timeout=None,
                     open_tty_fn=open_tty, rd_fn=rd, wr_fn=wr,
                     exec_cmd=None):
    """
    Send `loady <addr>`, wait for the YMODEM 'C' poll, hand the tty to `sb`
    for the YMODEM transfer, then send `go <addr>`.

    tfd: open, non-blocking fd already sitting at the U-Boot '=>' prompt.
         This function CLOSES tfd (both to hand it to sb, and again at the
         end since U-Boot's gadget is expected to drop at `go`).
    save: callable(bytes) -- append raw bytes to the session/CLI log.
    Returns (ok: bool, msg: str).
    """
    if not os.path.exists(payload):
        return False, f"payload not found: {payload}"
    size = os.path.getsize(payload)
    if sb_timeout is None:
        # generous heuristic: minimum 30s, plus ~2s per KiB (very conservative
        # for a USB link -- payload is expected to be tiny, a few tens of KiB)
        sb_timeout = max(30, 10 + size // 512)

    wr_fn(tfd, f"loady 0x{addr:x}\r\n".encode())
    save(f"\n# [loady_over_acm: sent 'loady 0x{addr:x}']\n".encode())

    # Wait for U-Boot's ymodem 'C' (0x43) poll byte on FRESH reads only (the
    # echoed command text itself never contains an uppercase 'C').
    saw_c = False
    t0 = time.time()
    while time.time() - t0 < c_wait:
        b, dead = rd_fn(tfd, 0.2)
        if b:
            save(b)
            if b"\x43" in b:
                saw_c = True
                break
        if dead:
            return False, "loady-failed (tty died waiting for ymodem 'C')"
    if not saw_c:
        try:
            os.close(tfd)
        except OSError:
            pass
        return False, "loady-failed (no ymodem 'C' poll seen -- loady did not start)"

    # --- hand off to sb: stop reading, close our fd, open a FRESH BLOCKING
    # fd for sb's exclusive use. Two live fds reading the same character
    # device would race for the incoming bytes and corrupt the YMODEM
    # stream, so ours must be fully closed first.
    try:
        os.close(tfd)
    except OSError:
        pass

    sb_fd = open_tty_fn(False)  # nonblock=False: sb wants blocking read/write
    try:
        proc = subprocess.run(
            [SB_BIN, "--ymodem", "-v", payload],
            stdin=sb_fd, stdout=sb_fd, stderr=subprocess.PIPE,
            timeout=sb_timeout,
        )
    except subprocess.TimeoutExpired:
        return False, f"loady-failed (sb timed out after {sb_timeout}s)"
    except FileNotFoundError:
        return False, f"loady-failed ({SB_BIN} not found)"
    finally:
        try:
            os.close(sb_fd)
        except OSError:
            pass

    save(b"\n# [sb stderr]\n" + (proc.stderr or b"") + b"\n")
    if proc.returncode != 0:
        return False, f"loady-failed (sb exit={proc.returncode}, see sb stderr in log)"

    # Resume reading (non-blocking) to see U-Boot's transfer summary and the
    # '=>' prompt return, then send `go`.
    tfd2 = open_tty_fn(True)
    b, _ = rd_fn(tfd2, 3.0)
    save(b)
    # U-Boot's `go` does NOT synchronize caches (and this build has no
    # dcache/icache commands): loady writes the image through the D-cache and
    # a raw `go` executes stale garbage — verified twice via untouched DRAM
    # breadcrumbs + U-Boot exception-panic reboots. The caller should instead
    # loady an ELF to a staging address and pass exec_cmd="bootelf -p 0x...",
    # which loads segments to their link addresses AND flush_cache()es them.
    cmd = exec_cmd if exec_cmd else f"go 0x{addr:x}"
    wr_fn(tfd2, (cmd + "\r\n").encode())
    save(f"\n# [loady_over_acm: sent '{cmd}']\n".encode())
    time.sleep(0.2)
    b, _ = rd_fn(tfd2, 0.5)
    save(b)
    try:
        os.close(tfd2)
    except OSError:
        pass
    return True, "go sent"


def capture_reenum(save, stream_stdout=True,
                    reenum_timeout=15.0, alive_wait=20.0,
                    open_tty_fn=open_tty, rd_fn=rd):
    """
    Wait up to reenum_timeout seconds for /dev/ttyACM0 to reappear (the
    microkernel's own gadget coming up after U-Boot's dropped at `go`), then
    stream up to alive_wait seconds looking for BZDOS-MK-ALIVE.

    Returns one of: "ENUMERATED+ALIVE", "no re-enumeration",
    "enumerated, no ALIVE marker".
    """
    t0 = time.time()
    appeared = False
    while time.time() - t0 < reenum_timeout:
        if os.path.exists(TTY):
            appeared = True
            break
        time.sleep(0.1)
    if not appeared:
        save(b"\n# [no re-enumeration within timeout]\n")
        return "no re-enumeration"

    save(b"\n# [ttyACM re-enumerated -- capturing microkernel console]\n")
    time.sleep(0.3)  # let USB enumeration settle before opening
    try:
        kfd = open_tty_fn(True)
    except Exception as e:
        save(f"\n# [reopen failed: {e}]\n".encode())
        return "no re-enumeration"

    alive = False
    t1 = time.time()
    while time.time() - t1 < alive_wait:
        b, dead = rd_fn(kfd, 0.3)
        if b:
            save(b)
            if stream_stdout:
                try:
                    sys.stdout.buffer.write(b)
                    sys.stdout.flush()
                except Exception:
                    pass
            if ALIVE_MARK in b or ALIVE_MARK in _CSI.sub(b'', b):
                alive = True
        if dead:
            save(b"\n# [ttyACM dropped during capture]\n")
            break
    try:
        os.close(kfd)
    except OSError:
        pass
    return "ENUMERATED+ALIVE" if alive else "enumerated, no ALIVE marker"


# ---------------------------------------------------------------------- CLI

def main():
    ap = argparse.ArgumentParser(description="Load & run the bzdOS microkernel over U-Boot USB-OTG ACM console")
    ap.add_argument("payload", help="path to microkernel raw binary (objcopy -O binary)")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=DEFAULT_ADDR,
                     help="load/entry address (default 0x42000000)")
    ap.add_argument("--log", default=None, help="also write full session bytes to this file")
    ap.add_argument("--present-timeout", type=float, default=30.0,
                     help="seconds to wait for /dev/ttyACM0 to appear initially")
    ap.add_argument("--uboot-timeout", type=float, default=8.0,
                     help="seconds to spend catching the U-Boot '=>' prompt")
    ap.add_argument("--reenum-timeout", type=float, default=15.0,
                     help="seconds to wait for ttyACM re-enumeration after 'go'")
    ap.add_argument("--alive-timeout", type=float, default=20.0,
                     help="seconds to capture the re-enumerated console looking for BZDOS-MK-ALIVE")
    args = ap.parse_args()

    logf = open(args.log, "wb") if args.log else None

    def save(d):
        if isinstance(d, str):
            d = d.encode()
        if logf:
            logf.write(d)
            logf.flush()

    def verdict(v, ok):
        print(f"\n=== VERDICT: {v} ===")
        if logf:
            save(f"\n# VERDICT: {v}\n")
            logf.close()
        sys.exit(0 if ok else 1)

    if not wait_present(args.present_timeout):
        verdict("U-Boot-not-caught (ttyACM0 never appeared)", False)

    try:
        tfd = open_tty(True)
    except Exception as e:
        verdict(f"U-Boot-not-caught (open_tty failed: {e})", False)

    ok, buf = catch_uboot(tfd, args.uboot_timeout)
    save(buf)
    if not ok:
        try:
            os.close(tfd)
        except OSError:
            pass
        verdict("U-Boot-not-caught", False)
    print("[loady_over_acm] U-Boot prompt caught.")
    save(b"\n# [U-Boot prompt caught]\n")

    ok, msg = do_loady_and_go(tfd, save, args.payload, args.addr)
    print(f"[loady_over_acm] {msg}")
    if not ok:
        verdict(msg, False)

    v = capture_reenum(save, reenum_timeout=args.reenum_timeout, alive_wait=args.alive_timeout)
    verdict(v, v == "ENUMERATED+ALIVE")


if __name__ == "__main__":
    main()
