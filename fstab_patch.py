#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Locate (and optionally patch) the guest's on-disk /etc/fstab via the HV's
emmc_bio PIO driver, over the EMAC debug channel. READ-ONLY unless --write."""
import sys, time, subprocess, argparse
sys.path.insert(0, "/tmp/claude-0/-opt-bzdos/dd035069-c4a1-41c1-ae8a-9af1253403f0/scratchpad")
from hvdbg import HV
import ufs2walk

PART_START = 278562        # GPT p3 (freebsd-ufs "rootfs") first LBA
SCRATCH_PA = 0x50030000    # HV-scratch, inside DTB-reserved hv-scratch window

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
    """hv.call, retried — under guest-boot load EMAC drops the reply and
    hv.call returns None; that's transient, retry. Persistent None => real."""
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
    # stage the 512 bytes into SCRATCH_PA word-by-word, then emmc_bio_write
    for i in range(0, 512, 4):
        word = int.from_bytes(data[i:i+4], "little")
        hv.write_word(SCRATCH_PA + i, word)
    rc = _call(WR, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_write(lba={lba}) rc={rc}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="actually overwrite /etc/fstab entries (default: read-only)")
    args = ap.parse_args()

    print(f"emmc_bio_read=0x{RD:x} emmc_bio_write=0x{WR:x}")
    ufs = ufs2walk.UFS2(read_block, PART_START, write_block=write_block)
    ufs.mount()
    print("mount OK — UFS2 magic validated")
    info = ufs.lookup("/etc/fstab")
    print(f"/etc/fstab: ino={info['ino']} size={info['size']} "
          f"mode=0o{info['mode']:o} bsize={info['block_bytes']} "
          f"first_direct_lbas={info['direct_lbas'][:3]}")
    content = ufs.read_file("/etc/fstab")
    print("=== /etc/fstab (current, on disk) ===")
    print(content.decode("latin1", "replace"))
    print("=== end ===")

    if not args.write:
        print("\n(read-only; re-run with --write to patch)")
        return

    if b"mmcsd0" not in content and b"/dev/" not in content:
        print("REFUSING to write: content doesn't look like fstab (no /dev/ entries)")
        return
    # Correct device (mmcsd0 -> virtio vtbd0) AND pass=0 so rc.d/fsck skips the
    # (interactive, dirty-FS) fsck that was hanging the boot. Drop the swap
    # entry (p4) to avoid another missing-device error. Root is remounted rw
    # via this entry. Pad with a '#' comment to the EXACT original size so the
    # patch leaves no NUL bytes in the file (getfsent-safe).
    orig_size = info["size"]
    line = b"/dev/vtbd0p3  /  ufs  rw,noatime  0 0\n"
    if len(line) > orig_size:
        print("REFUSING: new line longer than original fstab"); return
    pad = orig_size - len(line)
    new = line + b"#" + b"x" * (pad - 2) + b"\n" if pad >= 2 else line + b"#" * pad
    assert len(new) == orig_size, (len(new), orig_size)
    print(f"NEW fstab ({len(new)} bytes):\n{new.decode()}")
    res = ufs2walk.patch_file_first_block(ufs, "/etc/fstab", new)
    print(f"PATCHED lba={res['lba']}: wrote {len(new)} bytes; re-read verify passed")
    print("=== /etc/fstab (after) ===")
    print(ufs.read_file("/etc/fstab").decode("latin1", "replace"))

if __name__ == "__main__":
    main()
