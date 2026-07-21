#!/usr/bin/env python3
"""Compute a GPT 'recover' for the Chimp eMMC: the ~2.5GB image was dd'd onto an
8GB (DISK_BLOCKS=15269888) eMMC, so the backup GPT header sits at LBA 4955906
(the image end) instead of the disk end, GEOM flags the secondary GPT corrupt +
there's no protective MBR, and /dev/mmcsd0p3 is never created (mount error 19).

Given the reliably-read primary header (512B @ LBA1) + partition array (2048B @
LBA2) this builds the blocks to write:
  LBA 0                : protective MBR (was missing)
  LBA 1                : fixed primary header (backupLBA/lastUsable -> disk end, new CRC)
  LBA (LAST-32)..(LAST-1): backup partition array (copy of the primary array)
  LBA LAST             : backup header (points at that array, new CRC)
where LAST = DISK_BLOCKS-1.

Pure computation, no board I/O. build_recover() returns {lba: 512-byte block}.
"""
import struct, zlib

DISK_BLOCKS = 15269888
LAST = DISK_BLOCKS - 1                    # 15269887

def _hdr_crc(hdr92: bytes) -> int:
    b = bytearray(hdr92[:92]); b[16:20] = b"\x00\x00\x00\x00"
    return zlib.crc32(bytes(b)) & 0xffffffff

def build_recover(primary_hdr: bytes, part_array: bytes):
    assert primary_hdr[:8] == b"EFI PART", primary_hdr[:8]
    hdrsize = struct.unpack("<I", primary_hdr[12:16])[0]
    assert hdrsize == 92, hdrsize
    npe, pesz = struct.unpack("<II", primary_hdr[80:88])
    stored_pcrc = struct.unpack("<I", primary_hdr[88:92])[0]
    calc_pcrc = zlib.crc32(part_array[:npe*pesz]) & 0xffffffff
    assert calc_pcrc == stored_pcrc, "part-array CRC mismatch: read is not byte-perfect (0x%08x != 0x%08x)" % (calc_pcrc, stored_pcrc)
    # verify our header-CRC method reproduces the stored one
    assert _hdr_crc(primary_hdr) == struct.unpack("<I", primary_hdr[16:20])[0], "header CRC method wrong"

    arr_blocks = (npe*pesz + 511)//512
    part_lba_backup = LAST - arr_blocks   # backup array just below the backup header

    # ---- fixed primary header ----
    p = bytearray(primary_hdr[:512])
    struct.pack_into("<Q", p, 24, 1)            # current LBA
    struct.pack_into("<Q", p, 32, LAST)         # backup LBA -> disk end
    struct.pack_into("<Q", p, 48, part_lba_backup - 1)  # last usable LBA
    struct.pack_into("<Q", p, 72, 2)            # partition entries LBA (primary array stays @2)
    struct.pack_into("<I", p, 16, 0)
    struct.pack_into("<I", p, 16, _hdr_crc(bytes(p)))
    primary_fixed = bytes(p)

    # ---- backup header (mirror) ----
    b = bytearray(primary_hdr[:512])
    struct.pack_into("<Q", b, 24, LAST)         # current LBA = disk end
    struct.pack_into("<Q", b, 32, 1)            # backup LBA -> primary
    struct.pack_into("<Q", b, 48, part_lba_backup - 1)  # last usable
    struct.pack_into("<Q", b, 72, part_lba_backup)      # backup array LBA
    struct.pack_into("<I", b, 16, 0)
    struct.pack_into("<I", b, 16, _hdr_crc(bytes(b)))
    backup_hdr = bytes(b)

    # ---- protective MBR ----
    m = bytearray(512)
    # one 0xEE partition covering the whole disk (LBA 1 .. min(disk-1, 0xFFFFFFFF))
    m[446] = 0x00                     # not bootable
    m[447:450] = bytes([0x00, 0x02, 0x00])   # CHS start (conventional)
    m[450] = 0xEE                     # GPT protective type
    m[451:454] = bytes([0xFF, 0xFF, 0xFF])   # CHS end (max)
    struct.pack_into("<I", m, 454, 1)                     # first LBA = 1
    struct.pack_into("<I", m, 458, min(LAST, 0xFFFFFFFF)) # num sectors
    m[510] = 0x55; m[511] = 0xAA
    pmbr = bytes(m)

    out = {0: pmbr, 1: primary_fixed, LAST: backup_hdr}
    for i in range(arr_blocks):
        out[part_lba_backup + i] = part_array[i*512:(i+1)*512].ljust(512, b"\x00")
    return out, dict(npe=npe, pesz=pesz, part_lba_backup=part_lba_backup,
                     last_usable=part_lba_backup-1, backup_hdr_lba=LAST)

if __name__ == "__main__":
    # self-test of the CRC math with a synthetic minimal GPT header round-trip
    h = bytearray(512); h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000); struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<Q", h, 24, 1); struct.pack_into("<Q", h, 32, 4955906)
    struct.pack_into("<Q", h, 40, 34); struct.pack_into("<Q", h, 48, 4955873)
    struct.pack_into("<Q", h, 72, 2); struct.pack_into("<I", h, 80, 16); struct.pack_into("<I", h, 84, 128)
    arr = bytearray(2048); arr[0:16] = bytes(range(16))   # dummy entry
    struct.pack_into("<I", h, 88, zlib.crc32(bytes(arr))&0xffffffff)
    struct.pack_into("<I", h, 16, _hdr_crc(bytes(h)))
    blocks, info = build_recover(bytes(h), bytes(arr))
    print("self-test OK:", info, "| blocks to write:", sorted(blocks))
