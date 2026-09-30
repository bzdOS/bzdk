#!/usr/bin/env python3
"""Read scrub.c's counters live (symbols from the ELF, over the debug channel).

    env -u http_proxy -u https_proxy python3 scrub.py [ELF]
"""
import subprocess
import sys

sys.path.insert(0, '/opt/bzdos/microkernel')
from hvdbg import HV  # noqa: E402

NAMES = ['g_scrub_state', 'g_scrub_on', 'g_scrub_repair_on', 'g_scrub_hw_crc',
         'g_scrub_boot_bad', 'g_scrub_passes', 'g_scrub_mismatch',
         'g_scrub_repairs', 'g_scrub_words_fixed', 'g_scrub_unrepairable',
         'g_scrub_accepts']
STATE = {0: 'off', 1: 'build table', 2: 'boot baseline', 3: 'image too big'}


def main():
    elf = sys.argv[1] if len(sys.argv) > 1 else \
        '/opt/bzdos/microkernel/microkernel-dbg.elf'
    syms = {}
    for line in subprocess.run(['aarch64-linux-gnu-nm', elf],
                               capture_output=True, text=True,
                               check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    hv = HV()
    vals = {}
    for n in NAMES:
        w = hv.read_words(syms[n], 1)
        vals[n] = w[0] if w else None
    lb = hv.read_words(syms['g_scrub_last_bad'], 2)
    for n in NAMES:
        v = vals[n]
        extra = f' ({STATE.get(v, "?")})' if n == 'g_scrub_state' else ''
        print(f'{n[8:]:14} {v}{extra}')
    if lb:
        print(f'{"last_bad":14} {lb[0] | (lb[1] << 32):#x}')


if __name__ == '__main__':
    main()
