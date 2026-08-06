#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""crash_report.py -- ROADMAP B3 "crash-forensics from EL2": turn whatever
survives a guest panic + warm reset (breadcrumb windows, the flight
recorder) into a human-readable report with real "function file:line"
resolution, using the full-DWARF kernel.debug on the HOST side.

WHY THIS EXISTS: the board already captures a panic's raw facts —
el2_exc.c's EXC1 breadcrumb (ESR/ELR/FAR/IPA), backtrace.c's BTR1 (a
frame-pointer-walked list of return addresses) and BTS1 (a cheap on-board
symbol+offset resolve against whatever symtab kload.c parsed this boot),
and flightrec.c's FLTR ring (the last ~2048 (kind,a0,a1) events). But
NOTHING host-side ever reads BTR1/BTS1 at all (triage.py doesn't touch
either window), and nothing anywhere turns a raw ELR/PC into "panic()
kern_shutdown.c:883" the way `aarch64-linux-gnu-addr2line kernel.debug`
already can in seconds by hand (see project memory:
freebsd-kernel-debug-built.md). This is that missing, scriptable step —
deliberately host-side and DWARF-aware, so EL2 never has to link a DWARF
parser (ROADMAP B3's own framing).

Two independent things live in one file because they share the same inputs
(a captured breadcrumb/flightrec snapshot + kernel.debug) and the same
selftest harness:

  1. build_report(capture, elf_path) -- pure function, no hardware, no
     sockets. Turns a `capture` dict (see its docstring) into readable text:
     the fault site, a symbolized backtrace, the recent event timeline
     (decoded, not just hex), and the faulting address named by device.
  2. Addr2Line -- a thin, batching wrapper around
     `aarch64-linux-gnu-addr2line -f -C -e kernel.debug`, one process per
     report (not one per address -- an 85 MB debug binary costs real
     startup time, see the selftest's own timing note).

CLI:
    crash_report.py [--elf PATH] [--tail N]
        Connect to the live board (hvdbg.HV()), gather EXC1/BTR1/BTS1/FLTR,
        symbolize against kernel.debug, print the report.
    crash_report.py selftest
        Board-free: fabricates capture dicts (a synthetic flightrec ring and
        breadcrumb set) using REAL addresses picked out of kernel.debug via
        `nm`, so resolution is against real DWARF, not guesswork -- and
        separately builds and validates an actual core.elf (via the hosted
        test_coredump_elf --dump helper) with `readelf`/`llvm-readelf` and a
        scripted `gdb` batch run against kernel.debug, closing ROADMAP B3's
        "valid vmcore a debugger will actually open" requirement with a
        tool, not eyeballed hex. Cases that need kernel.debug/addr2line/nm/
        readelf/gdb SKIP (not fail) when any of those are unavailable, so a
        board-free `make test` run never depends on this host having them --
        see _cli_selftest()'s SKIP accounting, same convention
        ci.sh already uses for the Zephyr target.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armdec                                   # noqa: E402
import triage                                    # noqa: E402  (reused: rd(),
                                                  # FLTR_MAGIC, FLTR_KINDS,
                                                  # describe_ipa(), the EXC1/
                                                  # FLTR window addresses)

DEFAULT_ELF = "/opt/bzdos/tftpboot/kernel.debug"

# ---------------------------------------------------------------------------
# Breadcrumb windows this file adds host-side reading for. EXC_BC and FLTR
# are triage.py's OWN constants (triage.EXC_BC, triage.FLTR) -- reused, not
# duplicated, so the two tools can never disagree about where they are.
# BTR1/BTS1 are NEW here: backtrace.c writes them on every recorded fault
# (el2_exc.c's B3 block) but nothing host-side has ever read them before.
# ---------------------------------------------------------------------------
BT_BC_BASE = 0x50000700      # backtrace.c:43   magic "BTR1"
BT_MAGIC = 0x42545231        # backtrace.c:44
BT_SLOTS = 32                # backtrace.c:45
BT_HDR = 8                   # backtrace.c:46

BTS_BC_BASE = 0x50000e00     # backtrace.c:181  magic "BTS1"
BTS_MAGIC = 0x42545331       # backtrace.c:182
BTS_SLOTS = 6                # backtrace.c:183
BTS_NAME_WORDS = 5           # backtrace.c:184
BTS_SLOT_WORDS = 7           # backtrace.c:186 (BTS_NAME_WORDS + 2)
BTS_HDR = 8                  # backtrace.c:187

EXC1_MAGIC = 0x45584331       # el2_exc.c / triage.py's dump_exc: "EXC1"

# flightrec.h event kinds whose a1 payload is a CODE address (a PC), worth
# feeding through addr2line -- see flightrec.h's own per-kind doc comments:
#   FLTR_K_FAULT(1)=frame->elr, FLTR_K_SYNC(7)=ELR, FLTR_K_HVVIOL(8)=ELR,
#   FLTR_K_GFAULT(11)=ELR_EL1. FLTR_K_GFAR(12)'s a1 is FAR_EL1 (a DATA
# address) and FLTR_K_DABT(10)'s a1 is a faulting IPA -- neither is a
# symbol lookup, both are handled via triage.describe_ipa()/armdec instead.
FLTR_PC_KINDS = (1, 7, 8, 11)


# ---------------------------------------------------------------------------
# Addr2Line: one batched subprocess per report, not one per address.
# ---------------------------------------------------------------------------
class Addr2Line:
    """Wraps `aarch64-linux-gnu-addr2line -f -C -e <elf>`. `resolve_many()`
    feeds every address in ONE process invocation (loading an 85 MB debug
    binary's DWARF is the dominant cost -- see the selftest's timing note --
    so batching turns O(n) multi-second calls into one)."""

    TOOL = "aarch64-linux-gnu-addr2line"

    def __init__(self, elf_path):
        self.elf_path = elf_path
        self.available = (shutil.which(self.TOOL) is not None
                           and os.path.isfile(elf_path))

    def resolve_many(self, addrs):
        """addrs: iterable of int. Returns {addr: {'func','loc','resolved'}}.
        Every addr gets an entry, even when unavailable/unresolved (marked
        resolved=False) -- callers never need a membership check."""
        uniq = sorted(set(int(a) for a in addrs))
        out = {a: {"func": "??", "loc": "??:0", "resolved": False} for a in uniq}
        if not self.available or not uniq:
            return out
        try:
            args = [self.TOOL, "-f", "-C", "-e", self.elf_path]
            args += ["0x%x" % a for a in uniq]
            r = subprocess.run(args, capture_output=True, text=True, timeout=120)
        except Exception:                                        # noqa: BLE001
            return out
        lines = r.stdout.splitlines()
        for i, a in enumerate(uniq):
            func = lines[2 * i] if 2 * i < len(lines) else "??"
            loc = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "??:0"
            resolved = not (func == "??" and loc.startswith("??"))
            out[a] = {"func": func, "loc": loc, "resolved": resolved}
        return out


def fmt_sym(resolved_entry):
    if not resolved_entry or not resolved_entry["resolved"]:
        return "(unresolved)"
    return "%s at %s" % (resolved_entry["func"], resolved_entry["loc"])


# ---------------------------------------------------------------------------
# Live gather (hv.read_words()-based). Each function is deliberately the
# ONLY place that knows a window's layout; build_report() below never reads
# a live register/window directly -- see its docstring.
# ---------------------------------------------------------------------------
def gather_exc(hv):
    """EXC1 @ triage.EXC_BC. Layout per triage.py's dump_exc() (the existing
    documented reader of this window) -- reused, not re-derived."""
    w = triage.rd(hv, triage.EXC_BC, 20)
    if not w or w[0] != EXC1_MAGIC:
        return None
    elr = (w[5] << 32) | w[4]
    far = (w[7] << 32) | w[6]
    ipa = (w[12] << 32) | w[11]
    return {"count": w[1], "kind": w[2], "esr": w[3],
            "elr": elr, "far": far, "ipa": ipa, "hvviol_count": w[19]}


def gather_btr(hv):
    """BTR1 @ BT_BC_BASE. Layout per backtrace.c:31-42."""
    w = triage.rd(hv, BT_BC_BASE, BT_HDR + BT_SLOTS * 2)
    if not w or w[0] != BT_MAGIC:
        return None
    nframes = min(w[1], BT_SLOTS)
    pcs = []
    for i in range(nframes):
        lo, hi = w[BT_HDR + i * 2], w[BT_HDR + i * 2 + 1]
        pcs.append((hi << 32) | lo)
    return {"nframes": w[1], "total_walks": w[2], "pcs": pcs}


def gather_bts(hv):
    """BTS1 @ BTS_BC_BASE. Layout per backtrace.c:162-187."""
    w = triage.rd(hv, BTS_BC_BASE, BTS_HDR + BTS_SLOTS * BTS_SLOT_WORDS)
    if not w or w[0] != BTS_MAGIC:
        return None
    m = min(w[1], BTS_SLOTS)
    entries = []
    for i in range(m):
        base = BTS_HDR + i * BTS_SLOT_WORDS
        raw = b"".join(struct.pack("<I", x) for x in w[base:base + BTS_NAME_WORDS])
        name = raw.split(b"\0", 1)[0].decode("ascii", "replace")
        off = (w[base + BTS_NAME_WORDS + 1] << 32) | w[base + BTS_NAME_WORDS]
        entries.append({"name": name, "offset": off})
    return {"nresolved": w[1], "total_calls": w[2], "entries": entries}


def gather_fltr(hv, tail=64):
    """FLTR ring @ triage.FLTR. Layout/decoding mirrors triage.py's
    dump_fltr() (the existing documented reader), returning the last `tail`
    events newest-first as plain dicts instead of pre-formatted text, so
    build_report() can symbolize the PC-bearing ones."""
    h = triage.rd(hv, triage.FLTR, 8)
    if not h or h[0] != triage.FLTR_MAGIC:
        return None
    total, head, cap, stride = h[1], h[2], h[3], h[4]
    slots = None
    for _ in range(15):
        slots = hv.read_words_stable(triage.FLTR + 32, cap * stride, tries=4)
        if slots:
            break
    events = []
    if slots:
        n = min(tail, cap)
        for j in range(n):
            i = (head - 1 - j) % cap
            o = i * stride
            kind = slots[o]
            a0 = (slots[o + 2] << 32) | slots[o + 1]
            a1 = (slots[o + 4] << 32) | slots[o + 3]
            events.append({"kind": kind, "a0": a0, "a1": a1})
    return {"total": total, "head": head, "capacity": cap, "stride": stride,
            "events": events}


def gather_capture_from_hv(hv, tail=64):
    """The ONE live entry point: everything build_report() needs, in the
    plain-dict shape its docstring documents. Safe to call against a wedged
    board (each gather_* function tolerates a read failure by returning
    None), same posture triage.py's own readers take."""
    return {"exc": gather_exc(hv), "btr": gather_btr(hv), "bts": gather_bts(hv),
            "fltr": gather_fltr(hv, tail)}


# ---------------------------------------------------------------------------
# build_report(): pure formatting. No hv, no socket, no subprocess besides
# Addr2Line -- this is the function the selftest exercises offline.
# ---------------------------------------------------------------------------
def section(title):
    return ["", "=== %s " % title + "=" * max(0, 62 - len(title)), ""]


def build_report(capture, elf_path=DEFAULT_ELF, addr2line=None):
    """capture: {'exc': {...}|None, 'btr': {...}|None, 'bts': {...}|None,
    'fltr': {...}|None} -- exactly gather_capture_from_hv()'s return shape,
    or a hand-fabricated dict of the same shape (see the selftest). Returns
    the report as one string. Never raises on a missing/None section --
    "no breadcrumb this boot" is a normal, reportable outcome, not an
    error (a guest that never panicked has no EXC1/BTR1 at all)."""
    a2l = addr2line if addr2line is not None else Addr2Line(elf_path)
    exc, btr, bts, fltr = (capture.get(k) for k in ("exc", "btr", "bts", "fltr"))

    addrs = set()
    if exc:
        addrs.add(exc["elr"])
    if btr:
        addrs.update(btr["pcs"])
    if fltr:
        for e in fltr["events"]:
            if e["kind"] in FLTR_PC_KINDS:
                addrs.add(e["a1"])
    resolved = a2l.resolve_many(addrs)

    out = ["bzdOS crash report (ROADMAP B3) -- kernel.debug: %s%s"
           % (elf_path, "" if a2l.available else "  [UNAVAILABLE -- "
              "addr2line/kernel.debug missing, symbols below are raw hex]")]

    out += section("PANIC / FAULT SITE")
    if exc:
        out.append("  ELR (fault PC) = 0x%016x  %s"
                    % (exc["elr"], fmt_sym(resolved.get(exc["elr"]))))
        out += ["  " + s for s in armdec.decode_esr(exc["esr"])]
        out.append("  FAR = 0x%016x  %s" % (exc["far"], armdec.classify_guest_va(exc["far"])))
        out.append("  IPA = 0x%016x  %s" % (exc["ipa"], triage.describe_ipa(exc["ipa"])))
        out.append("  fault count this boot=%d  kind=%d  HV-window violations=%d"
                    % (exc["count"], exc["kind"], exc["hvviol_count"]))
    else:
        out.append("  no EXC1 breadcrumb -- either no fault was ever recorded this")
        out.append("  boot, or this build predates the EXC1 window.")

    out += section("BACKTRACE (frame-pointer walk, symbolized via kernel.debug)")
    if btr and btr["pcs"]:
        if btr["nframes"] > len(btr["pcs"]):
            out.append("  (walk found %d frames, showing the first %d -- BTR1's own cap)"
                        % (btr["nframes"], len(btr["pcs"])))
        for i, pc in enumerate(btr["pcs"]):
            out.append("  #%-2d 0x%016x  %s" % (i, pc, fmt_sym(resolved.get(pc))))
    else:
        out.append("  no BTR1 backtrace breadcrumb")
    if bts and bts["entries"]:
        out.append("")
        out.append("  on-board cheap resolve (BTS1, symbol+offset only, no file:line) --")
        out.append("  cross-check against the addr2line resolution above:")
        for i, e in enumerate(bts["entries"]):
            name = e["name"] if e["name"] else "(no symtab match)"
            out.append("    #%-2d %s+0x%x" % (i, name, e["offset"]))

    out += section("RECENT EVENT TIMELINE (flight recorder, newest first)")
    if fltr:
        if not fltr["events"]:
            out.append("  FLTR ring present but empty (or unreadable this pass)")
        for j, e in enumerate(fltr["events"]):
            kind, a0, a1 = e["kind"], e["a0"], e["a1"]
            name = triage.FLTR_KINDS.get(kind, str(kind))
            line = "  -%-3d %-8s a0=0x%016x a1=0x%016x" % (j + 1, name, a0, a1)
            if kind in FLTR_PC_KINDS:
                line += "\n         %s" % fmt_sym(resolved.get(a1))
            if kind == 10:                      # DABT: a1 = faulting IPA
                line += "\n         at %s" % triage.describe_ipa(a1)
            if kind in (7, 10):                  # SYNC / DABT: a0 = ESR
                d = armdec.decode_esr(a0 & 0xFFFFFFFF)
                keep = [ln.strip() for ln in d
                        if ln.strip().startswith(("EC ", "DFSC", "WnR"))]
                if keep:
                    line += "\n         %s" % "; ".join(keep)
            if kind in (11, 12):                 # GFAULT / GFAR: a0 has EC+ESR+level
                lvl = (a0 >> 40) & 1
                who = "EL0 (userland)" if lvl == 0 else "EL1 (guest kernel)"
                line += "\n         from %s" % who
            out.append(line)
    else:
        out.append("  no FLTR ring (this build predates flightrec.c, or read failed)")

    return "\n".join(out)


# ---------------------------------------------------------------------------
# Live CLI
# ---------------------------------------------------------------------------
def _cli_live(args):
    import hvdbg
    hv = hvdbg.HV()
    capture = gather_capture_from_hv(hv, tail=args.tail)
    print(build_report(capture, elf_path=args.elf))
    return 0


# ---------------------------------------------------------------------------
# selftest -- board-free. See module docstring for what SKIPs vs FAILs.
# ---------------------------------------------------------------------------
def _nm_functions(elf_path):
    """[(addr:int, name:str), ...] for every defined FUNC-ish symbol nm
    reports, sorted by address. Used to fabricate captures against REAL
    kernel.debug addresses instead of made-up ones."""
    r = subprocess.run(["aarch64-linux-gnu-nm", elf_path],
                       capture_output=True, text=True, timeout=60)
    out = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t"):
            try:
                out.append((int(parts[0], 16), parts[2]))
            except ValueError:
                continue
    return out


def _have(*tools):
    return all(shutil.which(t) for t in tools)


def case_missing_windows_render_without_crashing():
    rep = build_report({"exc": None, "btr": None, "bts": None, "fltr": None},
                       elf_path="/nonexistent/kernel.debug")
    assert "no EXC1 breadcrumb" in rep
    assert "no BTR1 backtrace breadcrumb" in rep
    assert "no FLTR ring" in rep
    assert "UNAVAILABLE" in rep


def case_unresolved_pc_does_not_crash():
    cap = {"exc": {"count": 1, "kind": 10, "esr": 0x96000010, "elr": 0x1234,
                   "far": 0x5678, "ipa": 0x0A000050, "hvviol_count": 0},
          "btr": None, "bts": None, "fltr": None}
    rep = build_report(cap, elf_path="/nonexistent/kernel.debug")
    assert "(unresolved)" in rep
    assert "vblk" in rep, "IPA 0x0A000050 must be named via triage.describe_ipa()"


def case_fltr_dabt_names_the_device():
    cap = {"exc": None, "btr": None, "bts": None,
          "fltr": {"total": 1, "head": 1, "capacity": 8, "stride": 5,
                    "events": [{"kind": 10, "a0": 0x96000010 & 0xFFFFFFFF,
                                "a1": 0x0A001064}]}}
    rep = build_report(cap, elf_path="/nonexistent/kernel.debug")
    assert "vnet" in rep and "InterruptACK" in rep, \
        "IPA 0x0A001064 is vnet+0x64 == InterruptACK per triage.py's VIRTIO_REGS"


def _real_elf_cases(elf_path):
    """Cases needing a real kernel.debug + addr2line + nm. Returns
    (ran, skipped) -- SKIP (not fail) if the tooling is unavailable, per the
    module docstring."""
    if not (_have("aarch64-linux-gnu-addr2line", "aarch64-linux-gnu-nm")
            and os.path.isfile(elf_path)):
        return False, True

    syms = _nm_functions(elf_path)
    assert len(syms) > 100, "kernel.debug should have hundreds of symbols"
    # Three well-spread real functions, offset by a few bytes past their
    # start (mid-instruction is fine for addr2line -- it resolves by range,
    # not by requiring an exact symbol-start address).
    picks = [syms[len(syms) // 4], syms[len(syms) // 2], syms[3 * len(syms) // 4]]
    pcs = [addr + 4 for addr, _name in picks]
    names = [name for _addr, name in picks]

    def case_backtrace_resolves_real_symbols_in_order():
        cap = {"exc": None, "btr": {"nframes": 3, "total_walks": 1, "pcs": pcs},
              "bts": None, "fltr": None}
        rep = build_report(cap, elf_path=elf_path)
        for i, nm in enumerate(names):
            assert ("#%-2d" % i) in rep or ("#%d" % i) in rep
            assert nm in rep, "expected %r in report" % nm
        # order: #0's name must appear before #1's, etc.
        idxs = [rep.index(nm) for nm in names]
        assert idxs == sorted(idxs), "frames must render in walk order"

    def case_fault_site_resolves_function_and_file_line():
        pc, name = pcs[0], names[0]
        cap = {"exc": {"count": 1, "kind": 8, "esr": 0x96000010, "elr": pc,
                       "far": pc, "ipa": 0, "hvviol_count": 0},
              "btr": None, "bts": None, "fltr": None}
        rep = build_report(cap, elf_path=elf_path)
        a2l = Addr2Line(elf_path)
        expect = a2l.resolve_many([pc])[pc]
        assert expect["resolved"], "sanity: addr2line must resolve a real function addr"
        assert expect["func"] in rep
        assert expect["loc"] in rep

    def case_fltr_sync_event_symbolized():
        pc, name = pcs[1], names[1]
        cap = {"exc": None, "btr": None, "bts": None,
              "fltr": {"total": 1, "head": 1, "capacity": 8, "stride": 5,
                        "events": [{"kind": 7, "a0": 0x9600001f, "a1": pc}]}}
        rep = build_report(cap, elf_path=elf_path)
        assert name in rep

    for fn in (case_backtrace_resolves_real_symbols_in_order,
              case_fault_site_resolves_function_and_file_line,
              case_fltr_sync_event_symbolized):
        fn()
    return True, False


def _vmcore_case(elf_path, tmpdir):
    """The end-to-end 'valid vmcore a debugger will actually open' proof:
    build a real core.elf via the hosted test_coredump_elf --dump helper
    (exercising the ACTUAL fixed ELF-assembly mirror, not just Python), then
    validate it with readelf AND a scripted gdb batch run against
    kernel.debug -- asserted with a tool, not eyeballed. SKIPs if
    test_coredump_elf isn't built, or gdb/readelf are missing, or
    kernel.debug is absent -- a board-free `make test` run must not require
    any of them.

    Runtime note: gdb loading an 85 MB debug binary costs ~1.2s; this runs
    it ONCE per selftest invocation, in one batch script (not per-assertion)."""
    test_bin = os.path.join(HERE, "test_coredump_elf")
    readelf = shutil.which("aarch64-linux-gnu-readelf") or shutil.which("llvm-readelf")
    if not (os.path.isfile(test_bin) and os.access(test_bin, os.X_OK)
            and readelf and shutil.which("gdb") and os.path.isfile(elf_path)):
        return False, True

    syms = _nm_functions(elf_path)
    pc_addr, pc_name = syms[len(syms) // 3]
    sp = 0xFFFF0000AA000010

    core_path = os.path.join(tmpdir, "crash_report_selftest.core")
    r = subprocess.run([test_bin, "--dump", core_path,
                       "%x" % pc_addr, "%x" % sp],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, "test_coredump_elf --dump failed: %s" % r.stderr
    assert os.path.isfile(core_path)

    r = subprocess.run([readelf, "-a", core_path],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, "readelf rejected the produced core: %s" % r.stderr
    assert "CORE" in r.stdout or "ET_CORE" in r.stdout
    assert "LOAD" in r.stdout, "no PT_LOAD segment in the produced core"
    assert "NOTE" in r.stdout, "no PT_NOTE (NT_PRSTATUS) segment"

    gscript = [
        "gdb", "-q", "-batch",
        "-ex", "set debuginfod enabled off",
        "-ex", "file %s" % elf_path,
        "-ex", "core-file %s" % core_path,
        "-ex", "info registers pc sp",
        "-ex", "x/2xg $sp",
    ]
    r = subprocess.run(gscript, capture_output=True, text=True, timeout=60)
    combined = r.stdout + r.stderr
    assert ("0x%x" % pc_addr) in combined, "gdb did not report the fabricated PC"
    assert pc_name in combined, \
        "gdb did not resolve PC to %r via kernel.debug -- symbol lookup broken" % pc_name
    assert "Cannot access memory" not in combined, (
        "REGRESSION: gdb could not read stack memory at $sp -- this is "
        "exactly the 2026-08-06 VA-vs-PA bug this fix closes. Output:\n" + combined)
    return True, False


_ST_SIMPLE = [
    ("missing_windows_render_without_crashing", case_missing_windows_render_without_crashing),
    ("unresolved_pc_does_not_crash", case_unresolved_pc_does_not_crash),
    ("fltr_dabt_names_the_device", case_fltr_dabt_names_the_device),
]


def _cli_selftest(_args):
    import tempfile
    passed = failed = skipped = 0

    for name, fn in _ST_SIMPLE:
        print("[ RUN ] %s" % name)
        try:
            fn()
        except Exception:                                        # noqa: BLE001
            failed += 1
            print("[FAIL ] %s" % name)
            traceback.print_exc()
            continue
        passed += 1
        print("[ OK  ] %s" % name)

    elf_path = os.environ.get("BZDOS_KERNEL_DEBUG", DEFAULT_ELF)
    print("[ RUN ] real-kernel.debug symbolization cases (elf=%s)" % elf_path)
    try:
        ran, skip = _real_elf_cases(elf_path)
    except Exception:                                              # noqa: BLE001
        failed += 1
        print("[FAIL ] real-kernel.debug symbolization cases")
        traceback.print_exc()
    else:
        if skip:
            skipped += 1
            print("[ SKIP] real-kernel.debug symbolization cases "
                  "(addr2line/nm/kernel.debug unavailable)")
        else:
            passed += 1
            print("[ OK  ] real-kernel.debug symbolization cases")

    print("[ RUN ] vmcore readelf+gdb validation")
    with tempfile.TemporaryDirectory() as td:
        try:
            ran, skip = _vmcore_case(elf_path, td)
        except Exception:                                          # noqa: BLE001
            failed += 1
            print("[FAIL ] vmcore readelf+gdb validation")
            traceback.print_exc()
        else:
            if skip:
                skipped += 1
                print("[ SKIP] vmcore readelf+gdb validation "
                      "(test_coredump_elf/readelf/gdb/kernel.debug unavailable)")
            else:
                passed += 1
                print("[ OK  ] vmcore readelf+gdb validation")

    n = passed + failed
    print("---- crash_report selftest: %d/%d passed, %d skipped ----"
          % (passed, n, skipped))
    return 0 if failed == 0 else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "selftest":
        return _cli_selftest(argv[1:])

    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF, help="path to kernel.debug")
    ap.add_argument("--tail", type=int, default=64,
                    help="flight-recorder events to include")
    args = ap.parse_args(argv)
    return _cli_live(args)


if __name__ == "__main__":
    sys.exit(main())
