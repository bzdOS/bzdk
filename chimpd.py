#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
chimpd.py — autonomous board supervisor for the bzdOS microkernel/hypervisor
debug path on Banana Pi M64 (Allwinner A64).

Does EVERYTHING: catches the board on every power-cycle, loads the hypervisor +
FreeBSD kernel, monitors the guest live via the EMAC network console, extracts
console output, detects hangs/panics, and auto-resets to repeat. No manual
commands needed — just run it and power-cycle the board.

REQUIRES (already in the tree):
  - loady_over_acm.py  (USB-OTG ACM loader + TFTP preflight + port lock)
  - tftpboot/kernel    (FreeBSD arm64 kernel ELF, 16 MB)
  - tftpboot/bananapi-min.dtb
  - microkernel-dbg.elf (the hypervisor+debugger image)

LOOP:
  1. Wait for /dev/ttyCHIMP (board power-cycled → U-Boot enumerates USB gadget)
  2. Catch U-Boot `=>` prompt
  3. TFTP preflight (auto-start dnsmasq if :69 is dead)
  4. TFTP FreeBSD kernel (16 MB → 0x44000000) + DTB (→ 0x4a000000)
  5. loady microkernel-dbg.ELF → staging 0x48000000 → `bootelf -p`
  6. EMAC network console comes up (raw ethertype 0x88B5 on br0)
  7. Monitor loop (sample every --interval seconds):
     a. GICT breadcrumb (0x50000800): timer ticks — is the hypervisor alive?
     b. vconsole ring header (0x50000f00): total_bytes — console progress?
     c. vconsole ring data (0x50000f10): extract FreeBSD boot console text
     d. firstfault `ff` + EXC (0x50000400): any guest panic?
     e. guest regs `gr`: ELR — where is the guest PC?
  8. Verdict + action:
     - console progressing → log new text, keep monitoring
     - frozen (ticks alive, console static AND no disk I/O) for N samples →
       HANG → reset → loop.  Console silence ALONE is not a hang: a booting
       FreeBSD is quiet for well over 40 s at a stretch while hammering the
       eMMC, so vblk's request counters count as progress too.
     - a guest that reached mountroot/login and then went quiet is left ALIVE
       (an idle system produces neither console output nor I/O)
     - panic detected → record → reset → loop
     - mountroot/login markers in console → BOOT OK, keep monitoring

USAGE:
  chimpd.py                     # loop forever, auto-reset on hang
  chimpd.py --once              # single boot cycle, then exit
  chimpd.py --no-reset          # monitor only, never WDT-reset
  chimpd.py --monitor-only      # board already running, just poll breadcrumbs
  chimpd.py --interval 5        # poll every 5 s (default 10)
  chimpd.py --hang-samples 6    # reset after 6 frozen samples (default 4)
