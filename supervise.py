#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Self-healing board supervisor — keeps the HV loaded and recoverable with
ZERO human power-cycles. Prints ONE line per state transition / action (so a
Monitor watching it notifies only on meaningful events, never floods).

States (by USB VID:PID at /sys/bus/usb/devices/1-4 + EMAC liveness):
  UBOOT   1f3a:efe8            -> board in U-Boot download gadget: LOAD the build.
  HV_OK   1d6b:0010 + EMAC ans -> hypervisor running & inspectable: leave alone.
  WEDGED  1d6b:0010 + EMAC dead-> HV up but debug channel dead: after a grace
                                  period, break-glass reset over USB-ACM
                                  (magic bytes -> wdt_debug_hold -> WDOG reboot
                                  to U-Boot), which the next loop reloads.
  GONE    no gadget            -> off the USB bus entirely: the ONE case that
                                  still needs a physical power-cycle; we say so.

Recovery paths, in order of gentleness — none need a human:
  * U-Boot present  -> reliable_load (soft, no reset needed)
  * EMAC alive      -> reliable_load's own reboot_clean-over-EMAC
  * EMAC dead       -> USB-ACM break-glass (this file)
"""
import os, sys, time, subprocess
import bzd_board as B

USB_NODE = B.USB_NODE
ACM = B.GUEST_ACM_TTY
# Break-glass magic (must match usbacm.c bg_seq): 0x00 '~' B Z R S T 0x00
BG_SEQ = bytes([0x00, 0x7e, 0x42, 0x5a, 0x52, 0x53, 0x54, 0x00])
WEDGE_GRACE_S = 60          # EMAC dead this long -> reset
POLL_S = 5

def vidpid():
    # Delegates to bzd_board's shared reader. NOTE (discrepancy found while
    # centralizing): the original implementation here caught only OSError
    # around both sysfs reads; the shared helper catches more broadly per
    # field (matching reliable_load.py's original _rd()), so a
    # never-observed-live non-OSError read failure now returns None instead
    # of propagating. See bzd_board.usb_vidpid_str()'s docstring.
    return B.usb_vidpid_str(USB_NODE)

def emac_alive():
    """Liveness: does the HV answer a breadcrumb read? Tries a few times so a
    momentarily-busy EMAC (heavy guest boot) doesn't read as dead — the raw
    read flaps under load, which used to oscillate the state machine."""
    try:
        from hvdbg import HV
        hv = HV()
        for _ in range(3):
            if hv.read_words(B.VCONSOLE_HDR_PA, 2):
                return True
        return False
    except Exception:
        return False

def stamp():
    # no wall clock dependency issues — use monotonic-ish via subprocess date
    return subprocess.run(["date","+%H:%M:%S"],capture_output=True,text=True).stdout.strip()

def emit(msg):
    print(f"[{stamp()}] {msg}", flush=True)

def do_load():
    emit("→ loading build via reliable_load…")
    try:
        r = subprocess.run([sys.executable, "reliable_load.py", "--expect-vbk", "--cycles", "2"],
                           cwd="/opt/bzdos/microkernel", capture_output=True, text=True, timeout=400)
    except subprocess.TimeoutExpired:
        emit("  load TIMED OUT (>400s) — will retry on next UBOOT poll")
        return False
    except Exception as e:
        emit(f"  load ERROR {e!r} — will retry")
        return False
    ok = "LOADED & VERIFIED" in (r.stdout or "")
    last = (r.stdout or "").strip().splitlines()
    emit(f"  load {'OK' if ok else 'FAILED'}: {last[-1] if last else '(no output)'}")
    return ok

def break_glass():
    emit("→ EMAC dead + HV gadget: break-glass reset over USB-ACM…")
    try:
        fd = os.open(ACM, os.O_RDWR | os.O_NONBLOCK)
        for _ in range(3):                 # send a few times in case of drops
            for b in BG_SEQ:
                for _try in range(20):
                    try: os.write(fd, bytes([b])); break
                    except BlockingIOError: time.sleep(0.05)
                time.sleep(0.01)
            time.sleep(0.3)
        os.close(fd)
        emit("  break-glass sequence sent; WDOG should reboot to U-Boot in ~16s")
        return True
    except OSError as e:
        emit(f"  break-glass FAILED to open {ACM}: {e}")
        return False

def main():
    last_state = None
    cand = None; cand_n = 0          # debounce: require N consecutive readings
    wedge_since = None
    uboot_handled = False            # BUGFIX (2026-07-22): latches per raw UBOOT
    # sighting, independent of the debounced `state`. The old code gated
    # do_load() on the DEBOUNCED state staying "UBOOT", which lingers for one
    # extra poll after a successful load (debounce needs 2 confirmations to
    # leave UBOOT once entered) — that extra poll re-triggered do_load() on an
    # already-healthy HV. reliable_load then saw a non-U-Boot gadget, assumed
    # "need reset", and reset a perfectly fine board — an infinite self-
    # inflicted reload loop with no hardware fault involved. Fix: gate the
    # ACTION on the raw (undebounced) vidpid latch below; keep debounce only
    # for WEDGED/GONE, which are the states actually prone to flappy reads.
    while True:
        vp = vidpid()
        if vp == B.UBOOT_VIDPID:
            raw = "UBOOT"
        elif vp == B.HVCON_VIDPID:
            raw = "HV_OK" if emac_alive() else "WEDGED"
        elif vp is None:
            raw = "GONE"
        else:
            raw = f"OTHER({vp})"

        if raw != "UBOOT":
            uboot_handled = False    # reset the latch once we've left U-Boot

        # Debounce: a new state must persist 2 polls before we ANNOUNCE it or
        # act on WEDGED/GONE, so a single flappy EMAC read never drives those.
        if raw == last_state:
            cand = None; cand_n = 0
        elif raw == cand:
            cand_n += 1
        else:
            cand = raw; cand_n = 1
        confirmed = (raw == "UBOOT") or (cand_n >= 2)

        if confirmed and raw != last_state:
            emit(f"state: {last_state} -> {raw}")
            last_state = raw
            cand = None; cand_n = 0
            if last_state != "WEDGED":
                wedge_since = None

        if raw == "UBOOT" and not uboot_handled:
            do_load()
            uboot_handled = True
            time.sleep(POLL_S)
            continue

        if last_state == "WEDGED":
            if wedge_since is None:
                wedge_since = time.monotonic()
            elif time.monotonic() - wedge_since > WEDGE_GRACE_S:
                if break_glass():
                    wedge_since = None
                    time.sleep(20)   # let the WDOG fire + U-Boot come up
                    continue

        if last_state == "GONE":
            # only real "please power-cycle" case; nag at most every ~2 min
            if wedge_since is None or time.monotonic() - wedge_since > 120:
                emit("⚠ board OFF the USB bus — this is the ONE case needing a physical power-cycle")
                wedge_since = time.monotonic()

        time.sleep(POLL_S)

if __name__ == "__main__":
    main()
