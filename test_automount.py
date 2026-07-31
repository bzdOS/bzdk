#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause

"""test_automount.py — board-free test of reliable_load.auto_mount_root().

Unlike the C hosted tests in this directory, which exercise a REFERENCE
reimplementation of the logic (and so cannot catch a transcription error by
construction), this drives the REAL function: it hands auto_mount_root() a live
pty and lets it do real os.open/setraw/read/write against a scripted "guest".

Regression covered (soak13, 2026-07-31): the harness reported "mountroot> never
seen within timeout" on a guest that was sitting AT the prompt. FreeBSD prints
that prompt exactly once and then blocks in gets(), while musb_putc() drops the
newest byte whenever its TX ring is full (musb.c:1595) — and nothing drains
/dev/ttyACM0 during serial_load/verify. A single dropped print was therefore
unrecoverable by listening harder, which is why the function now POKES.
"""
import os, sys, time, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import reliable_load as R

LOG = []
R.C.slog = lambda m: (LOG.append(m), print("      LOG:", m))[1]


def run_case(name, guest_fn, expect_ok, timeout=8.0, poke_every=1.0):
    global LOG
    LOG = []
    master, slave = os.openpty()
    R.GUEST_TTY = os.ttyname(slave)
    R.ROOT_MOUNTFROM = "ufs:/dev/vtbd0p3"
    stop = threading.Event()
    t = threading.Thread(target=guest_fn, args=(master, stop), daemon=True)
    t.start()
    print(f"\n=== {name} ===")
    got = R.auto_mount_root(timeout=timeout, poke_every=poke_every)
    stop.set()
    time.sleep(0.2)
    os.close(master); os.close(slave)
    ok = (got == expect_ok)
    print(f"  -> returned {got}, expected {expect_ok}: {'PASS' if ok else 'FAIL'}")
    return ok, list(LOG)


def guest_prompt_now(m, stop):
    """Prints the prompt immediately, unprompted (the happy path)."""
    time.sleep(0.5)
    os.write(m, b"Mounting from ufs:/dev/vtbd0p3 failed with error 19.\r\n")
    os.write(m, b"mountroot> ")
    while not stop.is_set():
        d = _try_read(m)
        if d and b"vtbd0p3" in d:
            os.write(m, b"\r\nTrying to mount root from ufs:/dev/vtbd0p3\r\n")
        time.sleep(0.05)


def guest_prompt_dropped(m, stop):
    """The prompt print was LOST (musb ring drop). Guest is silent until it
    receives a CR, then re-asks — the real soak13 scenario."""
    while not stop.is_set():
        d = _try_read(m)
        if d:
            if b"vtbd0p3" in d:
                os.write(m, b"\r\nTrying to mount root from ufs:/dev/vtbd0p3\r\n")
            elif b"\r" in d:
                os.write(m, b"\r\nmountroot> ")
        time.sleep(0.05)


def guest_prompt_mangled(m, stop):
    """Prompt arrives with an INSERTED space and a DROPPED byte."""
    time.sleep(0.5)
    os.write(m, b"mountroo t> ")   # inserted space + missing 't'
    while not stop.is_set():
        d = _try_read(m)
        if d and b"vtbd0p3" in d:
            os.write(m, b"\r\nstart_init: trying /sbin/init\r\n")
        time.sleep(0.05)


def guest_silent(m, stop):
    """Guest is genuinely dead — must FAIL, and say so distinguishably."""
    while not stop.is_set():
        _try_read(m)
        time.sleep(0.05)


def guest_noisy_no_prompt(m, stop):
    """Console alive and chattering but never at mountroot> (e.g. panic loop).
    Must FAIL and dump the tail."""
    while not stop.is_set():
        _try_read(m)
        os.write(m, b"panic: cannot mount root\r\n")
        time.sleep(0.3)


def _try_read(m):
    import fcntl
    fl = fcntl.fcntl(m, fcntl.F_GETFL)
    fcntl.fcntl(m, fcntl.F_SETFL, fl | os.O_NONBLOCK)
    try:
        return os.read(m, 4096)
    except (BlockingIOError, OSError):
        return b""


results = []

ok, log = run_case("happy path: prompt arrives on its own", guest_prompt_now, True)
results.append(("passive detect", ok))
results.append(("passive detect did NOT poke",
                not any("poking" in m for m in log)))

ok, log = run_case("soak13 case: prompt print was DROPPED", guest_prompt_dropped, True)
results.append(("poke recovers dropped prompt", ok))
results.append(("poke was actually used",
                any("poking" in m for m in log)
                and any("after 1 poke" in m or "poke(s)), injecting" in m
                        or "poke(s), injecting" in m for m in log)))

ok, log = run_case("mangled prompt (inserted space + dropped byte)",
                   guest_prompt_mangled, True)
results.append(("whitespace/drop tolerant match", ok))

ok, log = run_case("genuinely silent guest", guest_silent, False, timeout=4.0)
results.append(("silent guest fails", ok))
results.append(("silent guest is distinguishable",
                any("produced NOTHING" in m for m in log)))

ok, log = run_case("noisy guest, never at prompt", guest_noisy_no_prompt, False,
                   timeout=4.0)
results.append(("noisy-no-prompt fails", ok))
results.append(("noisy failure dumps tail",
                any("console tail" in m for m in log)))

print("\n================ SUMMARY ================")
bad = 0
for name, ok in results:
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    bad += (not ok)
print(f"\n{len(results) - bad}/{len(results)} checks passed")
sys.exit(1 if bad else 0)
