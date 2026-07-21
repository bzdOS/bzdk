#!/usr/bin/env python3
"""Fix root-ownership/permission bits on specific files in the guest's UFS2
root, directly via the HV's emmc_bio driver — needed because login/PAM
refuse to use files not owned by uid 0 or that are group/other-writable.

Exact checks replicated (fetched from FreeBSD releng/15.1 source, not
guessed): lib/libutil/_secure_path.c (for /etc/login.conf, called with
uid=0,gid=0) and contrib/openpam/lib/libpam/openpam_check_owner_perms.c's
openpam_check_desc_owner_perms() (for /etc/pam.d/*). Both are satisfied by:
  - di_uid == 0
  - mode has neither S_IWGRP (0o020) nor S_IWOTH (0o002) set
di_gid is irrelevant to both checks once group-write is clear, so it is left
untouched.

READ-ONLY by default. --write requires --i-verified-the-dump-above.
"""
import sys, time, subprocess, argparse, struct

sys.path.insert(0, "/tmp/scratch")
from hvdbg import HV
import ufs2walk

PART_START = 278562
SCRATCH_PA = 0x50030000
DEV_BSIZE = 512

DI_MODE = 0     # uint16
DI_UID  = 4     # uint32  (di_mode@0,di_nlink@2,di_uid@4,di_gid@8 — dinode.h)
S_IWGRP = 0o020
S_IWOTH = 0o002

TARGETS = ["/etc/login.conf", "/etc/pam.d/login"]

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
    w = _rw(SCRATCH_PA, 128)
    if not w:
        raise IOError(f"EMAC readback failed for lba={lba}")
    return b"".join(x.to_bytes(4, "little") for x in w)

def write_block(lba, data):
    assert len(data) == 512
    for i in range(0, 512, 4):
        hv.write_word(SCRATCH_PA + i, int.from_bytes(data[i:i+4], "little"))
    rc = _call(WR, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_write(lba={lba}) rc={rc}")
    back = read_block(lba)
    if back != data:
        raise IOError(f"VERIFY FAILED at lba={lba}: read-after-write mismatch")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--i-verified-the-dump-above", action="store_true",
                    dest="confirmed")
    args = ap.parse_args()

    ufs = ufs2walk.UFS2(read_block, PART_START, write_block=write_block)
    ufs.mount()
    print("mount OK — UFS2 magic validated\n")

    plan = []
    for path in TARGETS:
        info = ufs.lookup(path)
        ino = info["ino"]
        fsba = ufs._ino_to_fsba(ino)
        fsbo = ufs._ino_to_fsbo(ino)
        abs_lba_block = ufs._abs_lba_for_fsblock(fsba)
        dinode_byte_off = fsbo * ufs2walk.DINODE_SIZE
        sector_index = dinode_byte_off // DEV_BSIZE
        intra_off = dinode_byte_off % DEV_BSIZE
        assert intra_off + ufs2walk.DINODE_SIZE <= DEV_BSIZE, \
            "dinode straddles a sector boundary — unexpected, aborting"
        sector_lba = abs_lba_block + sector_index

        sec = read_block(sector_lba)
        (mode,) = struct.unpack_from("<H", sec, intra_off + DI_MODE)
        (uid,) = struct.unpack_from("<I", sec, intra_off + DI_UID)

        needs_fix = (uid != 0) or (mode & (S_IWGRP | S_IWOTH))
        print(f"{path}: ino={ino} lba={sector_lba} intra_off={intra_off}  "
              f"uid={uid} mode=0o{mode:o}  "
              f"{'NEEDS FIX' if needs_fix else 'already OK'}")
        plan.append(dict(path=path, lba=sector_lba, off=intra_off,
                         mode=mode, uid=uid, needs_fix=needs_fix))

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
        struct.pack_into("<I", sec, p["off"] + DI_UID, 0)
        struct.pack_into("<H", sec, p["off"] + DI_MODE, new_mode)
        write_block(p["lba"], bytes(sec))
        print(f"{p['path']}: patched -> uid=0 mode=0o{new_mode:o}, "
              f"verified")

    print("\nRe-checking all targets...")
    for path in TARGETS:
        info = ufs.lookup(path)
        ino = info["ino"]
        fsba = ufs._ino_to_fsba(ino); fsbo = ufs._ino_to_fsbo(ino)
        abs_lba_block = ufs._abs_lba_for_fsblock(fsba)
        off = fsbo * ufs2walk.DINODE_SIZE
        lba = abs_lba_block + off // DEV_BSIZE
        sec = read_block(lba)
        (mode,) = struct.unpack_from("<H", sec, (off % DEV_BSIZE) + DI_MODE)
        (uid,) = struct.unpack_from("<I", sec, (off % DEV_BSIZE) + DI_UID)
        ok = uid == 0 and not (mode & (S_IWGRP | S_IWOTH))
        print(f"  {path}: uid={uid} mode=0o{mode:o}  "
              f"{'OK' if ok else 'STILL WRONG!'}")

if __name__ == "__main__":
    main()
