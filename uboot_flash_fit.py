#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""uboot_flash_fit.py -- replace U-Boot proper (the FIT at eMMC LBA 80) from the stock prompt.

Only the FIT is written. The SPL at LBA 16 is never touched, so the BROM
always finds the same, 3-months-proven SPL; a bad FIT is rolled back with
this same tool from any U-Boot prompt (or, lacking one, docs/brick-recovery.md).

This is the procedure that flashed u-boot-wdt.itb on 2026-09-25 16:21, made
reusable instead of an inline one-off. Guards, in order, each aborting BEFORE
anything is written:
  - the TFTP'd image has exactly the size of the local file;
  - `mmc dev 1` is the eMMC (mmc@1c11000);
  - the FIT currently at LBA 80 is byte-identical to --current (so a stale
    idea of what is on the card can never be overwritten blind);
then: write, read back, `cmp.b` the whole image, and only then a warm reset
into the new loader, whose `version` must carry --expect-version.

Run ONLY after the candidate passed uboot_chainload_test.py, with chimpd
stopped (it holds the tty lock and would catch the prompt itself).

    uboot_flash_fit.py --fit u-boot-retry.itb --current u-boot-wdt.itb \\
                       --expect-version "Sep 26 2026 - 13:46"
Files are names inside /opt/bzdos/tftpboot. Log: /var/tmp/uboot_flash.log.
"""
import argparse, os, struct, subprocess, sys, time

sys.path.insert(0, '/opt/bzdos/microkernel')
import loady_over_acm as L
import bzd_board as B
import bmc_client

TFTPROOT = '/opt/bzdos/tftpboot'
FIT_LBA = 0x50
LOAD = 0x4a000000       # the image to write
READ = 0x4c000000       # what is on the card
LOG = open('/var/tmp/uboot_flash.log', 'a')


def log(m):
    line = time.strftime('%H:%M:%S ') + m
    print(line, flush=True)
    LOG.write(line + '\n'); LOG.flush()


def fit_size(path):
    with open(path, 'rb') as f:
        h = f.read(8)
    if h[:4] != bytes.fromhex('d00dfeed'):
        sys.exit(f"{path}: not a FIT (magic {h[:4].hex()})")
    n = struct.unpack('>I', h[4:8])[0]
    if n != os.path.getsize(path):
        sys.exit(f"{path}: FIT totalsize 0x{n:x} != file size 0x{os.path.getsize(path):x}")
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--fit', required=True, help='new FIT, name in the TFTP root')
    ap.add_argument('--current', required=True, help='FIT that must be on the card now')
    ap.add_argument('--expect-version', required=True,
                    help='substring of the new loader\'s `version` line (its build stamp)')
    a = ap.parse_args()

    new_n = fit_size(os.path.join(TFTPROOT, a.fit))
    cur_n = fit_size(os.path.join(TFTPROOT, a.current))
    new_sec, cur_sec = (new_n + 511) // 512, (cur_n + 511) // 512
    # The FIT must not grow into whatever follows it; both loaders so far are
    # 0x6ab/0x6b7 sectors, far below the 0x3fb0 the boot area leaves.
    if new_sec > 0x3fb0:
        sys.exit(f"new FIT is 0x{new_sec:x} sectors -- too big for the boot area")
    log(f"##### flash {a.fit} (0x{new_n:x} B, 0x{new_sec:x} sectors) over {a.current} "
        f"(0x{cur_n:x} B) at eMMC LBA 0x{FIT_LBA:x}; SPL untouched")

    try:
        log("reset: " + str(bmc_client.BMC().reset()).strip())
    except Exception as e:
        log(f"reset verb: {e!r} (continuing: maybe already at U-Boot)")
    t0 = time.time(); fd = None
    while time.time() - t0 < 60:
        if os.path.exists(L.TTY):
            try:
                fd = L.open_tty(); break
            except Exception:
                pass
        time.sleep(0.02)
    if fd is None:
        log("!!! no tty"); return 2
    ok, _ = L.catch_uboot(fd, 12)
    log(f"stock prompt caught: {ok}")
    if not ok:
        return 3

    def rd(secs, stop=None):
        out = b''; t = time.time()
        while time.time() - t < secs:
            try:
                d = os.read(fd, 65536)
                if d:
                    out += d
            except BlockingIOError:
                pass
            if stop and stop in out:
                break
            time.sleep(0.03)
        return out.decode('latin1', 'replace').replace('\r', '')

    def cmd(c, secs=3, stop=None):
        os.write(fd, (c + '\n').encode())
        return rd(secs, stop)

    cmd(f"setenv autostart no ; setenv netretry no ; setenv ipaddr {B.BOARD_IP} ; "
        f"setenv serverip {B.SRV_IP}", 2)
    for name, addr, n in ((a.current, READ, cur_n), (a.fit, LOAD, new_n)):
        o = cmd(f"tftpboot 0x{addr:x} {name}", 40, b"Bytes transferred")
        bt = [l for l in o.splitlines() if 'Bytes transferred' in l]
        log(f"tftp {name}: " + (bt[0].strip() if bt else o[-300:]))
        if not bt or f"= {n} " not in bt[0] + ' ':
            log("!!! wrong size, abort -- nothing written"); return 4
    # The reference copy of --current now sits at READ; move it aside so the
    # card can be read into READ and compared against it.
    REF = 0x4e000000
    cmd(f"cp.b 0x{READ:x} 0x{REF:x} 0x{cur_n:x}", 4)

    o = cmd("mmc dev 1", 4); log("mmc dev 1: " + o.strip().replace('\n', ' | ')[-120:])
    o = cmd("mmc info", 4)
    log("mmc info: " + ' | '.join(l.strip() for l in o.splitlines()
                                   if any(k in l for k in ('Device', 'Capacity', 'Name'))))
    if '1c11000' not in o:
        log("!!! mmc dev 1 is not the eMMC (1c11000): abort -- nothing written"); return 5
    cmd(f"mmc read 0x{READ:x} 0x{FIT_LBA:x} 0x{cur_sec:x}", 8)
    o = cmd(f"cmp.b 0x{READ:x} 0x{REF:x} 0x{cur_n:x}", 20, b"same")
    log("card vs --current: " + o.strip().splitlines()[-1][:120])
    if 'were the same' not in o:
        log(f"!!! LBA 0x{FIT_LBA:x} does not hold {a.current}: abort -- nothing written"); return 6

    log("WRITING ...")
    o = cmd(f"mmc write 0x{LOAD:x} 0x{FIT_LBA:x} 0x{new_sec:x}", 15)
    log("mmc write: " + o.strip().splitlines()[-1][:120])
    o = cmd(f"mmc read 0x{READ:x} 0x{FIT_LBA:x} 0x{new_sec:x}", 8)
    log("read back: " + o.strip().splitlines()[-1][:120])
    o = cmd(f"cmp.b 0x{LOAD:x} 0x{READ:x} 0x{new_n:x}", 20, b"same")
    log("verify: " + o.strip().splitlines()[-1][:120])
    if 'were the same' not in o:
        log(f"!!! VERIFY FAILED -- NOT resetting; the prompt is still alive. Put the old "
            f"loader back by hand from it: tftpboot 0x{LOAD:x} {a.current} ; "
            f"mmc write 0x{LOAD:x} 0x{FIT_LBA:x} 0x{cur_sec:x}")
        return 7

    log("flashed and verified. warm reset via WDOG -> SPL loads the new FIT")
    t_rst = time.time()
    cmd("mw.l 0x1c20cb4 1 ; mw.l 0x1c20cb8 0xb1 ; mw.l 0x1c20cb0 0x14af", 1)
    try:
        os.close(fd)
    except OSError:
        pass
    since = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t_rst + 1))
    seen = False
    while time.time() - t_rst < 60:
        usb = subprocess.run(['journalctl', '-k', '--since', since, '-o', 'short-precise',
                              '--no-pager'], capture_output=True, text=True).stdout
        if any('idProduct=efe8' in l for l in usb.splitlines()):
            seen = True; break
        time.sleep(0.5)
    log(f"new loader's gadget seen: {seen} (+{time.time() - t_rst:.0f}s)")
    if not seen:
        log("!!! new loader did not enumerate: docs/brick-recovery.md"); return 8
    fd = None; t = time.time()
    while fd is None and time.time() - t < 10:
        try:
            fd = L.open_tty()
        except Exception:
            time.sleep(0.05)
    if fd is None:
        log("!!! gadget but no tty"); return 8
    ok, _ = L.catch_uboot(fd, 8)
    log(f"new loader prompt caught: {ok}")
    if not ok:
        return 8
    v = [l for l in cmd("version").splitlines() if l.startswith('U-Boot')]
    log("version: " + str(v[:1]))
    if not v or a.expect_version not in v[0]:
        log(f"!!! running loader is not the one written (want '{a.expect_version}')"); return 9
    m = [l for l in cmd("md.l 0x1c20cb8 1").splitlines() if l.startswith('01c20cb8')]
    log("WDOG_MODE (0xb1 = armed 16 s): " + str(m))
    log("booting it: run bootcmd")
    os.write(fd, b"run bootcmd\n")
    o = rd(90, b"Booting kernel")
    log("bootcmd reached bootm: " + str('Booting kernel' in o))
    try:
        os.close(fd)
    except OSError:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
