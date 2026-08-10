#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""breakglass_cycle.py — the v1-gate "20 survived break-glass resets in a row".

ROADMAP §2's last purely-numeric open criterion after the soak. One attempt is:

    healthy baseline  -> send `\\x00~BZRST\\x00` to /dev/ttyACM0
                      -> the HW watchdog resets the board to U-Boot (~16 s)
                      -> reload (catch U-Boot -> TFTP -> loady+bootelf -> verify)
                      -> the STANDARD recovery this project needs every time a
                         running guest is stopped uncooperatively:
                             fsck_ffs -y /dev/vtbd0p3        (twice if needed)
                             mount -u -o reload / ; mount -u -o rw /
                             service netif restart ; route add default GW
                             service sshd start
                      -> verify healthy: ssh answers, root is rw, fsck clean
                      -> record PASS, streak += 1

    python3 breakglass_cycle.py --attempts 20

The gate closes when the STREAK reaches 20 — 20 in a row, not 20 out of 30. So
the default behaviour on a failed attempt is to STOP LOUDLY: a harness that
keeps hammering a board which is no longer coming back produces nothing but a
longer log and more chances to corrupt the guest filesystem. `--keep-going`
exists for deliberate diagnosis runs, and says so in the report.

WHY THE BREAK-GLASS PATH DESERVES ITS OWN HARNESS: it is the recovery of last
resort. When EMAC is dark and the HV will not answer, those eight bytes are the
only thing between a remote fix and a physical power-cycle — and this project's
whole no-touch operating model (SESSION-RULES R0) rests on them. "It worked
when we tried it" is not a stability claim; 20 in a row is.

RESUMABLE / IDEMPOTENT: state in breakglass-state.json, events appended and
fsync'd to breakglass-events.jsonl. Re-run the same command to continue at the
attempt where it stopped; the streak survives. `--restart` zeroes it.

THE LEDGER: every attempt is also recorded in boot_ledger.py — the SAME
cumulative ledger that carries the "100 clean boots" streak — with source
"breakglass", so `python3 boot_ledger.py` prints both gates from one file.
Break-glass records are deliberately excluded from the clean-boot streak
arithmetic (see boot_ledger.py): an attempt is not a reload attempt, and the
reload inside it is recorded separately by reliable_load.py itself.

MID-RUN, FROM ANOTHER TERMINAL (never touches the board):
    python3 breakglass_cycle.py --status
    tail -f breakglass-events.jsonl

DRY RUN (no board; this is what CI runs):
    python3 breakglass_cycle.py --dry-run

UNVERIFIED: not run against real hardware from this session (another session
owned the board). The mechanics it drives are all board-proven code
(supervise.break_glass, reliable_load, guest_sh, a2_cycle); the loop, the
verification and the bookkeeping around them have been exercised only against
soaklib.FakeBoard. Run `--attempts 2` under supervision first.
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
from soaklib import (WARN, FAIL, HarnessStop, EventLog,       # noqa: E402
                     RunState, Clock, FakeClock)

DEFAULT_STATE = os.path.join(HERE, "breakglass-state.json")
DEFAULT_EVENTS = os.path.join(HERE, "breakglass-events.jsonl")
DEFAULT_LOGDIR = os.path.join(HERE, "breakglass-logs")
GATE = 20

_stop = {"flag": False}


def _sig(signum, _frame):
    _stop["flag"] = True
    print(f"\nsignal {signum}: stopping after this attempt (state is on disk; "
          f"re-run the same command to resume)", flush=True)


# ── the pieces of one attempt ────────────────────────────────────────────
def wait_for(pred, clock, timeout_s, step_s=5.0):
    """Poll `pred` until true or timeout. Returns elapsed seconds, or None."""
    t0 = clock.now()
    while clock.now() - t0 < timeout_s:
        if pred():
            return clock.now() - t0
        if _stop["flag"]:
            return None
        clock.sleep(step_s)
    return None


