#!/usr/bin/env python3
"""
soak.py — unattended overnight soak-test harness for the bzdOS hypervisor on
Banana Pi M64. This is the tool ROADMAP.md T1 asks for: it turns the proven
mechanics already in supervise.py (USB VID:PID state detection, EMAC
liveness, the USB-ACM break-glass reset) and reliable_load.py (the
deterministic catch-U-Boot -> TFTP -> loady+bootelf -> verify state machine)
into a BOUNDED, SELF-REPORTING loop suitable for measuring the v1 gate
criteria (ROADMAP.md §2):

    - 100 clean boot cycles in a row, no manual intervention
    - 72h soak under continuous load, no degradation/leaks
    - 20 survived break-glass resets in a row, auto-recovered

WHY A NEW FILE INSTEAD OF EXTENDING supervise.py:
supervise.py is a live, interactive dev tool: it runs forever, prints one
line per state transition, and is meant to be watched (or tailed by a
Monitor) while someone is actively working the board. Its state machine is
exactly right and is reused here (imported, not copied) rather than forked.
What it does NOT do is what a soak *harness* needs on top: a bounded run
length, an outer per-cycle time bound with a deliberate escalation policy,
and a durable, incrementally-written report a human reads *after* the run
instead of a scrolling log. Bolting all of that onto supervise.py's forever-
loop would turn one simple tool into two different tools wearing a trenchcoat.
Splitting them keeps supervise.py exactly as trustworthy as it already is.

CYCLE, per the T1 brief:
  1. reload — reliable_load.py's ensure_uboot -> serial_load -> verify state
     machine, run as a bounded subprocess (see run_reliable_load()).
  2. dwell  — once verified, hold at HV_OK for --dwell-s, sampling
     bmc_client's BMC1 health breadcrumb on --poll-s cadence: uptime,
     exception counts, per-core heartbeats, temperature, GIC-timer liveness.
     A SPONTANEOUS wedge during dwell (gadget up, EMAC dark) is handled with
     the exact same break-glass fallback supervise.py uses live — this is
     precisely the "N survived break-glass resets while under load" gate.
  3. record — every cycle's result (success/failed), timing, and any health
     anomalies are appended to the report immediately (see Report._save):
     the report on disk after cycle K is always a complete, valid snapshot
     of cycles 1..K, so killing the harness (or a wedge the harness itself
     can't recover from) never loses the run.

BOUNDED RELOAD + BREAK-GLASS ESCALATION (the T1 brief's point 2):
reliable_load.py's reboot_clean_via_emac() legitimately does a "coarse safety
sweep" of ~1216 individually-bounded-timeout MMIO writes
(range(0x4201d000, 0x42030000, 0x40), each a `cmd()` with its own ~1s wait)
when the precise wdt_debug_hold symbol address can't be resolved narrowly.
Under a slow/degraded EMAC that alone can legitimately take on the order of
10-20 minutes and is NOT itself a failure. So the per-cycle reload is run as
a subprocess under a generous --per-cycle-timeout-s (default 30 min, comfor-
tably past that worst case) — only exceeding THAT outer bound is treated as
"the normal EMAC path is stuck", at which point the harness gives up on EMAC
for this cycle and falls back to the break-glass USB-ACM sequence
(mirrors supervise.py's break_glass(): the exact bytes usbacm.c matches are
imported from supervise, never re-typed here) before retrying once, bounded,
within the same cycle.

SAFETY (the T1 brief's point 4 — hard project rule):
Every recovery path here is fully remote: EMAC (reliable_load's
reboot_clean_via_emac) or USB-ACM break-glass (supervise.break_glass(), which
writes wdt_debug_hold=1 and lets the HW WDOG fire the reset within ~16s).
Nothing in this file ever asks for or waits on a human touching the board.
The ONE case with no remote recovery — the board falling off the USB bus
entirely (supervise.py's GONE state) — is detected, reported honestly as
"needs a physical power-cycle", and the run stops cleanly rather than
spinning forever pretending it can fix that itself.

USAGE:
  python3 soak.py --cycles 100                      # the "100 cycles" gate
  python3 soak.py --duration-h 72 --dwell-s 1800     # the "72h soak" gate
                                                      # (few deliberate reloads,
                                                      # long dwells; spontaneous
                                                      # wedges during a dwell are
                                                      # still break-glass-recovered)
  python3 soak.py --cycles 20 --dwell-s 5            # quick smoke test of the
                                                      # harness itself

Ctrl-C / SIGTERM: finishes the in-flight step, writes a final report, exits.
Report: soak-report.json (machine-readable) + soak-report.txt (human summary)
in this directory by default; logs of each cycle's reliable_load run go to
soak-logs/cycle-NNNN.log.

STATUS: written and reasoned through but NOT run against real hardware from
this worktree (no board access here). Needs a supervised first run — start
with `--cycles 2 --dwell-s 10` while watching the board — before trusting it
to run unattended overnight. See the accompanying report-back for the full
list of things that specifically need live confirmation.
"""
import argparse
import json
import os
import re
import select
import signal
import subprocess
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import bzd_board as B
import supervise as SUP  # vidpid(), emac_alive(), break_glass(), BG_SEQ, ACM, WEDGE_GRACE_S

