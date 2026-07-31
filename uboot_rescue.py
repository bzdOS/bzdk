#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""uboot_rescue.py — aggressively break into U-Boot during the 3s bootdelay and
neutralise the broken autonomous bootcmd (which does bootelf on a stale-cache
TFTP'd ELF -> jumps to garbage -> crashes the board every boot). Spams Ctrl-C
from the instant ttyCHIMP appears, catches the '=>' prompt, sets bootdelay=-1 so
U-Boot parks at the prompt forever (recoverable), then saveenv.

Run this, THEN power-cycle the board. Keeps retrying for a long time."""
import sys, os, time
sys.path.insert(0, "/opt/bzdos/microkernel")
import loady_over_acm as L

def try_once():
    if not os.path.exists(L.TTY):
        return False
    try:
        fd = L.open_tty(True)
    except Exception:
        return False
    try:
        # spam Ctrl-C hard for a few seconds to beat bootdelay=3
        buf = b""
        t0 = time.time()
        while time.time() - t0 < 6:
            L.wr(fd, b"\x03")
            b, _ = L.rd(fd, 0.15)
            if b: buf += b
            cl = L._CSI.sub(b"", buf).decode("latin1", "replace")
            if "=>" in cl[-8:]:
                break
        else:
            return False
        # at prompt — park it
        def ucmd(c, w=8):
            L.rd(fd, 0.2); L.wr(fd, (c + "\r").encode()); b2=b""; s=time.time()
            while time.time()-s<w:
                x,_=L.rd(fd,0.3); b2+=x or b""
                cl=L._CSI.sub(b"",b2).decode("latin1","replace")
                if "=>" in cl[-6:] and len(cl)>len(c)+6: break
            return L._CSI.sub(b"",b2).decode("latin1","replace")
        print("[rescue] ✅ U-Boot пойман — паркую bootdelay=-1")
        ucmd("setenv bootdelay -1", 4)
        out = ucmd("saveenv", 10)
        print("[rescue] saveenv:", "OK" if ("Writing" in out or "done" in out or "OK" in out) else out[-80:])
        print("[rescue] printenv bootdelay:", ucmd("printenv bootdelay", 4).strip().splitlines()[-2:])
        print("[rescue] ГОТОВО — U-Boot запаркован. Дальше можно грузить chimpd'ом.")
        return True
    finally:
        try: os.close(fd)
        except OSError: pass

def main():
    print("[rescue] жду ttyCHIMP и спамлю Ctrl-C… ПЕРЕДЁРНИ плату сейчас.")
    t0 = time.time()
    while time.time() - t0 < 600:
        if try_once():
            return 0
        time.sleep(0.2)
    print("[rescue] таймаут 10 мин — не поймал")
    return 1

if __name__ == "__main__":
    sys.exit(main())
