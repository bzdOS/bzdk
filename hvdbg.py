#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
hvdbg.py — reusable host-side library for the bzdOS EL2 hypervisor live
debugger.  Wraps the raw-Ethernet EMAC console protocol (ethertype 0x88B5)
into a clean Python API: send commands, read breadcrumbs, compare memory,
walk page tables, restart the guest — all without a board reset.

Usage:
    from hvdbg import HV
    hv = HV()
    hv.cmd('gr')                     # guest registers
    hv.cmd('sr2')                    # EL2 sysregs
    hv.read_words(0x50000800, 8)     # GICT breadcrumb
    hv.dump(0x46abe252, 48)          # hex+ascii dump
    hv.cmp_elf_dram(                 # compare ELF source vs DRAM
        'microkernel-dbg.elf',       # hypervisor ELF (for symbols)
        '/opt/bzdos/tftpboot/kernel',# guest kernel ELF
        0x46000000, 0x44abe252, 0x46abe252, 48)
    hv.reenter_guest()               # restart guest without board reset

DBG_AUTH (ROADMAP T5, optional — docs/security-notes.md option 1): if the
board was built with -DDBG_AUTH (see Makefile/emac.c's dbg_auth_check()), it
only acts on console frames carrying a valid HMAC-SHA256 envelope. Pass the
same key here to talk to such a board:

    hv = HV(key="a1b2c3...")         # hex-encoded key, same bytes as the
                                      # board's -DDBG_AUTH_KEY="..."
    hv.cmd('gr')                     # frames are now signed automatically

Passing NO key (the default) sends byte-for-byte the same unauthenticated
frames as always -- nothing about this class's default behavior changed.
Only pass `key` when talking to a board actually built with -DDBG_AUTH and
that board's own -DDBG_AUTH_KEY; a default (no-flag) board has no envelope
parser at all, so signed frames sent to it would just show up as a command
line starting with 40 bytes of binary junk (harmless -- console_getc()'s
own zero-terminated-line convention still applies, it'd just be gibberish).
"""
import os, sys, socket, struct, time, re, subprocess, hmac, hashlib
import bzd_board as B

IFACE     = B.IFACE
ETYPE     = B.ETYPE
BOARD_MAC = B.BOARD_MAC
BCAST     = B.BCAST
CSI       = re.compile(rb'\x1b\[[0-9;?]*[a-zA-Z]')

# DBG_AUTH wire envelope (must match emac.c's dbg_auth_check() exactly):
#   [0:8]   nonce, 8 bytes big-endian uint64, strictly increasing
#   [8:40]  mac,   32 bytes HMAC-SHA256(key, nonce || cmd)
#   [40:]   cmd,   the command bytes, unchanged from the unauthenticated wire
DBG_AUTH_NONCE_LEN = 8
DBG_AUTH_MAC_LEN   = 32

class HV:
    """Connection to the running hypervisor debug monitor over EMAC."""
    def __init__(self, iface=IFACE, timeout=3, key=None):
        """`key`: optional hex string. When None (the default), behavior is
        UNCHANGED from before DBG_AUTH existed -- plain unauthenticated
        frames. When set, every outgoing frame is signed per the DBG_AUTH
        envelope above; only meaningful against a board built with
        -DDBG_AUTH and the SAME key (see emac.c/Makefile)."""
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                  socket.htons(ETYPE))
        self.sock.bind((iface, ETYPE))
        self.src_mac = self.sock.getsockname()[4]
        self.iface = iface
        self._dbgraw_sock = None  # lazy: separate AF_PACKET socket for
                                   # DBGRAW_ETHERTYPE (see dbgtools_peek()) --
                                   # self.sock is bound to ETYPE at the kernel
                                   # level (socket.htons(ETYPE) at creation),
                                   # so it NEVER receives frames of any other
                                   # ethertype no matter what's on the wire.
        self.timeout = timeout
        self._auth_key = bytes.fromhex(key) if key else None
        # Any strictly-increasing start point works (the board's own replay
        # counter resets to 0 at boot); seed from the wall clock so re-
        # running this script never re-uses a nonce from an earlier run.
        self._nonce = time.time_ns()
        self._drain(0.3)

    def close(self):
        try: self.sock.close()
        except: pass

    # ── low-level ──────────────────────────────────────────────────────
    def _sign(self, cmd_bytes):
        """Build the DBG_AUTH envelope for `cmd_bytes` (see module
        docstring). Only called when self._auth_key is set."""
        self._nonce += 1
        nonce = self._nonce.to_bytes(DBG_AUTH_NONCE_LEN, "big")
        mac = hmac.new(self._auth_key, nonce + cmd_bytes, hashlib.sha256).digest()
        assert len(mac) == DBG_AUTH_MAC_LEN
        return nonce + mac + cmd_bytes

    def _send(self, text):
        p = (text + "\r").encode("latin1", "replace")
        if self._auth_key is not None:
            p = self._sign(p)
        if len(p) < 46:
            p += b"\x00" * (46 - len(p))
        self.sock.send(BCAST + self.src_mac + struct.pack("!H", ETYPE) + p)

    def _drain(self, secs, early_prompt=b"dbg>"):
        """Collect board replies for up to `secs` seconds. Exits EARLY when
        the dbgmon prompt (`dbg> `) is seen — this turns a 3s fixed wait
        into a ~20ms round-trip. The prompt is appended by dbgmon after
        every command completes."""
        buf = b""
        t0 = time.time()
        self.sock.settimeout(0.02)
        while time.time() - t0 < secs:
            try:
                f = self.sock.recv(2048)
                if len(f) > 14 and struct.unpack("!H", f[12:14])[0] == ETYPE \
                   and f[6:12] == BOARD_MAC:
                    buf += f[14:].split(b"\x00")[0]
                    if early_prompt and early_prompt in buf:
                        break   # command complete — don't wait full timeout
            except (socket.timeout, OSError):
                pass
        return buf

    # ── public API ─────────────────────────────────────────────────────
    def cmd(self, command, wait=None):
        """Send a dbgmon command, return the text reply."""
        if wait is None: wait = self.timeout
        self._drain(0.05)  # quick flush (was 0.2 — too slow)
        self._send(command)
        return self._drain(wait).decode("latin1", "replace")

    def alive(self, timeout=2):
        """Quick liveness check."""
        self._drain(0.1)
        self._send("t")
        return bool(self._drain(timeout).strip())

    # dbgmon's `r` reply is drained as a stream of EMAC frames; measured
    # live 2026-07-29 on this board: n=4, n=256 and n=512 all complete in
    # ~0.062-0.066s (the per-command round trip dominates completely, the
    # payload is free), but n=1024 falls off a cliff to ~22s -- past this
    # width the reply no longer arrives as one prompt-terminated burst and
    # _drain() sits out its whole timeout. So batch AT this width, never
    # past it: read_words() chunks internally, callers can ask for any n.
    MAX_WORDS_PER_CMD = 512

    def rtt(self, refresh=False):
        """Measured round-trip time of one trivial dbgmon command, cached.

        Everything else here derives its waits from this instead of from a
        hardcoded constant: this channel's RTT spans two orders of
        magnitude depending on board state (~0.06s healthy; ~1.07s per
        command once MUSB's D+/D- pull-up has been dropped, see project
        memory reboot-clean-usb-pullup-drop-slows-cpu1), and a fixed
        `settle` tuned for the slow case wastes 5x the entire exchange in
        the fast one."""
        if refresh or getattr(self, "_rtt", None) is None:
            t0 = time.time()
            self.cmd("r 0x50000c00 1", 2)
            self._rtt = max(0.01, time.time() - t0)
        return self._rtt

    def auto_settle(self):
        """Inter-retry pause for the *_stable/_verified helpers: one RTT.

        A retry only helps once the board has had time to answer the
        previous command, and by construction that takes exactly one RTT --
        there is no separate settling process on the board to wait for. The
        old hardcoded 0.1-0.2s was ~3x the whole round trip on a healthy
        channel (measured: read_words_stable(128) 0.32s at settle=0.2 vs
        0.13s at settle=0), and simultaneously too SHORT to help on a
        pull-up-degraded one."""
        return self.rtt()

    def read_words(self, pa, n):
        """Read n 32-bit words from physical address pa. Returns list of ints.

        Chunked at MAX_WORDS_PER_CMD (see above) so a caller asking for a
        wide span gets many fast commands instead of one that stalls; a
        chunk that comes back incomplete fails the whole read (callers
        already retry, and a partial result would be indistinguishable
        from real data).

        VALIDATES the echoed address of every reply line against the
        requested range. Without this, a LATE reply from a previous command
        (board busy, host already timed out and moved on) gets parsed as the
        answer to the CURRENT read — observed live 2026-07-21: console-ring
        reads returning the vblk breadcrumb window's contents ("VBK1"
        garbage), poisoning hours of diagnostics. Lines whose address falls
        outside [pa, pa+n*4) are stale strays and are dropped; words are
        placed by their echoed address, so replies can arrive out of order
        and the result is still positionally correct (holes = read failed,
        caller retries)."""
        if n > self.MAX_WORDS_PER_CMD:
            out = []
            for off in range(0, n, self.MAX_WORDS_PER_CMD):
                part = self.read_words(pa + 4 * off,
                                       min(self.MAX_WORDS_PER_CMD, n - off))
                if not part:
                    return []        # any bad chunk fails the whole read
                out += part
            return out
        WORDS_PER_LINE = 4   # dbgmon.c cmd_read_words()'s own line width
        # COUNT MUST BE HEX. dbgmon.c parses EVERY numeric argument with
        # parse_hex() -- including this count -- so a decimal-looking "29" was
        # read as 0x29 = 41. That silently over-read for years, and worse, it
        # BROKE the read outright whenever n was not a multiple of 4: the extra
        # words push the last in-range line to a full 4 words while the
        # `expected` check below wants only n-i0 of them, the line is discarded
        # as corrupt, a slot stays None, and the whole read returns [].
        # Measured live 2026-08-04: n=25..27,29..31,33 failed 0/2, n=28 and
        # n=32 passed 2/2. `bmc_client.py health --raw` asks for exactly 29
        # words, so the BMC's read-it-even-mid-wedge path -- the entire reason
        # a software BMC exists -- had never once worked.
        r = self.cmd(f"r 0x{pa:x} 0x{n:x}", max(2, n // 50 + 2))
        slots = [None] * n
        for line in r.split("\n"):
            if ":" not in line: continue
            addr_s, rest = line.split(":", 1)
            try:
                addr = int(addr_s.strip(), 16)   # accepts with/without 0x
            except ValueError:
                continue
            if addr < pa or addr >= pa + 4 * n or (addr - pa) % 4:
                continue                      # stale stray from a prior cmd
            i0 = (addr - pa) // 4
            expected = min(WORDS_PER_LINE, n - i0)
            # This project's console/reply transport has a documented
            # byte-drop characteristic (see project memory) that can eat
            # one character out of an 8-hex-digit token mid-line. The old
            # parser here silently skipped any non-8-char token WITHOUT
            # advancing its slot index, which SHIFTS every later word on
            # that line into the wrong slot instead of just losing one --
            # confirmed live 2026-07-28: a 128-word bulk read disagreed
            # with 128 individual single-word reads at exactly one shifted
            # slot. Parse this line into a scratch list first; only commit
            # it if it produced EXACTLY the expected word count -- a short
            # line (a real drop) is discarded entirely (leaves those slots
            # None) rather than silently misaligning everything after it.
            words = []
            for p in rest.split():
                p = p.strip()
                if len(p) == 8:
                    try:
                        words.append(int(p, 16))
                    except ValueError:
                        pass
            if len(words) != expected:
                continue                      # corrupted line -- drop it, let caller retry
            for k, w in enumerate(words):
                slots[i0 + k] = w
        if any(s is None for s in slots):
            return [s for s in slots if s is not None][:0]  # incomplete -> []
        return slots

    def read_bytes(self, pa, n):
        """Read n bytes from physical address pa. Returns bytes."""
        # Hex count, same dbgmon parse_hex() contract as read_words() above.
        # This one merely wasted bandwidth rather than corrupting: "rb ... 100"
        # fetched 0x100 = 256 bytes and the slice below trimmed it back to 100,
        # so the DATA was right while the transfer was 2.5x larger than asked.
        r = self.cmd(f"rb 0x{pa:x} 0x{n:x}", max(2, n // 100 + 2))
        hexstr = ""
        for line in r.split("\n"):
            if ":" not in line: continue
            hexstr += line.split(":", 1)[1].strip().replace(" ", "")
        return bytes.fromhex(hexstr[:n*2]) if hexstr else b""

    def dump(self, pa, length):
        """Hex+ASCII dump. Returns the ASCII-column text."""
        # Hex length, same contract. Unlike read_bytes() this one has no
        # trimming slice, so a decimal length returned MORE dump text than the
        # caller asked for -- e.g. `d addr 64` dumped 0x64 = 100 bytes.
        r = self.cmd(f"d 0x{pa:x} 0x{length:x}", max(2, length // 60 + 2))
        ascii_text = b""
        for line in r.split("\n"):
            if "|" not in line: continue
            part = line.split("|", 1)[1]
            if "|" in part:
                part = part.split("|")[0]
            ascii_text += part.encode("latin1", "replace")
        return ascii_text

    def write_word(self, pa, val):
        """Write a 32-bit word to physical memory."""
        return self.cmd(f"w 0x{pa:x} 0x{val:x}", 1)

    def read_words_stable(self, pa, n, tries=8, settle=None):
        """Like read_words(), but for STATIC memory (a disk block staged in
        scratch RAM, a value nothing else is touching) where the caller
        wants confidence the transport didn't hand back a subtly-wrong-but-
        well-formed reply. Confirmed live 2026-07-28: even past
        read_words()'s own per-line word-count validation, a single hex
        digit inside an otherwise well-formed 8-char token can still come
        back wrong (two back-to-back reads of unchanged memory disagreeing
        at one word, same shape, different value -- nothing structurally
        invalid to catch). Retries until two CONSECUTIVE reads agree
        exactly, on the theory that the same transport glitch is unlikely
        to reproduce identically twice in a row.

        Do NOT use this for live-changing data (breadcrumb counters,
        heartbeats, anything another core is actively updating) -- it will
        never see two consecutive reads agree and will just burn through
        `tries` and return [] every time. Use plain read_words() for that;
        this method is specifically for the "this shouldn't be changing"
        case, same channel-independent guarantee either way (works
        identically whether the underlying debug link is EMAC or, on a
        board built with a different transport, anything else — the
        agreement check only depends on parsed reply content, not on how
        the bytes got here).

        `settle=None` (the default) derives the inter-retry pause from the
        measured channel RTT -- see auto_settle(). Pass an explicit value
        only to override that deliberately."""
        if settle is None:
            settle = self.auto_settle()
        # Stabilize PER COMMAND-SIZED CHUNK, not across the whole span.
        # read_words() happily chunks a wide read, but comparing the joined
        # result end-to-end makes the agreement test scale badly: one bad
        # hex digit anywhere fails the comparison for every chunk at once,
        # so the chance of any single attempt succeeding falls off with
        # width. Measured live 2026-07-29: n=512 stabilizes in 0.19s, while
        # n=1024 compared as one span burned all 8 tries and returned
        # nothing (12.3s). Per-chunk, cost is linear in width and a glitch
        # only re-reads its own 2 KiB.
        if n > self.MAX_WORDS_PER_CMD:
            out = []
            for off in range(0, n, self.MAX_WORDS_PER_CMD):
                part = self.read_words_stable(
                    pa + 4 * off, min(self.MAX_WORDS_PER_CMD, n - off),
                    tries=tries, settle=settle)
                if not part:
                    return []
                out += part
            return out
        prev = None
        for _ in range(tries):
            cur = self.read_words(pa, n)
            if cur and cur == prev:
                return cur
            prev = cur
            time.sleep(settle)
        return []

    def write_word_verified(self, pa, val, tries=6, settle=None):
        """write_word() plus confirmation via a stable read. Prefer this
        over bare write_word() whenever the caller actually depends on the
        value landing (staging a buffer before a hardware operation reads
        it, etc.) rather than firing-and-forgetting a register poke.

        `settle=None` derives the pause from the measured RTT, as in
        read_words_stable()."""
        if settle is None:
            settle = self.auto_settle()
        for _ in range(tries):
            self.write_word(pa, val)
            back = self.read_words_stable(pa, 1, tries=2, settle=settle)
            if back and back[0] == val:
                return True
            time.sleep(settle)
        return False

    def patch(self, pa, insn_word):
        """Hot-patch an instruction (write + I-cache flush)."""
        return self.cmd(f"patch 0x{pa:x} 0x{insn_word:x}", 2)

    def call(self, fn_pa, a0=0, a1=0, a2=0, a3=0):
        """Call a hypervisor function by PA. Returns x0 (uint64)."""
        r = self.cmd(f"call 0x{fn_pa:x} 0x{a0:x} 0x{a1:x} 0x{a2:x} 0x{a3:x}", 5)
        m = re.search(r'ret = 0x([0-9a-f]+)', r)
        if m:
            v = int(m.group(1), 16)
            if v == 0xDEAD:
                return None  # callee crashed
            return v
        return None

    def _reboot_clean_addr(self):
        """Look up reboot_clean's PA from the loaded hypervisor ELF (shifts each
        rebuild, so read it dynamically). Returns None if unavailable."""
        import os, subprocess
        elf = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "microkernel-dbg.elf")
        try:
            out = subprocess.check_output(
                ["aarch64-linux-gnu-nm", elf], stderr=subprocess.DEVNULL).decode()
            for line in out.splitlines():
                p = line.split()
                if len(p) == 3 and p[2] == "reboot_clean":
                    return int(p[0], 16)
        except Exception:
            pass
        return None

    def wdt_reset(self, prefer_fast=False):
        """Force a CLEAN board reset via the A64 watchdog.

        CRITICAL: the reset MUST first drop the MUSB D+/D- pull-up so the host
        sees a clean USB disconnect BEFORE the reset. Otherwise the gadget's
        pull-up stays asserted through the reset window, U-Boot's musb re-inits
        against a host that still thinks a device is attached, and the console
        gadget wedges (host 'device not accepting address, error -71') with NO
        remote recovery path left (bit us twice on 2026-07-19, see project
        memory wdt-reset-needs-usb-disconnect.md). Both paths below honor
        this — only the MECHANISM differs.

        DEFAULT path (address-INDEPENDENT, always tried unless prefer_fast):
        poke the MMIO directly (MUSB pull-up drop, then the WDOG arm
        sequence), retrying the ISCR/POWER reads until they succeed so the
        disconnect is NEVER skipped (the old bug: a failed read skipped the
        disconnect -> wedge). This works regardless of which build is
        actually running on the board right now.

        OPTIONAL fast path (prefer_fast=True): resolve reboot_clean()'s
        address from the microkernel-dbg.elf CURRENTLY ON DISK (via `nm`) and
        call it directly — the hypervisor does disconnect + WDOG arm + spin
        atomically on-board, with zero dependence on flaky EMAC reads.
        hv.call() will time out (reboot_clean never returns — the WDOG resets
        the SoC), which is expected/fine.

        WHY THE DEFAULT CHANGED (2026-07-26): the fast path silently
        does-nothing whenever the ELF on disk was rebuilt more recently than
        the board's last cycle — nm resolves an address for a DIFFERENT
        binary than the one actually running, hv.call() jumps to garbage in
        the running image, and nothing indicates failure (no exception, no
        reset). This cost significant live-debugging time chasing a
        "wdt_reset did nothing" mystery this session. The MMIO fallback has
        no such build-vs-running-binary skew — it addresses fixed hardware
        registers, so it's now the default; the symbol-resolved call is only
        used when the caller explicitly opts in via prefer_fast=True (e.g.
        interactive use where the caller knows the board is already running
        exactly what's on disk).

        BUILD-ID GUARD (2026-07-26, dbgtools.h): even with prefer_fast=True,
        this now checks check_build_id() first. On a confirmed MISMATCH
        (both sides read successfully and differ) it logs a warning and
        falls through to the address-independent MMIO path below instead of
        trusting the nm-resolved address -- this is the exact failure mode
        that motivated making the MMIO path the default in the first place,
        now caught explicitly instead of silently doing nothing. An
        inconclusive check (either side unreadable) does NOT block the fast
        path -- it proceeds exactly as before, since "can't prove they
        differ" isn't the same as "known safe" but the fast path was already
        opt-in for callers who accept that risk."""
        if prefer_fast:
            match, expected, actual = self.check_build_id()
            if match is False:
                print(f"wdt_reset: build-id MISMATCH (expected {expected!r} "
                      f"from this checkout, board reports {actual!r}) -- "
                      f"nm-resolved reboot_clean() address would be for the "
                      f"WRONG binary; skipping the fast path, using the "
                      f"address-independent MMIO fallback instead",
                      file=sys.stderr)
            else:
                addr = self._reboot_clean_addr()
                if addr is not None:
                    try:
                        self.call(addr)          # atomic clean reset on-board; won't return
                    except Exception:
                        pass
                    return
                # fall through to the MMIO path below if the symbol wasn't found
        # ---- default: MMIO with retried reads (never skip the disconnect) ----
        MUSB = B.MUSB_BASE
        def _read1(pa):
            for _ in range(20):
                w = self.read_words(pa, 1)
                if w:
                    return w[0]
                time.sleep(0.15)
            return None
        iscr = _read1(MUSB + 0x400)
        if iscr is not None:
            v = iscr & ~((1 << 4) | (1 << 5) | (1 << 6))  # don't ack change-detect
            self.write_word(MUSB + 0x400, v & ~(1 << 16))  # clear pull-up enable
        poww = _read1(MUSB + 0x40)
        if poww is not None:
            self.write_word(MUSB + 0x40, poww & ~0x40)      # clear SOFTCONN (bit6)
        for pa, val in B.WDOG_ARM_SEQUENCE:
            self.write_word(pa, val)

    # ── dbgtools: CPU1 heartbeat / build-id / entry-hold (2026-07-26) ───
    # See dbgtools.h/hv_addrmap.h (HVMAP_DBGTOOLS_*) for the firmware side.
    DBGRAW_ETHERTYPE = 0x88B7
    DBGRAW_MAGIC     = b"DBGT"
    DBGRAW_REPLY_LEN = 4 + 4 + 4 + 4 + 32   # magic, heartbeat, hold, release, build_id

    def dbgtools_peek(self, tries=3, wait=0.3):
        """RAW, protocol-independent read of {heartbeat, hold, release,
        build_id}. Serviced directly inside the board's emac_poll() RX demux
        (ETHERTYPE_DBGRAW, see emac.c/dbgtools.c) -- NEITHER dbgmon_service()
        NOR gdbstub_poll()'s command dispatch is involved, so this keeps
        answering even while CPU1 is deep inside gdbstub's command_loop()
        servicing an active RSP session (command_loop still calls
        gdb_getc()->emac_poll() every iteration to receive further packets).

        HONEST LIMITATION: this is NOT lower than software polling -- there
        is no interrupt-driven RX in this design. If CPU1 has stopped
        calling emac_poll() at all (a genuine crash / hard hang, as opposed
        to "busy answering a different protocol"), this gets no reply
        either, same as everything else. That IS still useful information:
          - reply arrives, heartbeat climbing on repeated calls -> CPU1 is
            fully healthy; whatever seems "unresponsive" is a higher-level
            RSP/dbgmon state issue, not CPU1 itself.
          - reply arrives, heartbeat FROZEN across repeated calls -> CPU1 is
            stuck inside one iteration (e.g. gdbstub's command_loop()) but
            still pumping emac_poll() -- busy, not dead.
          - no reply after `tries` attempts -> CPU1 has genuinely stopped
            calling emac_poll() -- the one case this cannot see through.

        Returns {} if no reply arrived in `tries` attempts, else a dict with
        heartbeat/hold/release (ints) and build_id (str)."""
        # self.sock is bound to socket.htons(ETYPE) (the console ethertype)
        # AT THE KERNEL LEVEL (AF_PACKET protocol arg + bind()) -- it will
        # NEVER receive a DBGRAW_ETHERTYPE reply no matter what dbgtools.c
        # actually sends (confirmed live: tcpdump saw the board's DBGRAW
        # reply on the wire, heartbeat climbing between frames, but
        # self.sock.recv() never delivered it -- a kernel-level protocol
        # filter, not a parsing bug). Needs its OWN raw socket.
        if self._dbgraw_sock is None:
            s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                               socket.htons(self.DBGRAW_ETHERTYPE))
            s.bind((self.iface, self.DBGRAW_ETHERTYPE))
            self._dbgraw_sock = s
        sock = self._dbgraw_sock
        frame = (BCAST + self.src_mac +
                 struct.pack("!H", self.DBGRAW_ETHERTYPE) +
                 b"\x00" * 46)
        for _ in range(tries):
            sock.settimeout(0.02)
            try:
                while True:
                    sock.recv(2048)
            except (socket.timeout, OSError):
                pass
            sock.send(frame)
            sock.settimeout(wait)
            t0 = time.time()
            while time.time() - t0 < wait:
                try:
                    f = sock.recv(2048)
                except (socket.timeout, OSError):
                    break
                if len(f) < 14 + self.DBGRAW_REPLY_LEN:
                    continue
                if struct.unpack("!H", f[12:14])[0] != self.DBGRAW_ETHERTYPE:
                    continue
                if f[6:12] != BOARD_MAC:
                    continue
                pl = f[14:14 + self.DBGRAW_REPLY_LEN]
                if pl[0:4] != self.DBGRAW_MAGIC:
                    continue
                hb, hold, rel = struct.unpack("<III", pl[4:16])
                build_id = pl[16:48].split(b"\x00", 1)[0].decode(
                    "latin1", "replace")
                return dict(heartbeat=hb, hold=hold, release=rel,
                            build_id=build_id)
        return {}

    def heartbeat(self, tries=3, wait=0.3):
        """Convenience wrapper: just the CPU1 debug-loop heartbeat counter,
        or None if unreachable (see dbgtools_peek()'s doc for what that
        does/doesn't tell you). Call twice a beat apart and compare: if it
        climbed, CPU1 is genuinely spinning; if it's the identical nonzero
        number both times, CPU1 is alive-but-stuck in one iteration."""
        r = self.dbgtools_peek(tries=tries, wait=wait)
        return r.get('heartbeat') if r else None

    def _build_id_from_elf(self, elf="microkernel-dbg.elf"):
        """Read the build-id string actually COMPILED INTO the ELF on disk, by
        locating dbgtools.c's g_build_id_str and mapping its vaddr to a file
        offset through the section table.

        This, not git, is the right source of truth: callers use this check to
        decide whether symbol addresses resolved from THIS ELF describe the
        running image, and those addresses come from the ELF's own contents.
        Deriving the expectation from `git rev-parse HEAD` instead (what this
        did until 2026-07-30) reports a bogus skew after ANY commit that did
        not rebuild the ELF -- including a commit touching only host-side
        Python, which cannot affect the hypervisor binary at all. That
        false positive blocked emmc_raw.py mid-investigation.

        Returns None if anything is unavailable, meaning "can't compare".
        """
        d = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(d, elf)
        if not os.path.exists(path):
            return None
        try:
            nm = subprocess.check_output(
                ["aarch64-linux-gnu-nm", path],
                stderr=subprocess.DEVNULL).decode(errors="replace")
            addr = None
            for line in nm.splitlines():
                p = line.split()
                if len(p) == 3 and p[2] == "g_build_id_str":
                    addr = int(p[0], 16)
                    break
            if addr is None:
                return None
            secs = subprocess.check_output(
                ["aarch64-linux-gnu-readelf", "-S", "-W", path],
                stderr=subprocess.DEVNULL).decode(errors="replace")
            for line in secs.splitlines():
                # "  [ 2] .rodata  PROGBITS  <vaddr> <off> <size> ..."
                m = re.search(r"\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+"
                              r"([0-9a-f]+)\s+([0-9a-f]+)", line)
                if not m or m.group(2) == "NOBITS":
                    continue
                sa, so, sz = (int(m.group(3), 16), int(m.group(4), 16),
                              int(m.group(5), 16))
                if sz and sa <= addr < sa + sz:
                    with open(path, "rb") as fh:
                        fh.seek(addr - sa + so)
                        raw = fh.read(32)
                    return raw.split(b"\x00", 1)[0].decode("latin1")
        except Exception:
            return None
        return None

    def _expected_build_id(self):
        """The build-id the ELF on disk carries, falling back to recomputing it
        the way the Makefile would (`git rev-parse --short=12 HEAD` plus '+'
        when the tree is dirty) only when the ELF cannot be read -- e.g. a
        checkout with no build in it yet. None means "can't compare, don't
        block on it"; the Makefile's date-stamp fallback is not reproducible
        after the fact and is not attempted."""
        from_elf = self._build_id_from_elf()
        if from_elf:
            return from_elf
        cwd = os.path.dirname(os.path.abspath(__file__))
        try:
            rev = subprocess.check_output(
                ["git", "rev-parse", "--short=12", "HEAD"],
                cwd=cwd, stderr=subprocess.DEVNULL).decode().strip()
        except Exception:
            return None
        dirty = subprocess.call(
            ["git", "diff", "--quiet"], cwd=cwd,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0
        return rev + ("+" if dirty else "")

    def check_build_id(self):
        """Compare the LIVE board's build-id (read via the normal dbgmon `r`
        command -- this check assumes dbgmon is at least somewhat responsive,
        unlike dbgtools_peek() above which assumes nothing) against what this
        checkout's git state would have produced. Returns
        (match: bool or None, expected, actual) -- match is None if either
        side was unreadable (can't compare, caller should not treat that as
        a mismatch)."""
        expected = self._expected_build_id()
        w = self.read_bytes(0x50021010, 32)   # HVMAP_DBGTOOLS_BUILDID
        actual = w.split(b"\x00", 1)[0].decode("latin1", "replace") if w else None
        if expected is None or not actual:
            return (None, expected, actual)
        return (expected == actual, expected, actual)

    def hold_guest_before_entry(self):
        """Arm the pause-before-guest-entry gate (main_dbg.c/main_gdb.c,
        checked immediately before kload_enter()) via dbgmon's `hold` verb.

        IMPORTANT, NOT SYMMETRIC WITH A COLD LOAD: this word lives in the
        hv-scratch DRAM window, which a WARM reset (wdt_reset()) preserves
        but a fresh COLD TFTP load does NOT reliably start from zero either
        way -- dbgtools_init() (dbgtools.c) explicitly forces it OFF on
        every cold boot precisely so a stray bit pattern can never wedge a
        normal chimpd cycle. That means calling this method only ever takes
        effect on the NEXT WARM reset (self.wdt_reset()) -- it does NOT
        arm anything for a fresh cold TFTP reload (e.g. reliable_load.py /
        chimpd's normal cycle), because nothing runs early enough in a cold
        boot to have read this word before dbgtools_init() clears it. Call
        this, THEN self.wdt_reset(), in that order, on an ALREADY-RUNNING
        board -- never expect it to survive a full power-cycle or a fresh
        TFTP flash."""
        return self.cmd("hold", 2)

    def release_guest_entry(self):
        """Release a currently-held boot (see hold_guest_before_entry()).
        No-op if nothing is currently held."""
        return self.cmd("release", 2)

    # ── breadcrumb shortcuts ───────────────────────────────────────────
    def gict(self):
        """Read the GICT timer breadcrumb. Returns dict."""
        w = self.read_words(B.GICT_BC_DRAM_PA, 8)
        if len(w) < 8: return {}
        return dict(
            magic=w[0], ticks_lo=w[1], ticks_hi=w[2],
            period=w[4], init_done=w[6], ctl_live=w[7])

    def vconsole(self):
        """Read the vconsole ring header. Returns dict.

        From layout v2 the header is SELF-DESCRIBING: word[3]=version,
        word[4]=buffer base, word[5]=size. Trust those over B.VCONSOLE_RING_*,
        which are only fallbacks for a pre-v2 board. The whole point: this file
        used to read a hardcoded 4 KiB at a hardcoded address while the firmware
        wrote 64 KiB somewhere else, and neither side could notice."""
        w = self.read_words(B.VCONSOLE_HDR_PA, 8)
        if not w or len(w) < 4:
            return {}
        d = dict(magic=w[0], total_bytes=w[1], fault_count=w[2])
        ver = w[3] if len(w) >= 4 else 0
        # A pre-v2 firmware wrote 0 here; a plausible version is small.
        if 1 <= ver <= 64 and len(w) >= 6 and w[4]:
            d.update(layout=ver, buf_base=w[4], buf_size=w[5])
        else:
            d.update(layout=0, buf_base=B.VCONSOLE_RING_PA,
                     buf_size=B.VCONSOLE_RING_SZ)
        return d

    def vconsole_text(self, length=None):
        """Dump and return the vconsole ring data as text."""
        hdr = self.vconsole()
        n = hdr.get('total_bytes', 0)
        if n == 0: return b""
        if length: n = min(n, length)
        # Size from the header, so a firmware/host size drift can't silently
        # truncate the read or corrupt wrap arithmetic.
        return self.dump(hdr['buf_base'], min(n, hdr['buf_size']))

    # ── flight recorder (flightrec.c/.h, ROADMAP B4) ────────────────────
    # Generic (kind, a0, a1) event ring at 0x50012000, magic "FLTR". See
    # flightrec.h for the full wire format -- this just mirrors it in
    # Python so a crash can be read back as a readable timeline instead of
    # a raw memory blob. No existing reader touched this ring before B4
    # (grepped "FLTR"/"flightrec" across every *.py in the tree: only the
    # source files themselves matched).
    FLTR_BASE       = B.FLTR_BASE
    FLTR_MAGIC      = 0x464C5452       # "FLTR"
    FLTR_HDR_WORDS  = 8
    FLTR_SLOT_WORDS = 5                # kind, a0 lo/hi, a1 lo/hi
    FLTR_KIND_NAMES = {
        1: "FAULT",    # el2_exc.c fault path: a0=ESR a1=ELR
        2: "TRAP",     # el2_exc.c PSCI SMC intercept: a0=fnid a1=ELR
        3: "IRQ",      # vgic.c vgic_inject()/vgic_inject_hw(): a0=vintid a1=LR word
        4: "VIRTIO",   # vblk_emmc.c vblk_kick (a0=qidx a1=served) or
                       # vnet_emac.c QueueNotify (a0=reg off a1=notified val)
        5: "CONSOLE",  # vconsole.c: a0=byte a1=direction (0=TX guest->host, 1=RX host->guest)
        6: "TIMER",    # gic_timer.c: a0=CNTV_CTL, a1=signed(CNTV_CVAL-CNTVCT)
        7: "SYNC",     # el2_exc.c: a0=(EC<<32)|ESR, a1=ELR (guest sync trap)
        8: "HVVIOL",   # el2_exc.c A1: guest->HV stage-2 abort blocked; a0=IPA a1=ELR
    }

    def flightrec(self):
        """Read the FLTR ring header. Returns {} if unreadable or the magic
        doesn't match (ring not yet initialized / stale read)."""
        w = self.read_words(self.FLTR_BASE, self.FLTR_HDR_WORDS)
        if len(w) < self.FLTR_HDR_WORDS or w[0] != self.FLTR_MAGIC:
            return {}
        return dict(magic=w[0], total=w[1], head=w[2], capacity=w[3],
                     slot_words=w[4])

    def flightrec_entries(self, last=256):
        """Read up to `last` most-recent FLTR ring entries, OLDEST FIRST.
        Each entry is {kind, kind_name, a0, a1}. Returns [] if the ring
        looks uninitialized/unreadable.

        `last=None` reads the ENTIRE ring (up to `capacity` entries, ~40 KiB
        / ~10K words over the EMAC r-command link) -- slow; only pass this
        if the last few hundred events genuinely aren't enough context.
        Default 256 is picked to be a quick, still-plenty-of-context read
        after a crash (each event is one trap/IRQ/virtio-op/console-byte,
        so 256 of them is usually many seconds of guest activity)."""
        hdr = self.flightrec()
        if not hdr:
            return []
        cap, total, head, sw = (hdr['capacity'], hdr['total'], hdr['head'],
                                 hdr['slot_words'])
        if not cap or not sw:
            return []
        valid = min(total, cap)
        n = valid if last is None else min(last, valid)
        if n == 0:
            return []
        # head is the next slot to be WRITTEN, i.e. the oldest surviving
        # entry once the ring has wrapped (total > cap). Walk back `n`
        # slots from head to find where this read should start.
        start = (head - n) % cap
        idxs = [(start + i) % cap for i in range(n)]

        entries = []
        i = 0
        while i < n:
            j = i
            while j + 1 < n and idxs[j + 1] == (idxs[j] + 1) % cap:
                j += 1                       # extend this contiguous run
            run_len = j - i + 1
            base_pa = (self.FLTR_BASE +
                       (self.FLTR_HDR_WORDS + idxs[i] * sw) * 4)
            words = self.read_words(base_pa, run_len * sw)
            for k in range(run_len):
                off = k * sw
                if off + sw > len(words):
                    break                     # short read (stale/dropped reply)
                kind = words[off]
                a0 = words[off + 1] | (words[off + 2] << 32)
                a1 = words[off + 3] | (words[off + 4] << 32)
                entries.append(dict(
                    kind=kind,
                    kind_name=self.FLTR_KIND_NAMES.get(kind, "K%d" % kind),
                    a0=a0, a1=a1))
            i = j + 1
        return entries

    def flightrec_text(self, last=256):
        """Human-readable crash timeline: one line per flight-recorder
        event, oldest first, so it reads top-to-bottom leading up to
        whatever's most recent (typically the crash/reset)."""
        entries = self.flightrec_entries(last=last)
        if not entries:
            return "(flightrec ring empty/unreadable -- bad magic or no reply)"
        lines = []
        for e in entries:
            k, a0, a1 = e['kind'], e['a0'], e['a1']
            if k == 1:
                lines.append("FAULT   esr=0x%x elr=0x%x" % (a0, a1))
            elif k == 2:
                lines.append("TRAP    psci-smc fnid=0x%x elr=0x%x" % (a0, a1))
            elif k == 3:
                lines.append("IRQ     vintid=%d lr=0x%x" % (a0, a1))
            elif k == 4:
                lines.append("VIRTIO  a0=0x%x a1=0x%x "
                              "(vblk: qidx/served; vnet: reg-off/notify-val)"
                              % (a0, a1))
            elif k == 5:
                c = a0 & 0xff
                if c == 10: ch = "\\n"
                elif c == 13: ch = "\\r"
                elif 32 <= c < 127: ch = chr(c)
                else: ch = "."
                direction = "RX host->guest" if a1 else "TX guest->host"
                lines.append("CONSOLE %-15s '%s' (0x%02x)" % (direction, ch, c))
            elif k == 6:
                # CNTV sample: a0=CNTV_CTL, a1=signed(CNTV_CVAL-CNTVCT)
                ctl = a0 & 0x7
                flags = "".join([
                    "E" if ctl & 1 else "-",   # ENABLE
                    "M" if ctl & 2 else "-",   # IMASK
                    "S" if ctl & 4 else "-",   # ISTATUS (firing)
                ])
                dl = a1 - (1 << 64) if a1 >= (1 << 63) else a1
                lines.append("TIMER   cntv_ctl=%s(0x%x) cval-now=%+d %s"
                              % (flags, ctl, dl,
                                 "[past/firing]" if dl < 0 else "[future/re-armed]"))
            elif k == 7:
                # guest sync trap: a0=(EC<<32)|ESR, a1=ELR
                ec = (a0 >> 32) & 0x3f
                esr = a0 & 0xffffffff
                ecname = {0x18: "MSR/MRS(TVM)", 0x24: "DABT-lower",
                          0x16: "HVC", 0x17: "SMC", 0x20: "IABT-lower",
                          0x21: "IABT", 0x22: "PC-align", 0x25: "DABT",
                          0x3c: "BRK"}.get(ec, "EC0x%x" % ec)
                lines.append("SYNC    %-14s esr=0x%x elr=0x%x" % (ecname, esr, a1))
            elif k == 8:
                lines.append("HVVIOL  guest->HV blocked ipa=0x%x elr=0x%x" % (a0, a1))
            else:
                lines.append("KIND=%d  a0=0x%x a1=0x%x" % (k, a0, a1))
        return "\n".join(lines)

    def ffv(self):
        """Read the vector first-fault record. Returns the raw text."""
        return self.cmd("ffv", 4)

    def gr(self):
        """Read guest registers. Returns the raw text."""
        return self.cmd("gr", 4)

    def sr2(self):
        """Read EL2 system registers. Returns dict."""
        r = self.cmd("sr2", 3)
        regs = {}
        for line in r.split("\n"):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                try: regs[k.strip()] = int(v.strip(), 16)
                except: pass
        return regs

    # ── page table walk (respects VTCR.SL0=1) ──────────────────────────
    def walk_stage2(self, ipa):
        """Walk stage-2 for an IPA. Returns the final PA or None."""
        vtcr = self.sr2().get('vtcr_el2', 0)
        vttbr = self.sr2().get('vttbr_el2', 0)
        # SL0=1: start at level 1
        l1_table = vttbr & 0xFFFFFFFFF000  # BADDR
        l1_idx = (ipa >> 30) & 0x1FF
        desc = self.read_words(l1_table + l1_idx * 8, 2)
        if len(desc) < 2: return None
        d = desc[0] | (desc[1] << 32)
        if (d & 3) == 3:  # table → L2
            l2_base = d & 0xFFFFFFFFF000
            l2_idx = (ipa >> 21) & 0x1FF
            d2w = self.read_words(l2_base + l2_idx * 8, 2)
            if len(d2w) < 2: return None
            d2 = d2w[0] | (d2w[1] << 32)
            if (d2 & 3) == 1:  # 2MB block
                return (d2 & 0xFFFFFFE00000) | (ipa & 0x1FFFFF)
            if (d2 & 3) == 3:  # table → L3
                l3_base = d2 & 0xFFFFFFFFF000
                l3_idx = (ipa >> 12) & 0x1FF
                d3w = self.read_words(l3_base + l3_idx * 8, 2)
                if len(d3w) < 2: return None
                d3 = d3w[0] | (d3w[1] << 32)
                if (d3 & 3) == 3:  # 4KB page
                    return (d3 & 0xFFFFFFFFF000) | (ipa & 0xFFF)
        if (d & 3) == 1:  # 1GB block
            return (d & 0xFFFFFFC0000000) | (ipa & 0x3FFFFFFF)
        return None

    # ── guest restart without board reset ──────────────────────────────
    def reenter_guest(self, kload_syms=None):
        """Re-copy kernel segments (fix rodata) and re-enter the guest.
        Requires the hypervisor ELF for symbol addresses. Returns True
        if the guest was re-entered (check hv.alive() after)."""
        if kload_syms is None:
            kload_syms = self._find_syms()
        # Re-parse ELF + re-copy segments
        self.call(kload_syms['kload_parse_elf'], 0x44000000)
        self.call(kload_syms['kload_place_segments'], 0x44000000, 0x46000000)
        # Clear vconsole ring for fresh capture
        self.write_word(0x50000f04, 0)
        self.write_word(0x50000f08, 0)
        # Known-good values (from ELF + main_dbg.c constants)
        entry_pa = 0x46080000    # K_PABASE + (e_entry - kernbase)
        mi = 0xffff000001400000  # kernbase + (MODINFO_PA - pa_base)
        sp_el1 = 0x4c000000
        # Clear dbgmon_call_active (kload_enter is noreturn)
        if 'dbgmon_call_active' in kload_syms:
            self.write_word(kload_syms['dbgmon_call_active'], 0)
        # Re-enter (noreturn — eret to guest)
        self.cmd(f"call 0x{kload_syms['kload_enter']:x} "
                 f"0x{entry_pa:x} 0x{mi:x} 0x{sp_el1:x} 0", 5)
        return True

    def _find_syms(self, elf="microkernel-dbg.elf"):
        """Extract key function addresses from the hypervisor ELF."""
        out = subprocess.check_output(
            ["aarch64-linux-gnu-nm", elf], text=True)
        syms = {}
        wanted = ['kload_parse_elf', 'kload_place_segments',
                  'kload_build_modinfo', 'kload_entry_pa', 'kload_enter',
                  'stage2_init', 'stage2_enable', 'stage2_unmap_guest_vector',
                  'gic_timer_init', 'vconsole_init', 'gtrace_init',
                  'dbgmon_call_active']
        for line in out.split("\n"):
            parts = line.strip().split()
            if len(parts) == 3:
                addr, typ, name = parts
                for w in wanted:
                    if name == w:
                        syms[w] = int(addr, 16)
        return syms

    # ── ELF helpers ────────────────────────────────────────────────────
    @staticmethod
    def kernel_string_pa(kernel_elf, file_offset, kernbase_pa=0x46000000,
                         kernbase_va=0xffff000000000000):
        """Compute the DRAM PA for a string at a given ELF file offset."""
        with open(kernel_elf, 'rb') as f:
            f.seek(0x20); phoff = struct.unpack('<Q', f.read(8))[0]
            f.seek(0x36); phentsize = struct.unpack('<H', f.read(2))[0]
            f.seek(0x38); phnum = struct.unpack('<H', f.read(2))[0]
            for i in range(phnum):
                f.seek(phoff + i * phentsize)
                data = f.read(phentsize)
                p_type = struct.unpack('<I', data[0:4])[0]
                if p_type != 1: continue
                p_off = struct.unpack('<Q', data[8:16])[0]
                p_vaddr = struct.unpack('<Q', data[16:24])[0]
                p_filesz = struct.unpack('<Q', data[32:40])[0]
                if p_off <= file_offset < p_off + p_filesz:
                    va = p_vaddr + (file_offset - p_off)
                    pa = kernbase_pa + (va - kernbase_va)
                    return pa
        return None

    @staticmethod
    def decode_adrp_add(insn_adrp, pc, insn_add):
        """Decode ADRP + ADD immediate → target VA."""
        immlo = (insn_adrp >> 29) & 0x3
        immhi = (insn_adrp >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 20): imm -= (1 << 21)
        page = (pc & ~0xFFF) + (imm << 12)
        imm12 = (insn_add >> 10) & 0xFFF
        return page + imm12

    @staticmethod
    def decode_bl(insn, pc):
        """Decode BL instruction → target VA."""
        imm26 = insn & 0x3FFFFFF
        if imm26 & (1 << 25): imm26 -= (1 << 26)
        return pc + (imm26 << 2)