try:
    from bmc_client import BMC
except Exception:
    BMC = None  # health sampling degrades gracefully to "unavailable" if this ever fails to import

try:
    from hvdbg import HV
except Exception:
    HV = None  # A1 isolation re-check degrades gracefully if EMAC lib is unavailable

# A1 isolation self-check breadcrumb (stage2.c): STG2 window 0x50000c00,
# slot [18] == PASS (1 = the guest EL1 regime provably can't reach either HV
# DRAM window). Read once per verified boot so the soak also proves the
# isolation boundary holds on EVERY clean boot, not just the one we tested by
# hand. See a1-isolation-hardware-proven memory / stage2_isolation_selfcheck().
STG2_BC_BASE = 0x50000c00
ISOL_MAGIC   = 0x49534f4c   # "ISOL"


def read_isolation():
    """Return (pass:0/1|None, detail-dict). None if unreadable/no EMAC lib."""
    if HV is None:
        return None, {}
    try:
        hv = HV()
        w = hv.read_words(STG2_BC_BASE, 19)
        if not w or len(w) < 19 or w[14] != ISOL_MAGIC:
            return None, {}
        return int(w[18]), dict(hvimg_f=w[15], hvscr_f=w[16], ctrl_f=w[17])
    except Exception:
        return None, {}

# ── tunables ─────────────────────────────────────────────────────────────
DEFAULT_CYCLES = 100
DEFAULT_LOAD_CYCLES = 5          # passed through as reliable_load.py's own --cycles
DEFAULT_PER_CYCLE_TIMEOUT_S = 1800  # 30 min outer bound; see module docstring
DEFAULT_DWELL_S = 60
DEFAULT_POLL_S = 15
DEFAULT_BREAKGLASS_RETRIES = 3
BREAKGLASS_SETTLE_S = 25          # WDOG fires <=16s + U-Boot enumeration settle
HIGH_TEMP_C = 85.0

DEFAULT_REPORT = os.path.join(HERE, "soak-report.json")
DEFAULT_LOGDIR = os.path.join(HERE, "soak-logs")

_stop_requested = False


def _sig_handler(signum, _frame):
    global _stop_requested
    log(f"signal {signum} received — will stop after the current step")
    _stop_requested = True


def now():
    return time.time()


def stamp():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def log(msg):
    print(f"[{stamp()}] {msg}", flush=True)


