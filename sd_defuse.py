#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""sd_defuse.py — erase the SD card's eGON boot signature via the hypervisor.

WHY THIS EXISTS: a U-Boot image was written to the SD card that hangs before
any console comes up. The A64 BROM tries MMC0 (the card) before the eMMC,
finds a valid eGON header at byte offset 8192, loads that image, and the
board goes dark -- no USB gadget, no EMAC, no lever, and the only way back
is physically pulling the card. The eMMC still holds the known-good loader,
but the BROM never reaches it while the card's signature is intact.

Zeroing sector 16 makes the BROM skip the card and fall through to the eMMC,
i.e. back to the layout that booted 393 times before. One 512-byte block.

It goes through the HV debug channel, so it needs nothing from the guest --
which is the point: the guest is wedged in early rc, because the card was
absent when it tried to mount /var and the failed mount took the console
session down with it.

ADDRESS SAFETY. hv.call() branches to a raw address inside a live
hypervisor, so a wrong address is a crash, and a crash is a reset, and a
reset is exactly the brick this script exists to prevent. The board's
build-id cannot clear the addresses here: the running image was built from a
dirty tree, so its id (d80c9e22ee8c+) matches no commit. Instead every
function is compared instruction-for-instruction, over its whole length,
against a rebuild of commit 821bd1e -- whose content is what that dirty tree
became. read/write/lock/unlock are byte-identical there; sd_bio_init differs
in exactly two instructions, an adrp/str pair addressing one global that
moved, which does not affect the entry address.

The reference image is therefore NOT fixed: it must be whatever the board
was actually loaded with, which changes every time it boots. After a TFTP
reboot that is microkernel-dbg.uimg, not the tree and not the worktree below
-- both report build-id d80c9e22ee8c+ and are still different binaries.
Point BZDOS_REF_ELF at the ELF that produced the running image; the default
matches the image the board was carrying on 2026-09-23 before it was reset.
A wrong reference makes this refuse to run, which is the intended outcome.

Rebuild that default with:
    git worktree add --detach <WT> 821bd1e && make -C <WT> dbg

