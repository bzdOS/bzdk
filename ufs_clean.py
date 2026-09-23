#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Inspect (and, with --write, mark clean) the root UFS2 superblock's clean
flag directly on the live eMMC, via the HV's emmc_bio driver over EMAC.

Field offsets/semantics verified against the real FreeBSD releng/15.1 source
(sys/ufs/ffs/fs.h, ffs_vfsops.c, ffs_subr.c, fsck_ffs/fsutil.c) by compiling a
mirrored struct fs and reading back offsetof() — not hand-counted. See the
research this session did for the full citation trail.

READ-ONLY by default. --write requires --i-verified-the-dump-above (a second
explicit flag) as a deliberate speed bump before touching a live disk.
"""
import os, sys, time, subprocess, argparse, struct

# Was hardcoded to a now-gone previous-session scratchpad path; use this
# script's own directory instead (where hvdbg.py/ufs_sblock_search.py
# actually live), fixed 2026-07-27 while adding the cg-search cross-check.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hvdbg import HV

PART_START = 278562          # GPT p3 (freebsd-ufs "rootfs") first LBA
SBLOCK_OFF_BYTES = 65536     # SBLOCK_UFS2 (fs.h) — byte offset within partition
# 0x50030000 is vnet_emac.c's breadcrumb window (VNET_BC_BASE), not free
# scratch DRAM -- confirmed live 2026-07-28 (see ufs2fuse.py). Use the
# confirmed-free gap instead (0x50021040..0x50030000, hv_addrmap.h).
# 0x50022000 is NOT free any more: emac.c's "WDEP" watchdog-episode record
# (HVMAP_WDEP_BASE) sits there, and the link-watchdog rewrites words 4 and 5
# of it whenever it samples the PHY. A block staged over it picks up the raw
# BMSR and the link flag at byte offset 0x10 -- on the way to the device and
# again in the read-back. Seen live 2026-09-23. hv_addrmap.h names 0x50023000
# as the next free page; nothing else in the tree references it.
SCRATCH_PA = 0x50023000

# struct fs byte offsets (gcc offsetof, verified against sizeof(struct fs)==1376)
OFF_SBSIZE        = 104
OFF_FS_MAGIC       = 1372
OFF_FS_CLEAN       = 209
OFF_FS_RONLY       = 210
OFF_FS_OLD_FLAGS   = 211
OFF_FS_PENDINGBLK  = 1104   # int64
OFF_FS_PENDINGINO  = 1112   # uint32
OFF_FS_CKHASH      = 1304   # uint32
OFF_FS_METACKHASH  = 1308   # uint32
OFF_FS_FLAGS       = 1312   # int32

# Added 2026-07-27 for the mountroot-real-bug-is-ufs-sblock-offset
# investigation (project memory) -- offsets confirmed via `gdb -batch -ex
# 'print (size_t)&((struct fs*)0)->fs_fpg' kernel.debug` (DWARF from the
# actual built kernel, not hand-counted), cross-checked: fs_magic came back
# 1372, matching OFF_FS_MAGIC above exactly.
OFF_FS_SBLKNO      = 8      # int32 -- fragment addr of superblock within a cg
OFF_FS_FSIZE       = 52     # int32 -- fragment size, bytes
OFF_FS_NCG         = 44     # int32 -- number of cylinder groups
OFF_FS_FSBTODB     = 100    # int32 -- frag-to-disk-block shift count
OFF_FS_FPG         = 188    # int32 -- fragments per cylinder group

FS_UFS2_MAGIC   = 0x19540119
FS_UNCLEAN      = 0x00000001
FS_NEEDSFSCK    = 0x00000004
FS_FLAGS_UPDATED = 0x80
CK_SUPERBLOCK   = 0x0001

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

def read_sector(lba):
    rc = _call(RD, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_read(lba={lba}) rc={rc}")
    w = _rw(SCRATCH_PA, 128)
    if not w:
        raise IOError(f"EMAC readback failed for lba={lba}")
    return b"".join(x.to_bytes(4, "little") for x in w)

def write_sector(lba, data):
    assert len(data) == 512
    for i in range(0, 512, 4):
        hv.write_word(SCRATCH_PA + i, int.from_bytes(data[i:i+4], "little"))
    rc = _call(WR, lba, SCRATCH_PA)
    rc = rc if rc < 0x80000000 else rc - 0x100000000
    if rc != 0:
        raise IOError(f"emmc_bio_write(lba={lba}) rc={rc}")
    # mandatory read-after-write verify (same discipline as fstab_patch.py)
    back = read_sector(lba)
    if back != data:
        raise IOError(f"VERIFY FAILED at lba={lba}: read-after-write mismatch")

def sb_lba(sector_index):
    """sector_index: 0-based sector within the superblock region."""
    return PART_START + SBLOCK_OFF_BYTES // 512 + sector_index

def read_superblock(nsec):
    return b"".join(read_sector(sb_lba(i)) for i in range(nsec))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--i-verified-the-dump-above", action="store_true",
                    dest="confirmed")
    args = ap.parse_args()

    print(f"emmc_bio_read=0x{RD:x} emmc_bio_write=0x{WR:x}")
    print(f"PART_START={PART_START}  superblock starts at LBA {sb_lba(0)}")

    # Read the first 3 sectors (1536 B) to cover both fs_sbsize (@104, in
    # sector 0) and fs_magic (@1372, in sector 2) before deciding how many
    # more sectors the full superblock needs.
    head = b"".join(read_sector(sb_lba(i)) for i in range(3))
    (sbsize,) = struct.unpack_from("<i", head, OFF_SBSIZE)
    (magic,) = struct.unpack_from("<i", head, OFF_FS_MAGIC)
    print(f"fs_magic  @{OFF_FS_MAGIC} = 0x{magic:08x} "
          f"({'UFS2 OK' if magic == FS_UFS2_MAGIC else 'MISMATCH!'})")
    print(f"fs_sbsize @{OFF_SBSIZE} = {sbsize} bytes "
          f"({sbsize // 512} sectors)")
    if magic != FS_UFS2_MAGIC:
        print("ABORTING: not a valid UFS2 superblock at the expected LBA.")
        return
    if not (1376 <= sbsize <= 65536) or sbsize % 512:
        print(f"ABORTING: fs_sbsize={sbsize} looks unreasonable.")
        return

    nsec = sbsize // 512
    buf = bytearray(read_superblock(nsec))
    assert len(buf) == sbsize

    fs_clean, = struct.unpack_from("<b", buf, OFF_FS_CLEAN)
    fs_ronly, = struct.unpack_from("<b", buf, OFF_FS_RONLY)
    fs_old_flags, = struct.unpack_from("<B", buf, OFF_FS_OLD_FLAGS)
    fs_pendingblocks, = struct.unpack_from("<q", buf, OFF_FS_PENDINGBLK)
    fs_pendinginodes, = struct.unpack_from("<I", buf, OFF_FS_PENDINGINO)
    fs_ckhash, = struct.unpack_from("<I", buf, OFF_FS_CKHASH)
    fs_metackhash, = struct.unpack_from("<I", buf, OFF_FS_METACKHASH)
    fs_flags, = struct.unpack_from("<i", buf, OFF_FS_FLAGS)

    print(f"\n--- current superblock state ({sbsize} bytes read, "
          f"{nsec} sectors LBA {sb_lba(0)}..{sb_lba(nsec-1)}) ---")
    print(f"fs_clean         @{OFF_FS_CLEAN}  = {fs_clean}  "
          f"({'CLEAN' if fs_clean else 'DIRTY -- this is the boot-hang cause'})")
    print(f"fs_ronly         @{OFF_FS_RONLY}  = {fs_ronly}")
    print(f"fs_old_flags     @{OFF_FS_OLD_FLAGS}  = 0x{fs_old_flags:02x}  "
          f"(FS_FLAGS_UPDATED bit 0x80 {'SET (ok, fs_flags authoritative)' if fs_old_flags & FS_FLAGS_UPDATED else 'CLEAR -- fs_flags would be clobbered on next read, DO NOT proceed'})")
    print(f"fs_flags         @{OFF_FS_FLAGS}  = 0x{fs_flags:08x}  "
          f"(FS_UNCLEAN={'set' if fs_flags & FS_UNCLEAN else 'clear'}, "
          f"FS_NEEDSFSCK={'set' if fs_flags & FS_NEEDSFSCK else 'clear'})")
    print(f"fs_pendingblocks @{OFF_FS_PENDINGBLK} = {fs_pendingblocks}")
    print(f"fs_pendinginodes @{OFF_FS_PENDINGINO} = {fs_pendinginodes}")
    print(f"fs_metackhash    @{OFF_FS_METACKHASH} = 0x{fs_metackhash:08x}  "
          f"(CK_SUPERBLOCK {'SET -- checksum enforcement ACTIVE, must recompute' if fs_metackhash & CK_SUPERBLOCK else 'clear -- no checksum enforcement, safe to skip'})")
    print(f"fs_ckhash        @{OFF_FS_CKHASH}    = 0x{fs_ckhash:08x}")

    # --- mountroot-real-bug-is-ufs-sblock-offset cross-check (2026-07-27) --
    # The primary superblock at the standard location IS readable here (we
    # already passed the magic check above to get this far) -- so use its
    # own recorded geometry to settle which cylinder group the guest
    # kernel's ffs_sbsearch() recovery scan is landing on at byte 98304
    # (see project memory for the full chain). Read-only, always runs.
    fs_sblkno, = struct.unpack_from("<i", buf, OFF_FS_SBLKNO)
    fs_fsize, = struct.unpack_from("<i", buf, OFF_FS_FSIZE)
    fs_ncg, = struct.unpack_from("<i", buf, OFF_FS_NCG)
    fs_fpg, = struct.unpack_from("<i", buf, OFF_FS_FPG)
    print(f"\n--- cg-search cross-check (mountroot-real-bug-is-ufs-sblock-offset) ---")
    print(f"fs_sblkno={fs_sblkno}  fs_fsize={fs_fsize}  fs_ncg={fs_ncg}  fs_fpg={fs_fpg}")
    import ufs_sblock_search as usb
    cg0 = usb.cg0_offset(fs_fpg=fs_fpg, fs_sblkno=fs_sblkno, fs_fsize=fs_fsize)
    print(f"sanity check: cg0 offset = {cg0} (expect {usb.SBLOCK_UFS2}) "
          f"-> {'OK' if cg0 == usb.SBLOCK_UFS2 else 'MISMATCH -- geometry read is suspect'}")
    hits = usb.find_cg_for_byte_offset(fs_fpg=fs_fpg, fs_sblkno=fs_sblkno,
                                        fs_fsize=fs_fsize, fs_ncg=fs_ncg,
                                        target_bytes=98304)
    if hits:
        print(f"*** MATCH: cylinder group(s) {hits} land exactly on byte "
              f"offset 98304 -- this IS what the guest's ffs_sbsearch() "
              f"recovery scan is finding. Root cause fully explained: the "
              f"kernel's recovery-search mechanism is working exactly as "
              f"designed, given this filesystem's real geometry; the open "
              f"question becomes ONLY why the primary read at 65536 needed "
              f"this fallback in the first place.")
    else:
        print(f"NO cylinder group lands on byte offset 98304 with this "
              f"geometry -- the live 98304 finding is NOT explained by a "
              f"plain ffs_sbsearch() cg-scan on these parameters. Worth "
              f"re-reading ffs_sbsearch()/cgsblock() again with fresh eyes, "
              f"or checking whether the guest is reading DIFFERENT (stale/"
              f"corrupt) geometry than what's actually stored here.")

    need_ckhash = bool(fs_metackhash & CK_SUPERBLOCK)

    if not args.write:
        print("\n(read-only; re-run with --write "
              "--i-verified-the-dump-above to patch)")
        if need_ckhash:
            print("NOTE: fs_ckhash recompute (CRC-32C/Castagnoli, seed "
                  "0xFFFFFFFF, over the full sbsize buffer) would be "
                  "REQUIRED before any write — not yet implemented in this "
                  "script pending exact algorithm verification.")
        return

    if fs_old_flags & FS_FLAGS_UPDATED == 0:
        print("REFUSING: fs_old_flags lacks FS_FLAGS_UPDATED -- fs_flags "
              "edits would be silently clobbered on next superblock read.")
        return
    if not args.confirmed:
        print("REFUSING: pass --i-verified-the-dump-above once you've "
              "reviewed the values printed above.")
        return
    if need_ckhash:
        print("REFUSING: fs_metackhash has CK_SUPERBLOCK set -- checksum "
              "recompute is required and not implemented in this script "
              "yet. Aborting rather than writing a superblock whose "
              "checksum won't match (that would make the NEXT mount fail "
              "outright, worse than today's dirty-but-mountable state).")
        return

    # Safe to proceed: no checksum enforcement, FS_FLAGS_UPDATED is set.
    new_flags = fs_flags & ~(FS_UNCLEAN | FS_NEEDSFSCK)
    struct.pack_into("<b", buf, OFF_FS_CLEAN, 1)
    struct.pack_into("<i", buf, OFF_FS_FLAGS, new_flags)
    if fs_pendingblocks:
        struct.pack_into("<q", buf, OFF_FS_PENDINGBLK, 0)
    if fs_pendinginodes:
        struct.pack_into("<I", buf, OFF_FS_PENDINGINO, 0)

    print(f"\nWriting {nsec} sectors back (only content changed in "
          f"sector 0 and the fs_flags/pending sector)...")
    for i in range(nsec):
        sec = bytes(buf[i*512:(i+1)*512])
        orig = read_sector(sb_lba(i))   # re-read right before write, minimize race
        if sec == orig:
            continue   # unchanged sector — skip the write entirely
        write_sector(sb_lba(i), sec)
        print(f"  sector {i} (LBA {sb_lba(i)}): written + verified")

    print("\nRe-reading full superblock to confirm final state...")
    buf2 = read_superblock(nsec)
    fs_clean2, = struct.unpack_from("<b", buf2, OFF_FS_CLEAN)
    fs_flags2, = struct.unpack_from("<i", buf2, OFF_FS_FLAGS)
    print(f"fs_clean now = {fs_clean2}  fs_flags now = 0x{fs_flags2:08x}")
    print("DONE. Next boot's fsck should take the fast 'clean' path.")

if __name__ == "__main__":
    main()
