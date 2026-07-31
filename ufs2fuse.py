#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Mount the guest's live UFS2 root filesystem on the HOST, read/write,
entirely through the HV's emmc_bio driver over EMAC -- no guest cooperation
needed at all (works even with the guest wedged/single-user/panicked).

Built 2026-07-28 to replace fragile raw-console command injection (the
guest's RX injection ring is only 128 bytes and silently drops overflow --
a multi-hundred-byte heredoc scrambled into a runaway unrelated command
live) for fixing on-disk files (ownership, corrupt /etc/pam.d/system
content) from outside. Read is fully general; write only ever touches
blocks an inode ALREADY has allocated (ufs2walk.UFS2.write_file's direct-
block-only, no-new-allocation design) -- see ufs2walk.py's header for why
that's deliberately incomplete-but-safe rather than a full FFS block
allocator.

Usage:
    mkdir -p /mnt/chimp-root
    python3 ufs2fuse.py /mnt/chimp-root          # foreground, Ctrl-C to unmount
    python3 ufs2fuse.py /mnt/chimp-root -o allow_other &   # background

Then use normal tools against /mnt/chimp-root -- cp, chown, cat, python,
etc. Files whose new content doesn't fit in already-allocated direct
blocks raise EFBIG (cp/write refuses) rather than silently doing something
unsafe; that's a real, if narrow, limitation of this first cut.
"""
import sys, os, time, argparse, errno, stat as statmod

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fuse import FUSE, FuseOSError, Operations
import ufs2walk
from emmc_raw import EmmcRaw, PART_START

_raw = EmmcRaw()
read_block = _raw.read_block
write_block = _raw.write_block


class UFS2FS(Operations):
    def __init__(self):
        self.ufs = ufs2walk.UFS2(read_block, PART_START, write_block=write_block)
        self.ufs.mount()
        print(f"mounted: fs={self.ufs.fs}")

    def _stat(self, path):
        try:
            return self.ufs.lookup(path)
        except FileNotFoundError:
            raise FuseOSError(errno.ENOENT)

    def getattr(self, path, fh=None):
        st = self._stat(path)
        now = time.time()
        # Defensive clamp: a handful of files on this image (factory-build
        # defect, same family as the uid=1001 one) have a corrupt, absurd
        # (multi-TB) di_size left over from stale disk content past their
        # real EOF -- confirmed live 2026-07-28 that reporting it as-is
        # makes `cp`/`stat` fail with ERANGE (something in the kernel-FUSE
        # stat marshalling can't carry a value that large), before this
        # filesystem's own open(O_TRUNC) fix ever gets a chance to run.
        # This writer only ever supports direct-block files anyway, so
        # nothing genuine can legitimately exceed that many bytes.
        max_sane = 12 * self.ufs.fs["bsize"]
        size = st["size"] if st["size"] <= max_sane else max_sane
        return dict(
            st_mode=st["mode"],
            st_size=size,
            st_uid=st["uid"],
            st_gid=st["gid"],
            st_nlink=2 if statmod.S_ISDIR(st["mode"]) else 1,
            st_atime=now, st_mtime=now, st_ctime=now,
        )

    def readdir(self, path, fh):
        st = self._stat(path)
        entries = self.ufs.readdir(st["ino"])
        yield "."
        yield ".."
        for name, ino, dtype in entries:
            if name in (".", ".."):
                continue
            yield name

    def read(self, path, size, offset, fh):
        data = self.ufs.read_file(path)
        return data[offset:offset + size]

    def write(self, path, data, offset, fh):
        try:
            current = self.ufs.read_file(path)
        except FileNotFoundError:
            raise FuseOSError(errno.ENOENT)
        if offset > len(current):
            current = current + b"\x00" * (offset - len(current))
        new = current[:offset] + data + current[offset + len(data):]
        try:
            self.ufs.write_file(path, new)
        except ValueError as e:
            print(f"write refused: {e}")
            raise FuseOSError(errno.EFBIG)
        except IOError as e:
            print(f"write I/O error: {e}")
            raise FuseOSError(errno.EIO)
        return len(data)

    def truncate(self, path, length, fh=None):
        try:
            current = self.ufs.read_file(path)
        except FileNotFoundError:
            raise FuseOSError(errno.ENOENT)
        if length <= len(current):
            new = current[:length]
        else:
            new = current + b"\x00" * (length - len(current))
        try:
            self.ufs.write_file(path, new)
        except ValueError as e:
            print(f"truncate refused: {e}")
            raise FuseOSError(errno.EFBIG)

    # No-ops needed for a well-behaved read/overwrite-only mount.
    def open(self, path, flags):
        self._stat(path)
        if flags & os.O_TRUNC:
            # O_TRUNC on open(): the kernel doesn't always call truncate()
            # separately for this (confirmed live: plain `cp` onto a file
            # whose corrupt inherited di_size is absurdly large hit ERANGE
            # before write() ever ran) -- do it here so any writer relying
            # on O_CREAT|O_TRUNC semantics gets a real zero-length file to
            # write into, same as a normal filesystem.
            try:
                self.ufs.set_size(path, 0)
            except ValueError as e:
                print(f"open(O_TRUNC) size-reset refused: {e}")
                raise FuseOSError(errno.EIO)
        return 0

    def release(self, path, fh):
        return 0

    def flush(self, path, fh):
        return 0

    def fsync(self, path, fdatasync, fh):
        return 0

    def chmod(self, path, mode):
        raise FuseOSError(errno.EROFS)  # mode changes not wired up yet

    def chown(self, path, uid, gid):
        raise FuseOSError(errno.EROFS)  # use fix_pam_ownership.py-style direct patch for now


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mountpoint")
    ap.add_argument("-o", "--options", default="")
    args = ap.parse_args()
    fuse_opts = {}
    for opt in args.options.split(","):
        if not opt:
            continue
        if "=" in opt:
            k, v = opt.split("=", 1)
            fuse_opts[k] = v
        else:
            fuse_opts[opt] = True
    FUSE(UFS2FS(), args.mountpoint, nothreads=True, foreground=True, **fuse_opts)


if __name__ == "__main__":
    main()
