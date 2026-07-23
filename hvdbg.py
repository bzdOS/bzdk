#!/usr/bin/env python3
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
"""
import os, sys, socket, struct, time, re, subprocess

IFACE     = "br0"
ETYPE     = 0x88B5
BOARD_MAC = bytes.fromhex("02bd05000001")
BCAST     = b"\xff" * 6
CSI       = re.compile(rb'\x1b\[[0-9;?]*[a-zA-Z]')

class HV:
    """Connection to the running hypervisor debug monitor over EMAC."""
    def __init__(self, iface=IFACE, timeout=3):
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                  socket.htons(ETYPE))
        self.sock.bind((iface, ETYPE))
        self.src_mac = self.sock.getsockname()[4]
        self.timeout = timeout
        self._drain(0.3)

    def close(self):
        try: self.sock.close()
        except: pass

    # ── low-level ──────────────────────────────────────────────────────
    def _send(self, text):
        p = (text + "\r").encode("latin1", "replace")
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

    def read_words(self, pa, n):
        """Read n 32-bit words from physical address pa. Returns list of ints.

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
        r = self.cmd(f"r 0x{pa:x} {n}", max(2, n // 50 + 2))
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
            i = (addr - pa) // 4
            for p in rest.split():
                p = p.strip()
                if len(p) == 8 and i < n:
                    try:
                        slots[i] = int(p, 16); i += 1
                    except ValueError:
                        pass
        if any(s is None for s in slots):
            return [s for s in slots if s is not None][:0]  # incomplete -> []
        return slots

    def read_bytes(self, pa, n):
        """Read n bytes from physical address pa. Returns bytes."""
        r = self.cmd(f"rb 0x{pa:x} {n}", max(2, n // 100 + 2))
        hexstr = ""
        for line in r.split("\n"):
            if ":" not in line: continue
            hexstr += line.split(":", 1)[1].strip().replace(" ", "")
        return bytes.fromhex(hexstr[:n*2]) if hexstr else b""

    def dump(self, pa, length):
        """Hex+ASCII dump. Returns the ASCII-column text."""
        r = self.cmd(f"d 0x{pa:x} {length}", max(2, length // 60 + 2))
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

    def wdt_reset(self):
        """Force a CLEAN board reset via the A64 watchdog.

        CRITICAL: the reset MUST first drop the MUSB D+/D- pull-up so the host
        sees a clean USB disconnect BEFORE the reset. Otherwise the gadget's
        pull-up stays asserted through the reset window, U-Boot's musb re-inits
        against a host that still thinks a device is attached, and the console
        gadget wedges (host 'device not accepting address, error -71') with NO
        remote recovery path left (bit us twice on 2026-07-19).

        ROBUST path: call the hypervisor's own reboot_clean() — it does the
        disconnect + WDOG arm + spin atomically on the board, with ZERO
        dependence on flaky EMAC reads. hv.call() will time out (reboot_clean
        never returns — the WDOG resets the SoC), which is expected/fine.

        FALLBACK (reboot_clean addr unknown): poke the MMIO directly, but RETRY
        the ISCR/POWER reads until they succeed so the disconnect is NEVER
        skipped (the old bug: a failed read skipped the disconnect -> wedge)."""
        addr = self._reboot_clean_addr()
        if addr is not None:
            try:
                self.call(addr)          # atomic clean reset on-board; won't return
            except Exception:
                pass
            return
        # ---- fallback: MMIO with retried reads (never skip the disconnect) ----
        MUSB = 0x01c19000
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
        for pa, val in [(0x01c20cb4, 1), (0x01c20cb8, 0x21), (0x01c20cb0, 0x14af)]:
            self.write_word(pa, val)

    # ── breadcrumb shortcuts ───────────────────────────────────────────
    def gict(self):
        """Read the GICT timer breadcrumb. Returns dict."""
        w = self.read_words(0x50000800, 8)
        if len(w) < 8: return {}
        return dict(
            magic=w[0], ticks_lo=w[1], ticks_hi=w[2],
            period=w[4], init_done=w[6], ctl_live=w[7])

    def vconsole(self):
        """Read vconsole capture ring header. Returns dict."""
        w = self.read_words(0x50000f00, 4)
        if len(w) < 4: return {}
        return dict(magic=w[0], total_bytes=w[1], fault_count=w[2])

    def vconsole_text(self, length=None):
        """Dump and return the vconsole ring data as text."""
        hdr = self.vconsole()
        n = hdr.get('total_bytes', 0)
        if n == 0: return b""
        if length: n = min(n, length)
        return self.dump(0x50000f10, min(n, 0x1000))

    # ── flight recorder (flightrec.c/.h, ROADMAP B4) ────────────────────
    # Generic (kind, a0, a1) event ring at 0x50012000, magic "FLTR". See
    # flightrec.h for the full wire format -- this just mirrors it in
    # Python so a crash can be read back as a readable timeline instead of
    # a raw memory blob. No existing reader touched this ring before B4
    # (grepped "FLTR"/"flightrec" across every *.py in the tree: only the
    # source files themselves matched).
    FLTR_BASE       = 0x50012000
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
