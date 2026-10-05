#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""vcon_dump.py [bytes] — dump the tail of the hypervisor's guest-console capture
ring (vconsole.h: header 0x50000f00, bytes at 0x50040000, 64 KiB) to stdout.
Reads in word chunks (the reliable path), retries a failed chunk, never gives up
silently: a missing chunk is reported."""
import sys, struct
sys.path.insert(0, '/opt/bzdos/microkernel')
from hvdbg import HV
h = HV()
hd = None
for _ in range(6):
    hd = h.read_words(0x50000f00, 4) or h.read_words_stable(0x50000f00, 4)
    if hd and len(hd) == 4:
        break
if not hd or len(hd) != 4:
    sys.exit('console ring header unreadable (debug channel busy?)')
tb = hd[1]
want = int(sys.argv[1]) if len(sys.argv) > 1 else 65536
total = min(tb, 0x10000)
if tb > 0x10000:
    sys.exit("ring wrapped (%d bytes written): tail dump only" % tb)
n = min(want, total)
start = (total - n) & ~3
out = bytearray()
pa = 0x50040000 + start
left = (total - start + 3) // 4
while left > 0:
    k = min(128, left)
    for attempt in range(4):
        w = h.read_words(pa, k)
        if w and len(w) == k:
            break
    else:
        out += b'<<chunk %#x lost>>' ; pa += 4 * k; left -= k; continue
    out += struct.pack('<%dI' % k, *w)
    pa += 4 * k; left -= k
sys.stdout.write("total_bytes %d\n" % tb)
sys.stdout.write(out[:total - start].decode('latin1'))
