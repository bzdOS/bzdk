#!/usr/bin/env python3
"""Boot counter / safe mode (dbgtools.c), over the debug channel.

    safemode.py              show the counter and whether this boot is held
    safemode.py release      let a safe-mode (held) boot enter the guest
    safemode.py force N      set the counter to N (tests: N=4 makes the next
                             warm reset of the same image enter safe mode)

Addresses are hv_addrmap.h's fixed HVMAP_DBGTOOLS page, not nm symbols, so
this works against whatever image is running. Writes go through dbgmon's
`release` afterwards: it cleans the cache line that holds every word here
to DRAM, which is what makes a value survive the next warm reset.
"""
import sys

sys.path.insert(0, '/opt/bzdos/microkernel')
from hvdbg import HV  # noqa: E402

BASE = 0x50021000
SAFE_BOOTS, HEALTHY_S = 5, 90


def show(hv):
    w = hv.read_words(BASE, 16)
    if not w:
        sys.exit('no reply')
    magic_ok = w[12] == 0x544F4F42
    print(f'counter   : {w[13] if magic_ok else "not set"} '
          f'(safe mode at {SAFE_BOOTS} boots that die within {HEALTHY_S} s)')
    print(f'image id  : {w[14]:#010x}')
    print(f'safe mode : {"YES" if w[15] else "no"}')
    held = w[2] and not w[3]
    print(f'hold gate : HOLD={w[2]} RELEASE={w[3]} -> '
          f'{"guest HELD" if held else "guest not held"}')


def main():
    hv = HV()
    if len(sys.argv) > 1 and sys.argv[1] == 'release':
        print(hv.cmd('release', 3).strip())
    elif len(sys.argv) > 2 and sys.argv[1] == 'force':
        hv.write_word(BASE + 0x34, int(sys.argv[2], 0))
        hv.cmd('release', 3)       # dc civac of the whole line, see above
    show(hv)


if __name__ == '__main__':
    main()
