#!/usr/bin/env python3
"""uboot_setup.py — one-shot: catch U-Boot at its prompt and program a robust,
self-healing autonomous boot into the env, then saveenv.

WHY: the board kept needing a physical reset because the pre-HV window (U-Boot →
TFTP → loady → HV) has NO watchdog (the HV arms it only at the very end) and the
loader gave up on the first transient EMAC-link timeout (`sun8i_emac_eth_start:
Timeout`). This makes U-Boot itself load the HV in a BOUNDED RETRY loop so a
not-yet-up ethernet link is simply retried until it comes up — no host, no reset.

New env:
  boothv   = tftp kernel + dtb + HV elf, then bootelf   (all via TFTP)
  bootcmd  = retry boothv up to 20× with 3s gaps, then fall through to the prompt
  bootdelay= 3   (auto-runs bootcmd, but 3s window to interrupt for manual work)

All three images are served from /opt/bzdos/tftpboot (kernel, bananapi-min.dtb,
microkernel-dbg.elf). Run this AFTER the board is at U-Boot (post power-cycle).
"""
import sys, time
sys.path.insert(0, "/opt/bzdos/microkernel")
import loady_over_acm as L

BOARD_IP = "192.168.88.7"
SRV_IP   = "192.168.88.2"

BOOTHV = ("tftpboot 0x44000000 kernel && "
          "tftpboot 0x4a000000 bananapi-min.dtb && "
          "tftpboot 0x48000000 microkernel-dbg.elf && "
          "bootelf -p 0x48000000")
# bounded retry (20 * ~3s) then drop to prompt so we never lock ourselves out
BOOTCMD = ("for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "
           "run boothv; echo [bootcmd] retry $i; sleep 3; done; "
           "echo [bootcmd] gave up -> prompt")

def ucmd(fd, c, w=8):
    L.rd(fd, 0.2)
    L.wr(fd, (c + "\r").encode())
    buf = b""; t0 = time.time()
    while time.time() - t0 < w:
        b, _ = L.rd(fd, 0.3)
        if b: buf += b
        clean = L._CSI.sub(b'', buf).decode("latin1", "replace")
        if "=>" in clean[-6:] and len(clean) > len(c) + 6:
            break
    return L._CSI.sub(b'', buf).decode("latin1", "replace")

def main():
    print("[setup] ждём ttyCHIMP…")
    if not L.wait_present(timeout=120):
        print("[setup] ⛔ ttyCHIMP не появился за 120s — плата не в U-Boot?")
        return 2
    fd = L.open_tty(True)
    ok, buf = L.catch_uboot(fd, 20)
    if not ok:
        print("[setup] ⛔ U-Boot prompt не пойман")
        return 3
    print("[setup] ✅ U-Boot пойман")

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
        ucmd(fd, "setenv bootcmd 'run boothv; setexpr n $n + 1; if test $n -lt 20; "
                 "then sleep 3; run bootcmd; else echo [bootcmd] gave up -> prompt; fi'", 4)
    ucmd(fd, "setenv bootdelay 3", 4)
    out = ucmd(fd, "saveenv", 10)
    print("[setup] saveenv:", "OK" if ("Writing" in out or "done" in out or "OK" in out) else out[-120:])

    print("[setup] === проверка env ===")
    print(ucmd(fd, "printenv bootcmd", 5))
    print(ucmd(fd, "printenv boothv", 5))
    print(ucmd(fd, "printenv bootdelay", 4))
    print("[setup] ✅ готово. Следующая загрузка — автономная self-load с retry.")
    print("[setup] чтобы стартануть СЕЙЧАС без ресета: пошлю 'run bootcmd'")
    return 0

if __name__ == "__main__":
    sys.exit(main())
