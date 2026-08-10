#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""soak72.py — the v1-gate 72-hour soak: sustained guest load, sampled health.

ROADMAP §2 asks for "72h soak under continuous disk load, no degradation or
leaks". This is the harness for that number, built to be STARTED AND LEFT
ALONE by someone who is not watching it:

    python3 soak72.py --hours 72

WHAT IT MEASURES (and why it is not soak.py)
--------------------------------------------
soak.py is the T1 reload harness: its unit of work is "reload the board and
dwell", and it closed the "100 clean boots" gate. It never asks the GUEST to do
anything, so it cannot say whether a *running* system degrades. This harness
inverts that: the board is reloaded only when something forces it to be, and
the run is spent with the guest under a real mixed disk/CPU/metadata load while
its health is sampled. The measured quantity is CUMULATIVE HEALTHY LOAD TIME,
not wall time — time the harness could not verify does not count toward 72 h.

BENIGN VS REAL (the part that makes it trustworthy)
--------------------------------------------------
This project has a list of alarming-looking-but-normal events, and a harness
that flags them is a harness whose owner learns to ignore it. They live in
soaklib.classify_sample() with their justifications; the two that matter most
here: the hardware watchdog resetting the board to U-Boot is BY DESIGN
(SESSION-RULES R2) and so is recorded and recovered from rather than failing
the run, and a stale BMC1 latch means "motion unknown", never "frozen".

A reset is *counted*, and the final verdict reports it prominently — a 72 h
soak that needed nine resets is a different result from one that needed none,
and the report must not let those look the same.

RESUMABLE / IDEMPOTENT
----------------------
A 72-hour run WILL be interrupted. State lives in soak72-state.json (atomically
replaced) and every event is appended and fsync'd to soak72-events.jsonl. Re-run
the exact same command to resume: accumulated healthy hours, reset counts and
streaks carry over. `--restart` is the only way to zero them.

MID-RUN, FROM ANOTHER TERMINAL (never touches the board):
    python3 soak72.py --status
    tail -f soak72-events.jsonl

DRY RUN (no board at all; this is what CI runs):
    python3 soak72.py --dry-run

REUSED, NOT REIMPLEMENTED: bzdctl.collect_status (liveness), soak.py
(run_reliable_load / capture_counters / read_isolation), supervise.break_glass,
a2_cycle (guest ssh + fs_state), guest_sh (console), boot_ledger (the one
cumulative ledger). See soaklib.py's header for the full list.