"""
import os, sys, time, re, socket, struct, select, subprocess, argparse, signal
import bzd_board as B

# ── load loady_over_acm as a library (USB serial + TFTP preflight) ──────────
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import loady_over_acm as L

# ── constants ───────────────────────────────────────────────────────────────
HYP_ELF   = os.path.join(HERE, "microkernel-dbg.elf")
KERNEL    = "/opt/bzdos/tftpboot/kernel"
DTB       = "/opt/bzdos/tftpboot/bananapi-min.dtb"
KADDR     = 0x44000000
DTBADDR   = 0x4a000000
STAGE     = 0x48000000
BOARD_IP  = B.BOARD_IP
SRV_IP    = B.SRV_IP

IFACE     = B.IFACE
ETYPE     = B.ETYPE
BOARD_MAC = B.BOARD_MAC
BCAST     = B.BCAST

# breadcrumb addresses (host physical, see dbgmon.c / el2_exc.c / gic_timer.c)
# NOTE (discrepancy found while centralizing, preserved as-is — see
# bzd_board.py's header comment): this module's own docstring above claims
# the GICT breadcrumb is at 0x50000800 (the DRAM hv-scratch address hvdbg.py
# reads), but the constant actually POLLED here has always been the
# low-SRAM address 0x00018200. Not silently reconciled — flagged, not fixed.
BC_VCON   = B.VCONSOLE_HDR_PA   # vconsole: [0]magic [1]?? → actually total_bytes at +4
BC_EXC    = B.BC_EXC_SRAM_PA    # exception record
BC_GICT   = B.GICT_BC_SRAM_PA   # gic_timer (tick + IAR/EOI diagnostic)
VCON_RING = B.VCONSOLE_RING_PA  # vconsole ring data start
VCON_RING_SZ = B.VCONSOLE_RING_SZ  # ring is 4 KiB

# WDT reset (A64 watchdog, see reboot.c)
WDOG_CFG  = B.WDOG_CFG_PA
WDOG_MODE = B.WDOG_MODE_PA
WDOG_CTRL = B.WDOG_CTRL_PA

# console markers for verdict
MARK_BOOT   = b"Copyright (c) 1992"
MARK_MOUNT  = b"Trying to mount root"
MARK_LOGIN  = b"login:"
MARK_PANIC  = b"panic"
MARK_FATAL  = b"Fatal trap"

LOGDIR = "/opt/bzdos/chimpd-logs"
os.makedirs(LOGDIR, exist_ok=True)


# ── logging ─────────────────────────────────────────────────────────────────
def slog(msg):
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)

class Session:
    """One boot cycle's logs."""
    def __init__(self, n):
        self.n = n
        ts = time.strftime("%Y%m%d-%H%M%S")
        self.raw_log   = open(f"{LOGDIR}/session-{ts}-raw.log", "ab", buffering=0)
        self.console   = open(f"{LOGDIR}/session-{ts}-console.log", "ab", buffering=0)
        self.last_vcon_bytes = 0

    def raw(self, d):
        if isinstance(d, str): d = d.encode("latin1", "replace")
        self.raw_log.write(d)

    def con(self, d):
        if isinstance(d, str): d = d.encode("latin1", "replace")
        self.console.write(d)

    def close(self):
        for f in (self.raw_log, self.console):
            try: f.close()
            except: pass


# ── EMAC raw-Ethernet network console ───────────────────────────────────────
class NetCon:
    """AF_PACKET raw socket for the board's EMAC console (ethertype 0x88B5).
    Mirrors repl-cmd.py's protocol exactly."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                  socket.htons(ETYPE))
        self.sock.bind((IFACE, ETYPE))
        self.src_mac = self.sock.getsockname()[4]
        self.sock.settimeout(0.3)

    def send_cmd(self, text):
        p = (text + "\r").encode("latin1", "replace")
        if len(p) < 46:
            p += b"\x00" * (46 - len(p))
        self.sock.send(BCAST + self.src_mac + struct.pack("!H", ETYPE) + p)

    def drain(self, secs):
        """Collect all board replies for `secs` seconds."""
        buf = b""
        t0 = time.time()
        while time.time() - t0 < secs:
            remain = max(0.01, secs - (time.time() - t0))
            self.sock.settimeout(remain)
            try:
                frame = self.sock.recv(2048)
            except (socket.timeout, OSError):
                break
            if len(frame) < 14:
                continue
            if struct.unpack("!H", frame[12:14])[0] != ETYPE:
                continue
            if frame[6:12] != BOARD_MAC:
                continue
            payload = frame[14:].split(b"\x00", 1)[0]
            if payload:
                buf += payload
        return buf

    def cmd(self, text, wait=2.0):
        """Send a command, return the board's text reply."""
        self.drain(0.3)          # flush stale echo
        self.send_cmd(text)
        return self.drain(wait).decode("latin1", "replace")

    def is_alive(self, timeout=1.0):
        """Quick liveness check: send a no-op and see if the board responds."""
        self.drain(0.1)
        self.send_cmd("t")
        return bool(self.drain(timeout).strip())

    def wdt_reset(self):
        """Force a CLEAN board reset.

        MUST drop the MUSB USB pull-up before arming the WDOG, else U-Boot's
        console gadget wedges (host error -71) with no remote recovery (bit us
        twice 2026-07-19). ROBUST path: call the hypervisor's own reboot_clean()
        which does disconnect + WDOG + spin atomically on-board — no dependence
        on flaky EMAC reads. Fallback: bare WDOG writes (may wedge USB)."""
        addr = None
        try:
            out = subprocess.check_output(
                ["aarch64-linux-gnu-nm", HYP_ELF],
                stderr=subprocess.DEVNULL).decode()
            for line in out.splitlines():
                p = line.split()
                if len(p) == 3 and p[2] == "reboot_clean":
                    addr = int(p[0], 16); break
        except Exception:
            pass
        if addr is not None:
            self.cmd(f"call 0x{addr:x}", 0.5)   # atomic clean reset; won't return
            slog(f"  clean reset via reboot_clean @0x{addr:x}")
            return
        for pa, val in B.WDOG_ARM_SEQUENCE:
            self.cmd(f"w 0x{pa:x} {val:x}", 0.3)
        slog("  WDT reset commanded (bare WDOG — reboot_clean addr not found)")

    def close(self):
        try: self.sock.close()
        except: pass


