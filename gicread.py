#!/usr/bin/env python3
# Read (via EMAC/hvdbg) the live GIC distributor state for the MMC SPIs while
# the guest sits at mountroot, plus the vconsole ring's mount/mmc lines.
import sys, time, re
sys.path.insert(0, "/opt/bzdos/microkernel")
from hvdbg import HV

GICD = 0x01c81000

def bitword(hv, base, intid):
    # register base + (intid//32)*4, bit intid%32
    w = hv.read_words(base + (intid // 32) * 4, 1)
    return (w[0] >> (intid % 32)) & 1 if w else -1

def main():
    # poll in a loop to catch the mountroot window regardless of timing
    last_tb = None
    for it in range(40):
        hv = None
        for a in range(4):
            try:
                hv = HV(); u = hv.read_words(0x50000f00, 3)
                if len(u) >= 3 and u[0] == 0x55415254:
                    break
            except Exception:
                pass
            hv = None; time.sleep(0.5)
        if not hv:
            print("[%02d] EMAC molchit" % it); time.sleep(1.5); continue
        tb = u[1]
        line = "[%02d] tb=%d f=%d" % (it, tb, u[2])
        for intid, name in [(27, "tmr"), (92, "SD"), (94, "eMMC")]:
            en = bitword(hv, GICD + 0x100, intid)
            pd = bitword(hv, GICD + 0x200, intid)
            ac = bitword(hv, GICD + 0x300, intid)
            line += "  %s:E%dP%dA%d" % (name, en, pd, ac)
        try:
            hppir = hv.read_words(0x01c82000 + 0x18, 1)[0]
            line += "  HPPIR=%d" % (hppir & 0x3ff)
        except Exception:
            pass
        print(line); sys.stdout.flush()
        last_tb = tb
        time.sleep(1.5)
    u = [0, last_tb or 0, 0]
    if not hv:
        return 1

    # ring: mount / mmc lines
    tb = u[1]; sz = min(tb, 0x10000); words = []
    for off in range(0, sz, 0x100):
        try: words += hv.read_words(0x50000f10 + off, min(0x40, (sz - off) // 4 or 1))
        except Exception: words += [0] * 0x40
    b = bytearray()
    for w in words: b += bytes([w & 0xff, (w >> 8) & 0xff, (w >> 16) & 0xff, (w >> 24) & 0xff])
    seq = b[:tb] if tb <= 0x10000 else b[tb % 0x10000:] + b[:tb % 0x10000]
    s = "".join(chr(c) if 32 <= c < 127 else " " for c in seq)
    print("=== ring: mmc/mount/error lines ===")
    for kw in ["mmcsd", "mmcbus", "aw_mmc1", "failed with error", "retrying", "No card", "CMD", "timeout", "mmc0", "GEOM"]:
        hits = [m.start() for m in re.finditer(kw, s, re.I)]
        ex = repr(re.sub(r"  +", " ", s[hits[0]-4:hits[0]+64])) if hits else ""
        print("  %-18s x%-3d %s" % (kw, len(hits), ex))
    return 0

if __name__ == "__main__":
    sys.exit(main())
