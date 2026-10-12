#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""cutover.py -- move the board between subnets: U-Boot env, HV, guest, host.

The board's addresses live in three places, changed together here:
  * U-Boot env  ipaddr/netmask/gatewayip/serverip -- U-Boot's own TFTP
    fallback, and the HV's UDP debug address (bootcmd exports them, dbgip.h);
  * the guest   /etc/rc.conf ifconfig_vtnet0 + defaultrouter, /etc/resolv.conf;
  * this host   ~/.config/bzdk/board.conf (every tool), and the kernel
    netconsole target /usr/local/sbin/netconsole-fleet-chimp.sh.

    cutover.py plan     TARGET.conf          show what would change
    cutover.py apply    TARGET.conf [--no-reboot]
    cutover.py verify                        checks against board.conf
    cutover.py rollback BACKUP_DIR [--console]

`apply` backs everything up into a new directory under
/opt/bzdos/wt/subnet-move/backups/, writes the three places, and reboots the
board with a clean guest `shutdown -r now`. The board then comes up with the
new addresses wherever its cable is. While it is still on the old segment,
the raw-Ethernet debug channel and the guest console still work there
(`BZDK_TRANSPORT=l2`), so the new configuration is visible before anyone
moves a cable. `rollback` restores a backup: over ssh when the guest is
reachable, else (--console) through the guest console on the debug channel,
which needs the board on this host's segment.
"""
import argparse
import datetime
import os
import shlex
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MK = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, MK)
import bzd_board  # noqa: E402

BACKUPS = "/opt/bzdos/wt/subnet-move/backups"
NETCONSOLE_SH = "/usr/local/sbin/netconsole-fleet-chimp.sh"
LOCK = "/opt/bzdos/linux-work/.board.lock"


def run(argv, check=True, timeout=300, lock=False):
    if lock:
        argv = ["flock", "-w", "600", LOCK] + argv
    r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                       stdin=subprocess.DEVNULL)
    if check and r.returncode:
        raise SystemExit(f"FAILED: {' '.join(argv)}\n{r.stdout}{r.stderr}")
    return r


def ssh(host_ip, cmd, check=True, timeout=120):
    argv = ["ssh", "-o", "ConnectTimeout=10", "-o", "BatchMode=yes", "-i",
            bzd_board.GUEST_SSH_KEY, f"root@{host_ip}", cmd]
    return run(argv, check=check, timeout=timeout, lock=True)


def env_tool(*args):
    return run([sys.executable, os.path.join(MK, "uboot_env.py"), *args],
               lock=True).stdout


def mask_len(mask):
    return sum(bin(int(x)).count("1") for x in mask.split("."))


def guest_netcfg(t):
    """The guest-side shell edit for target config t (run with / rw)."""
    return (
        "sed -i '' -e 's|^ifconfig_vtnet0=.*|ifconfig_vtnet0=\"inet "
        f"{t['guest_ip']} netmask {t['netmask']}\"|' -e "
        f"'s|^defaultrouter=.*|defaultrouter={t['gateway']}|' /etc/rc.conf && "
        f"printf 'nameserver %s\\n' {t['dns']} > /etc/resolv.conf")


def load_target(path):
    t = dict(bzd_board.BOARD_CONF_DEFAULTS)
    t.update(bzd_board.parse_conf(open(path).read()))
    return t


def write_conf(t, path):
    keys = list(bzd_board.BOARD_CONF_DEFAULTS)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write("# ~/.config/bzdk/board.conf (written by tools/subnet/cutover.py"
                f" {datetime.datetime.now():%Y-%m-%d %H:%M})\n")
        for k in keys:
            f.write(f"{k:9s} = {t[k]}\n")
    os.replace(tmp, path)


def netconsole_text(target_ip):
    """netconsole-fleet-chimp.sh for a target that may be off-link: the
    remote MAC is the next hop's (the router's) when the route has `via`."""
    return f"""#!/bin/sh
