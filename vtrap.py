#!/usr/bin/env python3
"""Virtio-mmio trap cost, read live from el2_exc.c's g_vtrap counters.

Two snapshots INTERVAL seconds apart (over the debug channel, symbols from
the ELF), then per device and register class: traps/s, mean and max time
el2_trap spent from its C entry to the handler's return, and that time as
a share of one core. The asm entry/exit around el2_trap is not included.

    env -u http_proxy -u https_proxy python3 vtrap.py [INTERVAL] [ELF]
"""
import subprocess
import sys
import time

sys.path.insert(0, '/opt/bzdos/microkernel')
from hvdbg import HV  # noqa: E402

TICK_HZ = 24_000_000
DEVS = ['emmc', 'vnet', '-', 'vinput', 'sd', 'zram', 'vcons', 'vgicd',
        'wdog', 'rsb', 'scanout']
CLS = ['notify', 'isr', 'ack', 'other']   # vgicd: notify = GICD_SGIR
NDEV, NCLS, NCPU = 11, 4, 4


def syms(elf):
    out = subprocess.run(['aarch64-linux-gnu-nm', elf], capture_output=True,
                         text=True, check=True).stdout
    s = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            s[p[2]] = int(p[0], 16)
    return s


def snap(hv, base, longp, prep):
    n = NCPU * NDEV * NCLS * 3 * 2
    w = hv.read_words(base, n)
    lw = hv.read_words(longp, NCPU * NDEV * 2)
    pw = hv.read_words(prep, NCPU * 4)
    if len(w) != n or len(lw) != NCPU * NDEV * 2 or len(pw) != NCPU * 4:
        sys.exit('read failed')
    q = [w[i] | (w[i + 1] << 32) for i in range(0, n, 2)]
    tot = {}
    for c in range(NCPU):
        for d in range(NDEV):
            for k in range(NCLS):
                i = ((c * NDEV + d) * NCLS + k) * 3
                t = tot.setdefault((d, k), [0, 0, 0])
                t[0] += q[i]
                t[1] += q[i + 1]
                t[2] = max(t[2], q[i + 2])
    lq = [lw[i] | (lw[i + 1] << 32) for i in range(0, len(lw), 2)]
    long_n = [sum(lq[c * NDEV + d] for c in range(NCPU)) for d in range(NDEV)]
    pq = [pw[i] | (pw[i + 1] << 32) for i in range(0, len(pw), 2)]
    pre = (sum(pq[0::2]), sum(pq[1::2]))
    return tot, long_n, pre


def main():
    iv = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    elf = sys.argv[2] if len(sys.argv) > 2 else \
        '/opt/bzdos/microkernel/microkernel-dbg.elf'
    s = syms(elf)
    hv = HV()
    a, la, pa = snap(hv, s['g_vtrap'], s['g_vtrap_long'], s['g_vtrap_pre'])
    t0 = time.time()
    time.sleep(iv)
    b, lb, pb = snap(hv, s['g_vtrap'], s['g_vtrap_long'], s['g_vtrap_pre'])
    dt = time.time() - t0
    lng = ' '.join(f'{DEVS[d]}={lb[d] - la[d]}' for d in range(NDEV)
                   if lb[d] - la[d])
    print(f'interval {dt:.1f} s; long (>1 ms) samples: {lng or "none"}')
    pn, pt = pb[0] - pa[0], pb[1] - pa[1]
    if pn:
        print(f'prologue (C entry -> device dispatch), all guest data aborts: '
              f'{pn / dt:.1f}/s, mean {pt / pn / TICK_HZ * 1e6:.2f} us')
    print(f'{"dev":7}{"reg":8}{"traps/s":>10}{"mean us":>10}'
          f'{"max us":>10}{"core %":>9}')
    all_n = all_t = 0
    for (d, k), v in sorted(b.items()):
        n = v[0] - a[(d, k)][0]
        t = v[1] - a[(d, k)][1]
        if n == 0:
            continue
        all_n += n
        all_t += t
        print(f'{DEVS[d]:7}{CLS[k]:8}{n / dt:10.1f}'
              f'{t / n / TICK_HZ * 1e6:10.2f}{v[2] / TICK_HZ * 1e6:10.1f}'
              f'{t / TICK_HZ / dt * 100:9.3f}')
    if all_n:
        print(f'{"total":15}{all_n / dt:10.1f}'
              f'{all_t / all_n / TICK_HZ * 1e6:10.2f}{"":10}'
              f'{all_t / TICK_HZ / dt * 100:9.3f}')
    if pn:
        print(f'data aborts that reached dispatch but no counted handler: '
              f'{(pn - all_n) / dt:.1f}/s')


if __name__ == '__main__':
    main()