# ── breadcrumb parsing helpers ──────────────────────────────────────────────
def parse_words(text):
    """Extract all hex words from an `r <addr> <n>` reply.
    Format: '50000800: DEADBEEF CAFEBABE ...'"""
    words = []
    for line in text.split("\n"):
        line = line.strip()
        if ":" not in line:
            continue
        parts = line.split(":", 1)[1].split()
        for p in parts:
            p = p.strip()
            if len(p) == 8:
                try: words.append(int(p, 16))
                except ValueError: pass
    return words

def parse_dump_ascii(text):
    """Extract the ASCII column from a `d <addr> <len>` hex+ASCII dump.
    Format: '50000f10: 45 48 ... |EHCI version 1.0|'"""
    out = b""
    for line in text.split("\n"):
        if "|" not in line:
            continue
        ascii_part = line.split("|", 1)[1]
        if "|" in ascii_part:
            ascii_part = ascii_part.split("|")[0]
        out += ascii_part.encode("latin1", "replace")
    return out


# ── serial phase: catch U-Boot, TFTP, loady+bootelf ─────────────────────────
def ucmd(fd, c, w=8, save=None):
    """Send a U-Boot command, wait for prompt, return text reply."""
    L.rd(fd, 0.2)
    L.wr(fd, (c + "\r").encode())
    buf = b""
    t0 = time.time()
    while time.time() - t0 < w:
        b, _ = L.rd(fd, 0.3)
        if b:
            buf += b
            if save: save(b)
        clean = L._CSI.sub(b'', buf).decode("latin1", "replace")
        if "=>" in clean[-6:] and len(clean) > len(c) + 6:
            break
    return L._CSI.sub(b'', buf).decode("latin1", "replace")