# цель netconsole флота (fleet.py netconsole); чужие цели configfs не трогает
# (target address from tools/subnet/cutover.py; next-hop MAC when routed)
D={target_ip}; P=6666; T=/sys/kernel/config/netconsole/fleet-chimp
modprobe netconsole; mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
for i in 1 2 3 4 5; do ping -c1 -W2 $D >/dev/null 2>&1 && break; sleep 3; done
R=$(ip -o route get $D)
DEV=$(echo "$R" | sed -n 's/.* dev \\([^ ]*\\).*/\\1/p'); SRC=$(echo "$R" | sed -n 's/.* src \\([^ ]*\\).*/\\1/p')
NH=$(echo "$R" | sed -n 's/.* via \\([^ ]*\\).*/\\1/p'); NH=${{NH:-$D}}
ping -c1 -W2 $NH >/dev/null 2>&1
MAC=$(ip neigh show $NH dev $DEV | awk '{{for(i=1;i<NF;i++) if($i=="lladdr") print $(i+1)}}' | head -1)
[ -n "$DEV" ] && [ -n "$SRC" ] && [ -n "$MAC" ] || {{ echo "нет маршрута/MAC до $D" >&2; exit 1; }}
mkdir -p $T; echo 0 > $T/enabled
echo $DEV > $T/dev_name; echo $SRC > $T/local_ip; echo 6665 > $T/local_port
echo $D > $T/remote_ip; echo $P > $T/remote_port; echo $MAC > $T/remote_mac
echo 1 > $T/enabled
"""


def cmd_plan(a):
    cur, t = bzd_board.conf(), load_target(a.target)
    print("host board.conf:")
    for k in bzd_board.BOARD_CONF_DEFAULTS:
        mark = "  " if cur[k] == t[k] else "->"
        print(f"  {mark} {k:9s} {cur[k]:16s} {t[k]}")
    print("U-Boot env:", f"ipaddr={t['board_ip']} netmask={t['netmask']} "
          f"gatewayip={t['gateway']} serverip={t['server_ip']}")
    print("guest:", guest_netcfg(t))
    print(f"netconsole target: {t['guest_ip']}")


def cmd_apply(a):
    cur, t = dict(bzd_board.conf()), load_target(a.target)
    old_guest = cur["guest_ip"]
    bdir = os.path.join(BACKUPS, "cutover-" + datetime.datetime.now()
                        .strftime("%Y%m%d-%H%M%S"))
    os.makedirs(bdir)
    print(f"backups -> {bdir}")
    # 1. backups
    env_tool("backup", os.path.join(bdir, "uboot.env"))
    for f in ("rc.conf", "resolv.conf"):
        r = ssh(old_guest, f"cat /etc/{f}")
        open(os.path.join(bdir, f), "w").write(r.stdout)
    shutil.copy2(bzd_board.conf_path(), os.path.join(bdir, "board.conf"))
    if os.path.exists(NETCONSOLE_SH):
        shutil.copy2(NETCONSOLE_SH, os.path.join(bdir, "netconsole-fleet-chimp.sh"))
    bootcmd = env_tool("get", "bootcmd").strip().splitlines()[-1]
    if "bzdk_boot" not in bootcmd:
        raise SystemExit("bootcmd has no eMMC slot boot (emmc_boot.py install "
                         "first): refusing to depend on TFTP across a router")
    # 2. U-Boot env (the HV reads the same four values at its next boot)
    print(env_tool("set", f"ipaddr={t['board_ip']}", f"netmask={t['netmask']}",
                   f"gatewayip={t['gateway']}", f"serverip={t['server_ip']}"))
    # 3. guest: keep a .stock copy of the first rc.conf this tool ever saw
    ssh(old_guest, "mount -uw / && "
        "{ [ -f /etc/rc.conf.stock ] || cp -p /etc/rc.conf /etc/rc.conf.stock; } && "
        "{ [ -f /etc/resolv.conf.stock ] || cp -p /etc/resolv.conf /etc/resolv.conf.stock; } && "
        + guest_netcfg(t) + " && sync; mount -ur /")
    print(ssh(old_guest, "grep -E '^(ifconfig_vtnet0|defaultrouter)=' /etc/rc.conf; "
              "cat /etc/resolv.conf; mount | grep ' on / '").stdout)
    # 4. host
    write_conf(t, bzd_board.conf_path())
    if os.path.exists(NETCONSOLE_SH):
        open(NETCONSOLE_SH, "w").write(netconsole_text(t["guest_ip"]))
    print(f"host board.conf written ({t['transport']}, board {t['board_ip']}, "
          f"guest {t['guest_ip']})")
    # 5. reboot into it
    if a.no_reboot:
        print("not rebooting (--no-reboot): the board keeps its old addresses "
              "until the next boot")
        return
    ssh(old_guest, "shutdown -r now", check=False, timeout=30)
    print("guest shutting down; the board boots with the new addresses")
    print(f"rollback: {sys.argv[0]} rollback {bdir}")


def cmd_verify(_a):
    c = bzd_board.conf()
    ok = True

    def check(name, argv, timeout=60):
        nonlocal ok
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                           stdin=subprocess.DEVNULL)
        good = r.returncode == 0
        ok &= good
        tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or [""]
        print(f"  [{'OK' if good else 'FAIL'}] {name}: {tail[0]}")

    check("HV answers ping", ["ping", "-c3", "-W2", c["board_ip"]])
    check("debug handshake", ["flock", "-w", "600", LOCK, sys.executable,
                              os.path.join(MK, "hvauth.py")])
    check("dbgmon ip", ["flock", "-w", "600", LOCK, sys.executable, "-c",
                        f"import sys; sys.path.insert(0, {MK!r}); import hvdbg; "
                        "print(hvdbg.HV().cmd('ip').strip())"])
    check("guest ssh", ["flock", "-w", "600", LOCK] + bzd_board.guest_ssh_argv(10)
          + ["ifconfig vtnet0 | grep inet; netstat -rn | grep default; "
             "ping -c2 -W2 " + c["gateway"]])
    sys.exit(0 if ok else 1)


def cmd_rollback(a):
    bdir = a.backup
    old = bzd_board.parse_conf(open(os.path.join(bdir, "board.conf")).read())
    t = dict(bzd_board.BOARD_CONF_DEFAULTS)
    t.update(old)
    cur = bzd_board.conf()
    rc = open(os.path.join(bdir, "rc.conf")).read()
    rs = open(os.path.join(bdir, "resolv.conf")).read()
    restore = ("mount -uw / && printf %s " + shlex.quote(rc) + " > /etc/rc.conf && "
               "printf %s " + shlex.quote(rs) + " > /etc/resolv.conf && sync; mount -ur /")
    if a.console:
        # board on this host's segment, guest unreachable by IP: the console
        os.environ["BZDK_TRANSPORT"] = "l2"
        g = run([sys.executable, os.path.join(MK, "guest_sh.py"),
                 guest_netcfg(t).replace("sed -i ''", "mount -uw / && sed -i ''")
                 + " && sync; mount -ur /; /etc/rc.d/netif restart vtnet0; "
                 "/etc/rc.d/routing restart"], lock=True, timeout=600)
        print(g.stdout)
        guest = t["guest_ip"]
    else:
        guest = cur["guest_ip"]
        ssh(guest, restore)
    # The env is restored from the guest, so it needs the guest's address
    # that answers right now.
    env_back = os.path.join(bdir, "uboot.env")
    import uboot_env  # noqa: E402
    old_env = uboot_env.parse(open(env_back, "rb").read())
    sets = [f"{k}={old_env[k]}" for k in ("ipaddr", "netmask", "gatewayip",
                                           "serverip") if k in old_env]
    os.environ["BZDK_BOARD_CONF"] = os.path.join(bdir, "board.conf") if a.console else bzd_board.conf_path()
    print(env_tool("set", *sets))
    shutil.copy2(os.path.join(bdir, "board.conf"), bzd_board.BOARD_CONF_FILE)
    if os.path.exists(os.path.join(bdir, "netconsole-fleet-chimp.sh")):
        shutil.copy2(os.path.join(bdir, "netconsole-fleet-chimp.sh"), NETCONSOLE_SH)
    print("restored env, guest net config, board.conf; rebooting")
    run(["flock", "-w", "600", LOCK] + bzd_board.guest_ssh_argv(10)
        + ["shutdown -r now"], check=False, timeout=30)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan"); p.add_argument("target")
    p = sub.add_parser("apply"); p.add_argument("target")
    p.add_argument("--no-reboot", action="store_true")
    sub.add_parser("verify")
    p = sub.add_parser("rollback"); p.add_argument("backup")
    p.add_argument("--console", action="store_true")
    a = ap.parse_args()
    {"plan": cmd_plan, "apply": cmd_apply, "verify": cmd_verify,
     "rollback": cmd_rollback}[a.cmd](a)


if __name__ == "__main__":
    main()