def baseline(board, ev, cfg, clock):
    """Is the board healthy enough for this attempt to MEAN anything?

    Resetting an already-broken board and then "recovering" it would count a
    pass for work the reset did not do. So the baseline is checked first, and a
    board that is merely read-only or loadless is repaired BEFORE the attempt
    starts rather than being counted as a failure of the break-glass path."""
    usb = board.usb()
    if usb == "gone":
        raise HarnessStop("board is off the USB bus — no remote path exists to "
                          "bring it back (supervise.py's GONE state)")
    if usb == "uboot":
        ev.emit("baseline-in-uboot", WARN,
                detail="board is sitting in U-Boot; loading before the attempt")
        return "needs-load"
    st = board.status()
    if not st.get("reachable"):
        ev.emit("baseline-emac-dark", WARN,
                detail="EMAC does not answer; treating as needs-reload so the "
                       "attempt starts from a known-good board")
        return "needs-load"
    if not board.guest_alive():
        fs = None
    else:
        fs = board.guest_fs_state()
    if fs is None or fs.get("readonly"):
        ev.emit("baseline-guest-unhealthy", WARN,
                detail=f"guest not ready before the attempt (fs={fs})")
        return "needs-recovery"
    return "ok"


def send_break_glass(board, ev, state, clock, cfg):
    """Send the magic bytes and PROVE the board actually reset.

    "Sent" is not "done" — this project has been burned by exactly that
    (a shutdown command whose failure went to /dev/null while the log said
    "sent"). The proof here is the USB identity flipping to U-Boot's own
    gadget, which cannot happen unless the watchdog really fired."""
    ev.emit("breakglass-send", "info",
            detail="writing the 8 magic bytes imported from supervise.BG_SEQ "
                   "(never retyped here) to the guest ACM tty")
    if not board.break_glass():
        raise HarnessStop("break-glass could not be sent: /dev/ttyACM0 would "
                          "not open. There is no other remote reset path once "
                          "EMAC is dark.")
    state.bump("breakglass_sent")
    el = wait_for(lambda: board.usb() == "uboot", clock, cfg["uboot_timeout_s"])
    if el is None:
        # Distinguish the two ways this fails, because they need different
        # answers: a board that vanished has no remote recovery at all.
        if board.usb() == "gone":
            raise HarnessStop(
                "after the break-glass reset the board vanished from the USB "
                "bus entirely. This is the ONE case with no remote recovery — "
                "stopping instead of pretending to retry.")
        raise HarnessStop(
            f"the break-glass bytes were written but the board never reached "
            f"U-Boot within {cfg['uboot_timeout_s']:.0f}s (usb={board.usb()}). "
            f"The watchdog reset did not happen — that is the failure this gate "
            f"exists to detect.")
    ev.emit("breakglass-reset-observed", "info", elapsed_s=round(el, 1),
            detail="USB identity flipped to U-Boot's gadget: the HW watchdog "
                   "really fired")
    return el


