#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""rescue_env.py — catch the U-Boot prompt and put bootcmd/boothv back.

WHY: on 2026-09-24 the U-Boot environment was edited to arm the SoC
watchdog from bootcmd, and the board was lost the same minute. This restores
the last environment the board is known to have booted from, byte-exact,
which is the right first move whenever a boot-chain edit is suspect: undo
the edit, then reason. (The 2026-09-25 re-read of the logs showed the edit
was NOT the killer -- in every loss bootcmd had not run at all; see
docs/sessions/2026-09-23-board-dark-root-cause.md, "Correction". The tool
stays, because the next boot-chain edit will want the same undo.)

The prompt exists only for the bootdelay window (3 s) before bootcmd runs,
and chimpd missed it by 12 s on its first try, so this does nothing but win
that race: poll hard for the node, grab it the instant it appears, flood
Ctrl-C, then type.

Run it BEFORE power-cycling the board, and stop chimpd first -- two readers
on the same tty steal each other's bytes and neither gets a prompt.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loady_over_acm as L

# Byte-exact values from uboot.env.backup-2026-09-24-pre-wdt, the last
# environment the board is known to have booted (01:39, before the edit).
GOOD = {
    "bootcmd": ("for i in 1 2 3 4 5 6 7 8 9 10; do run boothv; "
                "echo [bootcmd] retry $i; sleep 3; done; "
                "echo [bootcmd] gave up; mw.l 0x1c20cb4 1; "
                "mw.l 0x1c20cb8 0xb1; mw.l 0x1c20cb0 0x14af; "
                "echo [wdreset] SoC watchdog armed"),
    "boothv": ("setenv autostart no; tftpboot 0x4a000000 bananapi-min.dtb "
               "&& tftpboot 0x44000000 kernel "
               "&& tftpboot 0x48000000 microkernel-dbg.uimg "
               "&& setenv autostart yes && bootm 0x48000000"),
}


def main():
    deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 900)
    print(f"waiting for {L.TTY} — power-cycle the board now", flush=True)

    # Tight poll: the whole budget is one bootdelay. Anything that sleeps in
    # units of seconds here has already lost.
    fd = None
    while time.time() < deadline:
        if os.path.exists(L.TTY):
            try:
                fd = L.open_tty()
                break
            except Exception:
                pass          # node there but not openable yet; keep hammering
        time.sleep(0.02)
    if fd is None:
        sys.exit("device never appeared")
    print("node open, flooding Ctrl-C for the prompt", flush=True)

    ok, _ = L.catch_uboot(fd, 12)
    if not ok:
        os.close(fd)
        sys.exit("no U-Boot prompt caught")
    print("U-Boot prompt caught", flush=True)

    def send(line, settle=0.5):
        os.write(fd, (line + "\n").encode())
        time.sleep(settle)
        try:
            out = os.read(fd, 65536).decode("latin1", "replace")
        except BlockingIOError:
            out = ""
        return out

    for name, value in GOOD.items():
        # Single quotes: the values contain ';' and '$', which hush would
        # otherwise act on while the variable is being SET rather than run.
        print(send(f"setenv {name} '{value}'")[-200:], flush=True)
    print(send("saveenv", 4.0)[-400:], flush=True)
    print(send("printenv bootcmd", 1.5)[-400:], flush=True)
    print("done — leaving the board AT THE PROMPT, not booting it", flush=True)
    os.close(fd)


if __name__ == "__main__":
    main()
