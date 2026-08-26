#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""uboot_setup.py — one-shot: catch U-Boot at its prompt and program a robust,
self-healing autonomous boot into the env, then saveenv.

WHY: the board kept needing a physical reset because the pre-HV window (U-Boot →
TFTP → loady → HV) has NO watchdog (the HV arms it only at the very end) and the
loader gave up on the first transient EMAC-link timeout (`sun8i_emac_eth_start:
Timeout`). This makes U-Boot itself load the HV in a BOUNDED RETRY loop so a
not-yet-up ethernet link is simply retried until it comes up — no host, no reset.

CORRECTED 2026-08-23 (verified live, several clean cycles): the original
`tftpboot ... && bootelf -p 0x48000000` here NEVER actually jumped into the HV
after a network load — `bootelf` reliably loads+verifies (`md` even shows the
right bytes) and then silently returns to the U-Boot prompt without executing
it, on this board's U-Boot. Only a Y-modem (`loady`, CPU-mediated writes) load
followed by `bootelf` ever jumped. Root cause was never fully pinned down (a
stale-D-cache-after-EMAC-DMA theory doesn't survive `md` reading fresh bytes
from the exact same buffer bootelf's copy failed on), but the WORKING fix is
simple: package the HV's flat binary as a U-Boot legacy "Standalone Program"
image (`mkimage -A arm64 -O u-boot -T standalone ...` — see the new `make
hv-uimage` target) and boot it with `bootm`, which — being U-Boot's primary,
heavily-trodden network-boot path (used by literally every ARM board booting
Linux over TFTP) — does not carry whatever `bootelf`-specific defect this was.
`bootm` ALSO respects `autostart`: it loads+verifies unconditionally but only
jumps when `autostart=yes`, exactly like `bootelf` — see the two-phase pattern
below (`no` during the tftp phase, `yes` right before the jump).

New env:
  boothv   = tftp dtb + kernel + HV uimg, then bootm (all via TFTP)
  bootcmd  = retry boothv up to 10× with 3s gaps, then arm the hardware
             watchdog as a last-resort self-recovery (see uboot-reset-hangs-
             no-remote-lever: `reset`/`reboot` at this prompt HANGS the board
             with no remote lever, so the fallback must be the WDOG mw.l
             sequence, never `reset`)
  bootdelay= 3   (auto-runs bootcmd, but interruptible — see the low-latency
             catch technique below if you need to reprogram this again)

All three images are served from /opt/bzdos/tftpboot (kernel, bananapi-min.dtb,
microkernel-dbg.uimg — built by `make hv-uimage`). Run this AFTER the board is
at U-Boot (post power-cycle).

CATCHING THE PROMPT IS ITSELF TIME-SENSITIVE: with bootdelay=3 and this
bootcmd already programmed, the round trip from a `wdt_reset()` call to the
guest already loading can be under 6 seconds. Two separate process
invocations (one to reset, one to catch) reliably LOSE this race — the
interpreter-startup gap alone is enough to miss the whole countdown. Send the
reset and start flooding Ctrl-C for the prompt IN THE SAME PROCESS, with no
gap, exactly like this script's own `main()` below.
"""
import sys, time, os
sys.path.insert(0, "/opt/bzdos/microkernel")
import loady_over_acm as L

BOARD_IP = "192.168.88.7"
SRV_IP   = "192.168.88.2"

# CORRECTED 2026-08-23: tftp + bootm on a proper uImage, NOT tftp + bootelf on
# the raw ELF (see the module docstring for why). `setenv autostart no/yes`
# brackets exactly the tftp phase, mirroring the SAME two-phase requirement
# `bootelf` had (chimpd-autostart-bootelf: autostart=yes DURING a tftpboot
# destabilises the board) — bootm turns out to respect the same env var.
BOOTHV = ("setenv autostart no; "
          "tftpboot 0x4a000000 bananapi-min.dtb && "
          "tftpboot 0x44000000 kernel && "
          "tftpboot 0x48000000 microkernel-dbg.uimg && "
          "setenv autostart yes && "
          "bootm 0x48000000")
# Bounded retry (10 * ~3s), then arm the HARDWARE watchdog as the last-resort
# self-recovery -- never `reset`/`reboot` here (uboot-reset-hangs-no-remote-
# lever: that HANGS this board with no remote lever left at all). WDOG regs
# per uboot-watchdog-soft-reset: CFG=1 (reset whole system), MODE=0xb1 (16s +
# enable), CTRL=0x14af (restart, key 0x0A57).
BOOTCMD = ("for i in 1 2 3 4 5 6 7 8 9 10; do "
           "run boothv; echo [bootcmd] retry $i; sleep 3; done; "
           "echo [bootcmd] gave up; "
           "mw.l 0x1c20cb4 1; mw.l 0x1c20cb8 0xb1; mw.l 0x1c20cb0 0x14af; "
           "echo [wdreset] SoC watchdog armed")

def ucmd(fd, c, w=8):
    L.rd(fd, 0.2)
    L.wr(fd, (c + "\r").encode())
    buf = b""; t0 = time.time()
    while time.time() - t0 < w:
        b, dead = L.rd(fd, 0.3)
        if b: buf += b
        if dead: break
        clean = L._CSI.sub(b'', buf).decode("latin1", "replace")
        if clean.rstrip().endswith("=>") and len(clean) > len(c) + 2:
            break
    return L._CSI.sub(b'', buf).decode("latin1", "replace")

def catch(reset_first):
    """Open the tty and flood ^C until '=>' shows up. If reset_first, the
    wdt_reset() call and the flood happen in THIS SAME PROCESS with no gap --
    see the module docstring: once a working bootcmd is already programmed,
    the window from reset to the guest already loading can be well under
    10s, and two separate process invocations reliably lose that race on
    interpreter-startup latency alone."""
    if reset_first:
        import hvdbg
        hvdbg.HV().wdt_reset()

    t0 = time.time()
    while not os.path.exists(L.TTY):
        if time.time() - t0 > 30:
            return None
        time.sleep(0.005)
    fd = L.open_tty(True)

    buf = b""
    t1 = time.time()
    while time.time() - t1 < 30:
        try:
            os.write(fd, b"\x03")
        except OSError:
            pass
        b, dead = L.rd(fd, 0.05)
        if b: buf += b
        if dead:
            return None
        clean = L._CSI.sub(b'', buf).decode("latin1", "replace")
        if clean.rstrip().endswith("=>"):
            return fd
    return None

def main():
    reset_first = "--reset" in sys.argv
    print(f"[setup] catching U-Boot (reset_first={reset_first})…")
    fd = catch(reset_first)
    if fd is None:
        print("[setup] ⛔ U-Boot prompt не пойман")
        return 3
    print("[setup] ✅ U-Boot пойман")

    # disarm any watchdog a previous bootcmd give-up may have armed
    ucmd(fd, "mw.l 0x1c20cb8 0", 4)

    # probe hush 'for' support so we know the bootcmd will actually run
    r = ucmd(fd, "for i in 1 2; do echo FORTEST$i; done", 5)
    for_ok = ("FORTEST1" in r and "FORTEST2" in r)
    print(f"[setup] hush 'for' поддержан: {for_ok}")

    print("[setup] программирую env…")
    ucmd(fd, f"setenv ipaddr {BOARD_IP}", 4)
    ucmd(fd, f"setenv serverip {SRV_IP}", 4)
    ucmd(fd, "setenv autoload no", 4)
    ucmd(fd, f"setenv boothv '{BOOTHV}'", 4)
    if for_ok:
        ucmd(fd, f"setenv bootcmd '{BOOTCMD}'", 4)
    else:
        # fallback: bounded retry via a counter var (portable, no 'for')
        ucmd(fd, "setenv n 0", 4)
        ucmd(fd, "setenv bootcmd 'run boothv; setexpr n $n + 1; if test $n -lt 10; "
                 "then sleep 3; run bootcmd; else echo [bootcmd] gave up; "
                 "mw.l 0x1c20cb4 1; mw.l 0x1c20cb8 0xb1; mw.l 0x1c20cb0 0x14af; "
                 "echo [wdreset] SoC watchdog armed; fi'", 4)
    ucmd(fd, "setenv bootdelay 3", 4)
    out = ucmd(fd, "saveenv", 10)
    print("[setup] saveenv:", "OK" if ("Writing" in out or "done" in out or "OK" in out) else out[-120:])

    print("[setup] === проверка env ===")
    print(ucmd(fd, "printenv bootcmd", 5))
    print(ucmd(fd, "printenv boothv", 5))
    print(ucmd(fd, "printenv bootdelay", 4))
    print("[setup] ✅ готово. Следующая загрузка — автономная self-load с retry, "
          "чисто по сети (make hv-uimage держит microkernel-dbg.uimg свежим).")
    print("[setup] чтобы стартануть СЕЙЧАС без ресета: пошлю 'run bootcmd'")
    return 0

if __name__ == "__main__":
    sys.exit(main())
