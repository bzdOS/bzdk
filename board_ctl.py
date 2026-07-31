#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""board_ctl.py — single, observable board-state + recovery controller.

WHY THIS EXISTS (2026-07-27): every task-specific script this session
(chimpd.py, write_rootfs.py, various disk-repair one-offs) re-implemented its
own ad-hoc mix of "check if the device node exists", "try one reset method",
"hope it works" -- with no shared notion of what state the board is actually
in, no escalation ladder when the gentle reset method doesn't work, and
silent retry loops that give no visibility into whether they're progressing
or stuck. This module is the fix: one place that correlates ALL THREE
observable channels (USB-ACM device-node presence, EMAC heartbeat via
hvdbg's dbgtools_peek, and a bounded raw serial peek) into a single,
unambiguous state, and one escalating recovery ladder every other script
should call instead of hand-rolling reset logic again.

ROOT BUG THIS FIXES: loady_over_acm.wait_present() only checks "does the
device path exist right now" -- it does NOT wait for a disappear-then-
reappear transition. Since the device is usually ALREADY present (from
whatever was running before), calling it to "wait for a power-cycle" returns
instantly and callers end up racing a board that hasn't actually reset yet.
chimpd.py papers over this by retrying serial_load() in its own outer loop;
every other script either rediscovers this the hard way (this session, live,
repeatedly) or silently misbehaves. wait_for_power_cycle() below is the
correct primitive: it explicitly waits for absence THEN presence when the
device is already there, and just waits for presence otherwise.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loady_over_acm as L
from hvdbg import HV

# ── board states ─────────────────────────────────────────────────────────
NO_DEVICE = "NO_DEVICE"                 # /dev/ttyCHIMP doesn't exist at all
AT_UBOOT = "AT_UBOOT"                   # device present, responds to Ctrl-C+CR with "=>"
GUEST_OK = "GUEST_OK"                   # HV/EMAC alive, heartbeat climbing, no panic text
GUEST_CRASHED = "GUEST_CRASHED"         # HV/EMAC alive, but serial shows panic/reboot loop text
HV_ALIVE_UNKNOWN = "HV_ALIVE_UNKNOWN"   # HV/EMAC alive, heartbeat present but couldn't classify guest
UNRESPONSIVE = "UNRESPONSIVE"           # device present, EMAC dark, doesn't respond to U-Boot probe either

_PANIC_MARKERS = (b"panic", b"Rebooting", b"cpu_reset failed", b"Fatal")


def _log(msg, log=print):
    log(f"[board_ctl {time.strftime('%H:%M:%S')}] {msg}")


def _safe_catch_uboot(fd, secs):
    """loady_over_acm.catch_uboot() doesn't handle os.write() hitting a
    transient EAGAIN/BlockingIOError on the CDC-ACM gadget (observed live
    2026-07-27, right after a real re-enumeration -- the endpoint isn't
    immediately writable). Treat that as 'this attempt failed, caller
    should retry' rather than letting it crash the whole recovery ladder."""
    try:
        ok, _ = L.catch_uboot(fd, secs)
        return ok
    except BlockingIOError:
        return False


def _probe_uboot(fd, secs=4):
    """Cheap, SHORT version of loady_over_acm.catch_uboot -- just enough to
    tell if this is a live U-Boot prompt, not meant as the real catch."""
    return _safe_catch_uboot(fd, secs), b""


def _peek_serial(timeout=1.5):
    """Non-blocking, bounded raw read of whatever's already queued on the
    board's serial device -- does NOT send anything, just listens. Returns
    b"" if the device doesn't exist or nothing arrived."""
    try:
        fd = L.open_tty(True)
    except FileNotFoundError:
        return b""
    buf = b""
    t0 = time.time()
    try:
        while time.time() - t0 < timeout:
            try:
                d = os.read(fd, 65536)
                if d:
                    buf += d
            except BlockingIOError:
                time.sleep(0.05)
    finally:
        try:
            os.close(fd)
        except Exception:
            pass
    return buf


def board_state(hv=None, verbose=False, log=print):
    """Return one of the state constants above, by correlating device-node
    presence + EMAC heartbeat (dbgtools_peek, independent of dbgmon/gdbstub
    state) + a bounded raw serial peek. Never blocks longer than ~4s total.
    Pass an existing HV() instance via `hv` to avoid re-opening the raw
    socket on every call in a polling loop."""
    if not os.path.exists(L.TTY):
        if verbose:
            _log("no device node at all", log)
        return NO_DEVICE

    if hv is None:
        hv = HV()
    peek = hv.dbgtools_peek(tries=2, wait=0.3)

    if not peek:
        # EMAC dark. Could be genuinely at U-Boot (HV not loaded yet), or a
        # hard-wedged HV that's stopped answering anything. Disambiguate
        # with a real U-Boot probe (Ctrl-C + CR, looks for "=>").
        try:
            fd = L.open_tty(True)
        except FileNotFoundError:
            return NO_DEVICE
        try:
            ok, _ = _probe_uboot(fd, secs=4)
        finally:
            try:
                os.close(fd)
            except Exception:
                pass
        if ok:
            if verbose:
                _log("EMAC dark, U-Boot probe answered -> AT_UBOOT", log)
            return AT_UBOOT
        if verbose:
            _log("EMAC dark, U-Boot probe also silent -> UNRESPONSIVE", log)
        return UNRESPONSIVE

    # EMAC alive. Check for panic/reboot-loop text on the raw serial without
    # sending anything (guest console is independent of the EMAC debug
    # channel, so this is safe to check regardless of gdb_channel state).
    serial = _peek_serial(timeout=1.0)
    if any(m in serial for m in _PANIC_MARKERS):
        if verbose:
            _log(f"EMAC alive (hb={peek.get('heartbeat')}), "
                 f"panic markers seen on serial -> GUEST_CRASHED", log)
        return GUEST_CRASHED

    hb1 = peek.get("heartbeat")
    time.sleep(0.3)
    peek2 = hv.dbgtools_peek(tries=2, wait=0.3)
    hb2 = peek2.get("heartbeat") if peek2 else None
    if hb1 is not None and hb2 is not None:
        if hb2 > hb1:
            if verbose:
                _log(f"EMAC alive, heartbeat climbing ({hb1}->{hb2}) -> GUEST_OK", log)
            return GUEST_OK
        if verbose:
            _log(f"EMAC alive, heartbeat NOT climbing ({hb1}->{hb2}) "
                 f"-> HV_ALIVE_UNKNOWN (CPU1 alive but stuck in one iteration, "
                 f"or genuinely idle between polls)", log)
    return HV_ALIVE_UNKNOWN


def wait_for_power_cycle(timeout=3600, log=print):
    """The CORRECT 'wait for the board to power-cycle' primitive -- fixes
    loady_over_acm.wait_present()'s bug (see module docstring). If the
    device is currently present, first waits for it to disappear (proving a
    real disconnect/re-enumeration is happening), THEN waits for it to
    reappear. If it's already absent, just waits for presence. Returns True
    on success, False on timeout."""
    deadline = time.time() + timeout
    was_present = os.path.exists(L.TTY)
    if was_present:
        _log("device present now -- waiting for it to DISAPPEAR first "
             "(proves a real re-enumeration, not a stale node)", log)
        while os.path.exists(L.TTY):
            if time.time() > deadline:
                _log("device never disappeared -- no power-cycle happened", log)
                return False
            time.sleep(0.1)
        _log("device disappeared, now waiting for it to reappear", log)
    else:
        _log("device already absent -- waiting for it to appear", log)
    while not os.path.exists(L.TTY):
        if time.time() > deadline:
            _log("device never reappeared", log)
            return False
        time.sleep(0.1)
    _log("device present again -- power-cycle confirmed", log)
    return True


def force_to_uboot(timeout=600, log=print):
    """Escalating recovery ladder to get the board to a caught U-Boot
    prompt, from WHATEVER state it's currently in. Logs which rung it's on
    at every step (the thing that was missing and made recovery this
    session so hard to debug from outside). Returns an open fd positioned
    at a live '=>' U-Boot prompt on success (caller is responsible for
    closing it), or None on overall timeout.

    Ladder:
      0. Already at U-Boot? Return immediately (fast path).
      1. HV alive (any guest state)? Try hv.wdt_reset() (EMAC, gentlest,
         works even if the guest itself is unresponsive since it acts at
         the HV/watchdog-register level, not through the guest).
      2. Device present (any state)? Try the break-glass USB-ACM magic
         marker (works even without EMAC, but needs the GUEST's own ACM
         driver code to be running normally -- can fail if the guest is
         deep in a non-cooperative panic-print loop, as found live this
         session).
      3. Neither worked (or HV/device were already unresponsive) -- just
         wait for the board's own hardware watchdog to eventually fire on
         its own; this is the one method with no software dependency at
         all, just slower.
    Each rung, after acting, calls wait_for_power_cycle() with a bounded
    per-rung timeout before escalating to the next one.
    """
    deadline = time.time() + timeout
    hv = None

    state = board_state(verbose=True, log=log)
    if state == AT_UBOOT:
        _log("already at U-Boot, nothing to do", log)
        fd = L.open_tty(True)
        ok = _safe_catch_uboot(fd, 8)
        if ok:
            return fd
        os.close(fd)
        state = board_state(verbose=True, log=log)  # fall through to ladder if that somehow failed

    rung = 0
    while time.time() < deadline:
        rung += 1
        _log(f"=== recovery rung {rung}, current state={state} ===", log)

        if state == AT_UBOOT:
            # Caught it mid-ladder (e.g. the previous rung's power-cycle
            # landed but catch_uboot's own attempt happened to miss it, or
            # autostart raced us) -- try RIGHT NOW before doing anything
            # else. Sending break-glass/wdt_reset here would just throw
            # this window away (a real bug this ladder had on 2026-07-27:
            # it kept sending break-glass even after board_state() had
            # already confirmed AT_UBOOT, missing the window every time).
            fd = L.open_tty(True)
            if _safe_catch_uboot(fd, 8):
                _log("caught U-Boot immediately at top of rung", log)
                return fd
            os.close(fd)
            _log("was AT_UBOOT but catch attempt missed it -- continuing ladder", log)

        acted = False
        if state in (GUEST_OK, GUEST_CRASHED, HV_ALIVE_UNKNOWN):
            _log("HV/EMAC alive -- trying hv.wdt_reset() (gentlest)", log)
            try:
                if hv is None:
                    hv = HV()
                hv.wdt_reset()
                acted = True
            except Exception as e:
                _log(f"wdt_reset() raised {e!r}, moving on", log)
        if not acted or state != AT_UBOOT:
            _log("trying break-glass USB-ACM marker as well "
                 "(harmless if HV path above already worked)", log)
            try:
                fd = L.open_tty(True)
                payload = b"\x00~BZRST\x00"
                for byte in payload:
                    for _ in range(50):
                        try:
                            os.write(fd, bytes([byte]))
                            break
                        except BlockingIOError:
                            time.sleep(0.02)
                    time.sleep(0.005)
                os.close(fd)
                acted = True
            except FileNotFoundError:
                _log("device not present, can't break-glass right now", log)

        per_rung_timeout = min(120, max(10, deadline - time.time()))
        _log(f"waiting up to {per_rung_timeout:.0f}s for a power-cycle...", log)
        if wait_for_power_cycle(timeout=per_rung_timeout, log=log):
            _log("power-cycle detected, trying to catch U-Boot...", log)
            time.sleep(0.5)
            fd = L.open_tty(True)
            ok = _safe_catch_uboot(fd, 15)
            if ok:
                _log("U-Boot caught.", log)
                return fd
            os.close(fd)
            _log("power-cycled but couldn't catch U-Boot prompt (autostart "
                 "may have already booted past it) -- re-assessing", log)
        state = board_state(verbose=True, log=log)
        if state == AT_UBOOT:
            fd = L.open_tty(True)
            ok = _safe_catch_uboot(fd, 8)
            if ok:
                return fd
            os.close(fd)

    _log(f"gave up after {rung} rungs and {timeout}s total", log)
    return None


if __name__ == "__main__":
    for v in ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY"):
        os.environ.pop(v, None)
    st = board_state(verbose=True)
    print("current state:", st)
