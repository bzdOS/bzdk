#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""bzdctl — one entry point for the whole board lifecycle (ROADMAP B1 DoD).

    power -> boot-watch -> console -> reset -> crash-dump

all without ssh'ing to the host that happens to have the USB cable, and without
knowing which of a dozen scripts owns which verb. That "dozen scripts" is the
problem this file solves: the capability was already ~all there, but spread over
bmc_client.py (health/telemetry/console/wdt), board_ctl.py (U-Boot capture,
power-cycle detection), chimpd.py (boot supervision + TFTP reload),
boot_ledger.py (cumulative clean-boot streak), coredump-recv.py (crash
transport) and triage.py (post-mortem timeline). Nothing here reimplements any
of that -- every subcommand delegates. What is new is the *façade* and the web
view, which is the part of B1's DoD that was missing.

TWO TRANSPORTS, AND WHY IT MATTERS HERE
---------------------------------------
  * EMAC raw Ethernet (ethertype 0x88B5 on br0) -- BMC verbs, memory reads,
    health. Serviced by CPU1, so it answers even when the guest is wedged.
  * The USB-ACM tty (/dev/ttyACM0) -- U-Boot capture, image reload, guest shell.

Only ONE process may hold the tty at a time (two readers steal each other's
bytes and a healthy channel looks dead -- this has cost this project days). So
`serve` and `status` deliberately stay EMAC-only and never open the tty: a
dashboard left running in a browser tab must not be able to lock out a reload.
Subcommands that DO need the tty say so in their help text.

Usage:
    bzdctl.py status                 # one-screen state, EMAC only
    bzdctl.py health [--raw]         # BMC health record
    bzdctl.py power reset|hold|release|arm|disarm
    bzdctl.py power uboot            # catch the board in U-Boot (needs tty)
    bzdctl.py console [-n N] [--follow]
    bzdctl.py console --inject TEXT
    bzdctl.py boot-watch [--timeout S]
    bzdctl.py crash [--out DIR]
    bzdctl.py ledger
    bzdctl.py serve [--port 8088]    # read-only web dashboard, EMAC only