def recover_and_verify(board, ev, state, clock, cfg, n):
    """Reload, run the standard recovery, then prove the board is healthy."""
    os.makedirs(cfg["logdir"], exist_ok=True)
    log_path = os.path.join(cfg["logdir"], f"attempt-{n:03d}-reload.log")
    ok, elapsed, tail, timed_out = board.reload(
        log_path, timeout_s=cfg["reload_timeout_s"], boot_to_shell=True,
        load_cycles=cfg["load_cycles"], expect_vbk=cfg["expect_vbk"])
    ev.emit("reload-done", "info" if ok else FAIL, ok=ok,
            elapsed_s=round(elapsed, 1), timed_out=timed_out, detail=tail,
            log=log_path)
    if not ok:
        return False, f"reload after the reset failed: {tail}"

    rec = board.guest_recover(fsck_passes=cfg["fsck_passes"])
    passes = len(rec.get("fsck", []))
    if passes > 1:
        state.bump("fsck_second_pass_needed")
        ev.emit("fsck-second-pass", L.BENIGN,
                detail="first fsck pass reported STILL DIRTY and the rerun "
                       "cleared it — documented normal outcome, not a failure")
    if rec.get("repaired"):
        state.bump("fsck_repairs")
    ev.emit("recovery-done", "info" if rec.get("ok") else FAIL,
            ok=rec.get("ok"), fsck_passes=passes, repaired=rec.get("repaired"),
            still_dirty=rec.get("still_dirty"),
            detail=(rec.get("mount") or "")[:200])
    if not rec.get("ok"):
        return False, ("the standard recovery did not clean the filesystem "
                       f"(still_dirty={rec.get('still_dirty')})")

    # Independent verification, over a DIFFERENT channel than the recovery used:
    # the recovery talks to the console, so confirming over ssh proves netif and
    # sshd really came back rather than trusting the commands' own output.
    el = wait_for(board.guest_alive, clock, cfg["guest_timeout_s"])
    if el is None:
        return False, (f"guest never answered ssh within "
                       f"{cfg['guest_timeout_s']:.0f}s after recovery — netif "
                       f"or sshd did not come back")
    fs = board.guest_fs_state()
    if not fs:
        return False, "guest answered ssh but its filesystem state was unreadable"
    if fs.get("readonly"):
        return False, "root is still mounted read-only after recovery"
    ev.emit("verified-healthy", "info", elapsed_s=round(el, 1),
            detail=f"ssh up, root rw, df={fs.get('df')}")
    return True, (f"ssh up after {el:.0f}s, root rw"
                  + (", fsck repaired something" if rec.get("repaired") else
                     ", fsck found nothing"))


def one_attempt(board, ev, state, clock, cfg, n):
    t0 = clock.now()
    ev.emit("attempt-start", "info", attempt=n,
            streak=state.d.get("streak", 0))
    pre = baseline(board, ev, cfg, clock)
    if pre == "needs-load":
        okl, _el, tail, _to = board.reload(
            os.path.join(cfg["logdir"], f"attempt-{n:03d}-pre.log"),
            timeout_s=cfg["reload_timeout_s"], boot_to_shell=True,
            load_cycles=cfg["load_cycles"], expect_vbk=cfg["expect_vbk"])
        if not okl:
            raise HarnessStop(f"could not get the board to a healthy baseline "
                              f"before attempt {n}: {tail}")
        pre = "needs-recovery"
    if pre == "needs-recovery":
        rec = board.guest_recover(fsck_passes=cfg["fsck_passes"])
        ev.emit("baseline-recovery", "info" if rec.get("ok") else FAIL,
                ok=rec.get("ok"))
        if not rec.get("ok"):
            raise HarnessStop(f"could not get the guest healthy before attempt "
                              f"{n}; not counting a reset the board did not "
                              f"start ready for")

    send_break_glass(board, ev, state, clock, cfg)
    clock.sleep(cfg["settle_s"])
    ok, detail = recover_and_verify(board, ev, state, clock, cfg, n)

    rec = {"n": n, "ok": ok, "detail": detail,
           "duration_s": round(clock.now() - t0, 1),
           "iso": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(clock.now()))}
    state.d["attempts"] = state.d.get("attempts", []) + [rec]
    state.bump("attempts_done")
    if ok:
        state.d["streak"] = state.d.get("streak", 0) + 1
        state.d["best_streak"] = max(state.d.get("best_streak", 0),
                                     state.d["streak"])
        state.bump("passes")
    else:
        state.d["streak"] = 0
        state.bump("fails")
    state.save()
    L.ledger_record(ok, "breakglass", note=f"attempt {n}: {detail[:120]}")
    ev.emit("attempt-done", "info" if ok else FAIL, attempt=n, ok=ok,
            streak=state.d.get("streak", 0), detail=detail)
    return ok, detail


