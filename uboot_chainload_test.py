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
# --idle-reset: catch the candidate's prompt, then send NOTHING and prove the
# prompt resets the board by itself (CONFIG_BOOT_RETRY + RESET_TO_RETRY). This
# is the property that keeps a U-Boot whose USB gadget has wedged from sitting
# at `=>` for ever, feeding its own watchdog (2026-09-25 18:09 -> 09-26).
IDLE_RESET = '--idle-reset' in sys.argv
if IDLE_RESET:
    CATCH = True
IDLE_RESET_EXPECT_S = 120      # CONFIG_BOOT_RETRY_TIME of the candidate
NET_TEST = '--net-test' in sys.argv   # prove the safety net first, run no candidate
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

    if NET_TEST:
        # The safety net, alone, with no new code on the board: arm the SoC
        # watchdog from the STOCK prompt (the same three writes the stub and
        # the hypervisor use) and do nothing else. The stock loader has no
        # watchdog driver, so nothing pets it: the board must reset itself
        # and re-enumerate the stock gadget ~16 s later. Only when this has
        # been seen to work is a jump into a candidate allowed at all.
        t_arm = time.time()
        cmd("mw.l 0x1c20cb4 1 ; mw.l 0x1c20cb8 0xb1 ; mw.l 0x1c20cb0 0x14af", 1)
        log("watchdog armed from the stock prompt; waiting for the board to reset itself")
        try:
            os.close(fd)
        except OSError:
            pass
        while time.time() - t_arm < 40:
            since = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t_arm + 1))
            usb = subprocess.run(['journalctl', '-k', '--since', since, '-o', 'short-unix',
                                  '--no-pager'], capture_output=True, text=True).stdout
            if any('idProduct=efe8' in l for l in usb.splitlines()):
                log(f"NET OK: stock loader re-enumerated {time.time() - t_arm:.1f} s after arming")
                return 0
            time.sleep(0.5)
        log("!!! NET FAILED: no reset within 40 s of arming -- do NOT jump into anything")
        return 7
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
        marks = cmd("md.l 0x4bf00000 0x18", 3)
        words = [w for w in marks.replace('\r', ' ').split() if len(w) == 8 and
                 all(c in '0123456789abcdef' for c in w)]
        log(f"prompt caught at +{time.time() - t_go:.1f}s; DRAM words @0x4bf00000: {words[:6]}")
        try:
            fn = int(words[3] + words[2], 16)       # 0x4bf00008: last initcall (relocated)
            bir = int(words[5] + words[4], 16)      # 0x4bf00010: board_init_r (relocated)
            nm = subprocess.run(['aarch64-linux-gnu-nm', '/opt/bzdos/build/u-boot/u-boot'],
                                capture_output=True, text=True).stdout
            syms = {}
            for l in nm.splitlines():
                p_ = l.split()
                if len(p_) == 3 and p_[1] in 'Tt':
                    syms[int(p_[0], 16)] = p_[2]
            link_bir = [a for a, n in syms.items() if n == 'board_init_r'][0]
            off = bir - link_bir
            name = syms.get(fn - off, '?')
            log(f"last initcall: 0x{fn:x} (reloc off 0x{off:x}) = {name}")
            sub = int(words[6], 16)
            tlb = int(words[9] + words[8], 16); tlbsz = int(words[11] + words[10], 16)
            log(f"sub-mark 0x{sub:x} (10 before tlbi, 20 before ttbr, 21 after ttbr, 11 before M, "
                f"12 after M, 13 after dcache inval, 14 after C); tlb_addr 0x{tlb:x} size 0x{tlbsz:x}")
            fb = int(words[15] + words[14], 16); magic = int(words[16], 16)
            off = int(words[19] + words[18], 16); src = int(words[20], 16); sub = int(words[23] + words[22], 16)
            log(f"binman diag: fdt_blob=0x{fb:x} magic=0x{magic:x} path_offset(/binman)=0x{off:x} "
                f"fdt_src={src} first_subnode=0x{sub:x}")
            rc = int(words[13] + words[12], 16)
            log(f"initcall failure code at 0x4bf00030: 0x{rc:x}" + (f" ({rc - (1 << 64)})" if rc >> 63 else ""))
        except Exception as e:
            log(f"(initcall decode failed: {e!r})")
        # the candidate's pre-console buffer (CONFIG_PRE_CON_BUF_ADDR=0x4bf10000,
        # 16 KiB ring, civac'd per byte): everything it printed before its
        # console came up, including the initcall failure line or an abort.
        dump = cmd("md.l 0x4bf10000 0x400", 12)
        raw = bytearray()
        for l in dump.replace('\r', '').splitlines():
            p_ = l.split()
            if len(p_) >= 5 and p_[0].endswith(':') and len(p_[0]) == 9:
                for w in p_[1:5]:
                    if len(w) == 8:
                        raw += int(w, 16).to_bytes(4, 'little')
        txt = raw.rstrip(b'\x00').decode('latin1', 'replace')
        log("candidate pre-console buffer (%d bytes):\n%s" % (len(raw), txt[-3000:]))
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
            if IDLE_RESET:
                log("=> printenv bootretry\n" + cmd("printenv bootretry", 2)[-200:])
                # The last byte we send starts the candidate's idle timer.
                t_idle = time.time()
                os.close(fd)
                log(f"idle from now; expecting a self-reset ~{IDLE_RESET_EXPECT_S} s later")
                while time.time() - t_idle < IDLE_RESET_EXPECT_S + 60:
                    if usb_new_since(t_idle + 1):
                        dt = time.time() - t_idle
                        early = dt < IDLE_RESET_EXPECT_S - 5
                        log(f"{'!!! ' if early else ''}fresh loader enumerated {dt:.0f} s after "
                            f"the last byte" + (" -- EARLIER than the retry timeout: "
                            "something else reset it" if early else " -- idle prompt reset itself: PASS"))
                        return 9 if early else 0
                    time.sleep(1)
                log(f"!!! no reset within {IDLE_RESET_EXPECT_S + 60} s of idle: FAIL "
                    "(the stub's watchdog is serviced by the candidate's prompt loop)")
                return 8
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