"""
import argparse
import contextlib
import datetime
import html
import importlib.util
import io
import json
import os
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bmc_client                      # noqa: E402
import boot_ledger                     # noqa: E402


def _load_hyphenated(name, filename):
    """Import a module whose filename is not a valid identifier.

    coredump-recv.py cannot be `import`ed; renaming it would break every
    existing caller and doc reference, so load it by path instead."""
    path = os.path.join(HERE, filename)
    if not os.path.exists(path):
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
    except Exception:
        return None
    return mod


def _bmc(iface="br0"):
    return bmc_client.BMC(iface=iface)


# ── status ───────────────────────────────────────────────────────────────
def collect_status(iface="br0"):
    """Everything cheap and EMAC-only, as a plain dict (also feeds `serve`).

    Never raises: a dashboard polling a board that is mid-reboot must render
    "unreachable", not a traceback."""
    out = {"when": datetime.datetime.now().isoformat(timespec="seconds"),
           "reachable": False, "health": None, "moving": None, "error": None}
    try:
        b = _bmc(iface)
    except Exception as e:
        out["error"] = f"{type(e).__name__}: {e}"
        return out
    def sample():
        """One FRESH health snapshot, plus whether it is actually fresh.

        The BMC1 breadcrumb is a LATCH -- bmc.c rewrites it only when the `bmc
        health` verb runs. Reading DRAM twice therefore returns the identical
        record, so diffing those two reads reports every core as frozen. The
        first version of this function did exactly that, and duly announced
        cpu0/cpu1/cpu2 FROZEN on a board whose CPU1 heartbeat was demonstrably
        advancing. Poke the verb first so the latch is re-taken, and treat
        "could not poke" as UNKNOWN rather than as death: a stale latch says
        nothing about motion.

        This is the trap this project keeps re-stepping into -- diffing two
        samples of something nobody is updating. triage.py carries the same
        warning; so does project memory (breadcrumb-window-hygiene)."""
        fresh = False
        try:
            fresh = "BMC" in (b.health_text() or "")
        except Exception:
            fresh = False
        return b.health_raw(), fresh

    try:
        h1, f1 = sample()
        time.sleep(2.0)
        h2, f2 = sample()
        if h1 and h2:
            out["reachable"] = True
            out["health"] = h2
            out["fresh"] = bool(f1 and f2)
            if f1 and f2:
                out["moving"] = {
                    f"cpu{i}": (h1[f"hb_cpu{i}"] != h2[f"hb_cpu{i}"])
                    for i in range(4)
                    # 0xffffffff means the core posts no heartbeat at all (CPU3
                    # parks in WFI by design); absence is not a fault.
                    if h1[f"hb_cpu{i}"] != 0xffffffff
                }
                out["console_advancing"] = h1["cons_bytes"] != h2["cons_bytes"]
            else:
                # Latch readable but not refreshable: show the values, refuse to
                # claim anything about motion.
                out["moving"] = None
                out["console_advancing"] = None
        else:
            out["error"] = "no BMC1 record (board down, or magic mismatch)"
    except Exception as e:
        out["error"] = f"{type(e).__name__}: {e}"
    return out


def cmd_status(args):
    s = collect_status(args.iface)
    print(f"bzdctl status  {s['when']}")
    if not s["reachable"]:
        print(f"  UNREACHABLE over EMAC: {s['error']}")
        print("  Board may be in U-Boot or off. Next: bzdctl.py power uboot")
        return 1
    bmc_client.print_health(s["health"])
    mv = s.get("moving")
    if mv is None:
        print("  liveness    unknown (health verb unreachable; the record read "
              "is a stale latch)")
    else:
        alive = [c for c, m in mv.items() if m]
        # Not every still core is a sick one, and saying so indiscriminately is
        # how a management plane trains its owner to ignore it. CPU2 is the
        # async eMMC I/O server: with an idle guest it has nothing to do and
        # legitimately posts no progress. Only CPU0 (guest) and CPU1 (debug
        # core, pets the watchdog) standing still is genuinely alarming.
        idle_ok = {"cpu2"}
        dead = [c for c, m in mv.items() if not m and c not in idle_ok]
        quiet = [c for c, m in mv.items() if not m and c in idle_ok]
        print(f"  liveness    moving={','.join(alive) or 'none'}"
              f"{'  FROZEN=' + ','.join(dead) if dead else ''}"
              f"{'  idle=' + ','.join(quiet) if quiet else ''}")
    ca = s.get("console_advancing")
    print("  console     " + ("unknown" if ca is None else
          ("advancing" if ca else "quiet (normal for an idle guest)")))
    st = boot_ledger.stats()
    if st:
        print(f"  ledger      {_ledger_line(st)}")
    return 0


def _ledger_line(st):
    if isinstance(st, dict):
        keys = ("total", "ok", "fail", "streak", "best_streak")
        parts = [f"{k}={st[k]}" for k in keys if k in st]
        return "  ".join(parts) or json.dumps(st)[:120]
    return str(st)[:120]


# ── health / console / power ─────────────────────────────────────────────
def cmd_health(args):
    b = _bmc(args.iface)
    if args.raw:
        bmc_client.print_health(b.health_raw())
    else:
        print(b.health_text())
    return 0


def cmd_console(args):
    b = _bmc(args.iface)
    if args.inject is not None:
        print(b.inject(args.inject))
        return 0
    if args.follow:
        # The TX tee is a ring the board keeps appending to; poll it and print
        # only what is new. Ctrl-C is the intended way out.
        seen = ""
        try:
            while True:
                cur = b.tee(args.n) or ""
                if cur != seen:
                    tail = cur[len(seen):] if cur.startswith(seen) else cur
                    sys.stdout.write(tail)
                    sys.stdout.flush()
                    seen = cur
                time.sleep(1.0)
        except KeyboardInterrupt:
            print()
        return 0
    print(b.console(args.n))
    return 0


def cmd_power(args):
    b = _bmc(args.iface)
    act = args.action
    if act == "uboot":
        # The ONLY subcommand here that touches the tty.
        import board_ctl
        print("catching the board in U-Boot (this opens /dev/ttyACM0) ...")
        return 0 if board_ctl.force_to_uboot(timeout=args.timeout) else 1
    if act == "reset":
        print("issuing reboot_clean via BMC (link will drop) ...")
        print(b.reset())
        return 0
    return_map = {"hold": "hold", "release": "release",
                  "arm": "arm", "disarm": "disarm"}
    print(b.wdt(return_map[act]))
    return 0


# ── boot-watch ───────────────────────────────────────────────────────────
def cmd_boot_watch(args):
    """Watch a boot to first liveness, then record it in the cumulative ledger.

    Deliberately EMAC-only and read-only: it observes, it does not drive the
    reload. chimpd.py owns driving (TFTP + bootelf + supervision); duplicating
    that here would mean two supervisors fighting over one tty."""
    b = None
    t0 = time.time()
    last = None
    print(f"watching for liveness over EMAC (timeout {args.timeout}s) ...")
    while time.time() - t0 < args.timeout:
        try:
            if b is None:
                b = _bmc(args.iface)
            h = b.health_raw()
        except Exception:
            h = None
            b = None
        if h:
            if last is not None and h["hb_cpu1"] != last:
                el = time.time() - t0
                print(f"ALIVE after {el:.1f}s  "
                      f"(cpu1 heartbeat advancing, temp "
                      f"{h['temp_mc'] / 1000:.1f} C)")
                try:
                    boot_ledger.record(True, source="bzdctl-boot-watch",
                                       note=f"liveness at {el:.1f}s")
                except Exception as e:
                    print(f"  (ledger not updated: {e})")
                return 0
            last = h["hb_cpu1"]
        time.sleep(2.0)
    print("TIMED OUT with no advancing heartbeat")
    try:
        boot_ledger.record(False, source="bzdctl-boot-watch",
                           note="no liveness before timeout")
    except Exception:
        pass
    return 1


# ── crash collection ─────────────────────────────────────────────────────
def cmd_crash(args):
    """Collect one self-contained crash bundle.

    A crash report is only useful if it is gathered WITHOUT further poking the
    board, so this takes exactly what survives a warm reset (DRAM breadcrumbs)
    plus whatever the guest already pushed over the coredump channel."""
    outdir = args.out or os.path.join(
        HERE, "crash-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(outdir, exist_ok=True)
    wrote = []

    s = collect_status(args.iface)
    p = os.path.join(outdir, "status.json")
    with open(p, "w") as f:
        json.dump(s, f, indent=2, default=str)
    wrote.append(p)

    # triage.py is the project's canonical post-mortem view (build identity,
    # breadcrumbs, flight-recorder timeline). Shell out rather than import: it
    # is written as a script with output as its product.
    import subprocess
    try:
        r = subprocess.run([sys.executable, os.path.join(HERE, "triage.py")],
                           capture_output=True, text=True, timeout=600)
        p = os.path.join(outdir, "triage.txt")
        with open(p, "w") as f:
            f.write(r.stdout + ("\n--- stderr ---\n" + r.stderr if r.stderr else ""))
        wrote.append(p)
    except Exception as e:
        print(f"  triage failed: {e}")

    if args.coredump:
        cd = _load_hyphenated("coredump_recv", "coredump-recv.py")
        if cd is None:
            print("  coredump-recv.py unavailable; skipping core capture")
        else:
            try:
                sock = cd.open_coredump_socket(iface=args.iface)
                core = cd.recv_coredump(sock, idle_timeout=args.coredump)
                if core:
                    p = os.path.join(outdir, "guest.core")
                    with open(p, "wb") as f:
                        f.write(core)
                    wrote.append(p)
            except Exception as e:
                print(f"  coredump capture failed: {e}")

    print(f"crash bundle -> {outdir}")
    for p in wrote:
        print(f"  {os.path.basename(p)}  {os.path.getsize(p)} bytes")
    return 0


def cmd_ledger(_args):
    st = boot_ledger.stats()
    print(json.dumps(st, indent=2, default=str) if isinstance(st, dict) else st)
    return 0


# ── web dashboard ────────────────────────────────────────────────────────
PAGE = """<!doctype html><html><head><meta charset="utf-8">
<title>bzdOS Chimp — BMC</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:#f7f7f5;--fg:#1b1b1a;--mut:#6b6b66;--card:#fff;--line:#e2e2dd;
      --ok:#1f7a4d;--warn:#8a6d1f;--bad:#a3311f;--accent:#2d5f8a}