Run with the board powered and the HV alive (bzdctl.py status should answer).
"""
import subprocess
import sys
import time

sys.path.insert(0, '/opt/bzdos/microkernel')
from hvdbg import HV

# NOT emmc_raw.py's 0x50022000. That address was free when emmc_raw.py
# claimed it, but emac.c later put its "WDEP" watchdog-episode record on the
# same page, and the link-watchdog rewrites words 4 and 5 of it whenever it
# samples the PHY. Staging a disk block there means the BMSR value and the
# link flag land in the middle of the sector on their way to the card, and
# land in the read-back too, which reads as a failed write. First free page
# per hv_addrmap.h's own map.
SCRATCH_PA = 0x50023000
LOAD_BASE = 0x42000000
BOOT_LBA = 16                    # BROM looks for eGON at byte 8192
import os

WT = os.environ.get(
    'BZDOS_REF_DIR',
    '/tmp/scratch'
    '/scratchpad/wt-821bd1e')
ELF = os.environ.get('BZDOS_REF_ELF', WT + '/microkernel-dbg.elf')
BIN = os.environ.get('BZDOS_REF_BIN', WT + '/microkernel-dbg.bin')

NEEDED = ('sd_bio_init', 'sd_bio_read', 'sd_bio_write',
          'vblk_sd_trylock', 'vblk_sd_unlock')

# sd_bio_init's two known-different instructions: one global relocated
# between the running image and the 821bd1e rebuild. Anything beyond this
# budget means the reference image is the wrong one.
ALLOWED_MISMATCHES = {'sd_bio_init': 2}


def read_symbols(path):
    out = subprocess.run(["aarch64-linux-gnu-nm", "-S", path],
                         capture_output=True, text=True).stdout
    addr, size = {}, {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in ('T', 't'):
            addr[p[3]] = int(p[0], 16)
            size[p[3]] = int(p[1], 16)
    return addr, size


def main():
    addr, size = read_symbols(ELF)
    missing = [n for n in NEEDED if n not in addr]
    if missing:
        sys.exit(f"missing symbols in {ELF}: {missing}")
    img = open(BIN, 'rb').read()
    hv = HV()

    def call(fn, *args):
        for _ in range(15):
            r = hv.call(fn, *args)
            if r is not None:
                return r if r < 0x80000000 else r - 0x100000000
            time.sleep(0.4)
        raise IOError(f"hv.call(0x{fn:x}) got no reply")

    def verify(name):
        va, n = addr[name], size[name] // 4
        off = va - LOAD_BASE
        want = [int.from_bytes(img[off + 4 * i:off + 4 * i + 4], 'little')
                for i in range(n)]
        # A single hex digit can come back wrong on an otherwise well-formed
        # reply (seen live 2026-07-28), so one disagreeing word is not proof
        # of a wrong address -- only a mismatch that reproduces is real.
        best = None
        for _ in range(6):
            got = hv.read_words(va, n)
            if got and len(got) == n:
                bad = [i for i, (a, b) in enumerate(zip(want, got)) if a != b]
                if best is None or len(bad) < len(best):
                    best = bad
                if len(bad) <= ALLOWED_MISMATCHES.get(name, 0):
                    print(f"  verified {name} @0x{va:x} "
                          f"({n * 4} B, {len(bad)} known diffs)")
                    return
            time.sleep(0.5)
        sys.exit(f"{name} @0x{va:x}: {len(best or [])} mismatching "
                 f"instructions — wrong image, refusing to call it")

    def sector(lba):
        # Poison the staging buffer first, so what comes back is provably
        # off the card and not a leftover of what we just wrote.
        for off in range(0, 512, 4):
            hv.write_word(SCRATCH_PA + off, 0xa5a5a5a5)
        call(addr['vblk_sd_trylock'])
        rc = call(addr['sd_bio_read'], lba, SCRATCH_PA)
        call(addr['vblk_sd_unlock'])
        if rc != 0:
            sys.exit(f"sd_bio_read({lba}) failed rc={rc}")
        words = hv.read_words(SCRATCH_PA, 8)
        return b''.join(w.to_bytes(4, 'little') for w in words)

    print("verifying call targets against the running image ...")
    for name in NEEDED:
        verify(name)

    # The HV's SD stack failed to come up at boot (the card was out), so
    # every block call returns -100 until it is brought up again.
    print(f"sd_bio_init() -> {call(addr['sd_bio_init'])}")

    head = sector(BOOT_LBA)
    print(f"sector {BOOT_LBA} before: {head.hex(' ')}")
    if b'eGON' not in head:
        # Write ONLY when the signature is actually there. Sector 16 is not
        # spare space: LBA 2..33 is the GPT partition-entry array, so this
        # block is either a boot signature or live partition table, never
        # both. Blanking it on the strength of "it isn't zero" would destroy
        # a healthy GPT -- which is also why writing the U-Boot image here
        # corrupted the primary table in the first place, and why
        # `gpart recover` is the other half of undoing that.
        print("no eGON signature — nothing to defuse "
              "(this sector is GPT entry data; leaving it alone)")
        return

    # vblk_sd takes this lock on the guest-facing path; the debug core enters
    # sd_bio through a runtime call and is not hooked, so it must bracket its
    # own access or the two collide inside one controller.
    print("writing zeros ...")
    for off in range(0, 512, 4):
        hv.write_word(SCRATCH_PA + off, 0)
    call(addr['vblk_sd_trylock'])
    rc = call(addr['sd_bio_write'], BOOT_LBA, SCRATCH_PA)
    call(addr['vblk_sd_unlock'])
    if rc != 0:
        sys.exit(f"sd_bio_write({BOOT_LBA}) failed rc={rc}")

    head = sector(BOOT_LBA)
    print(f"sector {BOOT_LBA} after:  {head.hex(' ')}")
    if any(head):
        sys.exit("sector still non-zero — signature may still be live")
    print("OK — signature erased; the BROM will fall through to eMMC")


if __name__ == '__main__':
    main()
