# Why the guest kept coming up with no network — and what was actually wrong

Measured 2026-08-12 on the live guest. Every claim here is a direct read of the
running system, not inference.

## It was never the route or sshd

The recurring symptom — after any unclean stop the guest comes up with root
read-only, no hostname, no network, no sshd, and needs
`fsck_ffs -y` + `mount -u -o rw /` + `service netif restart` + `route add default`
+ `service sshd start` by hand — was diagnosed for a long time as "the network
config is not persistent". It is not. `/etc/rc.conf` already contains, and has
contained:

```
hostname="bsdos-chimp"
sshd_enable=YES
ifconfig_vtnet0="inet 192.168.88.82 netmask 255.255.255.0"
defaultrouter=192.168.88.1
```

Every one of those is correct and persistent. The manual "restore" steps were
re-applying settings that were already in the file.

## What was actually wrong: rc aborts before it reaches them

`/etc/defaults/rc.conf:117` ships `fsck_y_enable="NO"`, and `/etc/rc.d/fsck`
does this (line numbers from the guest's own copy):

```
:62    if checkyesno fsck_y_enable; then
:63        echo "File system preen failed, trying fsck -y ${fsck_y_flags}"
:64        fsck -y ${fsck_y_flags}
...
:75    stop_boot
```

So: an unclean stop leaves the root fs dirty → the boot-time `fsck -p` (preen)
fails on a dirty fs → with `fsck_y_enable=NO` every branch from there reaches
`stop_boot` → **`/etc/rc` stops before `hostname`, `netif`, `defaultrouter` and
`sshd` ever run.** The config was fine; control flow never got to it.

**Fix (applied 2026-08-12):** `fsck_y_enable="YES"` appended to the guest's
`/etc/rc.conf`, with the reasoning inline and a backup at
`/etc/rc.conf.pre-fsck-y-2026-08-12`.

Trade-off, taken deliberately: `fsck -y` repairs without a human looking. On a
rig that is hard-reset constantly this is right, and it is exactly what the
recovery tooling (`breakglass_cycle.py`, and the manual procedure) already did —
16 of the 20 break-glass attempts on 2026-08-11 needed a repair and none needed a
second pass.

**Not yet proven.** Confirming it needs an unclean stop followed by a boot, which
was not done while a long build was running on the guest. The config change is
verified present; the behaviour is not.

## The head of the corruption ratchet: /sbin/shutdown

Found in the same pass, and it explains why stops were unclean in the first
place:

```
/bin/ls          MISSING          (and /bin/ls fails to exec)
/bin/cat         17348632 bytes
/sbin/shutdown   17348632 bytes
/sbin/poweroff   (same shared, corrupt inode)
/bin/sh          167384 bytes     (sane)
/bin/cp          25304 bytes      (sane)
/sbin/reboot     15000 bytes      (sane)
```

`cat` and `shutdown` reporting the *same* absurd 17 MB size is the known
corrupt-`di_size` / shared-inode damage this project has hit before (project
memory: "Unclean-stop ratchet: /sbin/shutdown was ENOEXEC", corrupt inode 1725
shared between `shutdown` and `poweroff`).

That is a self-sustaining loop:

    /sbin/shutdown broken  ->  no clean stop possible
                           ->  fs always dirty at boot
                           ->  fsck -p fails, rc aborts (above)
                           ->  more unclean stops
                           ->  more corruption

`fsck_y_enable` breaks the *symptom* half of that loop. Restoring `shutdown`
breaks the *cause* half.

**Fixed 2026-08-12.** Official binaries from `base.txz` for 15.1-RELEASE,
installed over the corrupt ones:

```
/bin/ls          MISSING   -> 35352 bytes, executes
/sbin/shutdown   17348632  -> 15696 bytes, prints its usage line
/sbin/poweroff   17348632  -> 15696 bytes
```

`shutdown` printing `usage: shutdown [-] [-c | -f | -h | -p | -r | -k] ...` is the
proof that matters: the binary was previously the wrong size AND unable to exec, so
a clean stop was impossible. It is now possible.

Deliberately NOT offline UFS2 surgery on the image (banned, `WOW_FEATURES.md` §0
rule 7) and NOT a hand-patched binary: these are the official files.

Two things worth knowing for the next time:

- Download base.txz **on the build host**, not on the guest. Streaming ~156 MB
  over the guest's own link did not finish inside five minutes; the host has a
  fast link, and only ~66 KB of extracted files then has to cross the LAN.
- `tar -xf archive.txz <specific members>` stops reading once it has found them
  all, which makes `xz` report `Compressed data is corrupt / Unexpected end of
  file`. That is an artifact of closing the pipe early, **not** a bad download —
  the files extract fine. Confirmed by checking the archive's sha256 against the
  release `MANIFEST`: it matched exactly. Do not chase a phantom corrupt download
  the way this pass briefly did.

## What this changes operationally

The manual recovery in the scratchpad (`fsck_ffs -y` → `mount -u -o rw /` →
`netif`/`route`/`sshd`) should become unnecessary once the fsck flag is proven.
Until then, keep it. `breakglass_cycle.py` performs the same sequence itself and
is unaffected either way.
