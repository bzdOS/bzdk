#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""boot_streak.py — drive the "N clean boots in a row" gate, using the ONE
recovery path this project has actually proven.

WHY THIS EXISTS RATHER THAN `reliable_load.py --cycles N`. That is the obvious
tool and it does not work for a long streak: between cycles it asks the running
hypervisor to reboot itself over EMAC (`reboot_clean`), and that path is
documented-unreliable — it can fail from a perfectly healthy board (see the
reboot-clean-via-emac-unreliable note). Measured here 2026-08-26: three
consecutive cycles each ended `U-Boot gadget (1f3a:efe8) never appeared`, the
board dropped to `hv_alive=False`, and the streak counted zero.

What DOES work, every time it was used today, is board_ctl.force_to_uboot()'s
escalating ladder: already-at-U-Boot fast path, then hv.wdt_reset() over EMAC,
then the USB-ACM break-glass marker, then simply waiting for the board's own
hardware watchdog. None of those rungs needs a human. So each iteration here is
force_to_uboot() followed by a single-cycle reliable_load, which is exactly the
sequence that has been exercised repeatedly rather than a new mechanism.

Each iteration is ~4 minutes of wall clock (~1-2 min to reach U-Boot, ~2.5 min
to TFTP and verify), so a full 100 is ~6-7 hours. That cost is real and is the
reason this prints progress as it goes instead of only at the end.

The boot ledger (boot_ledger.py) is updated by reliable_load itself on every
clean load, so the cumulative streak this gate is about accrues without this
script recording anything of its own — deliberately, so there is one durable
counter rather than two that can disagree.

Usage:  python3 boot_streak.py [N]      (default 100)
Prints one line per cycle; a failure stops the run and says which cycle, since
a streak that continues past a failure is not a streak.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import board_ctl        # noqa: E402
import loady_over_acm   # noqa: E402  — release_port_lock(); see one_cycle()


def one_cycle(i, total):
    t0 = time.time()
    # Pass board_ctl's own log through rather than silencing it. Silencing it
    # was a real mistake on the first run of this script: a cycle can legitimately
    # spend minutes climbing the recovery ladder (each rung waits up to 120 s), and
    # with the log suppressed there is no way to tell 'waiting on rung 3' from
    # 'wedged' — which is exactly the distinction this project's operating rules
    # insist on making with two samples rather than one.
    fd = board_ctl.force_to_uboot(timeout=900)
    if fd is None:
        print("[%d/%d] FAILED: could not reach U-Boot" % (i, total), flush=True)
        return False
    os.close(fd)
    # RELEASE THE PORT LOCK, not just the tty fd. This cost a run to learn:
    # loady_over_acm holds the ACM serialization flock in a MODULE-GLOBAL
    # _port_lock_fd, and only release_port_lock() drops it — closing the tty fd
    # force_to_uboot() handed back does nothing to it. Without this, the
    # reliable_load subprocess below blocks forever waiting for a lock held by
    # its own parent, which presents exactly as the symptom that file's comment
    # warns about: "stops dead right after printing TFTP preflight and never
    # says why".
    loady_over_acm.release_port_lock()

    # Stream the child's output into our own log rather than capturing it. Also
    # learned the hard way in the same run: with the output captured there is no
    # way to see WHY a cycle is taking six minutes instead of two, and this
    # project's rules are explicit that "moving vs stuck" needs observation, not
    # inference. The cycle verdict is taken from the ledger-visible marker in
    # the streamed text via a tee.
    proc = subprocess.Popen([sys.executable, "reliable_load.py", "--cycles", "1"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    ok = False
    for line in proc.stdout:
        sys.stdout.write("        " + line)
        sys.stdout.flush()
        if "LOADED & VERIFIED" in line:
            ok = True
    proc.wait(timeout=1200)
    print("[%d/%d] %s  (%.0fs)" % (i, total, "clean" if ok else "FAILED",
                                   time.time() - t0), flush=True)
    return ok


def main():
    total = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    print("boot_streak: %d cycles, ~4 min each => ~%.1f h" %
          (total, total * 4 / 60.0), flush=True)
    for i in range(1, total + 1):
        if not one_cycle(i, total):
            print("boot_streak: STOPPED at cycle %d — the streak is %d, not %d"
                  % (i, i - 1, total), flush=True)
            return 1
    print("boot_streak: %d/%d clean" % (total, total), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