# ── the run ──────────────────────────────────────────────────────────────
def run(board, clock, ev, state, cfg):
    ev.emit("run-start", "info", resumed=state.resumed, board=board.kind,
            target=cfg["attempts"], streak=state.d.get("streak", 0),
            attempts_done=state.d.get("attempts_done", 0))
    while not _stop["flag"] and state.d.get("streak", 0) < cfg["attempts"]:
        n = state.d.get("attempts_done", 0) + 1
        if n > cfg["attempts"] + cfg["max_extra_attempts"]:
            raise HarnessStop(
                f"{n - 1} attempts made and the streak is only "
                f"{state.d.get('streak', 0)}/{cfg['attempts']} — stopping. The "
                f"gate wants {cfg['attempts']} IN A ROW; grinding out more "
                f"attempts will not produce it.")
        ok, detail = one_attempt(board, ev, state, clock, cfg, n)
        if not ok and not cfg["keep_going"]:
            raise HarnessStop(
                f"attempt {n} FAILED: {detail}\nStopping (the gate is "
                f"{cfg['attempts']} in a row, so continuing would only churn "
                f"the board). Use --keep-going for a diagnosis run.")
        clock.sleep(cfg["between_s"])
    return state.d.get("streak", 0) >= cfg["attempts"]


def verdict(state, cfg, ev, ok, stopped_reason=None):
    d = state.d
    lines = [
        f"bzdOS BREAK-GLASS gate — " + ("CLOSED" if ok else "not closed"),
        f"streak            : {d.get('streak', 0)}/{cfg['attempts']} in a row "
        f"(best ever this state file: {d.get('best_streak', 0)})",
        f"attempts           : {d.get('attempts_done', 0)} "
        f"({d.get('passes', 0)} pass, {d.get('fails', 0)} fail)",
        f"magic bytes sent   : {d.get('breakglass_sent', 0)}",
        f"fsck               : {d.get('fsck_repairs', 0)} attempts needed a "
        f"repair, {d.get('fsck_second_pass_needed', 0)} needed a 2nd pass",
    ]
    if cfg["keep_going"]:
        lines.append("NOTE: --keep-going was set, so a fail did not stop the "
                     "run; this is a diagnosis run, not a gate run")
    if stopped_reason:
        lines.append("STOPPED: " + stopped_reason.splitlines()[0][:150])
    print("\n" + L.banner(lines))
    ev.emit("verdict", "info" if ok else FAIL, gate_closed=bool(ok),
            streak=d.get("streak", 0), attempts=d.get("attempts_done", 0),
            stopped_reason=stopped_reason)
    return bool(ok)


def cmd_status(args):
    if not os.path.exists(args.state):
        print(f"no state file at {args.state} — no break-glass run started")
        return 1
    d = json.load(open(args.state))
    print(f"breakglass run {d.get('run_id')}")
    print(f"  streak         : {d.get('streak', 0)}/{args.attempts} "
          f"(best {d.get('best_streak', 0)})")
    print(f"  attempts       : {d.get('attempts_done', 0)} "
          f"({d.get('passes', 0)} pass / {d.get('fails', 0)} fail)")
    print(f"  restarts of run: {len(d.get('restarts', []))}")
    for a in (d.get("attempts") or [])[-8:]:
        print(f"    #{a['n']:>3} {'PASS' if a['ok'] else 'FAIL'} "
              f"{a['duration_s']:>7.1f}s  {str(a.get('detail'))[:90]}")
    evs = L.read_events(args.events)
    print(f"  events logged  : {len(evs)} in {args.events}")
    try:
        import boot_ledger
        bg = boot_ledger.breakglass_stats()
        print(f"  ledger         : streak {bg['current_streak']}/{GATE}, "
              f"{bg['passes']}/{bg['total']} attempts ok "
              f"(boot-ledger.jsonl, source=breakglass)")
    except Exception as e:
        print(f"  ledger         : unavailable ({e})")
    return 0