def serial_load(sess):
    """Full serial boot: catch U-Boot → TFTP kernel+DTB → loady+bootelf.
    Returns True on success."""
    slog("  [serial] ждём ttyCHIMP (board must power-cycle)…")
    if not L.wait_present(timeout=3600):
        slog("  [serial] ⛔ ttyCHIMP не появился за час")
        return False

    slog("  [serial] TFTP preflight")
    if not L.tftp_preflight():
        slog("  [serial] ⛔ TFTP сервер не поднялся")
        return False

    try:
        fd = L.open_tty(True)
    except Exception as e:
        slog(f"  [serial] ⛔ open_tty: {e}")
        return False

    ok, buf = L.catch_uboot(fd, 25)
    sess.raw(buf)
    if not ok:
        slog("  [serial] ⛔ U-Boot prompt не пойман")
        try: os.close(fd)
        except: pass
        return False
    slog("  [serial] ✅ U-Boot пойман")

    def save(d):
        if isinstance(d, str): d = d.encode()
        sess.raw(d)

    # autostart handling (verified 2026-07-16, both directions bite):
    #  - autostart=YES during tftpboot -> a successful/late transfer auto-`bootm`s
    #    the raw kernel and destabilises the board (gadget drops mid-load).
    #  - autostart=NO during bootelf -> bootelf LOADS segments but does NOT jump
    #    (HV never starts, silent NO_EMAC).
    # So: NO for the tftps, then flip to YES right before loady+bootelf.
    # net setup. netretry=no is CRUCIAL for load reliability: a failed tftp
    # (link not up yet) must fail FAST. With U-Boot's default ARP/tftp retry it
    # blocks the main loop for many seconds, starving the USB-serial gadget ->
    # the host drops ttyACM mid-load and the board goes off-bus. netretry=no ->
    # each tftp returns quickly, our own retry loop (below) paces the link-up
    # wait with short sleeps that DON'T starve the gadget. autostart=no here
    # (flipped to yes right before bootelf; both directions bite — see 2026-07-16).
    slog("  [serial] net setup (autostart no, netretry no)")
    ucmd(fd, f"setenv autostart no ; setenv netretry no ; setenv ipaddr {BOARD_IP} ; setenv serverip {SRV_IP}", 4, save)

    # Let the sun8i EMAC PHY finish auto-negotiation before the first tftp
    # (~1-3s after reset). Cheap insurance against the first-attempt link race.
    time.sleep(4)

    # tftp with fast-fail retry. With netretry=no each attempt fails in ~1-2s if
    # the link isn't up, so retries cycle quickly without long gadget-starving
    # blocks. Order matters: fetch the SMALL dtb FIRST — it warms the link (once
    # it succeeds the link is up), so the big 16MB kernel then transfers in ONE
    # clean pass (minimal gadget starvation, no mid-transfer drop).
    def tftp_retry(addr, fname, per_try_w, tries=12, gap=2):
        for i in range(1, tries + 1):
            r = ucmd(fd, f"tftpboot 0x{addr:x} {fname}", per_try_w, save)
            if "Bytes transferred" in r:
                if i > 1:
                    slog(f"  [serial]   (tftp {fname} ок с попытки {i})")
                return True
            slog(f"  [serial]   tftp {fname} попытка {i}/{tries} — линк не готов, пауза {gap}s")
            time.sleep(gap)
        return False

    # short per-try (6s) so a down-link doesn't block U-Boot's main loop long
    # enough to starve/drop the USB gadget; many quick tries give the PHY time
    # to negotiate without any single long stall.
    slog(f"  [serial] TFTP DTB → 0x{DTBADDR:x} (маленький, прогрев линка, короткие попытки)")
    if not tftp_retry(DTBADDR, "bananapi-min.dtb", 6, tries=15):
        slog("  [serial] ⛔ TFTP DTB failed после ретраев")
        try: os.close(fd)
        except: pass
        return False

    slog(f"  [serial] TFTP kernel → 0x{KADDR:x} (~55s for 16MB, линк уже прогрет)")
    if not tftp_retry(KADDR, "kernel", 90, tries=3):
        slog("  [serial] ⛔ TFTP kernel failed после ретраев")
        try: os.close(fd)
        except: pass
        return False

    # flip autostart ON so bootelf actually JUMPS to the loaded HV entry
    ucmd(fd, "setenv autostart yes", 4, save)
    slog(f"  [serial] loady {HYP_ELF} → staging 0x{STAGE:x} + bootelf -p (autostart yes)")
    ok, msg = L.do_loady_and_go(fd, save, HYP_ELF, addr=STAGE,
                                 exec_cmd=f"bootelf -p 0x{STAGE:x}")
    slog(f"  [serial] loady/bootelf: {msg}")
    try: os.close(fd)
    except: pass
    return ok


