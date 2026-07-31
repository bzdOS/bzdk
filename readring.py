#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
import sys, time, re
sys.path.insert(0, "/opt/bzdos/microkernel")
import loady_over_acm as L

def ucmd(fd, c, w=4):
    L.rd(fd, 0.15); L.wr(fd, (c + "\r").encode()); buf = b""; t0 = time.time()
    while time.time() - t0 < w:
        b, _ = L.rd(fd, 0.2); buf += b or b""
        cl = L._CSI.sub(b"", buf).decode("latin1", "replace")
        if "=>" in cl[-6:] and len(cl) > len(c) + 6:
            break
    return L._CSI.sub(b"", buf).decode("latin1", "replace")

def main():
    if not L.wait_present(15):
        print("no uboot"); return 1
    fd = L.open_tty(True)
    for _ in range(5):
        L.wr(fd, b"\x03"); time.sleep(0.2)
    L.catch_uboot(fd, 20)
    h = ucmd(fd, "md 0x50000f00 4")
    m = re.search(r"50000f00:\s+([0-9a-f]+)\s+([0-9a-f]+)", h)
    if not m or int(m.group(1), 16) != 0x55415254:
        print("vc header bad:", h.strip()[-50:]); return 0
    tb = int(m.group(2), 16)
    print("total_bytes=%d" % tb)
    n = min(tb, 0x10000)
    raw = bytearray(n); base = 0
    while base < n:
        cnt = min(0x200, n - base)
        out = ucmd(fd, "md.b 0x%x 0x%x" % (0x50000f10 + base, cnt), 4)
        for line in out.splitlines():
            mm = re.match(r"^([0-9a-f]{8}):((?: [0-9a-f]{2})+)", line)
            if mm:
                a = int(mm.group(1), 16) - 0x50000f10
                for k, hx in enumerate(mm.group(2).split()):
                    if 0 <= a + k < n:
                        raw[a + k] = int(hx, 16)
        base += cnt
    s = "".join(chr(c) if 32 <= c < 127 else " " for c in raw)
    print("--- ring[0:120] (should be boot banner if linear) ---")
    print(repr(s[:120]))
    # dump the readable text from the first aw_mmc attach onward (MMC bus enum)
    mt = re.search("aw_mmc", s, re.I)
    if mt:
        chunk = re.sub(r"  +", " ", s[mt.start()-40:mt.start()+1400])
        print("=== MMC section (from first aw_mmc) ===")
        print(chunk)
    # also the very tail (mountroot / last state)
    print("=== TAIL (last 400) ===")
    print(re.sub(r"  +", " ", s[-400:]))
    return 0

if __name__ == "__main__":
    sys.exit(main())