# ── dry run ──────────────────────────────────────────────────────────────
def dry_run(scenario, attempts=3, quiet=True, keep_going=False, tmpdir=None):
    import tempfile
    tmp = tmpdir or tempfile.mkdtemp(prefix="bg-dry-")
    clock = FakeClock()
    ev = EventLog(os.path.join(tmp, "events.jsonl"), clock,
                  run_id="dry-" + scenario, stdout=not quiet)
    state = RunState(os.path.join(tmp, "state.json"), clock, "breakglass",
                     new_state_defaults())
    cfg = make_cfg(argparse.Namespace(
        attempts=attempts, settle_s=25, between_s=10, uboot_timeout_s=120,
        guest_timeout_s=420, reload_timeout_s=1800, load_cycles=3,
        fsck_passes=2, expect_vbk=False, keep_going=keep_going,
        max_extra_attempts=2, logdir=os.path.join(tmp, "logs")))
    board = L.make_board(True, scenario, clock, ev)
    _stop["flag"] = False
    stopped, ok = None, False
    # The ledger is a real file in the repo; a dry run must not write to it.
    with L.patched(L, "ledger_record", lambda *a, **k: None):
        try:
            ok = run(board, clock, ev, state, cfg)
        except HarnessStop as e:
            stopped = str(e)
    if not quiet:
        verdict(state, cfg, ev, ok, stopped)
    return ok, stopped, dict(state.d), L.read_events(ev.path), board


DRY_CASES = [
    ("happy", True, ["breakglass-send", "breakglass-reset-observed",
                     "reload-done", "recovery-done", "verified-healthy",
                     "attempt-done"],
     "the whole loop: reset, reload, fsck+mount+netif+sshd, verified over ssh"),
    ("watchdog-reset", True, ["breakglass-reset-observed", "recovery-done"],
     "an extra watchdog reset landing mid-attempt does not break the loop"),
    ("fsck-still-dirty", False, ["recovery-done", "attempt-done"],
     "a filesystem fsck cannot clean fails the attempt and stops the run"),
    ("dead-board", False, ["breakglass-send"],
     "a board that never comes back stops loudly instead of churning"),
    ("flaky-verify", False, ["reload-done", "attempt-done"],
     "one attempt whose sshd never came back fails that attempt (the gate is "
     "N in a row) and stops by default"),
]


def cmd_dry_run(args):
    if args.scenario:
        ok, stopped, st, evs, board = dry_run(args.scenario, args.attempts,
                                             quiet=False,
                                             keep_going=args.keep_going)
        print(f"\nscenario={args.scenario} gate_closed={ok} stopped={stopped!r}")
        print(f"fake board calls: {board.calls}")
        return 0
    passed = failed = 0
    ledger0 = L.ledger_fingerprint()
    print("breakglass dry-run matrix (virtual clock, soaklib.FakeBoard, "
          "no board)\n")
    for scen, expect_ok, want, note in DRY_CASES:
        ok, stopped, st, evs, board = dry_run(scen, attempts=args.attempts)
        kinds = {e["kind"] for e in evs}
        problems = []
        if ok != expect_ok:
            problems.append(f"gate_closed={ok}, expected {expect_ok}")
        miss = [k for k in want if k not in kinds]
        if miss:
            problems.append(f"missing events {miss}")
        if not expect_ok and not stopped:
            problems.append("expected the run to stop loudly, but it did not")
        if expect_ok and st.get("streak") != args.attempts:
            problems.append(f"streak {st.get('streak')} != {args.attempts}")
        if problems:
            failed += 1
            print(f"[FAIL ] {scen:18} {'; '.join(problems)}")
        else:
            passed += 1
            print(f"[ OK  ] {scen:18} streak={st.get('streak')} "
                  f"attempts={st.get('attempts_done')} bg_sent="
                  f"{st.get('breakglass_sent')} events={len(evs)}  {note}")
            if stopped:
                print(f"         stopped loudly: {stopped.splitlines()[0][:110]}")
    passed, failed = _dry_resume_case(passed, failed, args.attempts)
    passed, failed = _dry_keep_going_case(passed, failed)
    # Every attempt records into the real boot ledger on a real run, so proving
    # a dry run does NOT is part of the harness's contract.
    if L.ledger_fingerprint() != ledger0:
        failed += 1
        print(f"[FAIL ] {'ledger-untouched':18} boot-ledger.jsonl CHANGED "
              f"during a dry run ({ledger0} -> {L.ledger_fingerprint()})")
    else:
        passed += 1
        print(f"[ OK  ] {'ledger-untouched':18} boot-ledger.jsonl byte-identical "
              f"after the whole matrix")
    n = passed + failed
    print(f"\n---- breakglass dry-run: {passed}/{n} passed ----")
    return 0 if failed == 0 else 1


