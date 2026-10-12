#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""emmc_boot.py -- U-Boot environment for booting the HV from the eMMC.

The hypervisor, the guest kernel and its DTB live on the ESP (mmc
${mmc_bootdev}:2, the guest's vtbd0p2) in two slots:

    bzdk/cur/{hv.uimg,kernel,dtb}   what hv_deploy.py writes
    bzdk/lkg/{hv.uimg,kernel,dtb}   last known good (hv_deploy.py promote)

bootcmd runs `bzdk_boot` first, then the old TFTP loop (`boothv`, routed
through gatewayip when serverip is off-link), then arms the watchdog:

  1. `env export -t` ipaddr/netmask/gatewayip/serverip/bzdk_net to
     0x4a0f0000 -- the HV's UDP debug transport reads its address there
     (dbgip.h); bzdk_net=dhcp makes it take a DHCP lease instead, with
     ipaddr etc. as the fallback (`netmode` below sets it);
  2. the slot record at 0x50021100 (hv_addrmap.h HVMAP_BOOTSLOT, survives a
     watchdog reset, cleared by a cold boot's garbage check): tries += 1;
  3. tries <= 3: boot `cur`; otherwise, or when `cur` does not load or its
     uImage CRC is bad: boot `lkg`; then TFTP.
  The HV zeroes `tries` after 90 s healthy when it came from `cur`. A `cur`
  that hangs or dies early is therefore given three warm resets, after
  which the board boots `lkg` and stays there until the next deploy
  (hv_deploy.py resets the tries). A board where both slots fail still
  has the TFTP path, and after it the watchdog.

U-Boot's binary is not touched: this is environment only. Variables:
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SLOT_PA = 0x50021100          # == hv_addrmap.h HVMAP_BOOTSLOT
SLOT_MAGIC = 0x42415a42       # "BZAB"
ENV_FILE_PA = 0x4f000000   # scratch for the RAM-only test env file
NETCFG_PA = 0x4a0f0000        # == dbgip.h DBGIP_CFG_PA
MAX_TRIES = 3

# The TFTP loop that was the whole bootcmd until 2026-10-12, kept verbatim
# as the fallback after the eMMC slots.
OLD_BOOTCMD = ("for i in 1 2 3 4 5 6 7 8 9 10; do run boothv; echo [bootcmd] "
               "retry $i; sleep 3; done; echo [bootcmd] gave up; mw.l 0x1c20cb4 "
               "1; mw.l 0x1c20cb8 0xb1; mw.l 0x1c20cb0 0x14af; echo [wdreset] "
               "SoC watchdog armed")

VARS = {
    # one slot: dtb, kernel, HV; the HV's uImage CRC is checked (iminfo)
    # before the slot record says which slot is booting.
    "bzdk_try": (
        "setenv autostart no; "
        "fatload mmc ${mmc_bootdev}:2 0x4a000000 bzdk/${bzdk_slot}/dtb && "
        "fatload mmc ${mmc_bootdev}:2 0x44000000 bzdk/${bzdk_slot}/kernel && "
        "fatload mmc ${mmc_bootdev}:2 0x48000000 bzdk/${bzdk_slot}/hv.uimg && "
        "iminfo 0x48000000 && "
        f"mw.l {SLOT_PA + 8:#x} ${{bzdk_code}} && mw.l {SLOT_PA + 12:#x} 0 && "
        "echo [bzdk] booting slot ${bzdk_slot} && "
        "setenv autostart yes && bootm 0x48000000; setenv autostart yes"),
    "bzdk_boot": (
        f"env export -t {NETCFG_PA:#x} gatewayip ipaddr netmask serverip "
        "bzdk_net; "
        f"if itest.l *{SLOT_PA:#x} != {SLOT_MAGIC:#x}; then "
        f"mw.l {SLOT_PA:#x} {SLOT_MAGIC:#x}; mw.l {SLOT_PA + 4:#x} 0; fi; "
        f"setexpr.l bzdk_n *{SLOT_PA + 4:#x} + 1; mw.l {SLOT_PA + 4:#x} ${{bzdk_n}}; "
        "echo [bzdk] eMMC boot, try ${bzdk_n}; "
        f"if itest ${{bzdk_n}} -le {MAX_TRIES}; then "
        "setenv bzdk_slot cur; setenv bzdk_code 1; run bzdk_try; fi; "
        "echo [bzdk] slot cur not booted, trying lkg; "
        "setenv bzdk_slot lkg; setenv bzdk_code 2; run bzdk_try; "
        f"echo [bzdk] slot lkg not booted, TFTP; mw.l {SLOT_PA + 8:#x} 3"),
}
NEW_BOOTCMD = "run bzdk_boot; " + OLD_BOOTCMD

__doc__ += "".join(f"\n  {k} = {v}\n" for k, v in VARS.items())


def nc_script(boot=True, slot=None):
    """Commands for uboot_nc.py run: define everything in RAM only (no
    saveenv) and boot through it once."""
    lines = ["# RAM-only test of the eMMC boot path (no saveenv)",
             "printenv ipaddr netmask gatewayip serverip mmc_bootdev",
             "fatls mmc ${mmc_bootdev}:2 bzdk/cur",
             "fatls mmc ${mmc_bootdev}:2 bzdk/lkg",
             f"md.l {SLOT_PA:#x} 4"]
    # Netconsole drops characters from long input lines (a 400-byte setenv
    # arrived without its head and left the prompt in an open quote), so the
    # long variables come from a file on the ESP: write `env_file()` to
    # bzdk/test.env first (through the guest), then import it here.
    lines += [f"fatload mmc ${{mmc_bootdev}}:2 {ENV_FILE_PA:#x} bzdk/test.env",
              f"env import -t {ENV_FILE_PA:#x} ${{filesize}}"]
    if slot:
        lines.append(f"mw.l {SLOT_PA + 4:#x} {0 if slot == 'cur' else MAX_TRIES}")
    if boot:
        lines.append("!run bzdk_boot")
    return "\n".join(lines) + "\n"


def env_file(netmask="255.255.255.0", gatewayip=None):
    """Text for `env import -t`: the boot variables for a RAM-only test."""
    lines = [f"{k}={v}" for k, v in VARS.items() if k != "bootcmd"]
    lines.append(f"netmask={netmask}")
    if gatewayip:
        lines.append(f"gatewayip={gatewayip}")
    return "\n".join(lines) + "\n"


def env_tool(*args):
    r = subprocess.run([sys.executable, os.path.join(HERE, "uboot_env.py"),
                        *args], capture_output=True, text=True, timeout=300,
                       stdin=subprocess.DEVNULL)
    if r.returncode:
        sys.exit(f"uboot_env.py {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("vars", help="print the variables")
    s = sub.add_parser("nc-script", help="uboot_nc.py script for a RAM-only test")
    s.add_argument("--no-boot", action="store_true")
    s.add_argument("--slot", choices=("cur", "lkg"),
                   help="preset the tries so this slot is the one booted")
    e = sub.add_parser("env-file", help="bzdk/test.env for the RAM-only test")
    e.add_argument("--netmask", default="255.255.255.0")
    e.add_argument("--gatewayip")
    i = sub.add_parser("install", help="write the variables + new bootcmd "
                       "into uboot.env (backs it up first)")
    i.add_argument("--backup", required=True, help="file for the old env")
    i.add_argument("--netmask", required=True)
    i.add_argument("--gatewayip", required=True)
    u = sub.add_parser("uninstall", help="bootcmd back to the TFTP loop only")
    n = sub.add_parser("netmode", help="HV address from DHCP or U-Boot's "
                       "static ipaddr (bzdk_net; also refreshes bzdk_boot "
                       "so that it exports bzdk_net)")
    n.add_argument("mode", choices=("dhcp", "static"))
    a = ap.parse_args()

    if a.cmd == "vars":
        for k, v in VARS.items():
            print(f"{k}={v}")
        print(f"bootcmd={NEW_BOOTCMD}")
    elif a.cmd == "nc-script":
        sys.stdout.write(nc_script(not a.no_boot, a.slot))
    elif a.cmd == "env-file":
        sys.stdout.write(env_file(a.netmask, a.gatewayip))
    elif a.cmd == "install":
        env_tool("backup", a.backup)
        cur = env_tool("get", "bootcmd").strip().splitlines()[-1]
        if cur not in (OLD_BOOTCMD, NEW_BOOTCMD):
            sys.exit(f"bootcmd is not the known TFTP loop, refusing:\n{cur}")
        sets = [f"{k}={v}" for k, v in VARS.items()]
        sets += [f"netmask={a.netmask}", f"gatewayip={a.gatewayip}",
                 f"bootcmd={NEW_BOOTCMD}"]
        sys.stdout.write(env_tool("set", *sets))
    elif a.cmd == "uninstall":
        sys.stdout.write(env_tool("set", f"bootcmd={OLD_BOOTCMD}"))
    elif a.cmd == "netmode":
        sys.stdout.write(env_tool("set", f"bzdk_net={a.mode}",
                                  f"bzdk_boot={VARS['bzdk_boot']}"))


if __name__ == "__main__":
    main()
