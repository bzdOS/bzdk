#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Fix root-ownership/permission bits on the guest's UFS2 root, directly via
the HV's emmc_bio driver over EMAC -- needed because login/PAM refuse to use
files not owned by uid 0 or that are group/other-writable.

Confirmed live 2026-07-28: typing "root" at a real `login:` prompt (reached
for the first time this session after the gtrace/TVM fix) produces
"login 45 - - pam_start(): System error" -- PAM refuses to initialize.
fix_ownership.py (an earlier session's version of this same idea) documents
the exact checks and file list below but depends on a `ufs2walk` module that
no longer exists on disk (lived in a different session's /tmp scratchpad).
This is a fresh, self-contained rewrite against the new ufs2walk.py in this
same directory -- same target list/checks, verified via this session's own
DWARF-checked field offsets.

Exact checks (from FreeBSD releng/15.1 source, not guessed):
  lib/libutil/_secure_path.c (for /etc/login.conf, called with uid=0,gid=0)
  contrib/openpam/lib/libpam/openpam_check_owner_perms.c's
  openpam_check_desc_owner_perms() (for /etc/pam.d/*, and indirectly every
  pam_*.so.6 module PAM dlopen()s). Both are satisfied by:
    - di_uid == 0
    - mode has neither S_IWGRP (0o020) nor S_IWOTH (0o002) set

READ-ONLY by default. --write requires --i-verified-the-dump-above.
"""
import sys, os, time, subprocess, argparse, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hvdbg import HV
import ufs2walk

PART_START = 278562          # GPT p3 (freebsd-ufs "rootfs") first LBA -- same
                              # constant as ufs_clean.py/fix_ownership.py,
                              # independently re-derived from the live GPT.
# NOT actually free scratch DRAM -- 0x50030000 is vnet_emac.c's own
# breadcrumb window (VNET_BC_BASE, hv_addrmap.h); confirmed live 2026-07-28
# via ufs2fuse.py's write corruption chase. Ownership writes here only ever
# touched offsets 0/4 (di_mode/di_uid), which happened not to land on a
# live counter, so this script's own writes were never visibly wrong -- but
# it was still clobbering vnet diagnostics on every call. Use the confirmed
# free gap (0x50021040..0x50030000, see hv_addrmap.h) instead.
SCRATCH_PA = 0x50022000

S_IWGRP = 0o020
S_IWOTH = 0o002

# Same factory-packaged-uid=1001 defect fix_ownership.py documented: the
# whole pam_*.so.6 module set, plus login.conf and the two pam.d files login
# actually touches.
_PAM_MODULES = [
    "chroot", "deny", "echo", "exec", "ftpusers", "group", "guest", "krb5",
    "ksu", "lastlog", "login_access", "nologin", "passwdqc", "permit",
    "radius", "rhosts", "rootok", "securetty", "self", "ssh", "tacplus",
    "unix", "xdg", "zfs_key",
]
TARGETS = ["/etc/login.conf", "/etc/pam.d/login", "/etc/pam.d/system"] + [
    f"/usr/lib/pam_{m}.so.6" for m in _PAM_MODULES
]

hv = HV()


nm = subprocess.run(["aarch64-linux-gnu-nm", "microkernel-dbg.elf"],
                     capture_output=True, text=True).stdout


def _sym(s):
    return next((int(l.split()[0], 16) for l in nm.splitlines()
                 if l.endswith(" T " + s)), None)


RD, WR = _sym("emmc_bio_read"), _sym("emmc_bio_write")


def _call(fn, *a):
    for _ in range(12):
        r = hv.call(fn, *a)
        if r is not None:
            return r
        time.sleep(0.4)
    raise IOError(f"hv.call(0x{fn:x}) returned None 12x (EMAC/HV down?)")


def read_block(lba):
    rc = _call(RD, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_read(lba={lba}) rc={rc}")
    # hv.read_words_stable(): read-until-two-agree, shared in hvdbg.py so
    # every disk-tooling script gets the same reliability guarantee (see
    # ufs2fuse.py's 2026-07-28 chase for why a plain read_words() isn't
    # always enough for static data like a staged disk block).
    w = hv.read_words_stable(SCRATCH_PA, 128, tries=8, settle=0.2)
    if not w:
        raise IOError(f"EMAC readback never stabilized for lba={lba}")
    return b"".join(x.to_bytes(4, "little") for x in w)


def write_block(lba, data):
    assert len(data) == 512
    for i in range(0, 512, 4):
        hv.write_word(SCRATCH_PA + i, int.from_bytes(data[i:i + 4], "little"))
    rc = _call(WR, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_write(lba={lba}) rc={rc}")
    # An immediate read-after-write can transiently return stale data (see
    # project memory "eMMC write verify transient fail") even though the
    # write itself landed -- confirmed live 2026-07-28: a verify "failure"
    # on lba=279374 turned out to already be uid=0 on a fresh re-read.
    # Retry with fresh reads before concluding the write really failed.
    for attempt in range(5):
        back = read_block(lba)
        if back == data:
            return
        time.sleep(0.5)
    raise IOError(f"VERIFY FAILED at lba={lba}: read-after-write mismatch "
                  f"(persisted across {attempt + 1} fresh re-reads)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--i-verified-the-dump-above", action="store_true",
                     dest="confirmed")
    args = ap.parse_args()

    ufs = ufs2walk.UFS2(read_block, PART_START)
    ufs.mount()
    print(f"mount OK -- UFS2 magic validated, fs={ufs.fs}\n")

    plan = []
    for path in TARGETS:
        try:
            st = ufs.lookup(path)
        except Exception as e:
            print(f"{path}: not found ({e}) -- skipping")
            continue
        needs_fix = (st["uid"] != 0) or (st["mode"] & (S_IWGRP | S_IWOTH))
        print(f"{path}: ino={st['ino']} lba={st['lba']} off={st['dinode_off']} "
              f"uid={st['uid']} mode=0o{st['mode']:o}  "
              f"{'NEEDS FIX' if needs_fix else 'already OK'}")
        plan.append(dict(path=path, lba=st["lba"], off=st["dinode_off"],
                          mode=st["mode"], uid=st["uid"], needs_fix=needs_fix))

    if not args.write:
        print("\n(read-only; re-run with --write "
              "--i-verified-the-dump-above to patch)")
        return
    if not args.confirmed:
        print("REFUSING: pass --i-verified-the-dump-above once you've "
              "reviewed the values printed above.")
        return

    for p in plan:
        if not p["needs_fix"]:
            print(f"{p['path']}: skipping, already compliant")
            continue
        sec = bytearray(read_block(p["lba"]))
        new_mode = p["mode"] & ~(S_IWGRP | S_IWOTH)
        struct.pack_into("<I", sec, p["off"] + 4, 0)         # di_uid
        struct.pack_into("<H", sec, p["off"] + 0, new_mode)  # di_mode
        try:
            write_block(p["lba"], bytes(sec))
            print(f"{p['path']}: patched -> uid=0 mode=0o{new_mode:o}, verified")
        except IOError as e:
            # The retry-with-fresh-reads in write_block() already fought the
            # documented transient-stale-immediate-read issue; if it STILL
            # reports a mismatch, don't let one flaky verify abort the whole
            # batch (confirmed live 2026-07-28: the write had in fact landed
            # correctly both times this fired). The final re-check pass
            # below re-reads every target fresh and reports the real ground
            # truth regardless of this warning.
            print(f"{p['path']}: WARNING write_block reported {e} -- "
                  f"continuing, final re-check below will confirm actual state")

    print("\nRe-checking all targets...")
    for path in TARGETS:
        try:
            st = ufs.lookup(path)
        except Exception:
            continue
        ok = st["uid"] == 0 and not (st["mode"] & (S_IWGRP | S_IWOTH))
        print(f"  {path}: uid={st['uid']} mode=0o{st['mode']:o}  "
              f"{'OK' if ok else 'STILL WRONG!'}")


if __name__ == "__main__":
    main()