# ── monitor phase: poll breadcrumbs over EMAC ───────────────────────────────
def monitor(sess, nc, interval, hang_samples, no_reset):
    """Poll breadcrumbs until hang/panic/boot-complete or timeout.
    Returns a verdict string."""
    frozen_count = 0
    last_ticks = None
    last_vcon_bytes = None
    last_io = None
    boot_ok = False
    pokes = 0
    # Bounded so a genuinely dead guest still reaches a verdict instead of being
    # poked forever; each poke costs ~5s and the 600s cap below still applies.
    MAX_POKES = 3
    max_monitor = 600  # 10 min cap per cycle

    slog("  [monitor] EMAC опрос breadcrumbs каждые %ds" % interval)
    t0 = time.time()
    sample = 0

    while time.time() - t0 < max_monitor:
        sample += 1

        # NOTE: dbgmon parses EVERY numeric argument with parse_hex(), the
        # word/byte COUNT included -- so a decimal-looking count means
        # something else entirely (13 asked for 0x13 = 19 words). The reads
        # below therefore spell their counts in hex. The old decimal ones
        # happened to work only because the extra words were unused.
        # ── GICT: timer alive? ──
        words = parse_words(nc.cmd(f"r 0x{BC_GICT:x} 8", 1.5))
        ticks_lo = words[1] if len(words) > 1 else 0
        ticks_hi = words[2] if len(words) > 2 else 0
        ticks = (ticks_hi << 32) | ticks_lo
        init_done = words[6] if len(words) > 6 else 0
        timer_alive = (last_ticks is not None and ticks != last_ticks)
        if last_ticks is not None:
            slog(f"  [monitor] #{sample} ticks=0x{ticks:x} (Δ={ticks-(last_ticks or 0):+d}) "
                 f"init_done={init_done} timer={'ALIVE' if timer_alive else 'FROZEN'}")

        # ── vconsole header: console bytes? ──
        vw = parse_words(nc.cmd(f"r 0x{BC_VCON:x} 4", 1.5))
        vcon_bytes = vw[1] if len(vw) > 1 else 0
        vcon_faults = vw[2] if len(vw) > 2 else 0
        vcon_growing = (last_vcon_bytes is not None and vcon_bytes != last_vcon_bytes)
        if vcon_growing:
            slog(f"  [monitor] #{sample} vconsole: {last_vcon_bytes}→{vcon_bytes} bytes "
                 f"(+{vcon_bytes-(last_vcon_bytes or 0)}), faults={vcon_faults}")

            # ── dump new console text ──
            dump = nc.cmd(f"d 0x{VCON_RING:x} "
                          f"0x{min(vcon_bytes - sess.last_vcon_bytes, VCON_RING_SZ):x}", 3)
            ascii_text = parse_dump_ascii(dump)
            if ascii_text:
                sess.con(ascii_text)
                # also stream to raw log
                sess.raw(f"\n# [console +{len(ascii_text)}B]\n".encode())
                sess.raw(ascii_text)
                # check for markers
                low = ascii_text.lower()
                if MARK_MOUNT.lower() in low:
                    slog("  [monitor] 🎯 mountroot detected!")
                    boot_ok = True
                elif MARK_LOGIN.lower() in low:
                    slog("  [monitor] 🎉 login prompt!")
                    boot_ok = True
                elif MARK_PANIC in ascii_text or MARK_FATAL in ascii_text:
                    slog("  [monitor] 💀 PANIC detected in console!")
                    sess.raw(b"\n# [PANIC]\n")
                    return "PANIC"

            sess.last_vcon_bytes = vcon_bytes

        # ── firstfault: any guest panic? ──
        if sample <= 2 or sample % 5 == 0:
            ff = nc.cmd("ff", 2)
            if "magic" not in ff.lower() and ff.strip() and "FF1V" in ff:
                slog(f"  [monitor] 💀 firstfault fired!\n{ff}")
                sess.raw(f"\n# [FIRSTFAULT]\n{ff}\n".encode())
                return "FIRSTFAULT"

        # ── exception breadcrumb ──
        if sample <= 2 or sample % 10 == 0:
            ew = parse_words(nc.cmd(f"r 0x{BC_EXC:x} 0xd", 1.5))
            if len(ew) > 3 and ew[0] != 0:
                exc_count = ew[1]
                if exc_count > 0 and sample > 2:
                    slog(f"  [monitor] ⚠ guest exception #{exc_count} (EXC breadcrumb)")
                    sess.raw(f"\n# [EXC #{exc_count} kind=0x{ew[2]:x} esr=0x{ew[3]:x} "
                             f"elr=0x{ew[5]:x}{ew[4]:08x}]\n".encode())

        # ── disk I/O: is the guest doing work it just isn't talking about? ──
        # A booting FreeBSD goes quiet for well over 40s at a stretch (fsck,
        # device probing, waiting on the eMMC) while hammering the disk the
        # whole time. Judging liveness on console growth alone therefore
        # condemns healthy boots: on 2026-08-04 this fired twice, ~50s in, on
        # the very boot that went on to reach `login:` with working ssh, and
        # WDT-reset it both times -- so no test needing the guest to reach
        # userland could run under chimpd at all. vblk's request counters
        # advance whenever the guest touches the disk, which is the missing
        # signal.
        io_growing = False
        try:
            iw = parse_words(nc.cmd(f"r 0x{B.VBLK_BC_PA:x} 0x28", 2))
            if len(iw) > 32:
                # reads, writes, kicks_seen -- any one moving means progress
                io = (iw[6], iw[7], iw[31])
                io_growing = (last_io is not None and io != last_io)
                if io_growing:
                    slog(f"  [monitor] #{sample} vblk r/w/kicks {last_io} → {io}")
                last_io = io
        except Exception:
            pass          # never let a diagnostic read change the verdict

        # ── hang detection ──
        # "frozen" = timer alive (ticks advancing) but NEITHER the console nor
        # the disk making progress. Anything still moving means alive.
        if last_ticks is not None and last_vcon_bytes is not None:
            if timer_alive and not vcon_growing and not io_growing:
                frozen_count += 1
                slog(f"  [monitor] frozen sample {frozen_count}/{hang_samples}")
                if frozen_count >= hang_samples:
                    # POKE BEFORE CONDEMNING. Quiet is not the same as dead:
                    # an idle guest at `login:` or a shell prompt produces no
                    # console output and no disk I/O by definition, and
                    # `mountroot>` is printed exactly ONCE and then waits (musb
                    # drops undrained bytes, so passive listening can never see
                    # it -- project memory: automount-must-poke-not-listen).
                    # `bmc con inject` with no tokens pushes a bare CR into the
                    # guest's UART RX, which any responsive getty/shell/prompt
                    # echoes. If the console grows after that, the guest was
                    # merely quiet and we must NOT reset it.
                    if pokes < MAX_POKES:
                        pokes += 1
                        slog(f"  [monitor] quiet {frozen_count} samples — poking "
                             f"the guest console (CR) {pokes}/{MAX_POKES}")
                        nc.cmd("bmc con inject", 2)
                        time.sleep(3)
                        pw = parse_words(nc.cmd(f"r 0x{BC_VCON:x} 0x4", 1.5))
                        after = pw[1] if len(pw) > 1 else vcon_bytes
                        if after != vcon_bytes:
                            slog(f"  [monitor] ✅ guest ANSWERED the poke "
                                 f"({vcon_bytes}→{after}) — alive but idle, "
                                 f"not a hang")
                            # Reset the poke budget too: it counts CONSECUTIVE
                            # UNANSWERED pokes, not pokes ever sent. Counting
                            # every poke would condemn a healthy idle guest on
                            # its fourth quiet stretch -- moving the false HANG
                            # from 40s out to a couple of minutes instead of
                            # removing it, which is the bug this whole change
                            # exists to fix.
                            pokes = 0
                            frozen_count = 0
                            last_vcon_bytes = after
                            last_ticks = ticks
                            time.sleep(interval)
                            continue
                        slog("  [monitor] no answer to the poke")
                    slog(f"  [monitor] ❄ HANG detected (timer alive; console, "
                         f"disk I/O and a console poke all produced nothing "
                         f"for {frozen_count} samples)")
                    # grab a final guest-reg snapshot and backtrace
                    gr = nc.cmd("gr", 2)
                    bt = nc.cmd("bt", 2)
                    sess.raw(f"\n# [HANG — guest regs]\n{gr}\n".encode())
                    slog(f"  [monitor] final gr:\n{gr}")
                    sess.raw(f"\n# [HANG — backtrace]\n{bt}\n".encode())
                    slog(f"  [monitor] backtrace:\n{bt}")
                    
                    # read VGIC breadcrumbs from SRAM C
                    vgbc = nc.cmd(f"r 0x{B.VGIC_BC_SRAM_PA:x} 0x10", 2)
                    slog(f"  [monitor] VGIC BC:\n{vgbc}")
                    
                    if boot_ok:
                        return "BOOT_OK_THEN_HANG"
                    return "HANG"
            else:
                frozen_count = 0

        if boot_ok and not vcon_growing and frozen_count == 0:
            # booted OK and stable — keep monitoring but don't reset
            pass

        last_ticks = ticks
        last_vcon_bytes = vcon_bytes

        # wait for next sample
        elapsed = time.time() - t0
        sleep = max(1, interval - (time.time() - (t0 + (sample - 1) * interval)))
        time.sleep(sleep)

    slog("  [monitor] monitor timeout reached")
    return "TIMEOUT" if not boot_ok else "BOOT_OK_TIMEOUT"


