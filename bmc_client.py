#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
bmc_client.py — host-side CLI for the bzdOS "software BMC" management plane.

This is the clean, named front end to the BMC verbs implemented in bmc.c. It
rides the EXACT SAME EMAC raw-Ethernet console (ethertype 0x88B5, board MAC
02:bd:05:00:00:01) that hvdbg.py/dbgmon already use — it simply subclasses HV
so all the low-level framing, draining and the reboot_clean() reset path come
for free. Every subcommand sends a `bmc <verb> ...` line and renders the reply.

    ./bmc_client.py health                 # structured status record
    ./bmc_client.py health --raw           # parse the BMC1 breadcrumb directly
    ./bmc_client.py temp
    ./bmc_client.py battery                # AXP803 VBAT/IBAT/charge status (RSB)
    ./bmc_client.py flags                  # list debug flags
    ./bmc_client.py flag usbacm 0          # (auto-arms) set a debug flag
    ./bmc_client.py console                # dump the guest console capture ring
    ./bmc_client.py console --stream       # follow live guest output (TX tee)
    ./bmc_client.py inject "mount -a"      # type a line into the guest console
    ./bmc_client.py reset                  # (auto-arms) clean reboot to U-Boot
    ./bmc_client.py wdt hold               # (auto-arms) let the HW WDOG reset
    ./bmc_client.py dump 0x50006000 32     # raw breadcrumb read (via dbgmon)

