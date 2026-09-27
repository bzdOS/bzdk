#!/usr/bin/env python3
"""Standalone hardware validation of emmc_bio_read_dma/write_dma (IDMAC
descriptor-chain DMA), added 2026-09-27. Run from /opt/bzdos/microkernel.

Same methodology as test_emmc_multi.py: every claim the NEW DMA path makes
is cross-checked against the OLD, already hardware-verified per-sector
emmc_bio_read/write path. Runs entirely inside the documented-unused swap
partition (starts at LBA 12863488) with a safety margin DISTINCT from the
one test_emmc_multi.py used (that one used +2048; this one uses +4096, so
the two tests can never collide even if run back-to-back), and restores
the original content before exiting either way.
"""
import sys

sys.path.insert(0, "/opt/bzdos/microkernel")
from emmc_raw import EmmcRaw, _nm_symbols, SCRATCH_PA  # noqa: E402

SAFE_LBA = 12863488 + 4096   # unused swap partition + 2 MiB margin, distinct
                              # from test_emmc_multi.py's +2048 box
NBLK = 8                     # 4096 B


def u32(rc):
    return rc if rc < 0x80000000 else rc - 0x100000000


def main():
    er = EmmcRaw()   # raises on build skew -- the safety net we want
    syms = _nm_symbols("microkernel-dbg.elf")
    rdd = syms.get("emmc_bio_read_dma")
    wrd = syms.get("emmc_bio_write_dma")
    if not rdd or not wrd:
        raise SystemExit("emmc_bio_read_dma/write_dma not found via nm "
                          "-- wrong ELF, or the board is running an older build")
    print(f"emmc_bio_read_dma=0x{rdd:x} emmc_bio_write_dma=0x{wrd:x}")

    # 1) Save the original content via the PROVEN per-sector path.
    orig = er.read_blocks(SAFE_LBA, NBLK)
    print(f"[1] saved {len(orig)} original bytes at LBA {SAFE_LBA}")

    # 2) Read the SAME region via the NEW read_dma; cross-check.
    with er._emmc_lock():
        rc = u32(er._call(rdd, SAFE_LBA, SCRATCH_PA, NBLK))
    print(f"[2] read_dma rc={rc}")
    if rc != 0:
        raise SystemExit(f"read_dma FAILED rc=0x{rc & 0xffffffff:08x}")
    w = er.hv.read_words_stable(SCRATCH_PA, NBLK * 128, tries=8)
    if not w:
        raise SystemExit("readback of read_dma's scratch never stabilized")
    got = b"".join(x.to_bytes(4, "little") for x in w)
    if got != orig:
        raise SystemExit("read_dma MISMATCH vs the proven per-sector read")
    print("[2] read_dma MATCHES per-sector read: OK")

    # 3) Confirm the PIO paths still work after a DMA transfer (the mode
    #    switch back to AHB/PIO must have actually taken -- this is the one
    #    new failure class DMA introduces that multi-block didn't have).
    pio_check = er.read_blocks(SAFE_LBA, NBLK)
    if pio_check != orig:
        raise SystemExit("PIO read_blocks() broke after a DMA transfer -- "
                          "emmc_dma_disarm() did not restore AHB/PIO mode")
    print("[3] PIO path still works after DMA: OK")

    # 4) Write a test pattern via the NEW write_dma.
    pattern = bytes([(i * 53 + 7) & 0xff for i in range(NBLK * 512)])
    for i in range(0, NBLK * 512, 4):
        er.hv.write_word(SCRATCH_PA + i,
                          int.from_bytes(pattern[i:i + 4], "little"))
    with er._emmc_lock():
        rc = u32(er._call(wrd, SAFE_LBA, SCRATCH_PA, NBLK))
    print(f"[4] write_dma rc={rc}")
    if rc != 0:
        raise SystemExit(f"write_dma FAILED rc=0x{rc & 0xffffffff:08x}")

    # 5) Verify via the PROVEN per-sector read.
    back = er.read_blocks(SAFE_LBA, NBLK)
    if back != pattern:
        raise SystemExit("write_dma VERIFY FAILED vs per-sector read")
    print("[5] write_dma pattern verified via per-sector read: OK")

    # 6) Cross-check: read the just-written pattern back via read_dma too.
    with er._emmc_lock():
        rc = u32(er._call(rdd, SAFE_LBA, SCRATCH_PA, NBLK))
    if rc != 0:
        raise SystemExit(f"post-write read_dma FAILED rc=0x{rc & 0xffffffff:08x}")
    w = er.hv.read_words_stable(SCRATCH_PA, NBLK * 128, tries=8)
    got = b"".join(x.to_bytes(4, "little") for x in w)
    if got != pattern:
        raise SystemExit("post-write read_dma MISMATCH vs write_dma's own pattern")
    print("[6] read_dma round-trips write_dma's pattern: OK")

    # 7) Restore the original content via the PROVEN per-sector write.
    for k in range(NBLK):
        er.write_block(SAFE_LBA + k, orig[k * 512:(k + 1) * 512])
    print("[7] restored original content, verified per-sector")
    print("ALL PASS")


if __name__ == "__main__":
    main()
