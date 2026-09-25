#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""uboot_chainload_test.py -- run a candidate U-Boot WITHOUT writing it anywhere.

WHY: writing an untested U-Boot to boot media cost a physically pulled card
on 2026-09-23. The BROM only ever sees the eMMC's loader; a candidate is
TFTP'd into DRAM at CONFIG_TEXT_BASE (0x4a000000 -- free once the running
U-Boot has relocated itself to the top of RAM) and started with `go`. If it
hangs, its own watchdog -- or the human at the switch -- returns the board to
the stock loader, and nothing on the media has changed.

Sequence:
  1. `bmc reset` the board (chimpd must be STOPPED: it holds the tty lock and
     would catch the prompt itself).
  2. Catch the stock U-Boot prompt inside its 3 s bootdelay.
  3. tftpboot <candidate> 0x4a000000 ; go 0x4a000000
  4. The candidate re-enumerates its USB gadget: reopen the tty when it comes
     back and record its console for WATCH_S seconds. With `--catch`, flood
     Ctrl-C at the candidate too and leave it at its prompt (for `wdt` tests).
Everything is logged to /var/tmp/uboot_chainload.log with host timestamps,
next to the host kernel's USB events for the same window.
"""
import os, subprocess, sys, time

sys.path.insert(0, '/opt/bzdos/microkernel')
import loady_over_acm as L
import bzd_board as B
import bmc_client

LOG = open('/var/tmp/uboot_chainload.log', 'a')
CAND = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else 'u-boot-wdt.bin'
CATCH = '--catch' in sys.argv
WATCH_S = 150


def log(msg):
    line = time.strftime('%H:%M:%S ') + msg
    print(line, flush=True); LOG.write(line + '\n'); LOG.flush()


def open_when_present(deadline_s):
    t0 = time.time()
    while time.time() - t0 < deadline_s:
        if os.path.exists(L.TTY):
            try:
                return L.open_tty()
            except Exception:
                pass
        time.sleep(0.02)
    return None


def rd(fd, secs, stop=None):
    out = b''; t0 = time.time()
    while time.time() - t0 < secs:
        try:
            d = os.read(fd, 65536)
            if d:
                out += d
        except BlockingIOError:
            pass
        except OSError:
            break                     # node went away: the candidate took over
        if stop and stop in out:
            break
        time.sleep(0.02)
    return out


def main():
    t_start = time.time()
    log(f"##### chain-load test of {CAND} (catch={CATCH})")
    try:
        log("reset: " + str(bmc_client.BMC().reset()))
    except Exception as e:
        log(f"reset verb: {e!r} (continuing: maybe the board is already at U-Boot)")
    fd = open_when_present(60)
    if fd is None:
        log("!!! no tty within 60 s"); return 2
    ok, _ = L.catch_uboot(fd, 12)
    if not ok:
        log("!!! stock prompt not caught"); os.close(fd); return 3
    log("stock U-Boot prompt caught")

    def cmd(line, secs, stop=None):
        os.write(fd, (line + '\n').encode())
        return rd(fd, secs, stop).decode('latin1', 'replace')

    out = cmd(f"setenv autostart no ; setenv netretry no ; setenv ipaddr {B.BOARD_IP} ; "
              f"setenv serverip {B.SRV_IP}", 2)
    out = cmd(f"tftpboot 0x4a000000 {CAND}", 40, b"Bytes transferred")
    if 'Bytes transferred' not in out:
        log("!!! tftp failed:\n" + out[-600:]); os.close(fd); return 4
    log("candidate in DRAM: " + [l for l in out.splitlines() if 'Bytes transferred' in l][0].strip())
    # Chain-loading U-Boot with `go` needs the MMU and caches OFF first, and
    # the candidate's lines cleaned to DRAM: entered with the MMU on,
    # dcache_enable() keeps the OLD page tables at the top of DRAM, where the
    # candidate then relocates itself. Attempt 1 (2026-09-25 12:38) did that:
    # "Starting application", six seconds of progress, silent hang, no
    # watchdog reset (not started yet). The stock loader has no `dcache off`
    # command, so a 128-byte trampoline (tools/chainload/chainload_stub.S)
    # does clean-by-VA, SCTLR_EL2 M/C/I clear, TLB invalidate, then br.
    out = cmd("tftpboot 0x48000000 chainload_stub.bin", 20, b"Bytes transferred")
    if 'Bytes transferred' not in out:
        log("!!! stub tftp failed:\n" + out[-400:]); os.close(fd); return 4
    t_go = time.time()
    log("go 0x48000000 (stub -> 0x4a000000)")
    os.write(fd, b"go 0x48000000\n")
    first = rd(fd, 6).decode('latin1', 'replace')
    log("stock side after go:\n" + first[-800:])
    try:
        os.close(fd)
    except OSError:
        pass

    # The candidate brings its own gadget up -> a NEW node. A node that is
    # merely still there is the stock gadget, abandoned (attempt 2 was fooled
    # by exactly that). So wait for the host to log a fresh enumeration.
    def usb_new_since(t):
        since = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t))
        out = subprocess.run(['journalctl', '-k', '--since', since, '-o', 'short-unix',
                              '--no-pager'], capture_output=True, text=True).stdout
        return [l for l in out.splitlines() if 'usb 1-4' in l and 'idProduct=efe8' in l]
    t_wait = time.time()
    while time.time() - t_wait < 40 and not usb_new_since(t_go + 1):
        time.sleep(0.5)
    if not usb_new_since(t_go + 1):
        log("!!! no fresh gadget within 40 s of go: candidate hung before USB init, "
            "and the stub's 16 s watchdog did not bring the stock loader back either")
        return 5
    fd = open_when_present(10)
    if fd is None:
        log("!!! gadget enumerated but no tty"); return 5
    log(f"fresh gadget + tty at +{time.time() - t_go:.1f}s after go")
    # Which loader is this? If the stub's watchdog fired, it is the STOCK
    # loader again -- catch its prompt and read the candidate's last progress
    # mark out of DRAM (0x4bf00000 survives a warm reset).
    ok, _ = L.catch_uboot(fd, 6)
    if ok:
        marks = cmd("md.l 0x4bf00000 1", 2)
        log(f"prompt caught at +{time.time() - t_go:.1f}s; DRAM progress mark: "
            + ' '.join(marks.split()[-3:]))
        log("(stub=0xb0 1=entry 2=after lowlevel_init 3=before _main "
            "4=board_init_f 5=board_init_r)")
        if not CATCH:
            log("this is the loader that came back after the candidate; leaving it at the prompt")
            os.close(fd); return 6
    if CATCH:
        ok, _ = L.catch_uboot(fd, 12)
        log("candidate prompt caught" if ok else "!!! candidate prompt NOT caught")
        if ok:
            for line in ("version", "wdt list", "wdt dev", "wdt dev watchdog@1c20ca0",
                         "wdt list"):
                log(f"=> {line}\n" + cmd(line, 2.5)[-700:])
            log("leaving the candidate at its prompt (its watchdog is being serviced by its own loop)")
            os.close(fd)
            return 0
    buf = b''
    t0 = time.time()
    while time.time() - t0 < WATCH_S:
        chunk = rd(fd, 2)
        if chunk:
            buf += chunk
        else:
            if not os.path.exists(L.TTY):
                log(f"candidate tty gone at +{time.time() - t_go:.1f}s (handover to the hypervisor?)")
                break
    txt = buf.decode('latin1', 'replace')
    log("candidate console:\n" + txt[-6000:])
    try:
        os.close(fd)
    except OSError:
        pass
    since = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t_start))
    usb = subprocess.run(['journalctl', '-k', '--since', since, '-o', 'short-precise', '--no-pager'],
                         capture_output=True, text=True).stdout
    ev = [l[:110] for l in usb.splitlines() if 'usb 1-4' in l and
          ('new ' in l or 'disconnect' in l or 'Product' in l or 'error' in l)]
    log("host USB events:\n" + '\n'.join(ev))
    return 0


if __name__ == '__main__':
    sys.exit(main())