Design note: destructive verbs (reset/flag/wdt hold|arm) require a two-step
ARM on the board (see bmc.c's safety gate). This client performs the arm
automatically right before the destructive send, so the human types one
command — the on-board window (~10 s, one-shot) still protects against a
stray/replayed frame firing a reset on its own.
"""
import contextlib, io, sys, time, struct, argparse, traceback

# Reuse the whole EMAC transport + reset machinery from hvdbg.py.
from hvdbg import HV

BMC_HEALTH_BASE = 0x50006000
BMC_HEALTH_MAGIC = 0x424D4331  # "BMC1"

# struct bmc_health field order (must match bmc.h). Names only; each is one
# 32-bit word except the lo/hi pairs which we recombine below.
# v1.1 appends AXP803 battery telemetry (vbat_mv..axp_ok) — these fill what
# was reserved padding in v1.0, so a v1.0-only board just reads zeros here.
# v1.2 appends eMMC wear telemetry (emmc_pre_eol_info..emmc_wear_ok) the
# same way, filling what was reserved padding in v1.1.
HEALTH_WORDS = [
    "magic", "version", "uptime_lo", "uptime_hi", "tick_lo", "tick_hi",
    "tick_delta", "exc_count", "last_exc_kind", "last_exc_esr",
    "guest_pc_lo", "guest_pc_hi", "online_map",
    "hb_cpu0", "hb_cpu1", "hb_cpu2", "hb_cpu3",
    "cons_bytes", "cons_faults", "temp_mc", "flags", "wdt_hold", "ffv_count",
    "vbat_mv", "ichg_ma", "idischg_ma", "batt_ts_mv", "batt_status", "axp_ok",
    "emmc_pre_eol_info", "emmc_life_est_a", "emmc_life_est_b", "emmc_wear_ok",
]

FLAG_BITS = [
    ("usbacm", 0), ("core_enable", 1), ("cpu1_wdog", 2),
    ("isolate_noemac", 3), ("no_guest", 4), ("block_reset", 5),
]

# BMC_BATT_* bits (axp803.h) — decoded from health["batt_status"].
BATT_STATUS_BITS = [
    ("present", 0), ("charging", 1), ("vbus", 2), ("die_hot", 3), ("chip_ok", 4),
]


class BMC(HV):
    """Software-BMC client: thin verb wrappers over the shared HV EMAC link."""

    # ── core verb helpers ───────────────────────────────────────────────
    def bmc(self, verb, wait=3):
        """Send one `bmc <verb>` line, return the text reply."""
        return self.cmd("bmc " + verb, wait)

    def arm(self, nonce=None):
        """Arm the on-board destructive-verb gate. Uses a time-based nonce so
        each arm is distinct. Returns True if the board acknowledged."""
        if nonce is None:
            nonce = int(time.time()) & 0xffffffff
        r = self.bmc(f"arm 0x{nonce:x}", 2)
        return "armed" in r

    # ── domain verbs ────────────────────────────────────────────────────
    def health_text(self):
        return self.bmc("health", 3)

    def health_fresh(self):
        """(health_dict, fresh) — the record, plus whether it was just rebuilt.

        USE THIS, not health_raw(), for anything that decides whether the
        board is alive or advancing.

        The BMC1 window in DRAM is a *snapshot*: CPU1 recomputes and
        republishes it when the `health` verb is serviced, and at no other
        time. health_raw() is a plain memory read, so on a board nobody is
        talking to it returns the same bytes forever — including the same
        uptime and the same per-core heartbeats. Any "did it advance?" loop
        built on health_raw() alone therefore concludes "dead" about a
        perfectly healthy board, every time.

        That is not hypothetical: `bzdctl boot-watch` was written that way and
        reported "TIMED OUT with no advancing heartbeat" against a board whose
        independent CPU1 counter was visibly climbing (2026-08-20). Verified
        both ways -- three health_raw() reads 3 s apart returned a byte-identical
        record; three health_text()+health_raw() pairs advanced uptime,
        hb_cpu0 and hb_cpu1 every time.

        `fresh` is False when the refresh verb did not answer. That case is
        still useful -- after a crash the verb dies but the last snapshot
        survives, which is exactly why health_raw() exists -- so this returns
        the stale record rather than None, and lets the caller say so.
        """
        try:
            fresh = "BMC" in (self.health_text() or "")
        except Exception:
            fresh = False
        return self.health_raw(), fresh

    def health_raw(self):
        """Read + decode the BMC1 breadcrumb straight out of DRAM (works even
        if the console text path is flaky — it's just a memory read).

        NOTE: this does NOT refresh the record; see health_fresh() above
        before using it to judge liveness."""
        words = self.read_words(BMC_HEALTH_BASE, len(HEALTH_WORDS))
        if len(words) < len(HEALTH_WORDS):
            return None
        d = dict(zip(HEALTH_WORDS, words))
        if d["magic"] != BMC_HEALTH_MAGIC:
            return None
        d["uptime"] = (d["uptime_hi"] << 32) | d["uptime_lo"]
        d["ticks"] = (d["tick_hi"] << 32) | d["tick_lo"]
        d["guest_pc"] = (d["guest_pc_hi"] << 32) | d["guest_pc_lo"]
        d["flag_names"] = [n for (n, b) in FLAG_BITS if d["flags"] & (1 << b)]
        d["batt_status_names"] = [n for (n, b) in BATT_STATUS_BITS
                                   if d["batt_status"] & (1 << b)]
        return d

    def temp(self):
        return self.bmc("temp", 2)

    def battery(self):
        return self.bmc("battery", 2)

    def flags(self):
        return self.bmc("flags", 2)

    def set_flag(self, name, val):
        self.arm()
        return self.bmc(f"flag {name} 0x{int(val):x}", 2)

    def console(self, n=0):
        return self.bmc(f"con read {n:x}" if n else "con read", 4)

    def postmortem(self, n=0, off=None):
        """Console text of the run BEFORE this one (the postmortem carry-over).

        This is the one that answers "what did it print before the board
        died?", because a crash bad enough to need an HV reload is a crash
        whose evidence the reload itself would otherwise destroy — the
        hypervisor now copies the ring aside on startup instead. See
        hv_addrmap.h's VCPM lane comment.

        off=None gives the TAIL (the last n bytes), which is almost always
        what you want; pass off to page through the whole carry-over, since
        the firmware caps one reply at 4 KiB to keep its service loop bounded.
        """
        if off is None:
            return self.bmc(f"con pm {n:x}" if n else "con pm", 4)
        return self.bmc(f"con pmat {off:x} {n:x}" if n
                        else f"con pmat {off:x}", 4)

    def inject(self, text):
        return self.bmc(f"con inject {text}", 2)

    def tee(self, n=0):
        return self.bmc(f"con tee {n:x}" if n else "con tee", 2)

    def reset(self):
        self.arm()
        # reboot_clean never returns; the link just drops. Short wait is fine.
        return self.bmc("reset", 1)

    def wdt(self, sub):
        if sub in ("hold", "arm"):
            self.arm()
        return self.bmc(f"wdt {sub}", 2)


# ── pretty printers ─────────────────────────────────────────────────────
def _avail(v, bits=32):
    """None if the field is an all-ones 'never written' sentinel, else v.

    Every breadcrumb window in this tree is left as 0xff.. until its owner
    writes it, and several of the fields latched into the BMC1 record have no
    owner in the current build at all: the EXC window is not written by this
    build (triage.py says so in as many words -- "do NOT read the all-ones as
    data"), CPU3 idles in WFI and never posts a heartbeat, and the tick counter
    was reading 0xffffffffffffffff.

    Printing those verbatim made the management plane report `timer STALLED`,
    `exc_count=4294967295` and a dead CPU3 on a completely healthy board. A BMC
    that cries wolf is worse than no BMC, because the one time it is right
    nobody looks. Distinguish "no data" from a measurement."""
    return None if v is None or v == (1 << bits) - 1 else v


def _fmt(v, unit=""):
    return "n/a" if v is None else f"{v}{unit}"


def print_health(d, motion=None):
    """motion: optional {'exc': bool, 'console': bool} from TWO samples.

    Without it, a frozen counter and a live one print identically -- and these
    breadcrumbs live in DRAM that SURVIVES A WARM RESET, so a scary
    last_esr/FAR can easily belong to a previous boot generation. That has
    caused real misdiagnosis in this project (2026-08-21: a second guest vCPU
    was read as being in a 22-million-exception fault storm; the counter had
    not moved in five seconds and the record was from the boot before). When
    the caller has sampled twice, say which it is."""
    if not d:
        print("no BMC1 record (board down, or magic mismatch)")
        return
    up_s = d["uptime"] // 24000000  # A64 arch timer 24 MHz -> seconds (approx)
    print(f"BMC health  v{d['version'] >> 16}.{d['version'] & 0xffff}")
    print(f"  uptime      ~{up_s}s  (cnt=0x{d['uptime']:x})")
    ticks = _avail(d["ticks"], 64)
    if ticks is None:
        # No tick counter in this build -- say so instead of inferring a stall
        # from a delta that was never computed from real data.
        print("  timer       n/a (this build publishes no tick counter)")
    else:
        print(f"  timer       ticks={ticks}  delta={d['tick_delta']}  "
              f"({'LIVE' if d['tick_delta'] else 'STALLED'})")
    print(f"  guest_pc    0x{d['guest_pc']:x}")
    exc = _avail(d["exc_count"])
    if exc is None:
        print("  exceptions  n/a (EXC breadcrumb not written by this build)")
    elif exc == 0:
        # A genuine, healthy zero (bmc.c's bmc_health_snapshot() reports 0
        # both for "never written" and for "written, zero recorded" -- either
        # way there IS no last fault). last_exc_kind==0 is itself the real
        # vector index for EL2_KIND_SYNC, so printing it here would read as
        # a SYNC exception that never happened -- same trap as the all-ones
        # sentinel this branch already guards against, just at value 0
        # instead of 0xffffffff.
        print("  exceptions  none recorded")
    else:
        adv = None if motion is None else motion.get("exc")
        if adv is None:
            tag = ""
        elif adv:
            tag = "  [CLIMBING -- live]"
        else:
            tag = "  [frozen -- HISTORICAL, may predate this boot]"
        print(f"  exceptions  count={exc}  last_kind=0x{d['last_exc_kind']:x}"
              f"  last_esr=0x{d['last_exc_esr']:08x}{tag}")
    hb = [_avail(d[f"hb_cpu{i}"]) for i in range(4)]
    # A core with no heartbeat is not necessarily faulty: CPU3 parks in WFI by
    # design and never posts one. Mark it "idle" rather than as a dead number.
    hb_s = " ".join("idle" if v is None else str(v) for v in hb)
    print(f"  cores       online=0x{d['online_map']:x}  hb=[{hb_s}]")
    print(f"  console     bytes={d['cons_bytes']}  faults={d['cons_faults']}  "
          f"ffv={d['ffv_count']}")
    t = d["temp_mc"]
    print(f"  temperature {'n/a' if t == 0 else f'{t/1000:.1f} C'}")
    if d.get("emmc_wear_ok"):
        eol = {0: "n/a", 1: "normal", 2: "WARNING (80%)", 3: "URGENT"}.get(
            d["emmc_pre_eol_info"], f"unknown({d['emmc_pre_eol_info']})")

        def life(v):
            # EXT_CSD's DEVICE_LIFE_TIME_EST fields are a BAND index, not a
            # direct percentage: 1 = 0-10% of the rated lifetime used, up to
            # 10 = 90-100%. Show the band, not a false-precision number.
            return "n/a" if v == 0 else f"{(v-1)*10}-{v*10}% used" if 1 <= v <= 10 else f"reserved({v})"

        print(f"  eMMC wear   pre_eol={eol}  "
              f"life_est_a={life(d['emmc_life_est_a'])}  "
              f"life_est_b={life(d['emmc_life_est_b'])}")
    else:
        print("  eMMC wear   not read (controller busy, or EXT_CSD unsupported)")
    print(f"  flags       0x{d['flags']:x}  {d['flag_names']}")
    print(f"  wdt_hold    {d['wdt_hold']}")
    if d.get("axp_ok"):
        # chip_ok with every reading at zero means the PMIC answered but no
        # battery telemetry came back -- typically no pack on the connector.
        # Reporting "vbat=0mV" as a measurement invites a hunt for a flat
        # battery that was never plugged in.
        if not any(d[k] for k in ("vbat_mv", "ichg_ma", "idischg_ma", "batt_ts_mv")):
            print(f"  battery     AXP803 present, no readings "
                  f"(no pack attached?)  {d['batt_status_names']}")
        else:
            print(f"  battery     vbat={d['vbat_mv']}mV  ichg={d['ichg_ma']}mA  "
                  f"idischg={d['idischg_ma']}mA  ts={d['batt_ts_mv']}mV(raw, not calibrated)  "
                  f"{d['batt_status_names']}")
    else:
        print("  battery     no AXP803 detected (RSB probe failed or absent)")


# ── selftest — pure, offline. No sockets, no board. ─────────────────────
#
# There were no tests at all for this file before this pass, despite three
# real bugs having already shipped and been fixed here (see the module and
# _avail() docstrings): read_words() silently breaking on any decimal count
# not a multiple of 4 (so `health --raw`'s 29-word ask never once worked),
# all-ones "never written" sentinels printed as measurements (STALLED timer,
# exc_count=4294967295, a "dead" CPU3 that parks in WFI by design, vbat=0mV
# for an absent battery), and (in bzdctl.py) a stale LATCH read twice being
# mistaken for a live sample. This suite pins down the parts of THIS file
# that are pure functions over plain data -- _avail()'s sentinel handling,
# health_raw()'s decode/reject logic, and print_health()'s degraded-data
# text -- with the three historical bugs as explicit regression cases so
# they cannot come back silently. `bzdctl.py selftest` covers the
# latch/freshness logic that lives in bzdctl.py itself.
#
# `python3 bmc_client.py selftest` (see main()); wired into `make test`.


class _FakeWordsBMC(BMC):
    """A BMC that answers read_words() from a canned list instead of a
    socket -- overriding __init__ means HV.__init__ (which opens a real
    AF_PACKET socket bound to an interface) never runs. Exercises the REAL
    health_raw() decode logic against synthetic DRAM content; no network, no
    hardware, matches the "no board" discipline the rest of this project's
    selftests already follow (coredump-recv.py, snapshot_net.py)."""

    def __init__(self, words):
        self._words = list(words)          # deliberately skip HV.__init__

    def read_words(self, pa, n):
        return list(self._words[:n]) if len(self._words) >= n else []


def _mkwords(**overrides):
    """One full, self-consistent HEALTH_WORDS-ordered word list for a
    plausible healthy board, with any field overridden by name. Defaults
    reflect the POST-firmware-fix contract (see bmc.c bmc_health_snapshot()):
    tick_* is always the retired all-ones sentinel, exc_count/last_exc_kind/
    last_exc_esr default to a genuine 0 (no exceptions recorded), hb_cpu3 is
    the "no such heartbeat" sentinel (parks in WFI by design)."""
    base = dict(
        magic=BMC_HEALTH_MAGIC, version=(1 << 16) | 1,
        uptime_lo=24000000 * 3600, uptime_hi=0,     # ~1h uptime
        tick_lo=0xffffffff, tick_hi=0xffffffff, tick_delta=0xffffffff,
        exc_count=0, last_exc_kind=0, last_exc_esr=0,
        guest_pc_lo=0x00001234, guest_pc_hi=0,
        online_map=0xf,
        hb_cpu0=10, hb_cpu1=20, hb_cpu2=5, hb_cpu3=0xffffffff,
        cons_bytes=500, cons_faults=0, temp_mc=35000,
        flags=0, wdt_hold=0, ffv_count=0,
        vbat_mv=4000, ichg_ma=100, idischg_ma=0, batt_ts_mv=1500,
        batt_status=0x1, axp_ok=1,
        emmc_pre_eol_info=1, emmc_life_est_a=2, emmc_life_est_b=0,
        emmc_wear_ok=1,
    )
    base.update(overrides)
    return [base[name] for name in HEALTH_WORDS]


def _captured(fn, *a, **kw):
    """Run fn(*a, **kw), return (result, everything it printed to stdout)."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        result = fn(*a, **kw)
    return result, buf.getvalue()


# ---- _avail(): sentinel-vs-measurement ----------------------------------
def case_avail_32bit_sentinel_is_none():
    assert _avail(0xffffffff) is None


def case_avail_64bit_sentinel_is_none():
    assert _avail((1 << 64) - 1, 64) is None


def case_avail_passthrough_for_real_values():
    assert _avail(0) == 0
    assert _avail(42) == 42
    assert _avail(0xfffffffe) == 0xfffffffe        # one below the sentinel


def case_avail_boundary_64bit_not_sentinel():
    # One below the 64-bit all-ones sentinel must NOT be treated as "no data"
    # -- a regression here would mean a real near-max tick count gets
    # silently hidden as "n/a".
    assert _avail((1 << 64) - 2, 64) == (1 << 64) - 2


# ---- health_raw(): decode / reject ---------------------------------------
def case_health_raw_rejects_bad_magic():
    bmc = _FakeWordsBMC(_mkwords(magic=0xdeadbeef))
    assert bmc.health_raw() is None


def case_health_raw_rejects_short_read():
    bmc = _FakeWordsBMC(_mkwords()[:-1])           # one word short
    assert bmc.health_raw() is None


def case_health_raw_decodes_wide_fields_and_flags():
    words = _mkwords(uptime_lo=0x11111111, uptime_hi=0x2,
                      guest_pc_lo=0x33333333, guest_pc_hi=0x4,
                      flags=(1 << 1) | (1 << 4),          # core_enable, no_guest
                      batt_status=(1 << 0) | (1 << 1))    # present, charging
    d = _FakeWordsBMC(words).health_raw()
    assert d is not None
    assert d["uptime"] == (0x2 << 32) | 0x11111111
    assert d["guest_pc"] == (0x4 << 32) | 0x33333333
    assert sorted(d["flag_names"]) == ["core_enable", "no_guest"]
    assert sorted(d["batt_status_names"]) == ["charging", "present"]


# ---- print_health(): the three historical "crying wolf" bugs -------------
def case_print_health_no_record_does_not_crash():
    _out, text = _captured(print_health, None)
    assert "no BMC1 record" in text


def case_print_health_exc_sentinel_hidden_as_na():
    """Regression for bug #2: a pre-fix board (or any board whose EXC1
    breadcrumb genuinely never got magic-gated firmware-side) that reports
    the raw all-ones sentinel must show "n/a", never the literal number."""
    d = _FakeWordsBMC(_mkwords(exc_count=0xffffffff)).health_raw()
    _out, text = _captured(print_health, d)
    assert "4294967295" not in text
    assert "exceptions" in text and "n/a" in text


def case_print_health_exc_zero_is_none_recorded_not_sync():
    """Post firmware-fix, exc_count==0 is a genuine measurement (no
    exceptions since boot) -- must not be confused with the sentinel path,
    and must not print last_kind=0x0 as if a real SYNC fault happened."""
    d = _FakeWordsBMC(_mkwords(exc_count=0, last_exc_kind=0,
                                last_exc_esr=0)).health_raw()
    _out, text = _captured(print_health, d)
    assert "exceptions" in text
    assert "last_kind" not in text            # nothing to show for "no fault"


def case_print_health_exc_nonzero_shows_last_fault():
    d = _FakeWordsBMC(_mkwords(exc_count=3, last_exc_kind=1,
                                last_exc_esr=0x96000010)).health_raw()
    _out, text = _captured(print_health, d)
    assert "count=3" in text
    assert "96000010" in text.lower()


def case_print_health_cpu3_idle_not_dead():
    """Regression for bug #2: CPU3 parks in WFI by design and never posts a
    heartbeat -- the all-ones sentinel must render as "idle", never as the
    literal 4294967295 that trained this project to distrust its own BMC."""
    d = _FakeWordsBMC(_mkwords(hb_cpu3=0xffffffff)).health_raw()
    _out, text = _captured(print_health, d)
    assert "4294967295" not in text
    assert "idle" in text


def case_print_health_tick_sentinel_is_na_not_stalled():
    """Regression for bug #2 (and the later firmware finding that the tick
    field was retired outright): the sentinel must read "n/a", never a
    confident-but-wrong "STALLED" (or, worse, "LIVE")."""
    d = _FakeWordsBMC(_mkwords(tick_lo=0xffffffff, tick_hi=0xffffffff,
                                tick_delta=0xffffffff)).health_raw()
    _out, text = _captured(print_health, d)
    assert "STALLED" not in text and "LIVE" not in text
    assert "n/a" in text


def case_print_health_battery_present_no_readings():
    """Regression for bug #2: chip_ok with every reading at zero is "present,
    no pack attached", never a measured "vbat=0mV"."""
    d = _FakeWordsBMC(_mkwords(axp_ok=1, vbat_mv=0, ichg_ma=0,
                                idischg_ma=0, batt_ts_mv=0)).health_raw()
    _out, text = _captured(print_health, d)
    assert "0mV" not in text and "0 mV" not in text
    assert "no readings" in text


def case_print_health_battery_real_readings_shown():
    d = _FakeWordsBMC(_mkwords(axp_ok=1, vbat_mv=3950, ichg_ma=250,
                                idischg_ma=0, batt_ts_mv=1480)).health_raw()
    _out, text = _captured(print_health, d)
    assert "3950" in text and "250" in text


def case_print_health_no_axp_detected():
    d = _FakeWordsBMC(_mkwords(axp_ok=0)).health_raw()
    _out, text = _captured(print_health, d)
    assert "no AXP803 detected" in text


def case_print_health_emmc_wear_shown():
    d = _FakeWordsBMC(_mkwords(emmc_wear_ok=1, emmc_pre_eol_info=2,
                                emmc_life_est_a=3, emmc_life_est_b=0)).health_raw()
    _out, text = _captured(print_health, d)
    assert "WARNING" in text          # pre_eol_info=2
    assert "20-30%" in text           # life_est_a=3 is a BAND, not "3%"
    assert "n/a" in text              # life_est_b=0


def case_print_health_emmc_wear_not_read():
    """emmc_wear_ok=0 must say so plainly, never print stale/zeroed fields
    as if they were a real (if boring) reading -- same trap this module
    already avoids for battery telemetry (case_print_health_no_axp_detected)."""
    d = _FakeWordsBMC(_mkwords(emmc_wear_ok=0, emmc_pre_eol_info=0,
                                emmc_life_est_a=0, emmc_life_est_b=0)).health_raw()
    _out, text = _captured(print_health, d)
    assert "not read" in text
    assert "pre_eol" not in text


_ST_CASES = [
    ("avail_32bit_sentinel_is_none",         case_avail_32bit_sentinel_is_none),
    ("avail_64bit_sentinel_is_none",         case_avail_64bit_sentinel_is_none),
    ("avail_passthrough_for_real_values",    case_avail_passthrough_for_real_values),
    ("avail_boundary_64bit_not_sentinel",    case_avail_boundary_64bit_not_sentinel),
    ("health_raw_rejects_bad_magic",         case_health_raw_rejects_bad_magic),
    ("health_raw_rejects_short_read",        case_health_raw_rejects_short_read),
    ("health_raw_decodes_wide_fields_and_flags",
     case_health_raw_decodes_wide_fields_and_flags),
    ("print_health_no_record_does_not_crash",
     case_print_health_no_record_does_not_crash),
    ("print_health_exc_sentinel_hidden_as_na",
     case_print_health_exc_sentinel_hidden_as_na),
    ("print_health_exc_zero_is_none_recorded_not_sync",
     case_print_health_exc_zero_is_none_recorded_not_sync),
    ("print_health_exc_nonzero_shows_last_fault",
     case_print_health_exc_nonzero_shows_last_fault),
    ("print_health_cpu3_idle_not_dead",      case_print_health_cpu3_idle_not_dead),
    ("print_health_tick_sentinel_is_na_not_stalled",
     case_print_health_tick_sentinel_is_na_not_stalled),
    ("print_health_battery_present_no_readings",
     case_print_health_battery_present_no_readings),
    ("print_health_battery_real_readings_shown",
     case_print_health_battery_real_readings_shown),
    ("print_health_no_axp_detected",         case_print_health_no_axp_detected),
    ("print_health_emmc_wear_shown",         case_print_health_emmc_wear_shown),
    ("print_health_emmc_wear_not_read",      case_print_health_emmc_wear_not_read),
]


def _cli_selftest(_args):
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
    print(f"---- bmc_client selftest: {passed}/{n} passed ----")
    return 0 if failed == 0 else 1


# ── CLI ─────────────────────────────────────────────────────────────────
def main(argv):
    if argv and argv[0] == "selftest":
        return _cli_selftest(argv[1:])
    ap = argparse.ArgumentParser(description="bzdOS software-BMC client")
    ap.add_argument("--iface", default="br0")
    sub = ap.add_subparsers(dest="cmd", required=True)

    h = sub.add_parser("health", help="structured status record")
    h.add_argument("--raw", action="store_true",
                   help="decode the BMC1 breadcrumb directly (memory read)")
    sub.add_parser("temp", help="SoC temperature")
    sub.add_parser("battery", help="AXP803 battery telemetry (VBAT/IBAT/status)")
    sub.add_parser("flags", help="list debug flags")
    f = sub.add_parser("flag", help="set a debug flag (auto-arms)")
    f.add_argument("name"); f.add_argument("value", type=int)
    c = sub.add_parser("console", help="dump / stream the guest console")
    c.add_argument("-n", type=lambda x: int(x, 0), default=0)
    c.add_argument("--stream", action="store_true", help="follow live TX tee")
    i = sub.add_parser("inject", help="type a line into the guest console")
    i.add_argument("text")
    sub.add_parser("reset", help="clean reboot to U-Boot (auto-arms)")
    w = sub.add_parser("wdt", help="watchdog control")
    w.add_argument("sub", choices=["hold", "release", "arm", "disarm"])
    d = sub.add_parser("dump", help="raw physical read (via dbgmon)")
    d.add_argument("addr", type=lambda x: int(x, 0))
    d.add_argument("count", type=lambda x: int(x, 0), default=16, nargs="?")

    args = ap.parse_args(argv)
    bmc = BMC(iface=args.iface)
    try:
        if args.cmd == "health":
            if args.raw:
                print_health(bmc.health_raw())
            else:
                print(bmc.health_text())
        elif args.cmd == "temp":
            print(bmc.temp())
        elif args.cmd == "battery":
            print(bmc.battery())
        elif args.cmd == "flags":
            print(bmc.flags())
        elif args.cmd == "flag":
            print(bmc.set_flag(args.name, args.value))
        elif args.cmd == "console":
            if args.stream:
                print("streaming guest console (Ctrl-C to stop)...")
                try:
                    while True:
                        out = bmc.tee(256)
                        s = out.replace("(tee empty)", "").strip("\r\n ")
                        if s:
                            sys.stdout.write(s)
                            sys.stdout.flush()
                        time.sleep(0.1)
                except KeyboardInterrupt:
                    print()
            else:
                print(bmc.console(args.n))
        elif args.cmd == "inject":
            print(bmc.inject(args.text))
        elif args.cmd == "reset":
            print(bmc.reset())
            print("(link dropped — board resetting to U-Boot)")
        elif args.cmd == "wdt":
            print(bmc.wdt(args.sub))
        elif args.cmd == "dump":
            for i, w_ in enumerate(bmc.read_words(args.addr, args.count)):
                if i % 4 == 0:
                    sys.stdout.write(f"\n{args.addr + i*4:08x}:")
                sys.stdout.write(f" {w_:08x}")
            print()
    finally:
        bmc.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