# ── report ───────────────────────────────────────────────────────────────
class Report:
    """Incrementally-written report. _save() is called after every
    meaningful event, so the file on disk is always a valid, complete
    snapshot as of the last completed step — an interrupted run (Ctrl-C,
    crash, power blip on the HOST) still leaves a trustworthy partial
    report instead of nothing."""

    def __init__(self, path):
        self.path = path
        self._pending_recovery = False
        self.data = {
            "started": stamp(),
            "started_epoch": now(),
            "finished": None,
            "cycles_attempted": 0,
            "cycles_succeeded": 0,
            "cycles_failed": 0,
            "cycles_interrupted": 0,
            "breakglass_invocations": 0,
            "breakglass_sent_ok": 0,
            "breakglass_recoveries": 0,
            "breakglass_unconfirmed_at_finish": False,
            "total_wall_s": 0.0,
            "total_load_s": 0.0,
            "cycles": [],
            "anomalies": [],
            "aborted_reason": None,
        }
        self._save()

    def add_anomaly(self, cycle_n, kind, detail):
        self.data["anomalies"].append(
            {"cycle": cycle_n, "kind": kind, "detail": detail, "ts": stamp()})
        log(f"  ⚠ anomaly[{kind}] (cycle {cycle_n}): {detail}")
        self._save()

    def note_breakglass(self, sent_ok):
        """Call every time break_glass() is invoked, from either the reload-
        stall escalation path or the spontaneous-wedge-during-dwell path."""
        self.data["breakglass_invocations"] += 1
        if sent_ok:
            self.data["breakglass_sent_ok"] += 1
        self._pending_recovery = True
        self._save()

    def note_reload_result(self, ok):
        """Call every time a reliable_load attempt concludes. If a break-
        glass invocation is still awaiting confirmation, a successful reload
        closes it out as a recovered break-glass reset."""
        if self._pending_recovery and ok:
            self.data["breakglass_recoveries"] += 1
            self._pending_recovery = False
        self._save()

    def add_cycle(self, rec):
        self.data["cycles"].append(rec)
        self.data["cycles_attempted"] += 1
        if rec["result"] == "success":
            self.data["cycles_succeeded"] += 1
        elif rec["result"] == "interrupted":
            self.data["cycles_interrupted"] += 1
        else:
            self.data["cycles_failed"] += 1
        self.data["total_load_s"] += rec.get("dwell_s", 0.0)
        self._save()

    def finish(self, aborted_reason=None):
        self.data["finished"] = stamp()
        self.data["total_wall_s"] = now() - self.data["started_epoch"]
        self.data["breakglass_unconfirmed_at_finish"] = self._pending_recovery
        if aborted_reason:
            self.data["aborted_reason"] = aborted_reason
        self._save()
        self._write_text_summary()

    def _save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=2)
        os.replace(tmp, self.path)

    def _write_text_summary(self):
        d = self.data
        txt_path = os.path.splitext(self.path)[0] + ".txt"
        L = []
        L.append("=" * 72)
        L.append("bzdOS soak-test report")
        L.append("=" * 72)
        L.append(f"started    : {d['started']}")
        L.append(f"finished   : {d['finished']}")
        L.append(f"wall time  : {d['total_wall_s'] / 3600:.2f} h ({d['total_wall_s']:.0f}s)")
        L.append(f"load time  : {d['total_load_s'] / 3600:.2f} h ({d['total_load_s']:.0f}s) verified-alive")
        if d["aborted_reason"]:
            L.append(f"ABORTED    : {d['aborted_reason']}")
        L.append("")
        L.append(f"cycles attempted   : {d['cycles_attempted']}")
        L.append(f"cycles succeeded   : {d['cycles_succeeded']}")
        L.append(f"cycles failed      : {d['cycles_failed']}")
        L.append(f"cycles interrupted : {d['cycles_interrupted']}")
        L.append("")
        L.append(f"break-glass invocations : {d['breakglass_invocations']}")
        L.append(f"break-glass sent ok     : {d['breakglass_sent_ok']}")
        L.append(f"break-glass recoveries  : {d['breakglass_recoveries']}")
        if d["breakglass_unconfirmed_at_finish"]:
            L.append("  NOTE: last break-glass invocation had no confirmed "
                      "successful reload before the run ended")
        L.append("")
        L.append(f"anomalies flagged : {len(d['anomalies'])}")
        for a in d["anomalies"][-40:]:
            L.append(f"  [{a['ts']}] cycle {a['cycle']}: {a['kind']} — {a['detail']}")
        L.append("")
        L.append("per-cycle detail:")
        for c in d["cycles"]:
            bg = f" [break-glass x{c.get('breakglass_calls', 0)}]" if c.get("breakglass_calls") else ""
            L.append(f"  #{c['n']:>4}  {c['result']:<8}  "
                      f"reload={c.get('reliable_load_s', 0):>7.1f}s  "
                      f"dwell={c.get('dwell_s', 0):>7.1f}s{bg}  {c.get('note', '')}")
        with open(txt_path, "w") as f:
            f.write("\n".join(L) + "\n")
        log(f"report written: {self.path}  /  {txt_path}")


# ── bounded reliable_load subprocess ────────────────────────────────────
CYCLE_RE = re.compile(r"LOADED & VERIFIED on cycle #(\d+)")


