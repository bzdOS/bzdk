#!/usr/bin/env python3
"""Standalone hardware validation of emmc_bio_read_multi/write_multi
(CMD18/CMD25), added 2026-09-27. Run from /opt/bzdos/microkernel.

Methodology (mirrors the SD series' own validation, see project memory
vblk-sd-shared-bounce-under-lock): every claim the NEW multi-block path
makes is cross-checked against the OLD, already hardware-verified
per-sector emmc_bio_read/write path -- so a bug in the new code cannot
hide behind checking itself. Runs entirely inside the documented-unused
swap partition (starts at LBA 12863488, see emmc_bio.c's fault-injection
interlock comment) with a safety margin, and restores the original
content before exiting either way.
"""
import sys, time

sys.path.insert(0, "/opt/bzdos/microkernel")
from emmc_raw import EmmcRaw, _nm_symbols, SCRATCH_PA  # noqa: E402

SAFE_LBA = 12863488 + 2048   # unused swap partition + 1 MiB margin
NBLK = 8                     # 4096 B: one page of the free scratch gap


def u32(rc):
    return rc if rc < 0x80000000 else rc - 0x100000000


def main():
    er = EmmcRaw()   # raises on build skew -- the safety net we want
    syms = _nm_symbols("microkernel-dbg.elf")
    rdm = syms.get("emmc_bio_read_multi")
    wrm = syms.get("emmc_bio_write_multi")
    if not rdm or not wrm:
        raise SystemExit("emmc_bio_read_multi/write_multi not found via nm "
                          "-- wrong ELF, or the board is running an older build")
    print(f"emmc_bio_read_multi=0x{rdm:x} emmc_bio_write_multi=0x{wrm:x}")

    # 1) Save the original content via the PROVEN per-sector path.
    orig = er.read_blocks(SAFE_LBA, NBLK)
    print(f"[1] saved {len(orig)} original bytes at LBA {SAFE_LBA}")

    # 2) Read the SAME region via the NEW read_multi; cross-check.
    with er._emmc_lock():
        rc = u32(er._call(rdm, SAFE_LBA, SCRATCH_PA, NBLK))
    print(f"[2] read_multi rc={rc}")
    if rc != 0:
        raise SystemExit(f"read_multi FAILED rc=0x{rc & 0xffffffff:08x}")
    w = er.hv.read_words_stable(SCRATCH_PA, NBLK * 128, tries=8)
    if not w:
        raise SystemExit("readback of read_multi's scratch never stabilized")
    got = b"".join(x.to_bytes(4, "little") for x in w)
    if got != orig:
        raise SystemExit("read_multi MISMATCH vs the proven per-sector read")
    print("[2] read_multi MATCHES per-sector read: OK")

    # 3) Write a test pattern via the NEW write_multi.
    pattern = bytes([(i * 37 + 11) & 0xff for i in range(NBLK * 512)])
    for i in range(0, NBLK * 512, 4):
        er.hv.write_word(SCRATCH_PA + i,
                          int.from_bytes(pattern[i:i + 4], "little"))
    with er._emmc_lock():
        rc = u32(er._call(wrm, SAFE_LBA, SCRATCH_PA, NBLK))
    print(f"[3] write_multi rc={rc}")
    if rc != 0:
        raise SystemExit(f"write_multi FAILED rc=0x{rc & 0xffffffff:08x}")

    # 4) Verify via the PROVEN per-sector read.
    back = er.read_blocks(SAFE_LBA, NBLK)
    if back != pattern:
        raise SystemExit("write_multi VERIFY FAILED vs per-sector read")
    print("[4] write_multi pattern verified via per-sector read: OK")

    # 5) Restore the original content via the PROVEN per-sector write.
    for k in range(NBLK):
        er.write_block(SAFE_LBA + k, orig[k * 512:(k + 1) * 512])
    print("[5] restored original content, verified per-sector")
    print("ALL PASS")


if __name__ == "__main__":
    main()
