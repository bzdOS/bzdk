#!/usr/bin/env python3
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

USB_NODE = "/sys/bus/usb/devices/1-4"
ACM = "/dev/ttyACM0"
# Break-glass magic (must match usbacm.c bg_seq): 0x00 '~' B Z R S T 0x00
BG_SEQ = bytes([0x00, 0x7e, 0x42, 0x5a, 0x52, 0x53, 0x54, 0x00])
WEDGE_GRACE_S = 60          # EMAC dead this long -> reset
POLL_S = 5

def vidpid():
    try:
        with open(f"{USB_NODE}/idVendor") as f: v = f.read().strip()
        with open(f"{USB_NODE}/idProduct") as f: p = f.read().strip()
        return f"{v}:{p}"
    except OSError:
        return None

def emac_alive():
    """Liveness: does the HV answer a breadcrumb read? Tries a few times so a
    momentarily-busy EMAC (heavy guest boot) doesn't read as dead — the raw
    read flaps under load, which used to oscillate the state machine."""
    try:
        from hvdbg import HV
        hv = HV()
        for _ in range(3):
            if hv.read_words(0x50000f00, 2):
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
    loaded_once = False
    while True:
        vp = vidpid()
        if vp == "1f3a:efe8":
            raw = "UBOOT"
        elif vp == "1d6b:0010":
            raw = "HV_OK" if emac_alive() else "WEDGED"
        elif vp is None:
            raw = "GONE"
        else:
            raw = f"OTHER({vp})"

        # Debounce: a new state must persist 2 polls before we act/announce,
        # so a single flappy EMAC read never drives the machine. UBOOT is
        # taken immediately (the gadget VID:PID is unambiguous, no flap).
        if raw == last_state:
            cand = None; cand_n = 0
        elif raw == cand:
            cand_n += 1
        else:
            cand = raw; cand_n = 1
        confirmed = (raw == "UBOOT") or (cand_n >= 2)

        state = last_state
        if confirmed and raw != last_state:
            emit(f"state: {last_state} -> {raw}")
            last_state = raw
            state = raw
            cand = None; cand_n = 0
            if state != "WEDGED":
                wedge_since = None

        if state == "UBOOT":
            do_load()
            loaded_once = True
            time.sleep(POLL_S)
            continue

        if state == "WEDGED":
            if wedge_since is None:
                wedge_since = time.monotonic()
            elif time.monotonic() - wedge_since > WEDGE_GRACE_S:
                if break_glass():
                    wedge_since = None
                    time.sleep(20)   # let the WDOG fire + U-Boot come up
                    continue

        if state == "GONE":
            # only real "please power-cycle" case; nag at most every ~2 min
            if wedge_since is None or time.monotonic() - wedge_since > 120:
                emit("⚠ board OFF the USB bus — this is the ONE case needing a physical power-cycle")
                wedge_since = time.monotonic()

        time.sleep(POLL_S)

if __name__ == "__main__":
    main()