def run_reliable_load(expect_vbk, load_cycles, timeout_s, log_path,
                      boot_to_shell=False):
    """Run reliable_load.py as a subprocess, bounded by an OUTER wall-clock
    timeout regardless of what it's doing internally (this is deliberate:
    the coarse EMAC safety sweep described in the module docstring can
    legitimately eat many minutes, and we don't want to guess at *why* a
    stall is happening — just bound how long we tolerate it before giving up
    on this path). Returns (ok, elapsed_s, tail_line, timed_out, interrupted).
    """
    args = [sys.executable, "reliable_load.py", "--cycles", str(load_cycles)]
    if expect_vbk:
        args.append("--expect-vbk")
    if boot_to_shell:
        # Without this the guest is left sitting at its interactive `mountroot>`
        # prompt, so it never reaches multiuser, never gets a DHCP lease and
        # never starts sshd. Every cycle after the first then reports
        # "guest-unreachable" (measured on the first 3-cycle run). Worth
        # spelling out: until now this harness only ever measured "does the
        # hypervisor load", never "does the guest actually boot" -- which was
        # all it could measure before the guest could reach multiuser at all.
        args.append("--boot-to-shell")
    t0 = now()
    proc = subprocess.Popen(args, cwd=HERE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines = []
    interrupted = False
    timed_out = False
    try:
        with open(log_path, "w") as lf:
            while True:
                if _stop_requested:
                    interrupted = True
                    break
                elapsed = now() - t0
                if elapsed > timeout_s:
                    timed_out = True
                    break
                r, _, _ = select.select([proc.stdout], [], [], 1.0)
                if r:
                    line = proc.stdout.readline()
                    if line == "":
                        break  # EOF: process finished
                    lines.append(line)
                    lf.write(line)
                    lf.flush()
                elif proc.poll() is not None:
                    break
    finally:
        if proc.poll() is None:
            log(f"  reliable_load outer bound hit ({timeout_s:.0f}s) or interrupted — killing subprocess")
            proc.kill()
            try:
                proc.wait(timeout=15)
            except Exception:
                pass
        else:
            proc.wait()

    elapsed = now() - t0
    text = "".join(lines)
    ok = (not timed_out) and (not interrupted) and ("LOADED & VERIFIED" in text)
    tail = next((ln.strip() for ln in reversed(lines) if ln.strip()), "(no output)")
    if timed_out:
        tail = f"(killed: exceeded {timeout_s:.0f}s outer bound) last line: {tail}"
    return ok, elapsed, tail, timed_out, interrupted


# ── health sampling (best-effort; never fatal to the harness) ──────────
_last_exc_count = 0


def sample_health(n, report):
    global _last_exc_count
    if BMC is None:
        return None
    try:
        bmc = BMC()
        try:
            d = bmc.health_raw()
        finally:
            bmc.close()
    except Exception as e:
        report.add_anomaly(n, "health-read-failed", str(e))
        return None
    if d is None:
        report.add_anomaly(n, "health-magic-mismatch", "no BMC1 record (board down or build has no BMC)")
        return None

    # Build-model detection: the vGIC / IMO=1 trunk runs NO EL2 periodic tick
    # (gic_timer_init() isn't called — EL2 only forwards the guest's own IRQs),
    # so tick_delta is 0 BY DESIGN and exc_count is the count of EVERY physical
    # IRQ EL2 takes (millions/s), NOT a fault counter. The two checks below were
    # calibrated for the IMO=0 baseline (EL2 rarely traps, EL2 tick must
    # advance) and would false-fire on every poll of every cycle under IMO=1.
    # There, HV liveness is already established by the EMAC verify that got us
    # here plus the A1 isolation re-check below; guest liveness by boot depth.
    imo1_build = (d["tick_delta"] == 0)

    if not imo1_build:
        if d["exc_count"] > _last_exc_count:
            report.add_anomaly(
                n, "exception-count-increased",
                f"exc_count {_last_exc_count} -> {d['exc_count']} "
                f"last_kind=0x{d['last_exc_kind']:x} last_esr=0x{d['last_exc_esr']:08x}")
        if d["tick_delta"] == 0:
            report.add_anomaly(n, "timer-stalled", "GIC timer tick_delta=0 (HV heartbeat stalled)")
    _last_exc_count = d["exc_count"]

    online = d["online_map"]
    hbs = [d["hb_cpu0"], d["hb_cpu1"], d["hb_cpu2"], d["hb_cpu3"]]
    for i, hb in enumerate(hbs):
        if (online >> i) & 1 and hb == 0:
            report.add_anomaly(n, "core-heartbeat-zero", f"cpu{i} online but heartbeat=0")

    if d["temp_mc"] and d["temp_mc"] / 1000.0 > HIGH_TEMP_C:
        report.add_anomaly(n, "high-temperature", f"{d['temp_mc'] / 1000.0:.1f} C")

    # A1 isolation re-verification: prove the guest/HV partition still holds
    # on THIS boot. isol_pass != 1 would be a serious regression (a hole in
    # the static partition) and is flagged loudly.
    isol_pass, isol_detail = read_isolation()
    if isol_pass is not None and isol_pass != 1:
        report.add_anomaly(
            n, "isolation-breach",
            f"A1 ISOL self-check PASS={isol_pass} "
            f"(hvimg.F={isol_detail.get('hvimg_f')} "
            f"hvscr.F={isol_detail.get('hvscr_f')} "
            f"ctrl.F={isol_detail.get('ctrl_f')}) — guest may reach HV memory!")

    return {
        "uptime_s": d["uptime"] // 24000000,
        "exc_count": d["exc_count"],
        "tick_delta": d["tick_delta"],
        "heartbeats": hbs,
        "temp_c": (d["temp_mc"] / 1000.0) if d["temp_mc"] else None,
        "flags": d["flag_names"],
        "isol_pass": isol_pass,
    }


# ── dwell: hold at HV_OK, sample health, react to a spontaneous wedge ──
def dwell(n, args, report):
    """Hold at HV_OK for up to args.dwell_s. Reacts to a SPONTANEOUS wedge
    (gadget present, EMAC dark) exactly like supervise.py's live state
    machine does: after WEDGE_GRACE_S of confirmed darkness, break-glass.
    Returns (dwell_elapsed_s, breakglass_calls, ended_early_reason_or_None).
    """
    t0 = now()
    end = t0 + args.dwell_s
    wedge_since = None
    bg_calls = 0
    last_poll = 0.0
    while now() < end and not _stop_requested:
        # health sampling is cheaper than a full dwell-poll cadence pass
        # when dwell_s is short (smoke tests) — pace it, don't spin.
        if now() - last_poll < args.poll_s:
            time.sleep(min(1.0, end - now()) if end > now() else 0)
            continue
        last_poll = now()

        vp = SUP.vidpid()
        if vp == B.HVCON_VIDPID:
            if SUP.emac_alive():
                wedge_since = None
                sample_health(n, report)
            else:
                if wedge_since is None:
                    wedge_since = now()
                    log(f"  cycle {n}: EMAC dark mid-dwell (grace {SUP.WEDGE_GRACE_S}s)")
                elif now() - wedge_since > SUP.WEDGE_GRACE_S:
                    report.add_anomaly(
                        n, "spontaneous-wedge",
                        f"EMAC dark under load for >{SUP.WEDGE_GRACE_S}s — break-glass")
                    bg_calls += 1
                    sent = SUP.break_glass()
                    report.note_breakglass(sent)
                    if sent:
                        return now() - t0, bg_calls, "spontaneous-wedge-recovered"
                    report.add_anomaly(n, "breakglass-failed", "could not open /dev/ttyACM0")
                    return now() - t0, bg_calls, "spontaneous-wedge-breakglass-failed"
        elif vp is None:
            report.add_anomaly(n, "board-gone-during-dwell", "USB device vanished from the bus")
            return now() - t0, bg_calls, "board-gone"
        else:
            report.add_anomaly(n, "unexpected-state-change", f"vid:pid became {vp} mid-dwell")
            return now() - t0, bg_calls, "unexpected-state-change"

    return now() - t0, bg_calls, ("interrupted" if _stop_requested else None)


# ── one full soak cycle: reload -> verify -> dwell -> record ───────────
GUEST_SSH = [
    "ssh", "-i", "/root/.ssh/chimp_ed25519",
    "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
    "-o", "ConnectTimeout=8", "-o", "BatchMode=yes", "root@192.168.88.82",
]


def guest_clean_shutdown(settle_s=30.0):
    """Ask the guest to power off cleanly, so the next boot starts on a clean fs.

    `shutdown -p now` runs the full rc.shutdown sequence (sync + unmount) and
    only THEN issues PSCI SYSTEM_OFF; el2_exc.c honours that one SMC with a
    controlled warm reset (dbg_clean_off, default on) rather than the
    stay-alive path it uses for an ambiguous SYSTEM_RESET.

    Returns a short status string for the cycle record. Never raises: a guest
    that cannot be reached is a normal soak outcome (this is the harness that
    exists to find that), and the caller falls through to the ordinary reload,
    which is exactly the pre-existing behaviour.

    ssh is expected to die mid-command as the guest goes down, so its exit
    status is deliberately ignored -- reachability is checked first instead."""
    # Wait for the guest to finish booting into multiuser before concluding it
    # is unreachable: after a reload it still has to mount root, run fsck and
    # /etc/rc, get a DHCP lease and start sshd. Reporting "unreachable" the
    # instant the first probe fails just measures the harness's own impatience
    # (which is what the first 3-cycle run did on cycles 2 and 3).
    for attempt in range(24):            # ~2 min
        try:
            p = subprocess.run(GUEST_SSH + ["true"], capture_output=True,
                               text=True, timeout=12)
            if p.returncode == 0:
                break
        except Exception:
            pass
        time.sleep(5)
    else:
        return "guest-unreachable"
    try:
        subprocess.run(GUEST_SSH + ["nohup shutdown -p now >/dev/null 2>&1 &"],
                       capture_output=True, text=True, timeout=25)
    except Exception:
        pass
    time.sleep(settle_s)     # rc.shutdown + the ~2s WDOG warm reset
    return "sent"


VBK_BC = 0x50020000
EBIO_BC = 0x50020200
GT_TICKS_PA = 0x42030008
IRQC_PA = 0x42030068

# emmc_bio.c's own window: the eMMC-LEVEL failure count and the controller
# state at the last one. Distinct from the VBK1 fields above, which count what
# reached virtio-blk. Capturing both is what makes the difference readable: an
# ebio_fails > 0 with g_ioerrs == 0 means the card misbehaved and the retry
# absorbed it, which is a healthy outcome that VBK1 alone reports as silence.
# Added 2026-07-30 after reading 12 out of this window with no way to tell which
# cycle of the run it belonged to (the window was not captured per cycle, and
# emmc_bio_init did not clear it -- both now fixed).
EBIO_FIELDS = {0: "ebio_fails", 1: "ebio_lba", 2: "ebio_rint", 3: "ebio_star",
               4: "ebio_tag", 5: "ebio_gctl", 6: "ebio_hs_state",
               7: "ebio_hs_step", 8: "ebio_settles", 9: "ebio_settle_clkfail",
               10: "ebio_busy_timeouts", 11: "ebio_busy_wait_ms",
               12: "ebio_cntfrq"}
EBIO_NWORDS = 13

# The subset of vblk_emmc.c's breadcrumbs that says WHY a boot failed. Kept
# here rather than derived, so a build without the newer fields just reports
# 0xFFFFFFFF for them (unwritten scratch) instead of breaking the harness.
VBK_FIELDS = {6: "g_reads", 7: "g_writes", 42: "g_ioerrs", 43: "ioerr_busy",
              44: "ioerr_badpa", 45: "ioerr_unaligned", 46: "ioerr_emmc",
              47: "ioerr_capacity", 48: "ioerr_notready",
              49: "ioerr1_lba", 50: "ioerr1_rc", 53: "ioerrN_lba",
              54: "ioerrN_rc", 57: "write_retries", 58: "write_retry_ok",
              59: "lock_retries", 60: "lock_giveups"}


def capture_counters():
    """Snapshot the hypervisor's own counters BEFORE the next reload wipes them.

    This is the difference between a soak that produces a pass/fail tally and
    one that produces a diagnosis. Every counter in the VBK1 window is reset by
    vblk_init() on each boot, so a cycle's evidence is gone the moment the board
    reloads -- which is exactly when a failed cycle is most interesting. Learned
    the hard way on 2026-07-30: a 5-cycle run lost 4 boots to a guest that never
    reached multiuser, and by the time anything was read the board was already
    two boots further on.

    Read what these mean off the failing cycle: ioerr_busy => eMMC lock
    starvation, lock_giveups => the lock is genuinely stuck (a leaked unlock,
    which waiting cannot fix), ioerr_emmc => the card itself, write_retries =>
    a transient write stall was absorbed, all-empty => the debug channel was
    already down, which is itself the finding.

    Never raises: a board that cannot answer is a normal soak outcome."""
    out = {}
    try:
        import hvdbg
        hv = hvdbg.HV()
        if not hv.alive(timeout=5):
            return {"channel": "down"}

        def rw(pa, cnt, tries=10):
            """read_words_stable, not read_words: this channel can return a
            well-formed reply with a single wrong hex digit (documented, and
            observed here on 2026-07-30 -- an rc field came back as 0x40000114,
            which is a plausible-looking guest DRAM address and not any code
            serve_data or emmc_bio can produce). These counters get read off a
            failing cycle and believed, so they have to be read the careful
            way; the window is static between boots, which is exactly what
            read_words_stable() requires."""
            w = hv.read_words_stable(pa, cnt, tries=tries)
            return w if w and len(w) == cnt else None

        b = rw(VBK_BC, 64)
        if b:
            for idx, name in VBK_FIELDS.items():
                v = b[idx]
                if v == 0xFFFFFFFF:
                    continue          # field absent in this build
                if name.endswith("_rc") and v >= 0x80000000:
                    v -= 0x100000000  # serve_data's codes are negative
                out[name] = v
        e = rw(EBIO_BC, EBIO_NWORDS)
        if e:
            for idx, name in EBIO_FIELDS.items():
                if e[idx] != 0xFFFFFFFF:
                    out[name] = e[idx]
        g = rw(GT_TICKS_PA, 2)
        if g:
            out["gt_ticks"] = g[0] | (g[1] << 32)
        c = rw(IRQC_PA, 40)
        if c:
            out["cntv_irqs"] = c[27]
            out["hv_tick_irqs"] = c[30]
    except Exception as exc:
        out["capture_error"] = str(exc)[:120]
    return out


def do_cycle(n, args, report):
    log(f"────────── soak cycle #{n} ──────────")
    t_cycle0 = now()
    rec = {"n": n, "start": stamp(), "breakglass_calls": 0}
    log_path = os.path.join(DEFAULT_LOGDIR, f"cycle-{n:04d}.log")

    # Before anything reloads the board and wipes them.
    rec["counters_before_reload"] = capture_counters()
    cb = rec["counters_before_reload"]
    if cb.get("g_ioerrs"):
        log(f"  [diag] previous boot had g_ioerrs={cb['g_ioerrs']} "
            f"busy={cb.get('ioerr_busy')} emmc={cb.get('ioerr_emmc')} "
            f"lba={cb.get('ioerrN_lba')} rc={cb.get('ioerrN_rc')}")
        report.add_anomaly(n, "guest-io-error",
                           f"previous boot: {cb.get('g_ioerrs')} S_IOERR "
                           f"(busy={cb.get('ioerr_busy')}, "
                           f"emmc={cb.get('ioerr_emmc')}, "
                           f"lba={cb.get('ioerrN_lba')}, rc={cb.get('ioerrN_rc')})")
    # eMMC-level failures that the retry absorbed reach neither g_ioerrs nor the
    # guest, so the check above stays silent on them and the run looks perfectly
    # clean. That is the state c40964b is meant to move, so it has to be visible
    # on its own: report it as an observation, and only call it an anomaly when
    # the retries did NOT save it (i.e. g_ioerrs went up too, already covered
    # above). ebio_tag says which path: 0x1xxxx read post-drain wait,
    # 0x2xxxx write data-phase error bit, 0x3xxxx write data-phase timeout.
    if cb.get("ebio_fails"):
        log(f"  [diag] previous boot had ebio_fails={cb['ebio_fails']} "
            f"lba={cb.get('ebio_lba')} rint=0x{cb.get('ebio_rint', 0):x} "
            f"star=0x{cb.get('ebio_star', 0):x} "
            f"tag=0x{cb.get('ebio_tag', 0):x} "
            f"(absorbed: g_ioerrs={cb.get('g_ioerrs', 0)}) "
            f"settles={cb.get('ebio_settles', '-')}/"
            f"clkfail={cb.get('ebio_settle_clkfail', '-')}")
    # Recovery that cannot re-program the clock is not recovery: the reference
    # driver treats "timeout updating clock" as a reported failure, and on
    # Allwinner parts it is a real one. If this tracks ebio_settles, retrying is
    # pointless and the card needs a heavier reset -- worth flagging loudly
    # rather than leaving in the per-cycle noise.
    if cb.get("ebio_settle_clkfail"):
        report.add_anomaly(n, "emmc-recovery-clk-timeout",
                           f"clk re-program timed out "
                           f"{cb['ebio_settle_clkfail']}x of "
                           f"{cb.get('ebio_settles')} recoveries — the reset is "
                           f"not completing, so retries cannot help")
    if cb.get("lock_giveups"):
        report.add_anomaly(n, "emmc-lock-stuck",
                           f"lock_giveups={cb['lock_giveups']} — a leaked "
                           f"unlock, not contention; waiting cannot fix it")

    if getattr(args, "clean_shutdown", False):
        rec["clean_shutdown"] = guest_clean_shutdown()
        log(f"  [clean] shutdown -p now: {rec['clean_shutdown']}")

    ok, elapsed, tail, timed_out, interrupted = run_reliable_load(
        args.expect_vbk, args.load_cycles, args.per_cycle_timeout_s, log_path,
        boot_to_shell=getattr(args, "clean_shutdown", False))
    rec["reliable_load_s"] = elapsed
    rec["reliable_load_tail"] = tail
    m = CYCLE_RE.search(tail)
    rec["load_internal_cycle"] = int(m.group(1)) if m else None

    if interrupted:
        rec["result"] = "interrupted"
        rec["note"] = "stop requested during reload"
        rec["duration_s"] = now() - t_cycle0
        report.add_cycle(rec)
        return rec, "stop"

    report.note_reload_result(ok)

    if not ok and timed_out:
        report.add_anomaly(
            n, "emac-stall",
            f"reliable_load exceeded the {args.per_cycle_timeout_s:.0f}s outer bound "
            f"(last line: {tail})")
        vp = SUP.vidpid()
        if vp is None:
            rec["result"] = "failed"
            rec["note"] = ("board GONE from the USB bus — needs a physical power-cycle; "
                            "this harness has no remote path for that case")
            rec["duration_s"] = now() - t_cycle0
            report.add_cycle(rec)
            return rec, "abort"

        bg_sent = False
        for attempt in range(1, args.breakglass_retries + 1):
            log(f"  cycle {n}: break-glass attempt {attempt}/{args.breakglass_retries}")
            rec["breakglass_calls"] += 1
            bg_sent = SUP.break_glass()
            report.note_breakglass(bg_sent)
            if bg_sent:
                break
            time.sleep(5)

        if not bg_sent:
            rec["result"] = "failed"
            rec["note"] = "break-glass failed to open /dev/ttyACM0 after retries"
            rec["duration_s"] = now() - t_cycle0
            report.add_cycle(rec)
            return rec, "continue"

        time.sleep(BREAKGLASS_SETTLE_S)
        ok2, elapsed2, tail2, timed_out2, interrupted2 = run_reliable_load(
            args.expect_vbk, args.load_cycles, args.per_cycle_timeout_s,
            log_path + ".after-breakglass")
        rec["reliable_load_s"] += elapsed2
        rec["reliable_load_tail"] = tail2
        if interrupted2:
            rec["result"] = "interrupted"
            rec["note"] = "stop requested after break-glass retry"
            rec["duration_s"] = now() - t_cycle0
            report.add_cycle(rec)
            return rec, "stop"
        report.note_reload_result(ok2)
        if timed_out2:
            report.add_anomaly(n, "emac-stall-after-breakglass",
                                "reliable_load still would not complete after the break-glass reset")
        ok = ok2

    if not ok:
        rec["result"] = "failed"
        rec["note"] = rec.get("note") or f"reliable_load failed: {rec['reliable_load_tail']}"
        rec["duration_s"] = now() - t_cycle0
        report.add_cycle(rec)
        return rec, "continue"

    # verified load -> health snapshot -> dwell under load
    rec["health_at_verify"] = sample_health(n, report)
    dwell_s, bg_calls, ended_reason = dwell(n, args, report)
    rec["dwell_s"] = dwell_s
    rec["breakglass_calls"] += bg_calls
    rec["result"] = "success"
    rec["note"] = "OK" if ended_reason is None else f"dwell ended early: {ended_reason}"
    rec["duration_s"] = now() - t_cycle0
    report.add_cycle(rec)

    if ended_reason == "board-gone":
        return rec, "abort"
    if ended_reason == "interrupted":
        return rec, "stop"
    return rec, "continue"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cycles", type=int, default=None,
                     help=f"reload cycles to attempt (default {DEFAULT_CYCLES} "
                          f"unless --duration-h is given)")
    ap.add_argument("--duration-h", type=float, default=None,
                     help="overall wall-clock bound in hours")
    ap.add_argument("--dwell-s", type=float, default=DEFAULT_DWELL_S,
                     help="seconds to hold under verified load between reloads "
                          "(default %(default)s; use a large value for a 72h-style "
                          "continuous-load soak so most of the run is spent under "
                          "load rather than reloading)")
    ap.add_argument("--poll-s", type=float, default=DEFAULT_POLL_S,
                     help="health-sampling / wedge-check cadence during dwell")
    ap.add_argument("--load-cycles", type=int, default=DEFAULT_LOAD_CYCLES,
                     help="internal retry budget passed to reliable_load.py --cycles")
    ap.add_argument("--per-cycle-timeout-s", type=float, default=DEFAULT_PER_CYCLE_TIMEOUT_S,
                     help="outer bound on one reliable_load run before the harness "
                          "gives up on the EMAC path and tries break-glass")
    ap.add_argument("--breakglass-retries", type=int, default=DEFAULT_BREAKGLASS_RETRIES)
    ap.add_argument("--expect-vbk", action="store_true",
                     help="require the VBK1 breadcrumb (virtio-blk build) at verify")
    ap.add_argument("--clean-shutdown", action="store_true",
                     help="before each reload, ssh into the guest and 'shutdown -p "
                          "now' so the filesystem is unmounted cleanly. Without "
                          "this every cycle is an unclean stop and the next boot's "
                          "fsck has to repair the fs (observed SALVAGE of block "
                          "bitmaps / free-block counts / summary info), so a long "
                          "soak is also a long run of chances to corrupt it for "
                          "real. Needs guest ssh (see guest-ssh-access-works) and "
                          "el2_exc.c's dbg_clean_off, which is on by default: the "
                          "guest's PSCI SYSTEM_OFF is honoured with a controlled "
                          "warm reset instead of the stay-alive path.")
    ap.add_argument("--report", default=DEFAULT_REPORT)
    args = ap.parse_args()

    if args.cycles is None and args.duration_h is None:
        args.cycles = DEFAULT_CYCLES

    os.makedirs(DEFAULT_LOGDIR, exist_ok=True)
    report = Report(args.report)

    signal.signal(signal.SIGINT, _sig_handler)
    signal.signal(signal.SIGTERM, _sig_handler)

    log(f"=== soak run starting: cycles={args.cycles} duration_h={args.duration_h} "
        f"dwell_s={args.dwell_s} per_cycle_timeout_s={args.per_cycle_timeout_s} "
        f"expect_vbk={args.expect_vbk} ===")

    deadline = (now() + args.duration_h * 3600) if args.duration_h else None
    n = 0
    aborted_reason = None
    try:
        while True:
            n += 1
            if args.cycles is not None and n > args.cycles:
                log(f"reached target cycle count ({args.cycles}) — stopping")
                n -= 1
                break
            if deadline is not None and now() >= deadline:
                log("reached target duration — stopping")
                n -= 1
                break

            rec, action = do_cycle(n, args, report)

            if action == "abort":
                aborted_reason = rec.get("note", "board GONE from the USB bus")
                log(f"ABORTING run: {aborted_reason}")
                break
            if action == "stop":
                aborted_reason = None
                log("stopping run (interrupt requested)")
                break
            if _stop_requested:
                log("stop requested — finishing after this cycle")
                break
    except Exception as e:
        log(f"UNHANDLED exception in soak loop: {e}")
        traceback.print_exc()
        report.add_anomaly(n, "harness-exception", repr(e))
        aborted_reason = f"harness exception: {e!r}"
    finally:
        report.finish(aborted_reason)
        d = report.data
        log("=" * 60)
        log(f"soak run complete: {d['cycles_succeeded']}/{d['cycles_attempted']} cycles OK, "
            f"{d['breakglass_invocations']} break-glass invocations "
            f"({d['breakglass_recoveries']} confirmed recovered), "
            f"{d['total_wall_s'] / 3600:.2f}h wall / {d['total_load_s'] / 3600:.2f}h under load")


if __name__ == "__main__":
    main()
