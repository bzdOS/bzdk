#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""warm_reset_soak.py -- does a warm (WDOG) reset come back, and under what load?

WHY: twice in two days a warm reset issued by the hypervisor never returned
to U-Boot: no gadget, no FEL, nothing, until someone cut the power. A cold
start with the same eMMC and the same environment comes back every time, so
whatever is wrong is state the WDOG reset does not clear -- and the WDOG
resets the SoC but not the PMIC, the eMMC, the SD card or the RTC domain.
Both losses happened with the guest busy on the eMMC. This measures that:
a series of resets with the guest idle, then a series under eMMC reads and
SD writes. A loss in one series and not the other is the answer.

Every cycle records three witnesses with wall-clock timestamps:
  uboot  -- U-Boot's download gadget enumerating on the host (kernel log)
  hv     -- the hypervisor's uptime counter going backwards (EMAC)
  guest  -- ssh answering
A cycle whose U-Boot gadget does not appear within UBOOT_DEADLINE_S is a
LOSS: the script says so loudly, then waits for the gadget to appear (that
is the human power-cycling the board), and carries on with the series so
one loss does not end the experiment.

Run only with someone at the power switch. Needs chimpd (or autoboot) to
bring the hypervisor back after each reset; this script only observes.
"""
import os, subprocess, sys, time

sys.path.insert(0, '/opt/bzdos/microkernel')
import bmc_client
import board_ctl

SSH = ['ssh', '-o', 'ConnectTimeout=8', '-o', 'BatchMode=yes',
       '-i', '/root/.ssh/chimp_ed25519', 'root@192.168.88.82']
UBOOT_DEADLINE_S = 40      # cold/warm start to U-Boot gadget is ~5 s
HV_DEADLINE_S = 300
GUEST_DEADLINE_S = 300

LOG = open('/var/tmp/warm_reset_soak.log', 'a')


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True)
    LOG.write(line + '\n'); LOG.flush()


def sh(cmd, timeout=30):
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout).stdout
    except subprocess.TimeoutExpired:
        return ''


def usb_uboot_seen_since(t_wall):
    since = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t_wall))
    out = sh(['journalctl', '-k', '--since', since, '-o', 'short-unix',
              '--no-pager'])
    for line in out.splitlines():
        if 'idProduct=efe8' in line:
            return float(line.split()[0])
    return None


def wait_uboot(t_wall, deadline_s):
    while time.time() - t_wall < deadline_s:
        t = usb_uboot_seen_since(t_wall)
        if t:
            return t
        time.sleep(1)
    return None


def guest(cmd, timeout=30):
    return sh(SSH + [cmd], timeout=timeout)


def wait_guest(deadline_s):
    t0 = time.time()
    while time.time() - t0 < deadline_s:
        if guest('echo UP').strip() == 'UP':
            return time.time()
        time.sleep(5)
    return None


def start_load():
    # eMMC: continuous raw reads of the root disk (root is ro, so reads are
    # the eMMC traffic the guest can generate; the 09-23 loss was during root
    # mount, i.e. reads). SD: a 2 GB write onto /opt.
    guest("nohup sh -c 'while :; do dd if=/dev/vtbd0 of=/dev/null bs=1M 2>/dev/null; done' >/dev/null 2>&1 &"
          " nohup sh -c 'while :; do dd if=/dev/zero of=/opt/soak.bin bs=1M count=2000 2>/dev/null; rm -f /opt/soak.bin; done' >/dev/null 2>&1 &")
    time.sleep(6)
    return guest("ps ax | grep -c '[d]d if='").strip()


def cycle(n, loaded):
    log(f"--- cycle {n} ({'LOADED' if loaded else 'idle'}) ---")
    base = board_ctl.uptime_now()
    if loaded:
        log(f"load started, dd processes: {start_load()}")
    log(f"baseline uptime {base}")
    t_reset = time.time()
    try:
        bmc_client.BMC().reset()
    except Exception as e:
        log(f"reset verb: {e!r}")
    t_ub = wait_uboot(t_reset, UBOOT_DEADLINE_S)
    if t_ub is None:
        log(f"!!! LOSS: no U-Boot gadget {UBOOT_DEADLINE_S} s after the reset. "
            f"POWER-CYCLE THE BOARD. Waiting for it ...")
        t_ub = wait_uboot(t_reset, 3600)
        if t_ub is None:
            log("no board for an hour, giving up"); return 'lost-noreturn'
        log(f"board back (power-cycled) after {t_ub - t_reset:.0f} s")
        result = 'LOSS'
    else:
        log(f"uboot +{t_ub - t_reset:.1f}s")
        result = 'ok'
    if board_ctl.wait_for_reboot_emac(base, timeout=HV_DEADLINE_S, log=log):
        log(f"hv    +{time.time() - t_reset:.0f}s")
    else:
        log("hv    NOT SEEN"); result += '/no-hv'
    tg = wait_guest(GUEST_DEADLINE_S)
    if tg:
        log(f"guest +{tg - t_reset:.0f}s")
        guest('rm -f /opt/soak.bin')
    else:
        log("guest NOT SEEN"); result += '/no-guest'
    log(f"=== cycle {n}: {result}")
    return result


def main():
    idle_n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    load_n = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    log(f"##### warm reset soak: {idle_n} idle + {load_n} loaded")
    res = []
    for i in range(idle_n):
        res.append(('idle', cycle(i + 1, False)))
    for i in range(load_n):
        res.append(('load', cycle(idle_n + i + 1, True)))
    log("##### summary")
    for k, r in res:
        log(f"  {k:5s} {r}")


if __name__ == '__main__':
    main()