def _dry_resume_case(passed, failed, attempts):
    """Stop half way through the streak, then resume and finish it: the streak
    must carry over, not restart."""
    import tempfile
    tmp = tempfile.mkdtemp(prefix="bg-resume-")
    statep = os.path.join(tmp, "state.json")
    eventsp = os.path.join(tmp, "events.jsonl")

    def one(target):
        clock = FakeClock()
        ev = EventLog(eventsp, clock, run_id="resume", stdout=False)
        state = RunState(statep, clock, "breakglass", new_state_defaults())
        cfg = make_cfg(argparse.Namespace(
            attempts=target, settle_s=25, between_s=10, uboot_timeout_s=120,
            guest_timeout_s=420, reload_timeout_s=1800, load_cycles=3,
            fsck_passes=2, expect_vbk=False, keep_going=False,
            max_extra_attempts=2, logdir=os.path.join(tmp, "logs")))
        board = L.make_board(True, "happy", clock, ev)
        _stop["flag"] = False
        with L.patched(L, "ledger_record", lambda *a, **k: None):
            ok = run(board, clock, ev, state, cfg)
        return ok, state

    ok1, st1 = one(2)
    ok2, st2 = one(attempts + 2)
    problems = []
    if not st2.resumed:
        problems.append("second run did not resume the state file")
    if st2.d["attempts_done"] <= st1.d["attempts_done"]:
        problems.append("resumed run restarted the attempt count")
    if st2.d["streak"] != attempts + 2:
        problems.append(f"streak did not carry over (got {st2.d['streak']})")
    if len(L.read_events(eventsp, kinds=["run-start"])) != 2:
        problems.append("event log did not record both run starts")
    if problems:
        print(f"[FAIL ] {'resume':18} {'; '.join(problems)}")
        return passed, failed + 1
    print(f"[ OK  ] {'resume':18} streak {st1.d['streak']} -> "
          f"{st2.d['streak']} across a restart, same state file")
    return passed + 1, failed


def _dry_keep_going_case(passed, failed):
    """--keep-going must NOT be the default: a failing attempt has to stop the
    run unless the operator explicitly asked otherwise.

    Uses "flaky-verify" (one bad attempt on an otherwise healthy board) rather
    than a permanently broken one, because a permanently broken board SHOULD
    stop even with --keep-going — and does, at the baseline check."""
    ok_default, stopped_default, st_d, _e, _b = dry_run("flaky-verify",
                                                        attempts=3)
    ok_kg, stopped_kg, st_k, _e2, _b2 = dry_run("flaky-verify", attempts=3,
                                                keep_going=True)
    problems = []
    if stopped_default is None:
        problems.append("default run did not stop on a failed attempt")
    if st_d.get("attempts_done") != 1:
        problems.append(f"default run made {st_d.get('attempts_done')} attempts "
                        f"after a failure (expected 1)")
    if st_k.get("attempts_done", 0) <= 1:
        problems.append("--keep-going did not continue past the failure")
    if ok_default:
        problems.append("the default run closed the gate despite a failure")
    # The gate is N IN A ROW, not N total: with --keep-going the failed attempt
    # must not count, so closing takes attempts_done > target.
    if ok_kg and st_k.get("attempts_done", 0) <= 3:
        problems.append("gate closed without a fresh run of 3 consecutive "
                        "passes after the failure")
    if st_k.get("streak") != 3 or st_k.get("fails") != 1:
        problems.append(f"expected streak 3 with 1 recorded fail, got "
                        f"streak={st_k.get('streak')} fails={st_k.get('fails')}")
    if problems:
        print(f"[FAIL ] {'stop-on-fail':18} {'; '.join(problems)}")
        return passed, failed + 1
    print(f"[ OK  ] {'stop-on-fail':18} default stopped after 1 failed attempt; "
          f"--keep-going made {st_k.get('attempts_done')}")
    return passed + 1, failed


# ── CLI ──────────────────────────────────────────────────────────────────
def new_state_defaults():
    return {"attempts_done": 0, "passes": 0, "fails": 0, "streak": 0,
            "best_streak": 0, "breakglass_sent": 0, "fsck_repairs": 0,
            "fsck_second_pass_needed": 0, "attempts": []}


