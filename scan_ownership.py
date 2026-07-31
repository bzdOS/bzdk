#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Recursively scan the guest's UFS2 root for the systemic bpi-image.sh
packaging defect: regular files owned by a stray build-time uid (1001)
instead of root. Found first on /etc/pam.d/*, /usr/lib/pam_*.so.6, then
/sbin/shutdown (setuid to uid 1001 -- explains 'shutdown: NOT super-user'
despite a genuine root shell: the kernel sets the child's EUID to the
FILE OWNER on exec of a setuid binary, unrelated to the caller's own uid).
fix_ownership.py's whack-a-mole (patch whatever login/PAM/shutdown happens
to touch next) was too narrow -- this scans every regular file under a set
of system directories and reports/fixes ALL of them in one pass.

Only the OWNER (di_uid) is changed to 0. setuid/setgid bits are LEFT ALONE
where legitimate (e.g. /usr/bin/su is meant to be setuid-root) -- fixing
the owner to root is what makes an intentionally-setuid binary behave
correctly; group/other write bits are still cleared for the non-setuid
PAM-style checks fix_ownership.py already handles, kept here as the same
policy for consistency.

READ-ONLY by default. --write requires --i-verified-the-dump-above.
"""
import sys, time, subprocess, argparse, struct

sys.path.insert(0, "/tmp/scratch")
from hvdbg import HV
import ufs2walk

PART_START = 278562
SCRATCH_PA = 0x50030000
DEV_BSIZE = 512

DI_MODE = 0
DI_UID = 4
S_IWGRP = 0o020
S_IWOTH = 0o002
IFMT = 0o170000
IFDIR = 0o040000
IFREG = 0o100000

# Top-level system directories to scan recursively. Not the whole disk
# (skips /home, /usr/src, /usr/ports-style trees, log/spool dirs where a
# non-root owner can be legitimate) -- these are the dirs whose files must
# behave as root-trusted system binaries/config for login/PAM/shutdown/rc
# to work at all.
ROOTS = ["/sbin", "/usr/sbin"]
# NOTE (2026-07-22): the full ["/etc", "/sbin", "/bin", "/usr/sbin", "/usr/bin",
# "/usr/lib", "/lib"] sweep was too slow to be practical -- 45 minutes and
# still not through /etc alone, one EMAC round-trip per file with no
# batching. Scoped down to the two directories that actually matter for
# reaching a working login + successful `shutdown` (already found
# /etc/pam.d/*, /usr/lib/pam_*.so.6, and /sbin/shutdown broken via targeted
# checks -- /usr/bin and /usr/lib's remaining contents don't block boot/
# login/shutdown, only running specific unrelated programs later). Restore
# the full list for a proper overnight unattended sweep if that's ever
# wanted; for same-session use, scope stays narrow.
MAX_DEPTH = 6  # generous; these trees are not that deep

hv = HV()

def _rw(a, n):
    for _ in range(40):
        w = hv.read_words(a, n)
        if w:
            return w
        time.sleep(0.3)
    return None

nm = subprocess.run(["aarch64-linux-gnu-nm", "microkernel-dbg.elf"],
                    capture_output=True, text=True).stdout
def _sym(s):
    return next((int(l.split()[0], 16) for l in nm.splitlines()
                if l.endswith(" T " + s)), None)
RD, WR = _sym("emmc_bio_read"), _sym("emmc_bio_write")

def _call(fn, *a):
    for _ in range(15):
        r = hv.call(fn, *a)
        if r is not None:
            return r
    raise IOError(f"hv.call(0x{fn:x}) returned None 15x (EMAC/HV down?)")

def read_block(lba):
    rc = _call(RD, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_read(lba={lba}) rc={rc}")
    w = _rw(SCRATCH_PA, 128)
    if not w:
        raise IOError(f"EMAC readback failed for lba={lba}")
    return b"".join(x.to_bytes(4, "little") for x in w)

def write_block(lba, data, old=None):
    """Write `data` (512 bytes) to `lba`. If `old` is given (the exact bytes
    read_block(lba) just returned, i.e. what's still sitting unmodified in
    SCRATCH_PA -- nothing else touches that staging buffer in between), skip
    re-writing any 4-byte word that's unchanged from `old`. A single
    dbgmon 'w' command is one full EMAC round-trip (~60ms); rewriting all
    128 words of every sector when only 1-2 actually differ (di_uid/di_mode
    inside one dinode) made this ~8s/file -- found live via tcpdump on br0
    while this was running, see the memory/session notes. Diffing against
    `old` cuts a typical ownership-fix write from 128 round-trips to 1-2."""
    assert len(data) == 512
    for i in range(0, 512, 4):
        if old is not None and old[i:i+4] == data[i:i+4]:
            continue
        hv.write_word(SCRATCH_PA + i, int.from_bytes(data[i:i+4], "little"))
    rc = _call(WR, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_write(lba={lba}) rc={rc}")
    back = read_block(lba)
    if back != data:
        raise IOError(f"VERIFY FAILED at lba={lba}: read-after-write mismatch")

def list_dir(ufs, dir_inode):
    """Return [(name, ino)] for every non-'.'/'..' entry, with d_type."""
    buf = ufs._read_direct_data(dir_inode)
    cur = 0
    out = []
    while cur + ufs2walk.DIRENT_NAME_OFF <= len(buf):
        d_ino, = struct.unpack_from("<I", buf, cur + ufs2walk.DIRENT_INO_OFF)
        d_reclen, = struct.unpack_from("<H", buf, cur + ufs2walk.DIRENT_RECLEN_OFF)
        d_namlen = buf[cur + ufs2walk.DIRENT_NAMLEN_OFF]
        if d_reclen < ufs2walk.DIRENT_NAME_OFF or cur + d_reclen > len(buf):
            break
        if d_ino != 0:
            name = bytes(buf[cur+ufs2walk.DIRENT_NAME_OFF: cur+ufs2walk.DIRENT_NAME_OFF+d_namlen]).decode("latin-1")
            if name not in (".", ".."):
                out.append((name, d_ino))
        cur += d_reclen
    return out

def scan(ufs, path, depth, findings, seen_inodes):
    if depth > MAX_DEPTH:
        print(f"  {path}: max depth reached, not descending")
        return
    try:
        dir_inode = ufs._resolve(path)
    except Exception as e:
        print(f"  {path}: cannot resolve ({e}) — skipping")
        return
    if (dir_inode["mode"] & IFMT) != IFDIR:
        print(f"  {path}: not a directory — skipping")
        return
    for name, ino in list_dir(ufs, dir_inode):
        child_path = path.rstrip("/") + "/" + name
        if ino in seen_inodes:
            continue  # hardlink/dup, already recorded
        try:
            inode = ufs._read_inode(ino)
        except Exception as e:
            print(f"  {child_path}: read_inode failed ({e}) — skipping")
            continue
        ifmt = inode["mode"] & IFMT
        if ifmt == IFDIR:
            scan(ufs, child_path, depth + 1, findings, seen_inodes)
        elif ifmt == IFREG:
            seen_inodes.add(ino)
            findings.append((child_path, ino, inode["mode"], inode["uid"]))
        # symlinks / device nodes / etc: not relevant to this defect, skip

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--i-verified-the-dump-above", action="store_true", dest="confirmed")
    args = ap.parse_args()

    ufs = ufs2walk.UFS2(read_block, PART_START, write_block=write_block)
    ufs.mount()
    print("mount OK — UFS2 magic validated\n")

    findings = []
    seen = set()
    for root in ROOTS:
        print(f"scanning {root} ...")
        scan(ufs, root, 0, findings, seen)

    bad = [(p, ino, mode, uid) for (p, ino, mode, uid) in findings if uid != 0]
    print(f"\n{len(findings)} regular files scanned under {ROOTS}")
    print(f"{len(bad)} with uid != 0:\n")
    for p, ino, mode, uid in bad:
        setuid = "SETUID " if (mode & 0o4000) else ""
        setgid = "SETGID " if (mode & 0o2000) else ""
        print(f"  {p}: ino={ino} uid={uid} mode=0o{mode & 0o7777:04o} {setuid}{setgid}")

    if not args.write:
        print("\n(read-only; re-run with --write --i-verified-the-dump-above to patch)")
        return
    if not args.confirmed:
        print("REFUSING: pass --i-verified-the-dump-above once you've reviewed the list above.")
        return

    print(f"\nPatching {len(bad)} files (uid -> 0, clearing group/other write)...")
    for p, ino, mode, uid in bad:
        fsba = ufs._ino_to_fsba(ino)
        fsbo = ufs._ino_to_fsbo(ino)
        abs_lba_block = ufs._abs_lba_for_fsblock(fsba)
        dinode_byte_off = fsbo * ufs2walk.DINODE_SIZE
        sector_index = dinode_byte_off // DEV_BSIZE
        intra_off = dinode_byte_off % DEV_BSIZE
        assert intra_off + ufs2walk.DINODE_SIZE <= DEV_BSIZE
        sector_lba = abs_lba_block + sector_index

        orig = read_block(sector_lba)
        sec = bytearray(orig)
        (cur_mode,) = struct.unpack_from("<H", sec, intra_off + DI_MODE)
        new_mode = cur_mode & ~(S_IWGRP | S_IWOTH)
        struct.pack_into("<I", sec, intra_off + DI_UID, 0)
        struct.pack_into("<H", sec, intra_off + DI_MODE, new_mode)
        write_block(sector_lba, bytes(sec), old=orig)
        print(f"  {p}: patched -> uid=0 mode=0o{new_mode:04o}, verified")

    print("\nRe-scanning to confirm...")
    findings2 = []
    seen2 = set()
    for root in ROOTS:
        scan(ufs, root, 0, findings2, seen2)
    remaining = [(p, ino, mode, uid) for (p, ino, mode, uid) in findings2 if uid != 0]
    if remaining:
        print(f"STILL {len(remaining)} with uid != 0 (unexpected):")
        for p, ino, mode, uid in remaining:
            print(f"  {p}: uid={uid}")
    else:
        print(f"DONE. All {len(findings2)} scanned files now uid=0.")

if __name__ == "__main__":
    main()
