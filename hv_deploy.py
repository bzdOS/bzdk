#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""hv_deploy.py -- put a hypervisor (and optionally guest kernel/DTB) into the
eMMC boot slots that U-Boot boots from (emmc_boot.py).

Files go through the running guest: scp into its /tmp (tmpfs), md5 checked
there, then copied onto the ESP (vtbd0p2, mounted read/write only for the
copy) as <name>.new, synced, md5 checked on the ESP, renamed over the old
file, and checked once more through a fresh read-only mount. A half-written
file therefore never has the name U-Boot loads; a crash mid-copy leaves the
old slot intact (and if it were not, U-Boot's uImage CRC check and the
slot fallback still catch it).

    hv_deploy.py status
    hv_deploy.py deploy [--hv F] [--kernel F] [--dtb F] [--slot cur|lkg]
    hv_deploy.py init                  both slots from the TFTP files
    hv_deploy.py promote               cur -> lkg (after cur proved itself)
    hv_deploy.py rollback              lkg -> cur
    hv_deploy.py reboot                clean guest `shutdown -r now`

`deploy` writes `cur` and resets the slot record's tries over the debug
channel (dbgmon `bootslot tries 0`), so the next boot tries the new image
three times before falling back to `lkg`. Default sources are what `make
hv-uimage` and the TFTP setup use: /opt/bzdos/tftpboot/{microkernel-dbg.uimg,
kernel,bananapi-min.dtb}. Every board action runs under the board lock
(/opt/bzdos/linux-work/.board.lock) for its own duration only.
"""
import argparse
import contextlib
import fcntl
import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bzd_board  # noqa: E402

LOCK = "/opt/bzdos/linux-work/.board.lock"
TFTP = "/opt/bzdos/tftpboot"
ESP_DEV = "/dev/vtbd0p2"
MNT = "/tmp/esp-deploy"
SLOTS = ("cur", "lkg")
FILES = {"hv": "hv.uimg", "kernel": "kernel", "dtb": "dtb"}
DEFAULT_SRC = {"hv": f"{TFTP}/microkernel-dbg.uimg", "kernel": f"{TFTP}/kernel",
               "dtb": f"{TFTP}/bananapi-min.dtb"}
UIMAGE_MAGIC = b"\x27\x05\x19\x56"


@contextlib.contextmanager
def board_lock():
    with open(LOCK, "a") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)


def ssh(cmd, check=True, timeout=300):
    r = subprocess.run(bzd_board.guest_ssh_argv(10) + [cmd], capture_output=True,
                       text=True, timeout=timeout, stdin=subprocess.DEVNULL)
    if check and r.returncode:
        raise SystemExit(f"guest: {cmd}\n{r.stdout}{r.stderr}")
    return r.stdout


def scp(src, dst):
    r = subprocess.run(["scp", "-q", "-o", "BatchMode=yes", "-o",
                        "ConnectTimeout=10", "-i", bzd_board.GUEST_SSH_KEY, src,
                        f"{bzd_board.GUEST}:{dst}"], capture_output=True,
                       text=True, timeout=600, stdin=subprocess.DEVNULL)
    if r.returncode:
        raise SystemExit(f"scp {src}: {r.stderr}")


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest()


def check_source(kind, path):
    b = open(path, "rb").read()
    if kind == "hv":
        if b[:4] != UIMAGE_MAGIC:
            raise SystemExit(f"{path}: not a U-Boot uImage")
        r = subprocess.run(["mkimage", "-l", path], capture_output=True, text=True)
        if r.returncode or "Standalone" not in r.stdout:
            raise SystemExit(f"{path}: mkimage -l does not accept it:\n{r.stdout}{r.stderr}")
    elif kind == "dtb":
        if b[:4] != b"\xd0\x0d\xfe\xed":
            raise SystemExit(f"{path}: not a flattened device tree")
    elif kind == "kernel":
        if b[:4] != b"\x7fELF":
            raise SystemExit(f"{path}: not an ELF kernel")
    return hashlib.md5(b).hexdigest()


def mount(rw):
    ssh(f"umount {MNT} 2>/dev/null; mkdir -p {MNT}", check=False)
    ssh(f"mount_msdosfs {'' if rw else '-o ro'} {ESP_DEV} {MNT}")


def unmount():
    ssh(f"sync; umount {MNT}", check=False)


def esp_md5s():
    """{slot: {name: md5 or None}} read through a fresh read-only mount."""
    mount(rw=False)
    try:
        out = ssh(f"cd {MNT} && for s in {' '.join(SLOTS)}; do for f in "
                  f"{' '.join(FILES.values())}; do [ -f bzdk/$s/$f ] && echo "
                  f"$s $f $(md5 -q bzdk/$s/$f) || echo $s $f -; done; done; "
                  f"df -k {MNT} | tail -1")
    finally:
        unmount()
    res = {s: {} for s in SLOTS}
    lines = out.strip().splitlines()
    for line in lines[:-1]:
        s, f, m = line.split()
        res[s][f] = None if m == "-" else m
    res["_df"] = lines[-1]
    return res


def write_slot(slot, items):
    """items: {name_on_esp: (local_path, md5)}"""
    stage = f"/tmp/deploy-{slot}"
    ssh(f"rm -rf {stage}; mkdir -p {stage}")
    for name, (path, m) in items.items():
        scp(path, f"{stage}/{name}")
        got = ssh(f"md5 -q {stage}/{name}").strip()
        if got != m:
            raise SystemExit(f"{name}: guest /tmp copy md5 {got} != {m}")
    mount(rw=True)
    try:
        ssh(f"mkdir -p {MNT}/bzdk/{slot}")
        for name, (path, m) in items.items():
            dst = f"{MNT}/bzdk/{slot}/{name}"
            ssh(f"cp {stage}/{name} {dst}.new && sync")
            got = ssh(f"md5 -q {dst}.new").strip()
            if got != m:
                raise SystemExit(f"{name}: ESP copy md5 {got} != {m}; "
                                 f"{dst}.new left, slot unchanged")
            ssh(f"mv -f {dst}.new {dst} && sync")
    finally:
        unmount()
        ssh(f"rm -rf {stage}", check=False)
    back = esp_md5s()[slot]
    for name, (path, m) in items.items():
        if back.get(name) != m:
            raise SystemExit(f"{slot}/{name}: re-read md5 {back.get(name)} != {m}")
        print(f"  {slot}/{name}  {m}  ok")


def copy_slot(src, dst):
    mount(rw=True)
    try:
        ssh(f"mkdir -p {MNT}/bzdk/{dst}")
        for name in FILES.values():
            a, b = f"{MNT}/bzdk/{src}/{name}", f"{MNT}/bzdk/{dst}/{name}"
            ssh(f"[ -f {a} ] && cp {a} {b}.new && sync && "
                f"[ $(md5 -q {a}) = $(md5 -q {b}.new) ] && mv -f {b}.new {b} && sync")
    finally:
        unmount()
    m = esp_md5s()
    for name in FILES.values():
        if m[src].get(name) is None or m[src].get(name) != m[dst].get(name):
            raise SystemExit(f"{dst}/{name} does not match {src}/{name} after the copy")
    print(f"{src} -> {dst}: {', '.join(FILES.values())} identical")


def hv_cmd(line):
    import hvdbg
    hv = hvdbg.HV()
    try:
        return hv.cmd(line)
    finally:
        try:
            hv.sock.close()
        except OSError:
            pass


def reset_tries():
    r = hv_cmd("bootslot tries 0")
    print(f"slot record: {r.strip()}")


def cmd_status(_a):
    with board_lock():
        m = esp_md5s()
        try:
            slot = hv_cmd("bootslot").strip()
        except Exception as e:      # debug channel down: still show the ESP
            slot = f"(debug channel: {e})"
    for s in SLOTS:
        for n in FILES.values():
            print(f"  {s}/{n:8s} {m[s].get(n) or '-'}")
    print(f"  ESP: {m['_df']}")
    print(f"  {slot}   (slot 1 cur, 2 lkg, 3 TFTP; healthy=1 after 90 s)")
    srcs = {k: md5(p) for k, p in DEFAULT_SRC.items() if os.path.exists(p)}
    for k, h in srcs.items():
        where = [s for s in SLOTS if m[s].get(FILES[k]) == h]
        print(f"  tftpboot {k:6s} {h} in: {', '.join(where) or 'no slot'}")


def cmd_deploy(a):
    items = {}
    for kind in ("hv", "kernel", "dtb"):
        path = getattr(a, kind)
        if path is None and (kind == "hv" or a.all):
            path = DEFAULT_SRC[kind]
        if path:
            items[FILES[kind]] = (path, check_source(kind, path))
    with board_lock():
        have = esp_md5s()
        missing = [n for n in FILES.values() if have[a.slot].get(n) is None
                   and n not in items]
        if missing:
            raise SystemExit(f"slot {a.slot} would be incomplete (no "
                             f"{', '.join(missing)}): pass them or use --all")
        write_slot(a.slot, items)
    if a.slot == "cur":
        with board_lock():
            reset_tries()


def cmd_init(_a):
    items = {FILES[k]: (p, check_source(k, p)) for k, p in DEFAULT_SRC.items()}
    for slot in SLOTS:
        with board_lock():
            write_slot(slot, items)


def cmd_copy(src, dst):
    with board_lock():
        copy_slot(src, dst)
        if dst == "cur":
            reset_tries()


def cmd_reboot(_a):
    with board_lock():
        ssh("shutdown -r now", check=False, timeout=30)
    print("guest shutting down; the board resets into U-Boot -> eMMC slot")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    d = sub.add_parser("deploy")
    d.add_argument("--hv")
    d.add_argument("--kernel")
    d.add_argument("--dtb")
    d.add_argument("--all", action="store_true",
                   help="also kernel+dtb from tftpboot when not given")
    d.add_argument("--slot", choices=SLOTS, default="cur")
    sub.add_parser("init")
    sub.add_parser("promote")
    sub.add_parser("rollback")
    sub.add_parser("reboot")
    a = ap.parse_args()
    {"status": cmd_status, "deploy": cmd_deploy, "init": cmd_init,
     "promote": lambda _a: cmd_copy("cur", "lkg"),
     "rollback": lambda _a: cmd_copy("lkg", "cur"),
     "reboot": cmd_reboot}[a.cmd](a)


if __name__ == "__main__":
    main()
