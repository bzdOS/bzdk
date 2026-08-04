#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""orient.py -- "what is going on right now?", answered from the live board.

Run this FIRST in any session (ORIENTATION.md says so). It exists because the
expensive failures in this project have never been hard bugs -- they were
sessions spent debugging the wrong thing:

  * a stale image on the board while reading fresh source ("build identity"),
  * a serial port already held by another process, which makes a healthy
    channel look completely dead,
  * a guest shell parked in an unterminated quote, ditto,
  * a guest that is merely idle read as "wedged", because one sample of a
    counter cannot tell frozen from slow.

triage.py answers all of that for the HYPERVISOR in great detail. This is the
thinner, wider view: hypervisor liveness AND guest state AND host-side
preconditions, in one screen, in a few seconds. When anything here looks wrong,
go to triage.py for depth.

Read-only: it samples counters and asks the guest harmless questions. It never
writes to the board.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TTY = "/dev/ttyACM0"


def hdr(s):
    print("\n=== %s %s" % (s, "=" * max(0, 62 - len(s))))


def port_check():
    """A busy tty is the single cheapest thing to rule out, so do it first."""
    hdr("HOST-SIDE PRECONDITIONS")
    if not os.path.exists(TTY):
        print("  %s MISSING -- board off, or usb_debug grabbed the gadget" % TTY)
        print("    (see memory usb-debug-hijacks-ttyacm: blacklist usb_debug)")
        return False
    try:
        out = subprocess.run(["fuser", "-v", TTY], capture_output=True,
                             text=True, timeout=15)
        busy = (out.stdout + out.stderr).strip()
    except Exception:
        busy = ""
    if busy and TTY in busy:
        print("  %s is BUSY -- another process holds it:" % TTY)
        print("    " + busy.replace("\n", "\n    "))
        print("  STOP. Two readers steal each other's bytes and the channel")
        print("  will look dead. Kill the other process (by PID, not pkill -f).")
        return False
    print("  %s present and free" % TTY)
    return True


def hv_state():
    """Build identity + is each core actually moving (two samples, not one)."""
    hdr("HYPERVISOR")
    try:
        sys.path.insert(0, HERE)
        import hvdbg
        h = hvdbg.HV()
    except Exception as e:
        print("  cannot open the debug channel: %s: %s" % (type(e).__name__, e))
        print("  -> board may be in U-Boot. See SESSION-RULES.md R3 (chimpd).")
        return

    # Ask hvdbg for the heartbeat through its OWN api rather than reading a
    # hardcoded address: the breadcrumb map drifts (SESSION-RULES R4 -- a stale
    # address produces a confident, wrong "the image is old" verdict), and the
    # heartbeat in particular is served by the debug protocol, not by a fixed
    # DRAM word. flightrec's header base is stable and asserted at compile time
    # in hv_addrmap.h, so reading that one directly is safe.
    def bc(pa, n):
        try:
            return h.read_words(pa, n) or []
        except Exception:
            return []

    def hb():
        try:
            return h.heartbeat()
        except Exception:
            return None

    a, hb1 = bc(0x50012000, 4), hb()
    time.sleep(2.5)
    b, hb2 = bc(0x50012000, 4), hb()

    def verdict(x, y):
        if x is None or y is None:
            return "unreadable"
        return "MOVING" if x != y else "FROZEN"

    fr_a = a[1] if len(a) > 1 else None
    fr_b = b[1] if len(b) > 1 else None
    print("  CPU0 flightrec total : %s" % verdict(fr_a, fr_b))
    print("  CPU1 heartbeat       : %s  (debug core)" % verdict(hb1, hb2))
    if verdict(hb1, hb2) == "FROZEN":
        print("    CPU1 frozen means the debug core itself is stuck -- rare and")
        print("    serious; the guest can look fine while this is true.")
    print("\n  For build identity, core detail, virtio/GIC/stage-2 breadcrumbs")
    print("  and the event timeline, run:  python3 triage.py")


def guest_state():
    hdr("GUEST (FreeBSD)")
    try:
        sys.path.insert(0, HERE)
        import guest_sh
    except Exception as e:
        print("  guest_sh unavailable: %s" % e)
        return
    probes = [
        ("uname",   "uname -r"),
        ("uptime",  "uptime | sed 's/^ *//'"),
        ("root fs", "df -h / | tail -1"),
        ("mount",   "mount | head -1"),
        ("network", "ifconfig vtnet0 | grep -E 'inet |status'"),
    ]
    fd = None
    readonly = False
    try:
        fd = guest_sh._open()
        for label, cmd in probes:
            r = guest_sh.run(cmd, fd=fd, quiet=True, timeout=30)
            if r is None:
                print("  %-8s : no framed reply (channel lossy; retried)" % label)
            else:
                if "read-only" in r:
                    readonly = True
                first = [l for l in r.splitlines() if l.strip()]
                print("  %-8s : %s" % (label, " | ".join(first) if first else "(empty)"))
    except Exception as e:
        print("  guest probe failed: %s: %s" % (type(e).__name__, e))
        print("  -> guest may be at mountroot> or not booted. Console:")
        print("     python3 guest_sh.py 'echo hi'")
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except Exception:
                pass
    if readonly:
        print("\n  ROOT IS READ-ONLY -- so /etc/rc never finished, which means no")
        print("  sshd and nowhere writable. Almost always the filesystem is")
        print("  marked unclean. Fix (proven 2026-08-04):")
        print("     fsck_ffs -y /dev/vtbd0p3")
        print("     mount -u -o reload / && mount -u -o rw /")


def pointers():
    hdr("WHERE TO LOOK NEXT")
    for line in (
        "SESSION-RULES.md   operating rules R0-R7 -- MANDATORY, read before acting",
        "DEBUG_RULES.md     live-hardware debugging rules",
        "ROADMAP.md         v1 gate + what is still open",
        "WOW_FEATURES.md    section 0 = hard prohibitions (rule 7: do not hot-patch disks)",
        "triage.py          deep hypervisor state; ALWAYS prefer it over `gr`",
    ):
        print("  " + line)
    print("\n  Hard rules that bite hardest: never make the user press reset;")
    print("  one process on the tty at a time; per-PE registers read over the")
    print("  debug channel are CPU1's bank, not the guest's.")


if __name__ == "__main__":
    print("bzdOS Chimp -- live orientation")
    ok = port_check()
    if ok:
        hv_state()
        guest_state()
    pointers()