# ── main loop ───────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="bzdOS Chimp board supervisor daemon")
    ap.add_argument("--once", action="store_true", help="single boot cycle, then exit")
    ap.add_argument("--no-reset", action="store_true", help="monitor only, never WDT-reset")
    ap.add_argument("--monitor-only", action="store_true",
                    help="board already running, skip load, just poll")
    ap.add_argument("--interval", type=float, default=10, help="poll interval seconds (default 10)")
    ap.add_argument("--hang-samples", type=int, default=4,
                    help="frozen samples before hang verdict (default 4)")
    args = ap.parse_args()

    slog(f"=== chimpd старт (once={args.once} no-reset={args.no_reset} "
         f"monitor-only={args.monitor_only} interval={args.interval}s "
         f"hang-samples={args.hang_samples}) ===")
    slog(f"    HYP_ELF={HYP_ELF}")
    slog(f"    kernel={KERNEL}  dtb={DTB}")

    # graceful shutdown
    shutting_down = [False]
    def sigterm(sig, frame):
        slog("SIGTERM — shutting down after current cycle")
        shutting_down[0] = True
    signal.signal(signal.SIGTERM, sigterm)
    signal.signal(signal.SIGINT, sigterm)

    n = 0
    while not shutting_down[0]:
        n += 1
        ts = time.strftime("%Y%m%d %H:%M:%S")
        slog(f"━━━ цикл #{n} ━━━ {ts}")

        sess = Session(n)

        if not args.monitor_only:
            ok = serial_load(sess)
            if not ok:
                slog(f"  загрузка не удалась — ждём следующего передёрга платы")
                sess.close()
                if args.once: break
                # wait for board to disappear then reappear
                while os.path.exists(L.TTY) and not shutting_down[0]:
                    time.sleep(0.5)
                continue

        # give the hypervisor a moment to bring EMAC up
        slog("  [monitor] ждём EMAC консоль (5s)…")
        time.sleep(5)

        nc = None
        try:
            nc = NetCon()
            if not nc.is_alive(2.0):
                slog("  [monitor] ⛔ EMAC не отвечает — hypervisor не встал?")
                sess.raw(b"\n# [EMAC no response]\n")
                verdict = "NO_EMAC"
            else:
                slog("  [monitor] ✅ EMAC живой")
                verdict = monitor(sess, nc, args.interval, args.hang_samples, args.no_reset)
        except Exception as e:
            slog(f"  [monitor] exception: {e}")
            verdict = f"ERROR:{e}"
        finally:
            if nc: nc.close()

        slog(f"  ВЕРДИКТ: {verdict}")
        sess.raw(f"\n# VERDICT: {verdict}\n".encode())
        sess.close()

        if args.once:
            break

        # auto-reset on hang/panic (unless --no-reset)
        # BOOT_OK_THEN_HANG deliberately NOT here. It used to be, which made the
        # `elif` below -- whose own message reads "плата дошла до boot —
        # оставляем живой" -- unreachable, so the stated intent never ran. The
        # consequence was that a guest which booted all the way to `login:` and
        # then went quiet (exactly what an idle system does: no console output,
        # no disk I/O) got WDT-reset anyway, and the board could never stay up
        # under supervision. Reaching a boot marker and then falling silent is
        # success, not a hang.
        if verdict in ("HANG", "PANIC", "FIRSTFAULT") and not args.no_reset:
            if nc:
                try:
                    nc2 = NetCon()
                    nc2.wdt_reset()
                    nc2.close()
                    slog("  ждём reset платы…")
                except Exception as e:
                    slog(f"  WDT reset failed: {e} — нужен ручной передёрг")
            # wait for board to disappear (reset) then loop
            t0 = time.time()
            while os.path.exists(L.TTY) and time.time() - t0 < 30 and not shutting_down[0]:
                time.sleep(0.5)
        elif verdict in ("BOOT_OK_TIMEOUT", "BOOT_OK_THEN_HANG"):
            slog("  плата дошла до boot — оставляем живой, продолжаем мониторинг")
        else:
            slog("  ждём следующего передёрга платы…")
            while os.path.exists(L.TTY) and not shutting_down[0]:
                time.sleep(1)
            while not os.path.exists(L.TTY) and not shutting_down[0]:
                time.sleep(1)

    slog("=== chimpd остановлен ===")


if __name__ == "__main__":
    main()