UNVERIFIED: the real-hardware path of this file has NOT been run against the
board (it was written while another session owned it). Everything below the
facade is existing, board-proven code; the harness logic around it has been
exercised only against soaklib.FakeBoard. Start the first real run with
`--hours 0.2` and watch it before trusting a 72 h unattended run.
"""
import argparse
import json
import os
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import soaklib as L                                          # noqa: E402
from soaklib import (BENIGN, WARN, RESET, RECOVER, FAIL,      # noqa: E402
                     HarnessStop, EventLog, RunState, Clock, FakeClock)

DEFAULT_STATE = os.path.join(HERE, "soak72-state.json")
DEFAULT_EVENTS = os.path.join(HERE, "soak72-events.jsonl")
DEFAULT_LOGDIR = os.path.join(HERE, "soak72-logs")

_stop = {"flag": False}


def _sig(signum, _frame):
    _stop["flag"] = True
    print(f"\nsignal {signum}: will stop after this poll (state is on disk; "
          f"re-run the same command to resume)", flush=True)


# ── one health sample ────────────────────────────────────────────────────
def sample(board, prev, state, cfg):
    """Build one sample dict. Every field is best-effort: an unreadable field
    must degrade that check, never abort the run."""
    s = {"usb": board.usb()}
    st = board.status()
    s["status"] = st
    h = (st.get("health") or {}) if isinstance(st, dict) else {}
    s["uptime_s"] = (h.get("uptime") // 24_000_000) if h.get("uptime") else None
    s["temp_c"] = (h.get("temp_mc") / 1000.0) if h.get("temp_mc") else None
    s["exc_count"] = h.get("exc_count")

    if not st.get("reachable"):
        # Re-read the USB identity AFTER the (slow, ~2 s) health sample: the
        # board can flip to U-Boot between the two reads, and classifying that
        # as "EMAC flap" costs a whole poll interval before the reset is seen.
        s["usb"] = board.usb()

    if st.get("reachable"):
        state.d["emac_dark_since"] = None
        iso, _detail = board.isolation()
        s["isol_pass"] = iso
        s["counters"] = board.counters()
    else:
        since = state.d.get("emac_dark_since")
        if since is None:
            since = state.d["emac_dark_since"] = state.clock.now()
        s["emac_dark_s"] = state.clock.now() - since
        s["isol_pass"] = None
        s["counters"] = {}

    if st.get("reachable"):
        s["guest_alive"] = board.guest_alive()
        s["fs"] = board.guest_fs_state() if s["guest_alive"] else None
        s["load"] = board.guest_load_progress()
        s["sysinfo"] = board.guest_sysinfo()
    else:
        s["guest_alive"] = False
        s["fs"] = s["load"] = s["sysinfo"] = None

    # load-stall bookkeeping has to live across samples
    gen = (s["load"] or {}).get("gen")
    if gen is not None and gen != state.d.get("last_load_gen"):
        state.d["last_load_gen"] = gen
        state.d["last_load_gen_ts"] = state.clock.now()
    s["load_gen"] = gen
    lg_ts = state.d.get("last_load_gen_ts")
    s["load_stalled_s"] = (state.clock.now() - lg_ts) if lg_ts else 0.0
    s["avail_kb"] = (s["sysinfo"] or {}).get("avail_kb")
    return s


# ── recovery actions ─────────────────────────────────────────────────────
def do_reload(board, ev, state, cfg, why):
    """Reload the HV (catch U-Boot -> TFTP -> bootelf -> verify) and put the
    guest back to work.

    Escalation is BOUNDED and flat (a loop, not recursion): at most two reload
    attempts with one break-glass reset between them. Churning through reload
    attempts on a board that is not coming back is precisely what an unattended
    harness must not do — it produces a longer log and nothing else."""
    os.makedirs(cfg["logdir"], exist_ok=True)
    for attempt in (1, 2):
        n = state.bump("reloads")
        log_path = os.path.join(cfg["logdir"], f"reload-{n:04d}.log")
        ev.emit("reload-start", "info", why=why, attempt=attempt, log=log_path)
        ok, elapsed, tail, timed_out = board.reload(
            log_path, timeout_s=cfg["reload_timeout_s"], boot_to_shell=True,
            load_cycles=cfg["load_cycles"], expect_vbk=cfg["expect_vbk"])
        ev.emit("reload-done", "info" if ok else WARN, ok=ok, attempt=attempt,
                elapsed_s=round(elapsed, 1), timed_out=timed_out, detail=tail)
        if ok:
            L.ledger_record(True, "soak72-reload", note=why)
            return True
        if board.usb() == "gone":
            ev.emit("board-off-usb", FAIL,
                    detail="board is off the USB bus: the ONE failure with no "
                           "remote recovery path (supervise.py's GONE state)")
            raise HarnessStop("board is off the USB bus — stopping; no remote "
                              "path can bring it back.")
        if attempt == 1:
            ev.emit("breakglass-escalation", RESET,
                    detail="reload would not complete; sending the break-glass "
                           "reset before the one final retry")
            state.bump("breakglass_sent")
            board.break_glass()
            L.ledger_record(True, "soak72-breakglass",
                            note="break-glass sent to unstick a failed reload")
            state.clock.sleep(cfg["breakglass_settle_s"])
    raise HarnessStop(
        f"reload failed twice for {why} (last: {tail}), with a break-glass "
        f"reset in between. The board is not coming back on its own — "
        f"stopping. State is on disk: fix the board and re-run the same "
        f"command to resume.")


def do_guest_recovery(board, ev, state, cfg, why):
    """The standard post-unclean-stop recovery: fsck (up to two passes) ->
    mount rw -> netif -> sshd. Then restart the load."""
    n = state.bump("guest_recoveries")
    ev.emit("guest-recovery-start", RECOVER, why=why, attempt=n)
    rec = board.guest_recover(fsck_passes=cfg["fsck_passes"])
    ev.emit("guest-recovery-done", "info" if rec.get("ok") else FAIL,
            ok=rec.get("ok"), fsck_passes=len(rec.get("fsck", [])),
            repaired=rec.get("repaired"), still_dirty=rec.get("still_dirty"),
            detail=(rec.get("mount") or "")[:200])
    if rec.get("repaired"):
        state.bump("fsck_repairs")
    if len(rec.get("fsck", [])) > 1:
        # Documented normal outcome: STILL DIRTY on pass 1, clean on pass 2.
        state.bump("fsck_second_pass_needed")
        ev.emit("fsck-second-pass", BENIGN,
                detail="first fsck pass reported STILL DIRTY and the rerun "
                       "cleared it — a documented normal outcome here")
    if not rec.get("ok"):
        raise HarnessStop(
            "the standard recovery did NOT bring the guest back "
            f"(still_dirty={rec.get('still_dirty')}, "
            f"mount={(rec.get('mount') or '')[:120]!r}). Stopping rather than "
            f"soaking a broken guest.")
    if not board.guest_start_load(cfg["size_mb"], cfg["idle_s"]):
        raise HarnessStop("could not restart the guest-side load after recovery")
    ev.emit("load-restarted", "info")
    state.d["last_load_gen"] = None
    state.d["last_load_gen_ts"] = state.clock.now()
    state.save()
    return True


def handle_reset(board, ev, state, cfg, kind, detail):
    """A reset (watchdog, wedge, or one we had to cause) is by design and is
    recoverable — count it, recover, keep soaking."""
    state.bump("resets")
    state.bump("resets_" + kind.replace("-", "_"))
    ev.emit("reset-recorded", RESET, kind_detail=kind, detail=detail,
            resets_total=state.d["resets"])
    if state.d["resets"] > cfg["max_resets"]:
        raise HarnessStop(
            f"{state.d['resets']} resets in this run exceeds --max-resets "
            f"{cfg['max_resets']}. Each one is individually by design, but this "
            f"many means the board is not stable enough for the 72 h claim.")
    if kind == "wedge":
        if not board.break_glass():
            raise HarnessStop("break-glass could not be sent (tty unavailable)")
        state.bump("breakglass_sent")
        L.ledger_record(True, "soak72-breakglass",
                        note="EMAC dark under load; break-glass")
        state.clock.sleep(cfg["breakglass_settle_s"])
    do_reload(board, ev, state, cfg, why=kind)
    do_guest_recovery(board, ev, state, cfg, why="after-" + kind)
    state.d["emac_dark_since"] = None
    state.save()


# ── the run ──────────────────────────────────────────────────────────────
def run(board, clock, ev, state, cfg):
    target_s = cfg["hours"] * 3600.0
    ev.emit("run-start", "info",
            resumed=state.resumed, board=board.kind,
            load_h=round(state.d["load_s"] / 3600.0, 3),
            target_h=cfg["hours"], poll_s=cfg["poll_s"])

    prev = None
    # Idempotent start: if the load is already running (resumed run), leave it.
    lp = board.guest_load_progress()
    if not (lp and lp.get("running")):
        if board.usb() == "hv" and board.status().get("reachable"):
            if board.guest_start_load(cfg["size_mb"], cfg["idle_s"]):
                ev.emit("load-started", "info", size_mb=cfg["size_mb"],
                        idle_s=cfg["idle_s"])
            else:
                ev.emit("load-start-failed", WARN,
                        detail="could not start the load yet; the first poll "
                               "will classify why and recover")
    else:
        ev.emit("load-already-running", "info", gen=lp.get("gen"))

    while not _stop["flag"] and state.d["load_s"] < target_s:
        s = sample(board, prev, state, cfg)
        events = L.classify_sample(s, prev, cfg)
        for severity, kind, detail in events:
            ev.emit(kind, severity, detail=detail)
        sev = L.worst(events)
        state.bump("samples")

        fails = [(k, d) for sv, k, d in events if sv == FAIL]
        resets = [(k, d) for sv, k, d in events if sv == RESET]
        recovers = [(k, d) for sv, k, d in events if sv == RECOVER]

        if fails:
            k, d = fails[0]
            state.d["failures"] = state.d.get("failures", []) + [
                {"ts": clock.now(), "kind": k, "detail": d}]
            state.save()
            raise HarnessStop(f"SOAK FAILURE [{k}]: {d}")

        if resets:
            k, d = resets[0]
            handle_reset(board, ev, state, cfg,
                         "wedge" if k == "wedge" else "watchdog", d)
            prev = None                      # counters/uptime restart from zero
            state.d["last_healthy_ts"] = None
            state.save()
            continue

        if recovers:
            k, d = recovers[0]
            if k == "load-not-running":
                if not board.guest_start_load(cfg["size_mb"], cfg["idle_s"]):
                    raise HarnessStop("the guest-side load will not start")
                ev.emit("load-restarted", "info", detail=d)
                state.d["last_load_gen_ts"] = clock.now()
            else:
                do_guest_recovery(board, ev, state, cfg, why=k)
            state.save()
            continue

        # Healthy (or merely warned) sample: this is the time that counts.
        last = state.d.get("last_healthy_ts")
        if last is not None:
            # Capped so that time the harness was NOT running, or the board was
            # recovering, can never be credited to the 72 h.
            state.d["load_s"] += min(clock.now() - last, cfg["poll_s"] * 2.0)
        state.d["last_healthy_ts"] = clock.now()
        state.bump("healthy_samples")
        if sev == 1:
            state.bump("warn_samples")

        # Degradation tracking: generations-per-hour, early vs recent. "No
        # degradation" is a criterion, so it needs a number, not a vibe.
        g, t = s.get("load_gen"), clock.now()
        if g is not None:
            w = state.d.setdefault("gen_window", [])
            w.append([t, g])
            del w[:-120]
            if state.d.get("early_rate") is None and \
                    len(w) >= 2 and (w[-1][0] - w[0][0]) >= cfg["early_window_s"]:
                dt = w[-1][0] - w[0][0]
                state.d["early_rate"] = (w[-1][1] - w[0][1]) / (dt / 3600.0)
            if len(w) >= 2 and (w[-1][0] - w[0][0]) > 0:
                dt = w[-1][0] - w[0][0]
                state.d["recent_rate"] = (w[-1][1] - w[0][1]) / (dt / 3600.0)
        state.d["max_temp_c"] = max(state.d.get("max_temp_c") or 0.0,
                                    s.get("temp_c") or 0.0)
        if state.d["samples"] % cfg["progress_every"] == 0:
            ev.emit("progress", "info",
                    load_h=round(state.d["load_s"] / 3600.0, 3),
                    target_h=cfg["hours"], resets=state.d["resets"],
                    gen=s.get("load_gen"),
                    recent_gens_per_h=(round(state.d["recent_rate"], 1)
                                       if state.d.get("recent_rate") else None))
        state.save()
        prev = {"exc_count": s.get("exc_count"), "uptime_s": s.get("uptime_s"),
                "load_gen": s.get("load_gen"), "avail_kb": s.get("avail_kb"),
                "dmesg_errors": (s.get("sysinfo") or {}).get("dmesg_errors")}
        clock.sleep(cfg["poll_s"])

    return state.d["load_s"] >= target_s


def verdict(state, cfg, ev, ok, stopped_reason=None, show=True):
    d = state.d
    early, recent = d.get("early_rate"), d.get("recent_rate")
    degraded = (early and recent and recent < early * cfg["degrade_frac"])
    lines = [
        "bzdOS 72h SOAK — " + ("GATE CLOSED" if ok and not degraded else
                               "not closed"),
        f"healthy load time : {d['load_s'] / 3600.0:.2f} h of {cfg['hours']} h",
        f"samples           : {d['samples']} ({d.get('healthy_samples', 0)} healthy,"
        f" {d.get('warn_samples', 0)} with warnings)",
        f"resets (by design): {d['resets']}  "
        f"(watchdog={d.get('resets_watchdog', 0)} wedge={d.get('resets_wedge', 0)})",
        f"break-glass sent  : {d.get('breakglass_sent', 0)}",
        f"reloads / recovery: {d.get('reloads', 0)} reloads, "
        f"{d.get('guest_recoveries', 0)} guest recoveries, "
        f"{d.get('fsck_repairs', 0)} with fsck repairs "
        f"({d.get('fsck_second_pass_needed', 0)} needed a 2nd fsck pass)",
        f"load rate         : early {early and round(early, 1)} gens/h, "
        f"recent {recent and round(recent, 1)} gens/h"
        + ("   <-- DEGRADED" if degraded else ""),
        f"max temperature   : {d.get('max_temp_c') or 0:.1f} C",
    ]
    if stopped_reason:
        lines.append("STOPPED: " + stopped_reason[:160])
    if d.get("failures"):
        lines.append(f"failures recorded : {len(d['failures'])} "
                     f"(first: {d['failures'][0]['kind']})")
    if show:
        print("\n" + L.banner(lines))
    ev.emit("verdict", "info" if ok and not degraded else FAIL,
            gate_closed=bool(ok and not degraded),
            load_h=round(d["load_s"] / 3600.0, 3), resets=d["resets"],
            degraded=bool(degraded), stopped_reason=stopped_reason)
    return bool(ok and not degraded)


def cmd_status(args):
    """Progress, from the files only — never touches the board, so it is safe
    to run from another terminal while the soak is live."""
    if not os.path.exists(args.state):
        print(f"no state file at {args.state} — no soak has been started")
        return 1
    d = json.load(open(args.state))
    print(f"soak72 run {d.get('run_id')}  (label {d.get('label')})")
    print(f"  healthy load time : {d.get('load_s', 0) / 3600.0:.2f} h "
          f"of {args.hours} h target")
    print(f"  samples           : {d.get('samples', 0)}  "
          f"healthy={d.get('healthy_samples', 0)} warn={d.get('warn_samples', 0)}")
    print(f"  resets            : {d.get('resets', 0)} "
          f"(watchdog={d.get('resets_watchdog', 0)}, wedge={d.get('resets_wedge', 0)})")
    print(f"  reloads/recoveries: {d.get('reloads', 0)}/{d.get('guest_recoveries', 0)}")
    print(f"  restarts of run   : {len(d.get('restarts', []))}")
    if d.get("failures"):
        print(f"  FAILURES          : {len(d['failures'])}")
        for f in d["failures"][-5:]:
            print(f"    {f.get('kind')}: {str(f.get('detail'))[:120]}")
    evs = L.read_events(args.events)
    print(f"  events logged     : {len(evs)} in {args.events}")
    bad = [e for e in evs if e.get("severity") in ("fail", "reset", "recover")]
    for e in bad[-10:]:
        print(f"    [{e.get('iso')}] {e.get('severity'):8} {e.get('kind')}: "
              f"{str(e.get('detail'))[:100]}")
    return 0


# ── dry run ──────────────────────────────────────────────────────────────
def dry_run(scenario, hours=0.5, poll_s=60.0, quiet=True, tmpdir=None):
    """Run the whole harness against soaklib.FakeBoard on a virtual clock.

    Returns (gate_closed, stopped_reason, state_dict, events). Every path a
    real run can take is reachable here, including the ones hardware cannot be
    asked to produce on demand."""
    import tempfile
    tmp = tmpdir or tempfile.mkdtemp(prefix="soak72-dry-")
    clock = FakeClock()
    ev = EventLog(os.path.join(tmp, "events.jsonl"), clock,
                  run_id="dry-" + scenario, stdout=not quiet)
    cfg = make_cfg(argparse.Namespace(
        hours=hours, poll_s=poll_s, size_mb=8, idle_s=1, max_resets=3,
        fsck_passes=2, reload_timeout_s=1800, load_cycles=3, expect_vbk=False,
        logdir=os.path.join(tmp, "logs"), progress_every=5))
    state = RunState(os.path.join(tmp, "state.json"), clock, "soak72",
                     new_state_defaults())
    board = L.make_board(True, scenario, clock, ev)
    _stop["flag"] = False
    stopped = None
    ok = False
    # The ledger is a real file in the repo; a dry run must never write to it.
    with L.patched(L, "ledger_record", lambda *a, **k: None):
        try:
            ok = run(board, clock, ev, state, cfg)
        except HarnessStop as e:
            stopped = str(e)
    closed = verdict(state, cfg, ev, ok, stopped, show=not quiet)
    return closed, stopped, dict(state.d), L.read_events(ev.path), board


DRY_CASES = [
    # (scenario, expect_gate_closed, must-appear event kinds, note)
    ("happy", True, ["healthy", "load-started", "verdict"],
     "72h of nothing going wrong closes the gate"),
    ("watchdog-reset", True,
     ["board-in-uboot", "reset-recorded", "reload-done", "guest-recovery-done",
      "load-restarted"],
     "a HW watchdog reset is by design: counted, recovered, run continues"),
    ("wedge", True, ["wedge", "reset-recorded", "reload-done"],
     "EMAC dark past the grace period -> break-glass -> reload -> recover"),
    ("repeated-resets", False, ["reset-recorded"],
     "too many resets stops the run even though each one is 'by design'"),
    ("dead-board", False, ["board-off-usb"],
     "a board that never comes back stops the run loudly"),
    ("isolation-breach", False, ["isolation-breach"],
     "A1 ISOL != 1 is an immediate hard failure"),
    ("data-corruption", False, ["data-verify-mismatch"],
     "the load's own read-back verify failing is the real thing a soak hunts"),
    ("load-stalled", False, ["load-stalled"],
     "a guest that stops making progress fails, it does not idle-pass"),
    ("fsck-still-dirty", False, ["guest-recovery-done"],
     "recovery that cannot clean the filesystem stops the run"),
    ("core-frozen", False, ["core-frozen"],
     "cpu0/cpu1 not advancing across two FRESH samples is a hard failure"),
    ("log-warnings", True, ["guest-log-errors"],
     "a WARN is recorded, does not stop the run, and still counts as healthy "
     "time (FreeBSD's ordinary chatter must not kill a 72 h run)"),
]


def cmd_dry_run(args):
    if args.scenario:
        closed, stopped, st, evs, board = dry_run(
            args.scenario, hours=args.hours, poll_s=args.poll_s, quiet=False)
        print(f"\nscenario={args.scenario} gate_closed={closed} "
              f"stopped={stopped!r}")
        print(f"fake board calls: {board.calls}")
        return 0
    passed = failed = 0
    ledger0 = L.ledger_fingerprint()
    print("soak72 dry-run matrix (virtual clock, soaklib.FakeBoard, no board)\n")
    for scen, expect_closed, want_kinds, note in DRY_CASES:
        closed, stopped, st, evs, board = dry_run(scen, hours=args.hours,
                                                  poll_s=args.poll_s)
        kinds = {e["kind"] for e in evs}
        missing = [k for k in want_kinds if k not in kinds]
        problems = []
        if closed != expect_closed:
            problems.append(f"gate_closed={closed}, expected {expect_closed}")
        if missing:
            problems.append(f"missing events {missing}")
        if not expect_closed and not stopped and closed is False and \
                scen != "repeated-resets":
            pass
        if problems:
            failed += 1
            print(f"[FAIL ] {scen:18} {'; '.join(problems)}")
        else:
            passed += 1
            print(f"[ OK  ] {scen:18} load={st['load_s'] / 3600.0:5.2f}h "
                  f"resets={st['resets']} events={len(evs)}  {note}")
            if stopped:
                print(f"         stopped loudly: {stopped[:110]}")
    # Resumability and idempotence are first-class requirements, so they get
    # their own cases rather than being inferred from the scenarios above.
    passed, failed = _dry_resume_case(passed, failed)
    passed, failed = _dry_idempotent_load_case(passed, failed)
    # A dry run only means something if the fake answers the same calls the
    # real board does. Class-level check: nothing is instantiated, nothing is
    # touched.
    parity = L.api_parity_problems()
    if parity:
        failed += 1
        print(f"[FAIL ] {'api-parity':18} {'; '.join(parity)}")
    else:
        passed += 1
        print(f"[ OK  ] {'api-parity':18} FakeBoard implements the same "
              f"{len(L.FACADE_METHODS)}-method facade as Board")
    # A simulated board must never be able to write a real gate number.
    if L.ledger_fingerprint() != ledger0:
        failed += 1
        print(f"[FAIL ] {'ledger-untouched':18} boot-ledger.jsonl CHANGED "
              f"during a dry run ({ledger0} -> {L.ledger_fingerprint()})")
    else:
        passed += 1
        print(f"[ OK  ] {'ledger-untouched':18} boot-ledger.jsonl byte-identical "
              f"after the whole matrix")
    n = passed + failed
    print(f"\n---- soak72 dry-run: {passed}/{n} passed ----")
    return 0 if failed == 0 else 1


def _dry_idempotent_load_case(passed, failed):
    """Restarting the harness against a guest that is ALREADY loaded must not
    kill and restart the load (that would reset the generation counter and lose
    the stall detector's baseline). It must adopt it."""
    import tempfile
    tmp = tempfile.mkdtemp(prefix="soak72-idem-")
    clock = FakeClock()
    ev = EventLog(os.path.join(tmp, "events.jsonl"), clock, run_id="idem",
                  stdout=False)
    cfg = make_cfg(argparse.Namespace(
        hours=0.05, poll_s=60.0, size_mb=8, idle_s=1, max_resets=3,
        fsck_passes=2, reload_timeout_s=1800, load_cycles=3, expect_vbk=False,
        logdir=os.path.join(tmp, "logs"), progress_every=50))
    state = RunState(os.path.join(tmp, "state.json"), clock, "soak72",
                     new_state_defaults())
    board = L.make_board(True, "happy", clock, ev)
    board.load_running = True                 # as if a previous run left it going
    board.gen = 77
    _stop["flag"] = False
    with L.patched(L, "ledger_record", lambda *a, **k: None):
        run(board, clock, ev, state, cfg)
    kinds = [e["kind"] for e in L.read_events(ev.path)]
    problems = []
    if "load-already-running" not in kinds:
        problems.append("did not adopt the already-running load")
    if "load-started" in kinds:
        problems.append("restarted a load that was already running")
    if board.calls.get("guest_start_load"):
        problems.append("called guest_start_load on an already-loaded guest")
    if problems:
        print(f"[FAIL ] {'idempotent-start':18} {'; '.join(problems)}")
        return passed, failed + 1
    print(f"[ OK  ] {'idempotent-start':18} adopted the running load "
          f"(gen {board.gen}) instead of restarting it")
    return passed + 1, failed


def _dry_resume_case(passed, failed):
    """Interrupt a run half way, then re-run the same command and prove the
    accumulated hours carry over instead of restarting from zero."""
    import tempfile
    tmp = tempfile.mkdtemp(prefix="soak72-resume-")
    statep = os.path.join(tmp, "state.json")
    eventsp = os.path.join(tmp, "events.jsonl")

    def one(hours):
        clock = FakeClock()
        ev = EventLog(eventsp, clock, run_id="resume", stdout=False)
        cfg = make_cfg(argparse.Namespace(
            hours=hours, poll_s=60.0, size_mb=8, idle_s=1, max_resets=3,
            fsck_passes=2, reload_timeout_s=1800, load_cycles=3,
            expect_vbk=False, logdir=os.path.join(tmp, "logs"),
            progress_every=50))
        state = RunState(statep, clock, "soak72", new_state_defaults())
        board = L.make_board(True, "happy", clock, ev)
        _stop["flag"] = False
        ok = run(board, clock, ev, state, cfg)
        return ok, state, ev

    ok1, st1, ev1 = one(0.25)
    ok2, st2, ev2 = one(0.50)
    problems = []
    if not st2.resumed:
        problems.append("second run did not resume the state file")
    if st2.d["load_s"] <= st1.d["load_s"]:
        problems.append("resumed run did not accumulate on top of the first")
    if len(L.read_events(eventsp)) <= len(L.read_events(eventsp, kinds=["run-start"])):
        problems.append("event log did not keep growing across the restart")
    if len(L.read_events(eventsp, kinds=["run-start"])) != 2:
        problems.append("expected exactly two run-start events")
    if problems:
        print(f"[FAIL ] {'resume':18} {'; '.join(problems)}")
        return passed, failed + 1
    print(f"[ OK  ] {'resume':18} "
          f"{st1.d['load_s'] / 3600.0:.2f}h then resumed to "
          f"{st2.d['load_s'] / 3600.0:.2f}h in the same state file")
    return passed + 1, failed


# ── CLI ──────────────────────────────────────────────────────────────────
def new_state_defaults():
    return {"load_s": 0.0, "samples": 0, "healthy_samples": 0,
            "warn_samples": 0, "resets": 0, "reloads": 0,
            "guest_recoveries": 0, "breakglass_sent": 0, "fsck_repairs": 0,
            "fsck_second_pass_needed": 0, "failures": [], "gen_window": [],
            "early_rate": None, "recent_rate": None, "max_temp_c": 0.0,
            "last_healthy_ts": None, "last_load_gen": None,
            "last_load_gen_ts": None, "emac_dark_since": None}


def make_cfg(a):
    return {"hours": a.hours, "poll_s": a.poll_s, "size_mb": a.size_mb,
            "idle_s": a.idle_s, "max_resets": a.max_resets,
            "fsck_passes": a.fsck_passes,
            "reload_timeout_s": a.reload_timeout_s,
            "load_cycles": a.load_cycles, "expect_vbk": a.expect_vbk,
            "logdir": a.logdir, "progress_every": a.progress_every,
            "wedge_grace_s": 60.0, "high_temp_c": L.HIGH_TEMP_C,
            "load_stall_warn_s": 300.0, "load_stall_fail_s": 900.0,
            "min_avail_kb": 64 * 1024, "breakglass_settle_s": 25.0,
            "early_window_s": 1800.0, "degrade_frac": 0.5}


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="soak72.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hours", type=float, default=72.0,
                    help="target CUMULATIVE HEALTHY load hours (default 72)")
    ap.add_argument("--poll-s", type=float, default=60.0,
                    help="health-sampling cadence")
    ap.add_argument("--size-mb", type=int, default=48,
                    help="write size per load generation inside the guest")
    ap.add_argument("--idle-s", type=int, default=5,
                    help="pause between load generations (0 = flat out)")
    ap.add_argument("--max-resets", type=int, default=20,
                    help="stop if the run needs more resets than this; each is "
                         "individually by design (R2), but a soak that needs "
                         "many is not a 72 h stability claim")
    ap.add_argument("--fsck-passes", type=int, default=2,
                    help="fsck_ffs attempts per recovery (a STILL DIRTY first "
                         "pass followed by a clean second is normal here)")
    ap.add_argument("--reload-timeout-s", type=float, default=1800.0)
    ap.add_argument("--load-cycles", type=int, default=3,
                    help="reliable_load.py's internal retry budget")
    ap.add_argument("--expect-vbk", action="store_true",
                    help="require the VBK1 breadcrumb at verify")
    ap.add_argument("--progress-every", type=int, default=10,
                    help="emit a progress event every N samples")
    ap.add_argument("--state", default=DEFAULT_STATE)
    ap.add_argument("--events", default=DEFAULT_EVENTS)
    ap.add_argument("--logdir", default=DEFAULT_LOGDIR)
    ap.add_argument("--iface", default="br0")
    ap.add_argument("--restart", action="store_true",
                    help="zero the counters and start a fresh run (the ONLY "
                         "way to lose accumulated hours)")
    ap.add_argument("--status", action="store_true",
                    help="print progress from the state/event files and exit; "
                         "never touches the board, safe while a run is live")
    ap.add_argument("--dry-run", action="store_true",
                    help="exercise every path against a fake board on a "
                         "virtual clock; no board, no network")
    ap.add_argument("--scenario", help="one dry-run scenario instead of the "
                                       "whole matrix")
    args = ap.parse_args(argv)

    if args.status:
        return cmd_status(args)
    if args.dry_run:
        if args.hours == 72.0:
            args.hours = 0.5          # virtual hours; the matrix wants it short
        return cmd_dry_run(args)

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    os.makedirs(args.logdir, exist_ok=True)
    clock = Clock()
    ev = EventLog(args.events, clock,
                  run_id=time.strftime("%Y%m%d-%H%M%S"))
    stopped = None
    ok = False
    try:
        state = RunState(args.state, clock, "soak72", new_state_defaults(),
                         restart=args.restart)
        board = L.make_board(False, None, clock, ev, iface=args.iface)
        ok = run(board, clock, ev, state, make_cfg(args))
    except HarnessStop as e:
        stopped = str(e)
        ev.emit("harness-stop", FAIL, detail=stopped)
        try:
            state
        except NameError:
            print("\n" + L.banner(["soak72 could not start", stopped[:150]]))
            return 1
    except Exception as e:                                  # pragma: no cover
        stopped = f"{type(e).__name__}: {e}"
        ev.emit("harness-crash", FAIL, detail=stopped)
        raise
    closed = verdict(state, make_cfg(args), ev, ok, stopped)
    if _stop["flag"] and not closed:
        print("(interrupted: re-run the same command to resume)")
        return 2
    return 0 if closed else 1


if __name__ == "__main__":
    sys.exit(main())
