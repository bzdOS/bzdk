#!/usr/bin/env python3
"""Standalone hardware validation of sd_bio_read_dma/write_dma (IDMAC
descriptor-chain DMA on the SD/SMHC0 controller), added 2026-09-27. Run
from /opt/bzdos/microkernel.

Same methodology as test_emmc_dma.py: every claim the NEW DMA path makes is
cross-checked against the OLD, already hardware-verified per-sector
sd_bio_read/write path. Runs inside the small (~1004 KiB) free gap on the SD
card's GPT (LBA 40..2047, before partition 1 starts at LBA 2048) -- clear of
both the GPT itself (LBA 0..39) and sdbox.c's own crash black-box at LBA 64
-- and restores the original content before exiting either way.
"""
import subprocess
import sys
import time

sys.path.insert(0, "/opt/bzdos/microkernel")
from hvdbg import HV  # noqa: E402

SAFE_LBA = 200          # inside the free GPT gap, clear of SDBOX_LBA=64
NBLK = 8                # 4096 B
SCRATCH_PA = 0x50023000  # documented free scratch DRAM (see emmc_raw.py)
ELF = "microkernel-dbg.elf"

NEEDED = ("sd_bio_read", "sd_bio_write", "sd_bio_read_dma",
          "sd_bio_write_dma", "vblk_sd_trylock", "vblk_sd_unlock")


def nm_symbols(path):
    out = subprocess.run(["aarch64-linux-gnu-nm", path],
                          capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t"):
            syms[parts[2]] = int(parts[0], 16)
    return syms


def u32(rc):
    return rc if rc < 0x80000000 else rc - 0x100000000


class Locked:
    def __init__(self, hv, addr):
        self.hv, self.addr = hv, addr

    def __enter__(self):
        for _ in range(200):
            r = self.hv.call(self.addr["vblk_sd_trylock"])
            if r == 1:
                return self
            time.sleep(0.01)
        raise SystemExit("could not acquire the SD lock (vCPU busy?) after 2s")

    def __exit__(self, *a):
        self.hv.call(self.addr["vblk_sd_unlock"])


def read_blocks(hv, addr, lock, lba, nblk):
    out = b""
    with lock:
        for i in range(nblk):
            rc = u32(hv.call(addr["sd_bio_read"], lba + i, SCRATCH_PA))
            if rc != 0:
                raise SystemExit(f"sd_bio_read FAILED at lba={lba+i} rc={rc}")
            w = hv.read_words_stable(SCRATCH_PA, 128, tries=8)
            if not w:
                raise SystemExit(f"readback never stabilized at lba={lba+i}")
            out += b"".join(x.to_bytes(4, "little") for x in w)
    return out


def write_blocks(hv, addr, lock, lba, data):
    assert len(data) % 512 == 0
    with lock:
        for i in range(len(data) // 512):
            chunk = data[i * 512:(i + 1) * 512]
            for off in range(0, 512, 4):
                hv.write_word(SCRATCH_PA + off,
                               int.from_bytes(chunk[off:off + 4], "little"))
            rc = u32(hv.call(addr["sd_bio_write"], lba + i, SCRATCH_PA))
            if rc != 0:
                raise SystemExit(f"sd_bio_write FAILED at lba={lba+i} rc={rc}")


def main():
    hv = HV()
    addr = nm_symbols(ELF)
    missing = [n for n in NEEDED if n not in addr]
    if missing:
        raise SystemExit(f"missing symbols via nm: {missing} -- wrong ELF, "
                          "or the board is running an older build")
    for n in NEEDED:
        print(f"{n}=0x{addr[n]:x}")
    lock = Locked(hv, addr)

    # 1) Save the original content via the PROVEN per-sector path.
    orig = read_blocks(hv, addr, lock, SAFE_LBA, NBLK)
    print(f"[1] saved {len(orig)} original bytes at LBA {SAFE_LBA}")

    # 2) Read the SAME region via the NEW read_dma; cross-check.
    with lock:
        rc = u32(hv.call(addr["sd_bio_read_dma"], SAFE_LBA, SCRATCH_PA, NBLK))
    print(f"[2] read_dma rc={rc}")
    if rc != 0:
        raise SystemExit(f"read_dma FAILED rc=0x{rc & 0xffffffff:08x}")
    w = hv.read_words_stable(SCRATCH_PA, NBLK * 128, tries=8)
    if not w:
        raise SystemExit("readback of read_dma's scratch never stabilized")
    got = b"".join(x.to_bytes(4, "little") for x in w)
    if got != orig:
        raise SystemExit("read_dma MISMATCH vs the proven per-sector read")
    print("[2] read_dma MATCHES per-sector read: OK")

    # 3) Confirm the PIO path still works after a DMA transfer.
    pio_check = read_blocks(hv, addr, lock, SAFE_LBA, NBLK)
    if pio_check != orig:
        raise SystemExit("PIO sd_bio_read() broke after a DMA transfer -- "
                          "sd_dma_disarm() did not restore AHB/PIO mode")
    print("[3] PIO path still works after DMA: OK")

    # 4) Write a test pattern via the NEW write_dma.
    pattern = bytes([(i * 61 + 3) & 0xff for i in range(NBLK * 512)])
    for i in range(0, NBLK * 512, 4):
        hv.write_word(SCRATCH_PA + i,
                       int.from_bytes(pattern[i:i + 4], "little"))
    with lock:
        rc = u32(hv.call(addr["sd_bio_write_dma"], SAFE_LBA, SCRATCH_PA, NBLK))
    print(f"[4] write_dma rc={rc}")
    if rc != 0:
        raise SystemExit(f"write_dma FAILED rc=0x{rc & 0xffffffff:08x}")

    # 5) Verify via the PROVEN per-sector read.
    back = read_blocks(hv, addr, lock, SAFE_LBA, NBLK)
    if back != pattern:
        raise SystemExit("write_dma VERIFY FAILED vs per-sector read")
    print("[5] write_dma pattern verified via per-sector read: OK")

    # 6) Restore the original content via the PROVEN per-sector write.
    write_blocks(hv, addr, lock, SAFE_LBA, orig)
    restored = read_blocks(hv, addr, lock, SAFE_LBA, NBLK)
    if restored != orig:
        raise SystemExit("RESTORE FAILED -- original content not recovered")
    print("[6] restored original content, verified per-sector")
    print("ALL PASS")


if __name__ == "__main__":
    main()
