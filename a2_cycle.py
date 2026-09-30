#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""a2_cycle.py — measure ROADMAP A2's DoD: guest-initiated clean shutdown, N times.

    "shutdown -p now in the guest -> EL2 catches PSCI SYSTEM_OFF -> clean warm
     reset -> the next boot finds the filesystem CLEAN, with no fsck repair,
     10 cycles in a row."

WHY THIS IS NOT soak.py. soak.py is the T1 harness and measures a different
thing: it *reloads* the board (catch U-Boot -> TFTP -> bootelf) and counts
survived break-glass resets. Nothing in it ever asks the GUEST to shut itself
down, so it cannot say anything about fs_clean. A2 is specifically about the
guest-initiated path, so it needs its own loop. Everything else is reused:
chimpd.py --once for the reload, ssh (or the console) for the guest.

ONE CYCLE
  1. wait for guest userland (ssh answers)
  2. note the filesystem state going in
  3. `shutdown -p now`, with its acceptance VERIFIED -- never fire this blind.
     The console mangles roughly half of all command lines, and a scheduled
     shutdown that silently never arrived looks exactly like a hypervisor that
     ignored SYSTEM_OFF. That mistake cost a full diagnostic detour on
     2026-08-04: the PSCI ring showed 840 calls and not one SYSTEM_OFF, i.e.
     the guest had never issued it.
  4. wait for the board to drop to U-Boot (USB id flips to U-Boot's gadget)
  5. reload with chimpd.py --once
  6. wait for guest userland again and VERIFY: root mounted rw, no fsck repair
     in /var/log/messages, filesystem size unchanged
  7. record; repeat

A cycle counts as a PASS only if step 6 finds a clean filesystem. Anything else
is a fail, recorded with its reason, and the run continues so one bad cycle does
not hide the rest.

The report is written after EVERY cycle, so killing the harness mid-run still
leaves a complete record of the cycles that finished.

    python3 a2_cycle.py --cycles 2      # smoke test first, always
    python3 a2_cycle.py --cycles 10     # the actual DoD

NOTE ON THE TTY: chimpd owns /dev/ttyACM0 while it loads, and only one process
may hold it. This harness therefore talks to the guest over SSH, not the
console, and never runs concurrently with its own chimpd child.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

GUEST = "192.168.88.82"
KEY = "/root/.ssh/chimp_ed25519"
# ConnectTimeout 60, not 8: sshd in the guest does a reverse lookup of the
# host that the LAN resolver never answers (10 s per try, measured
# 2026-09-30), so the banner arrives ~27 s late and an 8 s timeout made
# every soak probe fall back to the slow, lossy console.
SSH = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=60",
       "-o", "BatchMode=yes", "-i", KEY, f"root@{GUEST}"]
UBOOT_USB_ID = "1f3a:efe8"      # U-Boot's own gadget
HV_USB_ID = "1d6b:0010"         # the HV's CDC-ACM console


def sh(cmd, timeout=60):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"


def guest(cmd, timeout=60):
    return sh(SSH + [cmd], timeout)


def guest_up(timeout_s):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        rc, out, _ = guest("echo UP", 90)   # banner ~27 s, see SSH
        if rc == 0 and "UP" in out:
            return True
        time.sleep(10)
    return False


def usb_ids():
    _, out, _ = sh(["lsusb"], 20)
    return out


