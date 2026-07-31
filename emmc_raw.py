#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""emmc_raw.py — shared raw eMMC 512-byte block I/O over the HV debug
channel (hvdbg.HV), reading/writing the guest's on-disk filesystem directly
without any guest cooperation.

Consolidates what used to be independently duplicated (and independently
buggy) boilerplate across ufs_clean.py, fix_ownership.py,
fix_pam_ownership.py, and ufs2fuse.py: `nm`-based symbol resolution,
hv.call() retry wrapping, and the emmc_bio_read/write staging dance. Built
2026-07-28 after chasing the SAME class of bug (a wrong "scratch" address
silently colliding with vnet_emac.c's own breadcrumb window, a subtle hex-
parsing bug in the bulk read path, and non-retrying write error handling)
independently across those four call sites -- fixing it once here means
every future script sharing this module gets it for free, regardless of
which debug transport sits underneath hvdbg.HV (EMAC today; the read-
until-two-agree reliability check only depends on parsed values, not on
how the bytes arrived).
"""
import contextlib, json, os, subprocess, tempfile, time
from hvdbg import HV

PART_START = 278562          # GPT p3 (freebsd-ufs "rootfs") first LBA --
                              # read live off the real GPT, see project
                              # memory rootmount-gpt-healthy-blocker-
                              # guestside.md.

# NOT 0x50030000 -- that address is vnet_emac.c's own breadcrumb window
# (VNET_BC_BASE, hv_addrmap.h), confirmed live 2026-07-28 to be actively
# incrementing counters, not free scratch DRAM (every earlier script using
# 0x50030000 was silently racing/clobbering vnet diagnostics). Use the
# confirmed-free gap after the dbgtools lane instead (0x50021040..
# 0x50030000 -- see hv_addrmap.h's own grep-verified claim).
SCRATCH_PA = 0x50022000

# How many 512-byte sectors to stage into scratch before reading them back
# in ONE bulk command. emmc_bio_read() is CMD17 (strictly single-block, see
# emmc_bio.h), so the staging half still costs one hv.call() per sector --
# but the readback half is per-COMMAND, not per-byte (measured: a 512-word
# `r` costs the same 0.06s as a 4-word one), so batching the readback is
# close to free. 4 sectors = 2048 bytes = 512 words = exactly
# HV.MAX_WORDS_PER_CMD, the widest reply that still arrives as one
# prompt-terminated burst. Must stay <= that / 128.
BATCH_SECTORS = 4

_NM_CACHE = os.path.join(tempfile.gettempdir(), "bzdos-emmc-symcache.json")


def _nm_symbols(elf_path):
    """{name: addr} for the ELF's text symbols, cached on disk.

    `nm` on the HV image takes appreciably longer than an entire debug-
    channel round trip, and every one-shot script that touches the disk
    paid it again in EmmcRaw's constructor. Keyed on (path, mtime, size)
    so a rebuilt ELF invalidates itself -- this cache can go stale only if
    a build lands with byte-identical size AND mtime."""
    st = os.stat(elf_path)
    key = f"{os.path.abspath(elf_path)}:{int(st.st_mtime)}:{st.st_size}"
    try:
        with open(_NM_CACHE) as f:
            cache = json.load(f)
        if key in cache:
            return cache[key]
    except (OSError, ValueError):
        cache = {}
    nm = subprocess.run(["aarch64-linux-gnu-nm", elf_path],
                        capture_output=True, text=True).stdout
    syms = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] == "T":
            syms[parts[2]] = int(parts[0], 16)
    cache = {key: syms}     # one build's worth; no reason to grow unbounded
    try:
        with open(_NM_CACHE, "w") as f:
            json.dump(cache, f)
    except OSError:
        pass                # cache is an optimization, never a dependency
    return syms


class EmmcRaw:
    """One connection's worth of raw eMMC block I/O. Resolves
    emmc_bio_read/write's addresses from the given ELF (must match
    whatever build is actually running on the board -- a stale ELF here
    silently calls the wrong address, see dbgtools.h's build-id check for
    the general version of this hazard, not re-solved here)."""

    def __init__(self, elf_path="microkernel-dbg.elf", hv=None,
                 allow_build_skew=False):
        self.hv = hv or HV()

        # Symbol addresses come from the ELF on DISK, but hv.call() executes
        # on whatever the BOARD is running. If those disagree, every call
        # here jumps to a wrong address inside a live hypervisor -- not a
        # wrong answer, an arbitrary branch. Confirmed live 2026-07-29 that
        # this really happens (board running 79ae093f109c+ while the tree
        # had built 08688afa045f+), so refuse by default rather than leaving
        # it to each caller to remember, which is what the original version
        # of this docstring did.
        try:
            match, on_disk, running = self.hv.check_build_id()
        except Exception:
            match, on_disk, running = True, None, None   # no gate available
        if not match and not allow_build_skew:
            raise RuntimeError(
                f"BUILD SKEW: board is running {running!r} but {elf_path} is "
                f"{on_disk!r} -- emmc_bio_read/write addresses resolved here "
                f"would not match the running image. Reload the board, or "
                f"pass allow_build_skew=True if you have independently "
                f"confirmed the addresses agree.")

        syms = _nm_symbols(elf_path)
        self.RD = syms.get("emmc_bio_read")
        self.WR = syms.get("emmc_bio_write")
        if self.RD is None or self.WR is None:
            raise RuntimeError(
                f"emmc_bio_read/write not found via nm on {elf_path!r} -- "
                f"wrong ELF path, or a build without eMMC support")
        self.LOCK = syms.get("vblk_emmc_trylock")
        self.UNLOCK = syms.get("vblk_emmc_unlock")
        if self.LOCK is None or self.UNLOCK is None:
            raise RuntimeError(
                f"vblk_emmc_trylock/unlock not found via nm on {elf_path!r} "
                f"-- this class MUST hold the eMMC lock (see _emmc_lock); "
                f"refusing to touch the controller without it")

    @contextlib.contextmanager
    def _emmc_lock(self, tries=40):
        """Hold the eMMC-controller lock across a burst of emmc_bio calls.

        MANDATORY, not defensive. vblk_emmc.h's VBLK_EMMC_LOCK_PA comment spells
        out the contract: virtio-blk (CPU0/CPU2) takes this lock internally, but
        the CPU1 debug core "cannot be hooked at compile time (it enters
        emmc_bio via a runtime `call`)", so a CPU1 caller must bracket its
        access with vblk_emmc_trylock/vblk_emmc_unlock -- otherwise the two
        users collide inside one controller and "corrupt the in-flight
        transfer".

        This class did neither for its whole existence, so any measurement it
        took while the guest was doing virtio-blk I/O was unsound, and the
        probing could corrupt the guest's in-flight transfers.

        NOTE on what this does NOT fix. Twelve reads of ONE unchanging sector
        (LBA 287342) returning three different outcomes -- aarch64 code bytes
        9x, a UFS inode 2x, a hard rc=-1 3x -- was initially blamed on guest
        contention and appeared to be cured by adding this lock. It was not:
        the guest issued ZERO reads across that whole window (g_reads frozen at
        316), so there was no contention to fix. The real cause was
        emmc_bio_read()'s failure paths returning without settling the
        controller, letting one spurious error poison the following calls; the
        lock only helped because its two extra EMAC round-trips spaced the
        reads apart. That is fixed properly in emmc_bio.c
        (ebio_fail_settle()). Keep this lock anyway -- it is required by the
        contract above -- but do not rely on it for read integrity.

        Held only around the controller calls, never across the (slow) EMAC
        readback: scratch RAM is private to this class, so releasing early
        lets the guest back in without risking the staged data."""
        got = False
        for _ in range(tries):
            if self._call(self.LOCK) == 1:
                got = True
                break
            time.sleep(0.25)
        if not got:
            raise IOError(
                "eMMC lock busy after %d tries -- the guest is hammering the "
                "controller, or a previous crash left it held (inspect/clear "
                "VBLK_EMMC_LOCK_PA)" % tries)
        try:
            yield
        finally:
            # Never leave it held: a stuck lock makes EVERY guest virtio-blk
            # read fail S_IOERR, which historically presented as "no vtbd0pN
            # partitions" (see the same header comment).
            self._call(self.UNLOCK)

    def _call(self, fn, *a):
        for _ in range(12):
            r = self.hv.call(fn, *a)
            if r is not None:
                return r
            time.sleep(0.4)
        raise IOError(f"hv.call(0x{fn:x}) returned None 12x (EMAC/HV down?)")

    def read_blocks(self, lba, count):
        """Read `count` consecutive 512-byte sectors starting at `lba`.

        Stages them contiguously into scratch (one CMD17 per sector -- see
        BATCH_SECTORS) and pulls the whole span back in as few bulk reads
        as the channel allows, which is where the win is: per-sector
        readback used to cost a full stable-read cycle each.

        read_words_stable(): read-until-two-consecutive-reads-agree.
        Confirmed live 2026-07-28: even past hvdbg.read_words()'s own
        per-line word-count validation, a single hex digit inside an
        otherwise well-formed 8-char token can still come back wrong (two
        back-to-back reads of unchanged memory disagreeing at one slot,
        same shape, different value -- nothing structurally invalid for
        parsing alone to catch). Safe to apply across the whole staged span
        because nothing but this code writes scratch between the staging
        calls and the readback."""
        out = b""
        for base in range(0, count, BATCH_SECTORS):
            nsec = min(BATCH_SECTORS, count - base)
            with self._emmc_lock():
                for k in range(nsec):
                    rc = self._call(self.RD, lba + base + k, SCRATCH_PA + k * 512)
                    rc = rc if rc < 0x80000000 else rc - 0x100000000
                    if rc != 0:
                        raise IOError(f"emmc_bio_read(lba={lba + base + k}) rc={rc}")
            w = self.hv.read_words_stable(SCRATCH_PA, nsec * 128, tries=8)
            if not w:
                raise IOError(
                    f"EMAC readback never stabilized for lba={lba + base}"
                    f"+{nsec} (8 attempts, no two consecutive reads agreed)")
            out += b"".join(x.to_bytes(4, "little") for x in w)
        return out

    def read_block(self, lba):
        return self.read_blocks(lba, 1)

    def write_block(self, lba, data):
        assert len(data) == 512
        last_err = None
        for attempt in range(5):
            for i in range(0, 512, 4):
                self.hv.write_word(SCRATCH_PA + i,
                                    int.from_bytes(data[i:i + 4], "little"))
            with self._emmc_lock():
                rc = self._call(self.WR, lba, SCRATCH_PA)
            rc = rc if rc < 0x80000000 else rc - 0x100000000
            if rc != 0:
                # emmc_bio.c's emmc_bio_write() returns -2 for "card never
                # signaled program-done" -- a real (if hopefully transient)
                # hardware write-completion timeout. Retry the whole
                # stage+write rather than aborting immediately.
                last_err = IOError(f"emmc_bio_write(lba={lba}) rc={rc}")
                time.sleep(0.3)
                continue
            try:
                back = self.read_block(lba)
            except IOError as e:
                last_err = e
                continue
            if back == data:
                return
            last_err = IOError(f"VERIFY FAILED at lba={lba}: "
                               f"read-after-write mismatch")
        raise last_err
