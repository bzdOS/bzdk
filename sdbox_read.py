#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""sdbox_read.py -- decode the black box sector the hypervisor writes on its way
into reboot_clean() (sdbox.c): SD card LBA 64, read through the guest.

    python3 sdbox_read.py            # ssh into the guest, dd LBA 64, decode
    python3 sdbox_read.py file.bin   # decode a saved sector
"""
import struct, subprocess, sys, time

SSH = ['ssh', '-o', 'ConnectTimeout=8', '-o', 'BatchMode=yes',
       '-i', '/root/.ssh/chimp_ed25519', 'root@192.168.88.82']
REASON = {0: 'unknown', 1: 'bmc reset', 2: 'guest PSCI SYSTEM_OFF',
          3: 'guest PSCI SYSTEM_RESET', 4: 'EMAC-dark escalation', 5: 'repl reset'}
KIND = {1: 'FAULT', 2: 'TRAP/PSCI', 3: 'IRQ', 4: 'VIRTIO', 5: 'CONSOLE', 6: 'TIMER'}


def main():
    if len(sys.argv) > 1:
        raw = open(sys.argv[1], 'rb').read()
    else:
        raw = subprocess.run(SSH + ['dd if=/dev/vtbd1 bs=512 skip=64 count=1 2>/dev/null'],
                             capture_output=True, timeout=60).stdout
    if len(raw) < 512:
        sys.exit(f"short read: {len(raw)} bytes")
    w = struct.unpack('<128I', raw[:512])
    if w[0] != 0x58425A42:
        sys.exit(f"no black box (magic 0x{w[0]:08x}); the board never went through reboot_clean() "
                 f"with sdbox built in, or the SD stack was down when it did")
    cnt = (w[4] << 32) | w[3]
    print(f"black box: reason={w[2]} ({REASON.get(w[2], '?')})  boots(WDEP)={w[1]}  "
          f"core={w[5]}  cntpct={cnt} (~{cnt / 24e6:.1f} s of uptime)")
    print(f"  WDEP last BMSR=0x{w[6]:x}  WDEP[16]={w[7]}   vblk bc[56..63]={[hex(x) for x in w[8:16]]}")
    print(f"  flightrec: magic=0x{w[16]:x} total={w[17]} head={w[18]} cap={w[19]} stride={w[20]}")
    n = min(w[17], 20)
    for i in range(n):
        k, a0l, a0h, a1l, a1h = w[24 + i * 5: 29 + i * 5]
        print(f"  [{i:2d}] {KIND.get(k, str(k)):9s} a0=0x{(a0h << 32) | a0l:016x} a1=0x{(a1h << 32) | a1l:016x}")


if __name__ == '__main__':
    main()
