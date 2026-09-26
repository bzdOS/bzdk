#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""uboot_env.py — read and rewrite U-Boot's environment from the guest.

WHY: changing `bootcmd` (or anything else U-Boot reads at boot) currently
needs the U-Boot prompt, and the prompt only exists on uart0's physical pins
and the USB-ACM gadget. That is the one thing standing between this board and
running with no cable attached -- see docs/dropping-usb.md. U-Boot keeps its
environment in a plain file, `uboot.env` on the ESP, and the guest can mount
the ESP. So the prompt is not actually required: this rewrites the file.

docs/dropping-usb.md calls this step 2 and wants it precisely BECAUSE it does
not depend on netconsole. Netconsole is the interactive route and has to be
flashed to be tested; this one already works with the U-Boot that is on the
board, and stays working if a netconsole build turns out to be broken.

FORMAT: 4-byte little-endian CRC32 of everything after it, then NUL-separated
`key=value` entries, then NUL padding to the partition's env size (64 KiB
here). U-Boot rejects the file outright if the CRC does not match and falls
back to its built-in defaults -- which on this board means no bootcmd, i.e. a
board that stops at the prompt. So every write here is CRC-recomputed, parsed
back, and verified by re-reading the file off the card.

REQUIRES the vblk boot-guard floor to be at the ESP rather than 2 KiB into
it; before that fix the ESP could not be mounted read/write at all and
mount_msdosfs failed with EIO. See VBLK_BOOT_GUARD_LBA in vblk_emmc.h.

    uboot_env.py get [name]
    uboot_env.py set NAME=VALUE [NAME=VALUE ...]
    uboot_env.py unset NAME [NAME ...]
    uboot_env.py backup <file>
"""
import argparse
import binascii
import subprocess
import sys

GUEST = 'root@192.168.88.82'
KEY = '/root/.ssh/chimp_ed25519'
ESP_DEV = '/dev/vtbd0p2'
MNT = '/tmp/esp'
ENV_PATH = MNT + '/uboot.env'
ENV_SIZE = 65536


def ssh(cmd, binary=False, check=True):
    argv = ['ssh', '-o', 'ConnectTimeout=10', '-o', 'BatchMode=yes',
            '-i', KEY, GUEST, cmd]
    # stdin=DEVNULL: under `timeout` (or any non-foreground process group) an
    # ssh left reading the caller's stdin is stopped by SIGTTIN and the read
    # comes back empty -- which is what made sdbox_read.py fail 2026-09-26.
    r = subprocess.run(argv, capture_output=True, timeout=180,
                       stdin=subprocess.DEVNULL)
    if check and r.returncode:
        sys.exit(f"guest command failed: {cmd}\n{r.stderr.decode(errors='replace')}")
    return r.stdout if binary else r.stdout.decode(errors='replace')


def mount(rw=False):
    # Always start from a known state: a leftover read-only mount from an
    # earlier run would silently turn a write into a no-op.
    ssh(f'umount {MNT} 2>/dev/null; mkdir -p {MNT}', check=False)
    opt = '' if rw else '-o ro'
    ssh(f'mount_msdosfs {opt} {ESP_DEV} {MNT}')


def unmount():
    ssh(f'umount {MNT} 2>/dev/null', check=False)


def parse(blob):
    if len(blob) != ENV_SIZE:
        sys.exit(f"env is {len(blob)} bytes, expected {ENV_SIZE}")
    stored = int.from_bytes(blob[:4], 'little')
    body = blob[4:]
    actual = binascii.crc32(body) & 0xffffffff
    if stored != actual:
        sys.exit(f"env CRC mismatch (stored 0x{stored:08x}, computed "
                 f"0x{actual:08x}) — refusing to touch it")
    env = {}
    for entry in body.split(b'\x00'):
        if not entry:
            continue
        k, _, v = entry.partition(b'=')
        env[k.decode('latin1')] = v.decode('latin1')
    return env


def serialise(env):
    body = b''.join(f"{k}={v}".encode('latin1') + b'\x00'
                    for k, v in sorted(env.items()))
    if len(body) + 4 > ENV_SIZE:
        sys.exit(f"env too large: {len(body) + 4} > {ENV_SIZE}")
    body += b'\x00' * (ENV_SIZE - 4 - len(body))
    return (binascii.crc32(body) & 0xffffffff).to_bytes(4, 'little') + body


def read_env():
    mount(rw=False)
    try:
        return ssh(f'cat {ENV_PATH}', binary=True)
    finally:
        unmount()


def write_env(blob):
    # Stage to tmpfs and move into place, so a half-written file never exists
    # on the ESP: U-Boot reading a truncated env gets a CRC failure and boots
    # with no bootcmd at all.
    b64 = binascii.b2a_base64(blob, newline=False).decode()
    mount(rw=True)
    try:
        ssh(f"printf %s '{b64}' | b64decode -r > /tmp/uboot.env.new")
        staged = ssh('md5 -q /tmp/uboot.env.new').strip()
        ssh(f'cp /tmp/uboot.env.new {ENV_PATH} && sync')
        landed = ssh(f'md5 -q {ENV_PATH}').strip()
    finally:
        unmount()
    if staged != landed:
        sys.exit(f"write did not land: staged {staged}, on card {landed}")
    # Re-read through a fresh mount: the check above can be satisfied by the
    # buffer cache alone.
    back = read_env()
    if back != blob:
        sys.exit("verification read differs from what was written")
    parse(back)
    return landed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('get'); g.add_argument('name', nargs='?')
    s = sub.add_parser('set'); s.add_argument('assignment', nargs='+')
    u = sub.add_parser('unset'); u.add_argument('name', nargs='+')
    b = sub.add_parser('backup'); b.add_argument('file')
    a = ap.parse_args()

    blob = read_env()
    env = parse(blob)

    if a.cmd == 'get':
        if a.name:
            if a.name not in env:
                sys.exit(f"{a.name} is not set")
            print(env[a.name])
        else:
            for k in sorted(env):
                print(f"{k}={env[k]}")
        return

    if a.cmd == 'backup':
        open(a.file, 'wb').write(blob)
        print(f"wrote {len(blob)} bytes to {a.file} ({len(env)} variables)")
        return

    before = dict(env)
    if a.cmd == 'set':
        for item in a.assignment:
            if '=' not in item:
                sys.exit(f"expected NAME=VALUE, got {item!r}")
            k, _, v = item.partition('=')
            env[k] = v
    else:
        for k in a.name:
            if k not in env:
                sys.exit(f"{k} is not set")
            del env[k]

    for k in sorted(set(before) | set(env)):
        if before.get(k) != env.get(k):
            print(f"  - {k}={before.get(k)}" if k in before else f"  - {k} (unset)")
            print(f"  + {k}={env[k]}" if k in env else f"  + {k} (removed)")

    print(f"writing {len(env)} variables ...")
    print("ok, md5", write_env(serialise(env)))


if __name__ == '__main__':
    main()
