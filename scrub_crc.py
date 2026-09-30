#!/usr/bin/env python3
"""Patch scrub.c's g_scrub_table into a linked hypervisor ELF, in place.

The table holds the CRC-32 (zlib.crc32) of every 4 KiB chunk of
[_text_start, _rodata_end) as linked, so the running scrubber can tell a
chunk corrupted after boot from one that was already wrong when it was
loaded. Run after the link and before objcopy, so the .bin and the uImage
carry the table. The table itself lives in .data, outside the range it
covers, which is why patching it does not change any CRC in it.

    python3 scrub_crc.py microkernel-dbg.elf
"""
import struct
import subprocess
import sys
import zlib

CHUNK = 4096
MAX_BYTES = 0x40000
MAGIC = 0x42435243
NM = 'aarch64-linux-gnu-nm'


def sections(elf):
    """[(addr, file_offset, size, type)] from the ELF64 section headers."""
    shoff, = struct.unpack_from('<Q', elf, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', elf, 0x3A)
    out = []
    for i in range(shnum):
        o = shoff + i * shentsize
        _name, typ, _flags, addr, off, size = struct.unpack_from('<IIQQQQ',
                                                                 elf, o)
        out.append((addr, off, size, typ))
    return out


def image(elf, secs, base, length):
    """Bytes of [base, base+length) as loaded: PROGBITS content, zero gaps."""
    buf = bytearray(length)
    for addr, off, size, typ in secs:
        if typ != 1 or addr == 0:               # SHT_PROGBITS, allocated
            continue
        lo, hi = max(addr, base), min(addr + size, base + length)
        if lo < hi:
            buf[lo - base:hi - base] = elf[off + lo - addr:off + hi - addr]
    return bytes(buf)


def main():
    path = sys.argv[1]
    syms = {}
    for line in subprocess.run([NM, path], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    for s in ('_text_start', '_rodata_end', 'g_scrub_table'):
        if s not in syms:
            sys.exit(f'scrub_crc: {s} not in {path}')
    base, end, tab = syms['_text_start'], syms['_rodata_end'], \
        syms['g_scrub_table']
    length = end - base
    if length > MAX_BYTES:
        sys.exit(f'scrub_crc: text+rodata {length:#x} > {MAX_BYTES:#x}; '
                 f'raise SCRUB_MAX_BYTES in scrub.h')
    with open(path, 'rb') as f:
        elf = bytearray(f.read())
    secs = sections(elf)
    img = image(elf, secs, base, length)
    n = (length + CHUNK - 1) // CHUNK
    crcs = [zlib.crc32(img[i * CHUNK:(i + 1) * CHUNK]) for i in range(n)]
    blob = struct.pack(f'<4I{n}I', MAGIC, base, length, n, *crcs)
    for addr, off, size, typ in secs:
        if typ == 1 and addr <= tab and tab + len(blob) <= addr + size:
            elf[off + tab - addr:off + tab - addr + len(blob)] = blob
            break
    else:
        sys.exit('scrub_crc: g_scrub_table is not in a PROGBITS section')
    with open(path, 'wb') as f:
        f.write(elf)
    print(f'scrub_crc: {n} chunks over {base:#x}..{end:#x} '
          f'({length} bytes) -> g_scrub_table')


if __name__ == '__main__':
    main()
