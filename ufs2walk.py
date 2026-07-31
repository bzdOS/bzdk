#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Minimal UFS2 path-to-inode walker, directly over a raw block device
callback (read_block(lba)/write_block(lba,data), 512-byte sectors).

Field offsets verified via DWARF against the actual deployed kernel
(`gdb -batch -ex 'print (long)&((struct fs*)0)->fs_sblkno' kernel.debug`,
cross-checked against ufs_clean.py's independently-verified fs_sblkno@8/
fs_fsize@52/fs_ncg@44/fs_fpg@188/fs_magic@1372 -- all matched exactly):

  struct fs:    fs_sblkno@8 fs_iblkno@16 fs_dblkno@20 fs_ncg@44 fs_bsize@48
                fs_fsize@52 fs_frag@56 fs_ipg@184 fs_fpg@188 fs_magic@1372
  ufs2_dinode:  di_mode@0(u16) di_uid@4(u32) di_gid@8(u32) di_size@16(u64)
                di_db@112 (12 x u64 frag addrs) di_ib@208 (3 x u64), size=256
  struct direct: d_ino@0(u32) d_reclen@4(u16) d_type@6(u8) d_namlen@7(u8)
                 d_name@8

Lookups/reads only touch the superblock + inode/directory/data blocks the
walk actually visits. write_file()/set_size() below DO write, but only ever
into blocks the inode ALREADY has allocated (di_db direct pointers that are
already non-zero) -- deliberately no cylinder-group bitmap/allocation logic
here, so there is no path that can hand out a block another inode also
thinks it owns. That caps writes to files whose new content fits within
already-allocated direct blocks (no indirect-block writes either); growing
a file past that raises rather than attempting real allocation.
"""
import struct

SBLOCK_OFF_BYTES = 65536   # SBLOCK_UFS2
DINODE_SIZE = 256
ROOT_INO = 2

OFF_FS_MAGIC   = 1372
OFF_FS_SBLKNO  = 8
OFF_FS_IBLKNO  = 16
OFF_FS_DBLKNO  = 20
OFF_FS_NCG     = 44
OFF_FS_BSIZE   = 48
OFF_FS_FSIZE   = 52
OFF_FS_FRAG    = 56
OFF_FS_IPG     = 184
OFF_FS_FPG     = 188

FS_UFS2_MAGIC = 0x19540119


class UFS2:
    def __init__(self, read_block, part_start_lba, write_block=None):
        self._rb = read_block
        self._wb = write_block
        self.part_start = part_start_lba
        self.fs = None
        self._dircache = {}   # ino -> readdir() result, avoids re-walking
                               # shared ancestor directories (e.g. /usr/lib)
                               # once per sibling lookup() call

    def mount(self):
        head = b"".join(self._rb(self.part_start + SBLOCK_OFF_BYTES // 512 + i)
                         for i in range(3))
        (magic,) = struct.unpack_from("<i", head, OFF_FS_MAGIC)
        if magic != FS_UFS2_MAGIC:
            raise ValueError(f"not UFS2: fs_magic=0x{magic:08x}")
        (fpg,) = struct.unpack_from("<i", head, OFF_FS_FPG)
        (ipg,) = struct.unpack_from("<i", head, OFF_FS_IPG)
        (ncg,) = struct.unpack_from("<i", head, OFF_FS_NCG)
        (bsize,) = struct.unpack_from("<i", head, OFF_FS_BSIZE)
        (fsize,) = struct.unpack_from("<i", head, OFF_FS_FSIZE)
        (frag,) = struct.unpack_from("<i", head, OFF_FS_FRAG)
        (sblkno,) = struct.unpack_from("<i", head, OFF_FS_SBLKNO)
        (iblkno,) = struct.unpack_from("<i", head, OFF_FS_IBLKNO)
        self.fs = dict(fpg=fpg, ipg=ipg, ncg=ncg, bsize=bsize, fsize=fsize,
                        frag=frag, sblkno=sblkno, iblkno=iblkno)
        if bsize % 512 or fsize % 512 or bsize <= 0 or fsize <= 0:
            raise ValueError(f"unreasonable geometry: {self.fs}")
        self.inopb = bsize // DINODE_SIZE          # inodes per block
        self.nindir = bsize // 8                    # frag-addr ptrs per indirect block

    # ---- inode location -------------------------------------------------
    def _ino_to_fsba(self, ino):
        fs = self.fs
        cg = ino // fs["ipg"]
        cgoff = ino % fs["ipg"]
        cgbase = fs["fpg"] * cg
        return cgbase + fs["iblkno"] + (cgoff // self.inopb) * fs["frag"]

    def _ino_to_fsbo(self, ino):
        return ino % self.inopb

    def _frag_to_lba(self, fsba):
        byte_off = fsba * self.fs["fsize"]
        assert byte_off % 512 == 0
        return self.part_start + byte_off // 512

    def _read_dinode_raw(self, ino):
        fsba = self._ino_to_fsba(ino)
        fsbo = self._ino_to_fsbo(ino)
        block_lba = self._frag_to_lba(fsba)
        byte_in_block = fsbo * DINODE_SIZE
        sector_lba = block_lba + byte_in_block // 512
        intra = byte_in_block % 512
        assert intra + DINODE_SIZE <= 512, "dinode straddles a sector boundary"
        sec = self._rb(sector_lba)
        return sec, sector_lba, intra

    def stat(self, ino):
        sec, lba, off = self._read_dinode_raw(ino)
        mode, = struct.unpack_from("<H", sec, off + 0)
        uid, = struct.unpack_from("<I", sec, off + 4)
        gid, = struct.unpack_from("<I", sec, off + 8)
        size, = struct.unpack_from("<Q", sec, off + 16)
        db = struct.unpack_from("<12Q", sec, off + 112)
        ib = struct.unpack_from("<3Q", sec, off + 208)
        return dict(ino=ino, mode=mode, uid=uid, gid=gid, size=size,
                    db=db, ib=ib, lba=lba, dinode_off=off)

    # ---- directory data blocks -------------------------------------------
    def _read_block_bytes(self, fsba):
        lba = self._frag_to_lba(fsba)
        nsec = self.fs["bsize"] // 512
        return b"".join(self._rb(lba + i) for i in range(nsec))

    def _read_indirect_ptrs(self, fsba):
        raw = self._read_block_bytes(fsba)
        return struct.unpack_from(f"<{self.nindir}Q", raw, 0)

    def _iter_data_blocks(self, st):
        """Yield each data block's raw bytes (fs_bsize each) covering di_size."""
        nblocks = (st["size"] + self.fs["bsize"] - 1) // self.fs["bsize"]
        db = st["db"]
        for i in range(min(nblocks, 12)):
            if db[i] == 0:
                continue
            yield self._read_block_bytes(db[i])
        if nblocks > 12:
            if st["ib"][0] == 0:
                return
            ptrs = self._read_indirect_ptrs(st["ib"][0])
            for i in range(nblocks - 12):
                if i >= len(ptrs) or ptrs[i] == 0:
                    continue
                yield self._read_block_bytes(ptrs[i])

    def readdir(self, ino):
        cached = self._dircache.get(ino)
        if cached is not None:
            return cached
        st = self.stat(ino)
        entries = []
        consumed = 0
        for blk in self._iter_data_blocks(st):
            pos = 0
            while pos < len(blk) and consumed < st["size"]:
                d_ino, d_reclen, d_type, d_namlen = struct.unpack_from(
                    "<IHBB", blk, pos)
                if d_reclen == 0:
                    break
                if d_ino != 0 and d_namlen > 0:
                    name = blk[pos + 8: pos + 8 + d_namlen].decode(
                        "ascii", "replace")
                    entries.append((name, d_ino, d_type))
                pos += d_reclen
                consumed += d_reclen
        self._dircache[ino] = entries
        return entries

    def lookup(self, path):
        parts = [p for p in path.strip("/").split("/") if p]
        ino = ROOT_INO
        for i, name in enumerate(parts):
            entries = self.readdir(ino)
            match = next((e for e in entries if e[0] == name), None)
            if match is None:
                raise FileNotFoundError(f"{'/'.join(parts[:i+1])}: no such entry")
            ino = match[1]
        st = self.stat(ino)
        return st

    # ---- whole-file read/write -------------------------------------------
    def read_file(self, path):
        st = self.lookup(path)
        out = b"".join(self._iter_data_blocks(st))
        return out[:st["size"]]

    def _write_block_bytes(self, fsba, data):
        """Write `data` (up to fs_bsize bytes) starting at block `fsba`,
        touching only as many 512-byte sectors as `data` actually needs --
        NOT the full fs_bsize every time. Any bytes beyond `data` within
        this block are left completely untouched (whatever was already
        there). That's safe here specifically because every caller is
        immediately followed by set_size(), and di_size is authoritative
        for where the file's visible content ends -- nothing legitimate
        ever reads past it. Confirmed necessary live 2026-07-28: writing
        all 64 sectors of a 32KB block hit repeated genuine eMMC
        write-completion timeouts (emmc_bio_write rc=-2) well past the
        handful of sectors any of this writer's actual content needs."""
        assert len(data) <= self.fs["bsize"]
        lba = self._frag_to_lba(fsba)
        nsec = (len(data) + 511) // 512
        for i in range(nsec):
            chunk = data[i * 512:(i + 1) * 512]
            if len(chunk) < 512:
                chunk = chunk + b"\x00" * (512 - len(chunk))
            self._wb(lba + i, chunk)

    def _write_dinode_field(self, ino, byte_off, packed):
        """Read-modify-write one field inside ino's on-disk dinode sector."""
        sec, lba, doff = self._read_dinode_raw(ino)
        sec = bytearray(sec)
        sec[doff + byte_off: doff + byte_off + len(packed)] = packed
        self._wb(lba, bytes(sec))

    def set_size(self, path, new_size):
        """Set di_size directly. Never allocates/frees blocks -- shrinking
        just moves the visible EOF earlier (trailing bytes in the last
        already-allocated block become unreachable padding, harmless);
        growing is only allowed up to the capacity of blocks the inode
        ALREADY has allocated (see write_file)."""
        st = self.lookup(path)
        needed = (new_size + self.fs["bsize"] - 1) // self.fs["bsize"]
        if needed > 12:
            raise ValueError("set_size: only direct-block-range sizes "
                              "supported (<=12 * fs_bsize)")
        for i in range(needed):
            if st["db"][i] == 0:
                raise ValueError(f"set_size: block {i} isn't allocated -- "
                                  f"growing via new allocation not supported")
        self._write_dinode_field(st["ino"], 16, struct.pack("<Q", new_size))

    def write_file(self, path, data):
        """Overwrite a regular file's entire content with `data`. Only
        writes into direct blocks (index < 12) the inode already has
        allocated -- raises if `data` wouldn't fit in that many blocks."""
        st = self.lookup(path)
        bsize = self.fs["bsize"]
        needed = (len(data) + bsize - 1) // bsize if data else 0
        if needed > 12:
            raise ValueError(f"write_file: {path}: {len(data)} bytes needs "
                              f"{needed} blocks, only direct blocks (<=12) "
                              f"are supported by this writer")
        for i in range(needed):
            if st["db"][i] == 0:
                raise ValueError(f"write_file: {path}: block {i} isn't "
                                  f"allocated -- growing via new allocation "
                                  f"not supported")
        for i in range(needed):
            chunk = data[i * bsize:(i + 1) * bsize]
            self._write_block_bytes(st["db"][i], chunk)
        self.set_size(path, len(data))