def make_cfg(a):
    return {"attempts": a.attempts, "settle_s": a.settle_s,
            "between_s": a.between_s, "uboot_timeout_s": a.uboot_timeout_s,
            "guest_timeout_s": a.guest_timeout_s,
            "reload_timeout_s": a.reload_timeout_s,
            "load_cycles": a.load_cycles, "fsck_passes": a.fsck_passes,
            "expect_vbk": a.expect_vbk, "keep_going": a.keep_going,
            "max_extra_attempts": a.max_extra_attempts, "logdir": a.logdir}


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="breakglass_cycle.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--attempts", type=int, default=GATE,
                    help=f"consecutive successful resets required (gate: {GATE})")
    ap.add_argument("--settle-s", type=float, default=25.0,
                    help="wait after the magic bytes: WDOG fires within ~16 s, "
                         "plus U-Boot USB enumeration")
    ap.add_argument("--between-s", type=float, default=30.0,
                    help="pause between attempts")
    ap.add_argument("--uboot-timeout-s", type=float, default=180.0,
                    help="how long to wait for the USB identity to flip to "
                         "U-Boot before calling the reset itself failed")
    ap.add_argument("--guest-timeout-s", type=float, default=420.0,
                    help="how long to wait for guest ssh after recovery")
    ap.add_argument("--reload-timeout-s", type=float, default=1800.0)
    ap.add_argument("--load-cycles", type=int, default=3)
    ap.add_argument("--fsck-passes", type=int, default=2,
                    help="fsck_ffs attempts per recovery (STILL DIRTY then "
                         "clean on the rerun is a documented normal outcome)")
    ap.add_argument("--expect-vbk", action="store_true")
    ap.add_argument("--keep-going", action="store_true",
                    help="do NOT stop on a failed attempt (diagnosis runs "
                         "only; the gate needs N in a row)")
    ap.add_argument("--max-extra-attempts", type=int, default=5,
                    help="give up once this many attempts beyond the target "
                         "have been made without a full streak")
    ap.add_argument("--state", default=DEFAULT_STATE)
    ap.add_argument("--events", default=DEFAULT_EVENTS)
    ap.add_argument("--logdir", default=DEFAULT_LOGDIR)
    ap.add_argument("--iface", default="br0")
    ap.add_argument("--restart", action="store_true",
                    help="zero the streak and start fresh")
    ap.add_argument("--status", action="store_true",
                    help="print progress from the state/event files and exit; "
                         "never touches the board")
    ap.add_argument("--dry-run", action="store_true",
                    help="exercise every path against a fake board on a "
                         "virtual clock; no board, no network, no ledger write")
    ap.add_argument("--scenario", help="one dry-run scenario instead of the "
                                      "whole matrix")
    args = ap.parse_args(argv)

    if args.status:
        return cmd_status(args)
    if args.dry_run:
        if args.attempts == GATE:
            args.attempts = 3
        return cmd_dry_run(args)

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    os.makedirs(args.logdir, exist_ok=True)
    clock = Clock()
    ev = EventLog(args.events, clock, run_id=time.strftime("%Y%m%d-%H%M%S"))
    stopped, ok = None, False
    state = None
    try:
        state = RunState(args.state, clock, "breakglass", new_state_defaults(),
                         restart=args.restart)
        board = L.make_board(False, None, clock, ev, iface=args.iface)
        ok = run(board, clock, ev, state, make_cfg(args))
    except HarnessStop as e:
        stopped = str(e)
        ev.emit("harness-stop", FAIL, detail=stopped)
        if state is None:
            print("\n" + L.banner(["break-glass run could not start",
                                   stopped.splitlines()[0][:150]]))
            return 1
    closed = verdict(state, make_cfg(args), ev, ok, stopped)
    if _stop["flag"] and not closed:
        print("(interrupted: re-run the same command to resume the streak)")
        return 2
    return 0 if closed else 1


if __name__ == "__main__":
    sys.exit(main())