def wait_for_uboot(timeout_s):
    """The board dropping to U-Boot is the observable proof that EL2 honoured
    SYSTEM_OFF: the gadget identity changes."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if UBOOT_USB_ID in usb_ids():
            return True
        time.sleep(5)
    return False


def fs_state():
    """Facts about the guest filesystem, or None if unreachable."""
    # `grep -c` ALWAYS prints a count, and exits 1 when that count is zero. An
    # `|| echo 0` fallback therefore fires on the healthy path and appends a
    # SECOND zero, giving "0\n0" -- which compares unequal to "0" and failed a
    # cycle that had in fact passed. Fixed by asking for one number and parsing
    # the first integer out of whatever comes back, rather than string-matching
    # an exact "0".
    rc, out, _ = guest(
        'mount | head -1; echo "|"; df -k / | tail -1; echo "|"; '
        'grep -ci "WAS MODIFIED\\|UNEXPECTED SOFT UPDATE" /var/log/messages '
        '2>/dev/null; true', 60)
    if rc != 0:
        return None
    parts = [p.strip() for p in out.split("|")]
    if len(parts) < 3:
        return None
    repairs = 0
    for tok in parts[2].split():
        if tok.isdigit():
            repairs = int(tok)
            break
    return {"mount": parts[0], "df": parts[1], "repairs": repairs,
            "readonly": "read-only" in parts[0]}


def reload_board(log_path, timeout_s):
    with open(log_path, "wb") as f:
        try:
            subprocess.run([sys.executable, "-u",
                            os.path.join(HERE, "chimpd.py"), "--once"],
                           stdout=f, stderr=subprocess.STDOUT,
                           timeout=timeout_s)
        except subprocess.TimeoutExpired:
            return False
    return True


def one_cycle(n, args):
    rec = {"cycle": n, "t0": time.strftime("%Y-%m-%d %H:%M:%S")}

    if not guest_up(args.guest_timeout):
        rec.update(ok=False, why="guest never reached userland before the cycle")
        return rec
    before = fs_state()
    rec["before"] = before
    if before and before["readonly"]:
        # A read-only root means the previous stop was NOT clean -- report it
        # rather than shutting down again and hiding the evidence.
        rec.update(ok=False, why="root came up READ-ONLY (previous stop unclean)")
        return rec

    # Verified acceptance, never fire blind.
    rc, out, err = guest(
        "sync; nohup shutdown -p +1 >/tmp/a2.log 2>&1 & echo SCHEDULED", 60)
    if "SCHEDULED" not in out:
        rec.update(ok=False, why=f"could not schedule shutdown: {out or err}")
        return rec
    rc, out, _ = guest("sleep 3; cat /tmp/a2.log", 60)
    rec["shutdown_ack"] = out
    if "Shutdown at" not in out:
        rec.update(ok=False, why=f"shutdown did not confirm: {out}")
        return rec

    if not wait_for_uboot(args.off_timeout):
        rec.update(ok=False,
                   why="board never dropped to U-Boot -- SYSTEM_OFF not honoured")
        return rec
    rec["reached_uboot"] = True

    log = os.path.join(args.outdir, f"cycle-{n:03d}-reload.log")
    if not reload_board(log, args.reload_timeout):
        rec.update(ok=False, why=f"reload timed out (see {log})")
        return rec

    if not guest_up(args.guest_timeout):
        rec.update(ok=False, why=f"guest never came back after reload (see {log})")
        return rec
    after = fs_state()
    rec["after"] = after
    if not after:
        rec.update(ok=False, why="guest reachable but filesystem state unreadable")
        return rec
    if after["readonly"]:
        rec.update(ok=False, why="root mounted READ-ONLY -> filesystem was dirty")
        return rec
    if after["repairs"]:
        rec.update(ok=False, why=f"fsck repaired something (count={after['repairs']})")
        return rec

    rec.update(ok=True, why="clean shutdown -> clean boot, root rw, no repair")
    return rec


def main(argv=None):
    ap = argparse.ArgumentParser(description="Measure ROADMAP A2's DoD")
    ap.add_argument("--cycles", type=int, default=2,
                    help="how many clean-shutdown cycles (DoD asks 10)")
    ap.add_argument("--outdir", default=os.path.join(HERE, "a2-logs"))
    ap.add_argument("--guest-timeout", type=int, default=420,
                    help="seconds to wait for guest userland")
    ap.add_argument("--off-timeout", type=int, default=300,
                    help="seconds to wait for the board to reach U-Boot")
    ap.add_argument("--reload-timeout", type=int, default=1800,
                    help="seconds for one chimpd --once reload")
    args = ap.parse_args(argv)

    os.makedirs(args.outdir, exist_ok=True)
    report = {"started": time.strftime("%Y-%m-%d %H:%M:%S"),
              "requested_cycles": args.cycles, "cycles": []}
    path = os.path.join(args.outdir, "a2-report.json")

    streak = 0
    for n in range(1, args.cycles + 1):
        print(f"\n━━━ A2 cycle {n}/{args.cycles} ━━━", flush=True)
        rec = one_cycle(n, args)
        rec["t1"] = time.strftime("%Y-%m-%d %H:%M:%S")
        report["cycles"].append(rec)
        streak = streak + 1 if rec["ok"] else 0
        report["best_streak"] = max(report.get("best_streak", 0), streak)
        report["passes"] = sum(1 for c in report["cycles"] if c["ok"])
        # Written every cycle: a killed run still leaves a valid record.
        with open(path, "w") as f:
            json.dump(report, f, indent=2)
        print(f"  {'PASS' if rec['ok'] else 'FAIL'}: {rec['why']}", flush=True)

    print(f"\nA2: {report['passes']}/{args.cycles} clean cycles, "
          f"best streak {report.get('best_streak', 0)}")
    print(f"DoD (10 in a row): "
          f"{'CLOSED' if report.get('best_streak', 0) >= 10 else 'not yet'}")
    print(f"report: {path}")
    return 0 if report["passes"] == args.cycles else 1


if __name__ == "__main__":
    sys.exit(main())