@media (prefers-color-scheme:dark){:root{--bg:#16181a;--fg:#e8e8e4;--mut:#9a9a94;
      --card:#1e2124;--line:#2c3033;--ok:#4cc38a;--warn:#d4a72c;--bad:#e5714f}}
*{box-sizing:border-box}
body{margin:0;padding:1.5rem;background:var(--bg);color:var(--fg);
     font:15px/1.5 ui-sans-serif,system-ui,-apple-system,sans-serif}
h1{font-size:1.15rem;margin:0 0 .25rem;letter-spacing:.01em}
.sub{color:var(--mut);font-size:.85rem;margin-bottom:1.25rem}
.grid{display:grid;gap:1rem;grid-template-columns:repeat(auto-fit,minmax(270px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;
      padding:.9rem 1rem}
.card h2{font-size:.72rem;text-transform:uppercase;letter-spacing:.08em;
         color:var(--mut);margin:0 0 .6rem;font-weight:600}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
td{padding:.18rem 0;vertical-align:top}
td:first-child{color:var(--mut);padding-right:.75rem;white-space:nowrap}
td:last-child{text-align:right;font-family:ui-monospace,monospace;font-size:.86rem}
.pill{display:inline-block;padding:.1rem .5rem;border-radius:99px;
      font-size:.75rem;font-weight:600}
.ok{background:color-mix(in srgb,var(--ok) 18%,transparent);color:var(--ok)}
.bad{background:color-mix(in srgb,var(--bad) 18%,transparent);color:var(--bad)}
.warn{background:color-mix(in srgb,var(--warn) 20%,transparent);color:var(--warn)}
.note{color:var(--mut);font-size:.8rem;margin-top:1.25rem;max-width:60ch}
</style></head><body>
<h1>bzdOS Chimp — software BMC</h1>
<div class="sub">__WHEN__ · read-only · EMAC only (never opens the tty)</div>
__BODY__
<div class="note">Refreshes every 15 s. This view is deliberately read-only:
every destructive verb stays behind <code>bzdctl.py power …</code> on a
terminal, and nothing here can take the serial port away from a reload.</div>
</body></html>"""


def _rows(pairs):
    return "".join(f"<tr><td>{html.escape(str(k))}</td>"
                   f"<td>{v}</td></tr>" for k, v in pairs)


def render(s):
    if not s["reachable"]:
        body = ('<div class="card"><h2>state</h2>'
                '<span class="pill bad">unreachable</span>'
                f'<table>{_rows([("error", html.escape(str(s["error"])))])}</table>'
                '</div>')
        return PAGE.replace("__WHEN__", html.escape(s["when"])).replace("__BODY__", body)

    h = s["health"]
    up = h["uptime"] // 24000000          # A64 arch timer is 24 MHz
    t = h["temp_mc"]
    mv = s.get("moving")
    if mv is None:
        # Stale latch: do not paint cores green or red off data that is not
        # being refreshed.
        cores = '<span class="pill warn">liveness unknown (stale latch)</span>'
    elif not mv:
        cores = '<span class="pill warn">no heartbeats</span>'
    else:
        cores = " ".join(
            f'<span class="pill {"ok" if m else ("warn" if c == "cpu2" else "bad")}">'
            f'{c}</span>' for c, m in sorted(mv.items()))

    cards = [
        ("state", _rows([
            ("uptime", f"{up // 3600}h {(up % 3600) // 60}m"),
            ("temperature", "n/a" if not t else f"{t / 1000:.1f} &deg;C"),
            ("guest pc", f"0x{h['guest_pc']:x}"),
            ("wdt hold", h["wdt_hold"]),
        ])),
        ("cores", f"<div>{cores}</div><table>" + _rows([
            ("online map", f"0x{h['online_map']:x}"),
            ("console", {None: "unknown", True: "advancing",
                         False: "quiet"}[s.get("console_advancing")]),
        ]) + "</table>"),
        ("console / faults", _rows([
            ("bytes", f"{h['cons_bytes']:,}"),
            ("faults", f"{h['cons_faults']:,}"),
            ("first-fault latch", h["ffv_count"]),
        ])),
        ("flags", _rows([(n, "on") for n in h["flag_names"]] or [("(none)", "")])),
    ]

    # Battery: an absent pack must not read as a flat one.
    if h.get("axp_ok"):
        if any(h[k] for k in ("vbat_mv", "ichg_ma", "idischg_ma", "batt_ts_mv")):
            cards.append(("battery (AXP803)", _rows([
                ("vbat", f"{h['vbat_mv']} mV"),
                ("charge", f"{h['ichg_ma']} mA"),
                ("discharge", f"{h['idischg_ma']} mA"),
            ])))
        else:
            cards.append(("battery (AXP803)",
                          '<span class="pill warn">present, no readings</span>'))

    try:
        st = boot_ledger.stats()
        if isinstance(st, dict):
            cards.append(("boot ledger", _rows(list(st.items())[:6])))
    except Exception:
        pass

    body = '<div class="grid">' + "".join(
        f'<div class="card"><h2>{html.escape(name)}</h2>'
        + (inner if inner.lstrip().startswith("<") else f"<table>{inner}</table>")
        + "</div>" for name, inner in cards) + "</div>"
    return PAGE.replace("__WHEN__", html.escape(s["when"])).replace("__BODY__", body)


def cmd_serve(args):
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    iface = args.iface
    cache = {"at": 0.0, "data": None}
    TTL = 12.0        # one board poll per this window, however many browsers

    def snapshot():
        now = time.time()
        if cache["data"] is None or now - cache["at"] > TTL:
            cache["data"] = collect_status(iface)
            cache["at"] = now
        return cache["data"]

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass                       # don't spam the terminal

        def do_GET(self):
            if self.path.startswith("/status.json"):
                b = json.dumps(snapshot(), default=str).encode()
                ct = "application/json"
            else:
                b = render(snapshot()).encode()
                ct = "text/html; charset=utf-8"
            self.send_response(200)
            self.send_header("Content-Type", ct)
            self.send_header("Content-Length", str(len(b)))
            self.send_header("Refresh", "15")
            self.end_headers()
            self.wfile.write(b)

    srv = ThreadingHTTPServer((args.bind, args.port), H)
    print(f"bzdctl dashboard on http://{args.bind}:{args.port}/  "
          f"(json at /status.json)  Ctrl-C to stop")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print()
    return 0


# ── selftest — pure, offline. No sockets, no board. ─────────────────────
#
# bzdctl.py had no tests at all before this pass, despite carrying the exact
# bug its own docstrings warn about: collect_status()'s first version diffed
# two reads of the BMC1 breadcrumb -- which is a LATCH, rewritten only when
# the `bmc health` verb runs -- and duly reported cpu0/cpu1/cpu2 FROZEN on a
# board whose CPU1 heartbeat was demonstrably advancing (see collect_status's
# sample() docstring). This suite pins that down as an explicit regression,
# plus render()'s degraded-data paths and cmd_status's idle-vs-frozen
# classification. bmc_client.py's own `selftest` covers _avail()/health_raw()/
# print_health(); this one stays one layer up, at the fresh/stale-latch and
# HTML-rendering logic that lives in THIS file.
#
# `python3 bzdctl.py selftest` (see main()); wired into `make test`.


class _FakeBMC:
    """Scripted (health_raw, health_text) replies, consumed one call at a
    time. collect_status()'s sample() calls health_text() THEN health_raw()
    per sample, and samples twice per collect_status() call -- so a normal
    test scripts two of each. Entries in `text_seq` may be an Exception
    instance/class to simulate "the health verb itself is unreachable"
    (the exact "could not poke" case collect_status()'s docstring calls
    out), independently of whether health_raw() (a plain memory read)
    still succeeds."""

    def __init__(self, raw_seq, text_seq):
        self._raw = list(raw_seq)
        self._text = list(text_seq)

    def health_text(self):
        if not self._text:
            raise AssertionError("fake BMC: health_text() called more than scripted")
        v = self._text.pop(0)
        if isinstance(v, Exception):
            raise v
        if isinstance(v, type) and issubclass(v, Exception):
            raise v("scripted failure")
        return v

    def health_raw(self):
        if not self._raw:
            raise AssertionError("fake BMC: health_raw() called more than scripted")
        return self._raw.pop(0)


def _mkhealth(**overrides):
    """A fully-decoded health dict -- what health_raw() actually returns, not
    a hand-rolled parallel shape that could quietly drift from it. Built via
    bmc_client's OWN wire-decode path (_mkwords()/_FakeWordsBMC, the same
    helpers its selftest uses) so cmd_status()'s real call to
    bmc_client.print_health() gets every key it expects (version, ticks,
    exc_count, hb_cpuN, ...), not just the subset render() happens to read.
    `overrides` are HEALTH_WORDS field names, exactly as bmc_client._mkwords()
    takes them."""
    words = bmc_client._mkwords(**overrides)
    return bmc_client._FakeWordsBMC(words).health_raw()


def _mkstatus(**overrides):
    """A plausible collect_status()-shaped dict, for testing render()/
    cmd_status() in isolation from collect_status() itself."""
    base = dict(when="2026-08-06T00:00:00", reachable=True, error=None,
                health=_mkhealth(), moving={"cpu0": True, "cpu1": True},
                console_advancing=True)
    base.update(overrides)
    return base


@contextlib.contextmanager
def _patched(obj, name, value):
    """Swap one attribute on `obj` (a module or object) for the duration of
    the `with` block. Used to inject a fake BMC / fake collect_status /
    fake boot_ledger without ever touching a socket or a file that isn't
    this test's own."""
    orig = getattr(obj, name)
    setattr(obj, name, value)
    try:
        yield
    finally:
        setattr(obj, name, orig)


def _captured(fn, *a, **kw):
    """Run fn(*a, **kw) with stdout captured; return (result, printed text)."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        result = fn(*a, **kw)
    return result, buf.getvalue()


_THIS = sys.modules[__name__]


# ---- collect_status(): fresh-sample vs stale-latch ------------------------
def case_collect_status_moving_from_two_fresh_reads():
    h1 = _mkhealth(hb_cpu0=1, hb_cpu1=10, hb_cpu2=5, cons_bytes=100)
    h2 = _mkhealth(hb_cpu0=2, hb_cpu1=10, hb_cpu2=5, cons_bytes=150)
    fake = _FakeBMC(raw_seq=[h1, h2], text_seq=["dbg> BMC health v1.1", "dbg> BMC health v1.1"])
    with _patched(_THIS, "_bmc", lambda iface="br0": fake), \
         _patched(time, "sleep", lambda *_a, **_k: None):
        out = collect_status()
    assert out["reachable"] is True
    assert out["moving"] == {"cpu0": True, "cpu1": False, "cpu2": False}
    assert "cpu3" not in out["moving"]          # no-heartbeat sentinel excluded
    assert out["console_advancing"] is True


def case_collect_status_stale_latch_is_unknown_not_frozen():
    """THE regression: two reads of an UN-refreshed latch (identical values,
    health verb unreachable both times) must report liveness as unknown,
    never as every core FROZEN. This is the exact bug collect_status()'s
    docstring documents having shipped once already."""
    h = _mkhealth(hb_cpu0=1, hb_cpu1=1, hb_cpu2=1, cons_bytes=100)
    fake = _FakeBMC(raw_seq=[h, dict(h)], text_seq=["", ""])   # not fresh
    with _patched(_THIS, "_bmc", lambda iface="br0": fake), \
         _patched(time, "sleep", lambda *_a, **_k: None):
        out = collect_status()
    assert out["reachable"] is True             # the memory read itself worked
    assert out["moving"] is None                # NOT {"cpu0": False, ...}
    assert out["console_advancing"] is None


def case_collect_status_health_verb_exception_is_unknown_not_frozen():
    """Same regression, via the other route into "not fresh": health_text()
    itself raising (e.g. a transient console timeout), not just returning an
    empty string."""
    h = _mkhealth(hb_cpu0=7, hb_cpu1=7, hb_cpu2=7)
    fake = _FakeBMC(raw_seq=[h, dict(h)], text_seq=[TimeoutError, TimeoutError])
    with _patched(_THIS, "_bmc", lambda iface="br0": fake), \
         _patched(time, "sleep", lambda *_a, **_k: None):
        out = collect_status()
    assert out["moving"] is None
    assert out["reachable"] is True


def case_collect_status_never_raises_on_construction_failure():
    def _boom(iface="br0"):
        raise OSError("no such device br0")
    with _patched(_THIS, "_bmc", _boom):
        out = collect_status()
    assert out["reachable"] is False
    assert "OSError" in out["error"]


def case_collect_status_never_raises_on_missing_record():
    fake = _FakeBMC(raw_seq=[None, None], text_seq=["", ""])
    with _patched(_THIS, "_bmc", lambda iface="br0": fake), \
         _patched(time, "sleep", lambda *_a, **_k: None):
        out = collect_status()
    assert out["reachable"] is False
    assert "no BMC1 record" in out["error"]


def case_collect_status_never_raises_on_arbitrary_exception():
    class _Boom(_FakeBMC):
        def health_raw(self):
            raise RuntimeError("boom")
    with _patched(_THIS, "_bmc", lambda iface="br0": _Boom([], [])), \
         _patched(time, "sleep", lambda *_a, **_k: None):
        out = collect_status()
    assert out["reachable"] is False
    assert "RuntimeError" in out["error"]


# ---- cmd_status(): idle-vs-frozen classification --------------------------
def case_cmd_status_cpu2_idle_not_reported_frozen():
    s = _mkstatus(moving={"cpu0": True, "cpu1": True, "cpu2": False})
    with _patched(_THIS, "collect_status", lambda iface="br0": s), \
         _patched(boot_ledger, "stats", lambda: {}):
        _rc, text = _captured(cmd_status, argparse.Namespace(iface="br0"))
    assert "idle=cpu2" in text
    assert "FROZEN" not in text


def case_cmd_status_cpu0_frozen_is_flagged():
    s = _mkstatus(moving={"cpu0": False, "cpu1": True, "cpu2": True})
    with _patched(_THIS, "collect_status", lambda iface="br0": s), \
         _patched(boot_ledger, "stats", lambda: {}):
        _rc, text = _captured(cmd_status, argparse.Namespace(iface="br0"))
    assert "FROZEN=cpu0" in text


def case_cmd_status_stale_latch_prints_unknown_not_a_verdict():
    s = _mkstatus(moving=None)
    with _patched(_THIS, "collect_status", lambda iface="br0": s), \
         _patched(boot_ledger, "stats", lambda: {}):
        _rc, text = _captured(cmd_status, argparse.Namespace(iface="br0"))
    assert "unknown" in text
    assert "FROZEN" not in text


def case_cmd_status_unreachable_hints_at_uboot():
    s = _mkstatus(reachable=False, error="OSError: no such device", health=None,
                   moving=None)
    with _patched(_THIS, "collect_status", lambda iface="br0": s):
        rc, text = _captured(cmd_status, argparse.Namespace(iface="br0"))
    assert rc == 1
    assert "UNREACHABLE" in text


# ---- render(): degraded-data HTML paths -----------------------------------
def case_render_unreachable_shows_error_not_crash():
    s = _mkstatus(reachable=False, error="OSError: no such device", health=None,
                   moving=None)
    out = render(s)
    assert "unreachable" in out
    assert "OSError" in out


def case_render_stale_latch_does_not_paint_cores_green_or_red():
    out = render(_mkstatus(moving=None))
    assert "stale latch" in out
    assert 'pill ok"' not in out
    assert 'pill bad"' not in out


def case_render_no_heartbeats_dict_shows_pill():
    out = render(_mkstatus(moving={}))
    assert "no heartbeats" in out


def case_render_core_pills_match_idle_vs_frozen_convention():
    out = render(_mkstatus(moving={"cpu0": True, "cpu1": False, "cpu2": False}))
    assert 'pill ok">cpu0' in out
    assert 'pill bad">cpu1' in out       # not cpu2 -> genuinely frozen
    assert 'pill warn">cpu2' in out      # cpu2 idle is expected, not alarming


def case_render_battery_present_no_readings_not_shown_as_zero_mv():
    out = render(_mkstatus(health=_mkhealth(axp_ok=1, vbat_mv=0, ichg_ma=0,
                                             idischg_ma=0, batt_ts_mv=0)))
    assert "no readings" in out
    assert "0 mV" not in out


def case_render_battery_real_readings_shown():
    out = render(_mkstatus(health=_mkhealth(axp_ok=1, vbat_mv=3950, ichg_ma=250,
                                             idischg_ma=0, batt_ts_mv=1480)))
    assert "3950" in out


def case_render_console_state_labels():
    for val, want in ((None, "unknown"), (True, "advancing"), (False, "quiet")):
        out = render(_mkstatus(console_advancing=val))
        assert want in out, f"console_advancing={val!r} -> missing {want!r}"


_ST_CASES = [
    ("collect_status_moving_from_two_fresh_reads",
     case_collect_status_moving_from_two_fresh_reads),
    ("collect_status_stale_latch_is_unknown_not_frozen",
     case_collect_status_stale_latch_is_unknown_not_frozen),
    ("collect_status_health_verb_exception_is_unknown_not_frozen",
     case_collect_status_health_verb_exception_is_unknown_not_frozen),
    ("collect_status_never_raises_on_construction_failure",
     case_collect_status_never_raises_on_construction_failure),
    ("collect_status_never_raises_on_missing_record",
     case_collect_status_never_raises_on_missing_record),
    ("collect_status_never_raises_on_arbitrary_exception",
     case_collect_status_never_raises_on_arbitrary_exception),
    ("cmd_status_cpu2_idle_not_reported_frozen",
     case_cmd_status_cpu2_idle_not_reported_frozen),
    ("cmd_status_cpu0_frozen_is_flagged",
     case_cmd_status_cpu0_frozen_is_flagged),
    ("cmd_status_stale_latch_prints_unknown_not_a_verdict",
     case_cmd_status_stale_latch_prints_unknown_not_a_verdict),
    ("cmd_status_unreachable_hints_at_uboot",
     case_cmd_status_unreachable_hints_at_uboot),
    ("render_unreachable_shows_error_not_crash",
     case_render_unreachable_shows_error_not_crash),
    ("render_stale_latch_does_not_paint_cores_green_or_red",
     case_render_stale_latch_does_not_paint_cores_green_or_red),
    ("render_no_heartbeats_dict_shows_pill",
     case_render_no_heartbeats_dict_shows_pill),
    ("render_core_pills_match_idle_vs_frozen_convention",
     case_render_core_pills_match_idle_vs_frozen_convention),
    ("render_battery_present_no_readings_not_shown_as_zero_mv",
     case_render_battery_present_no_readings_not_shown_as_zero_mv),
    ("render_battery_real_readings_shown",
     case_render_battery_real_readings_shown),
    ("render_console_state_labels", case_render_console_state_labels),
]


def cmd_selftest(_args):
    passed = failed = 0
    for name, fn in _ST_CASES:
        print(f"[ RUN ] {name}")
        try:
            fn()
        except Exception:
            failed += 1
            print(f"[FAIL ] {name}")
            traceback.print_exc()
            continue
        passed += 1
        print(f"[ OK  ] {name}")
    n = passed + failed
    print(f"---- bzdctl selftest: {passed}/{n} passed ----")
    return 0 if failed == 0 else 1


# ── CLI ──────────────────────────────────────────────────────────────────
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="bzdctl.py",
        description="bzdOS Chimp board lifecycle: power, boot, console, crash")
    ap.add_argument("--iface", default="br0", help="EMAC debug interface")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status", help="one-screen board state (EMAC only)")

    h = sub.add_parser("health", help="BMC health record")
    h.add_argument("--raw", action="store_true",
                   help="decode the latched BMC1 breadcrumb (works mid-wedge)")

    p = sub.add_parser("power", help="reset / watchdog / U-Boot capture")
    p.add_argument("action",
                   choices=["reset", "hold", "release", "arm", "disarm", "uboot"])
    p.add_argument("--timeout", type=int, default=600,
                   help="for `uboot`: how long to wait for a watchdog cycle")

    c = sub.add_parser("console", help="guest console capture / inject")
    c.add_argument("-n", type=lambda x: int(x, 0), default=0)
    c.add_argument("--follow", action="store_true", help="poll the TX tee")
    c.add_argument("--inject", metavar="TEXT", help="push TEXT into guest RX")

    bw = sub.add_parser("boot-watch", help="wait for liveness, record in ledger")
    bw.add_argument("--timeout", type=int, default=300)

    cr = sub.add_parser("crash", help="collect a crash bundle")
    cr.add_argument("--out", help="output directory")
    cr.add_argument("--coredump", type=float, default=0,
                    help="also wait N seconds for a guest coredump push")

    sub.add_parser("ledger", help="cumulative clean-boot statistics")

    sv = sub.add_parser("serve", help="read-only web dashboard (EMAC only)")
    sv.add_argument("--port", type=int, default=8088)
    sv.add_argument("--bind", default="127.0.0.1")

    sub.add_parser("selftest", help="offline unit tests for bzdctl's own "
                   "logic -- no board, no network")

    args = ap.parse_args(argv)
    fn = {
        "status": cmd_status, "health": cmd_health, "power": cmd_power,
        "console": cmd_console, "boot-watch": cmd_boot_watch,
        "crash": cmd_crash, "ledger": cmd_ledger, "serve": cmd_serve,
        "selftest": cmd_selftest,
    }[args.cmd]
    return fn(args)


if __name__ == "__main__":
    sys.exit(main())
