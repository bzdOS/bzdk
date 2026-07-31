#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Mirror of FreeBSD's ffs_sbsearch() cylinder-group superblock-recovery scan
(sys/ufs/ffs/ffs_subr.c ~907-919, using cgbase/cgstart/cgsblock/fsbtodb from
sys/ufs/ffs/fs.h ~646-662), built to chase down a specific live finding:

Live boot capture (2026-07-27, printf-instrumented kernel, see project memory
mountroot-real-bug-is-ufs-sblock-offset.md) shows every automatic root-mount
attempt against ufs:/dev/vtbd0p3 failing with:
    UFS2 superblock failed: fs->fs_sblockactualloc (98304) != SBLOCK_UFS2
    (65536) && fs->fs_sblockactualloc (98304) != 0 (0)
98304 is NOT one of FreeBSD's fixed SBLOCKSEARCH candidates (65536, 8192,
floppy, 262144 — sys/ufs/ffs/fs.h ~75) — so it isn't a simple "tried the
wrong fixed offset" bug. Reading ffs_sbsearch() (ffs_subr.c ~798-920) shows
what actually happens on a ROOT mount (MNT_ROOTFS, ffs_vfsops.c ~921-922
routes root mounts through ffs_sbsearch(), not the simpler ffs_sbget(UFS_STDSB)
every other mount uses): if the standard-location read fails, it falls back
to a per-cylinder-group scan using geometry (fs_fpg/fs_sblkno/fs_fsbtodb)
recovered from a `struct fsrecovery` block newfs(8) stores near the start of
the partition. So 98304 is very likely cgsblock(fs, N) for some cylinder
group N != 0, computed from THIS filesystem's real on-disk geometry — which
this script can confirm/refute once the real fs_fpg/fs_sblkno/fs_fsbtodb are
read off the actual disk (they are NOT guessable from FreeBSD source; they
depend on how this specific filesystem was newfs'd).

This does NOT explain why the *first*, standard-location read at byte 65536
failed in the first place (that's the actual root cause; 98304 is just
where the kernel's fallback search happened to land while trying to
recover). That first-read failure needs the primary superblock's real bytes
(or the vconsole/HV EXC breadcrumbs around that specific access) to nail
down, which is live-board work — not attempted here.

Pure computation, no board I/O — same house style as gpt_recover.py.
"""
import struct

SBLOCK_UFS2 = 65536
SECTOR = 512

# Known, independently-verified GPT partition-3 (freebsd-ufs "rootfs") extent
# — read live via emmc_bio over the debug core, 2026-07-22 (see project
# memory rootmount-gpt-healthy-blocker-guestside.md), and reused by
# ufs_clean.py/fstab_patch.py/fix_ownership.py/scan_ownership.py:
PART_START_LBA = 278562
PART_LAST_LBA = 2858721
PART_SIZE_BYTES = (PART_LAST_LBA - PART_START_LBA + 1) * SECTOR


def cgbase(fs_fpg, cg):
    """fs.h:653 cgbase(fs,c) = ((ufs2_daddr_t)fs->fs_fpg) * c -- fragment addr."""
    return fs_fpg * cg


def cgstart(fs_fpg, cg, is_ufs2=True):
    """fs.h:660-662 cgstart -- UFS2 always takes the plain cgbase() branch
    (the fs_old_cgoffset adjustment is UFS1-only)."""
    if is_ufs2:
        return cgbase(fs_fpg, cg)
    raise NotImplementedError("only the UFS2 branch is mirrored here")


def cgsblock(fs_fpg, fs_sblkno, cg):
    """fs.h:658 cgsblock(fs,c) = cgstart(fs,c) + fs->fs_sblkno -- fragment
    address of cg `cg`'s superblock copy."""
    return cgstart(fs_fpg, cg) + fs_sblkno


def fsbtodb_bytes(frag_addr, fs_fsize):
    """fs.h:646 fsbtodb(fs,b) = b << fs->fs_fsbtodb, converting a fragment
    address to a 512-byte disk-block (sector) address. fs_fsbtodb is
    log2(fs_fsize/SECTOR) (the shift FreeBSD itself stores precomputed);
    reconstructed here from fs_fsize directly since that's what's actually
    knowable/guessable, not the raw shift count. Returns a BYTE offset
    (fragment_addr * fs_fsize), which is the same thing algebraically —
    fsbtodb's sector count times 512 equals the fragment count times
    fs_fsize, since a fragment addr counted in fs_fsize-byte units maps 1:1
    onto that many actual bytes."""
    return frag_addr * fs_fsize


def find_cg_for_byte_offset(fs_fpg, fs_sblkno, fs_fsize, fs_ncg, target_bytes):
    """Mirrors ffs_subr.c:907-919's scan: for cg in 0..fs_ncg-1, compute
    cgsblock(fs,cg) and convert to a byte offset via fsbtodb; return the
    list of (cg, byte_offset) pairs, flagging any that match target_bytes."""
    hits = []
    for cg in range(fs_ncg):
        frag_addr = cgsblock(fs_fpg, fs_sblkno, cg)
        byte_off = fsbtodb_bytes(frag_addr, fs_fsize)
        if byte_off == target_bytes:
            hits.append(cg)
    return hits


def cg0_offset(fs_fpg, fs_sblkno, fs_fsize):
    """cg=0's superblock offset MUST equal SBLOCK_UFS2 (65536) for any
    correctly-newfs'd UFS2 filesystem -- cg=0 is always the standard,
    primary location by construction. Used as a self-check on whatever
    fs_sblkno/fs_fsize pair is being tried, independent of the real-disk
    unknowns (fs_fpg doesn't matter for cg=0 since cgbase(fs,0)=0)."""
    return fsbtodb_bytes(cgsblock(fs_fpg, fs_sblkno, 0), fs_fsize)


if __name__ == "__main__":
    # ---- self-test: cg=0 must always land on SBLOCK_UFS2 (65536) --------
    # fs_sblkno is the fragment address of the superblock's position within
    # any single cylinder group; for UFS2 it's a fixed value independent of
    # fs_fpg (it lives right after the boot area, before the inode table).
    # Two plausible (fs_sblkno, fs_fsize) pairs that both correctly reproduce
    # SBLOCK_UFS2 for cg=0 (fs_fsize=4096 is the common default fragment
    # size; fs_sblkno=16 frags * 4096B/frag = 65536B checks out):
    for fs_fsize, fs_sblkno in [(4096, 16), (2048, 32), (8192, 8)]:
        off = cg0_offset(fs_fpg=0, fs_sblkno=fs_sblkno, fs_fsize=fs_fsize)
        status = "OK" if off == SBLOCK_UFS2 else "MISMATCH"
        print(f"self-test fs_fsize={fs_fsize} fs_sblkno={fs_sblkno}: "
              f"cg0 offset={off} (expect {SBLOCK_UFS2}) -> {status}")

    print()
    print(f"Partition 3 (rootfs) extent: LBA {PART_START_LBA}..{PART_LAST_LBA} "
          f"= {PART_SIZE_BYTES} bytes ({PART_SIZE_BYTES / (1 << 20):.1f} MiB)")
    print()
    print("TODO before this can confirm/refute the live 98304-byte-offset "
          "finding: read the REAL fs_fpg/fs_sblkno/fs_fsize/fs_ncg for this "
          "filesystem off the actual disk (either from the primary "
          "superblock at LBA (PART_START_LBA + 128), if it's readable at "
          "all with UFS_ALTSBLK-style raw parsing, or from the fsrecovery "
          "block FreeBSD's own search reads near byte (SBLOCK_UFS2 - "
          "secsize) -- see ffs_subr.c:877-899 for its exact layout: "
          "fsr_fpg, fsr_fsbtodb, fsr_sblkno, fsr_magic, fsr_ncg, packed at "
          "the END of some power-of-2 secsize block below byte 65536). "
          "Once known, call find_cg_for_byte_offset(fs_fpg, fs_sblkno, "
          "fs_fsize, fs_ncg, 98304) -- a non-empty result directly proves "
          "which cylinder group's alternate superblock the kernel's "
          "recovery scan is finding, closing this out definitively. An "
          "empty result would instead mean 98304 comes from somewhere this "
          "script doesn't model (worth re-reading ffs_sbsearch() again in "
          "that case, since fields might be misread/uninitialized on real "
          "hardware rather than cleanly matching a real cg).")

    # Placeholder call showing the exact invocation to use once the real
    # parameters are known (commented so this script doesn't print a
    # meaningless result with made-up numbers):
    # hits = find_cg_for_byte_offset(fs_fpg=<REAL>, fs_sblkno=<REAL>,
    #                                 fs_fsize=<REAL>, fs_ncg=<REAL>,
    #                                 target_bytes=98304)
    # print("cg(s) matching byte offset 98304:", hits)
