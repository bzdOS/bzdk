#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""vetrace.py — dump the HV's video-engine register trace ring (vetrap.c)."""
import sys
from hvdbg import HV
BASE = 0x500A0000
h = HV()
hdr = h.read_words_stable(BASE, 16)
if hdr[0] != 0x52544556:
    print("no VETR ring (magic %#x)" % hdr[0]); sys.exit(1)
n, size = hdr[1], hdr[2]
cnt = min(n, size)
start = n - cnt
print("entries written %d, isv0 %d" % (n, hdr[3]), file=sys.stderr)
words = []
i = 0
while i < cnt * 2:
    k = min(256, cnt * 2 - i)
    words += h.read_words(BASE + 0x40 + 4 * i, k)
    i += k
for e in range(cnt):
    w0, v = words[2 * e], words[2 * e + 1]
    off, wnr, sas, cpu, rep = w0 & 0xffff, (w0 >> 16) & 1, (w0 >> 17) & 3, (w0 >> 19) & 3, w0 >> 24
    pg = "VE SC CCU DRAMC".split()[(off >> 12) & 3]
    print("%5d cpu%d %s%d %s %04x %08x%s" % (start + e, cpu, "W" if wnr else "R",
          8 << sas, pg, off & 0xfff, v, (" x%d" % (rep + 1)) if rep else ""))
