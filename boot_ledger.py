#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
boot_ledger.py — persistent, cumulative boot-success ledger for the bzdOS
hypervisor's v1 "100 clean boots in a row" gate (ROADMAP §2).

The point (user's idea, 2026-07-25): DON'T run a dedicated soak to rack up the
number. Instead every board reload we do anyway — HDMI bring-up, a vGIC test,
a manual reliable_load, or a soak cycle — records its outcome HERE, so the
stability counter accrues organically from all our real board activity. The
"100 clean boots" gate closes when the running streak in this ledger reaches
100, whatever mix of work produced them.

A "clean boot" (ok=True) means reliable_load got the board through
reboot -> U-Boot -> TFTP -> loady+bootelf -> EMAC verify. If the caller also
passes isol_pass (the A1 STG2[18] self-check), that is recorded too, so the
ledger simultaneously proves isolation held on every counted boot.

Storage (append-only, never rewritten so history is never lost):
    boot-ledger.jsonl   one JSON object per reload attempt
    boot-ledger.txt     human summary, recomputed from the jsonl after each add

The same file also carries the OTHER purely-numeric v1 gate, "20 survived
break-glass resets in a row" (see BREAKGLASS_SOURCE_PREFIX below and
breakglass_cycle.py): one ledger, two gates, no parallel scoreboard.

CLI:
    python3 boot_ledger.py            # print the cumulative summary (both gates)
    python3 boot_ledger.py --tail 20  # last 20 entries
"""
import json, os, sys, time

HERE    = os.path.dirname(os.path.abspath(__file__))
LEDGER  = os.path.join(HERE, "boot-ledger.jsonl")
SUMMARY = os.path.join(HERE, "boot-ledger.txt")


# Entries older than this were scored by a supervisor with a KNOWN defect, so
# the counts they contribute cannot be taken at face value. chimpd judged
# liveness by console growth alone and declared "HANG" after 40 s of silence --
# but a booting FreeBSD is quiet for far longer than that while working the
# eMMC, and a guest that reached `login:` and went idle produces no console
# output at all. It reset healthy boots and recorded them as failures; that was
# proven on hardware and fixed 2026-08-05 (commit df611e3: liveness now counts
# disk I/O too, and the guest is POKED before being condemned).
#
# Direction of the error matters: healthy boots scored as failures INFLATE the
# failure count and TRUNCATE streaks, so the gate number is pessimistic rather
# than flattering. That is the safe direction, but it is still wrong, and the
# summary says so rather than leaving a reader to trust a clean-looking ✅.
SUPERVISOR_FIX_TS = 1785886000.0   # 2026-08-05, commit df611e3

# ── a second instrument that scored healthy boots as failures ──────────────
# `bzdctl boot-watch` judged liveness from bmc_client.health_raw(), a plain
# read of the BMC1 window in DRAM. That window is a SNAPSHOT: CPU1 rebuilds it
# only while servicing the `health` verb, so a raw-read loop compares a frozen
# record against itself and can never see an advancing heartbeat. Every
# boot-watch run therefore ended in "TIMED OUT with no advancing heartbeat"
# and wrote ok=False -- about boards that were demonstrably alive (the
# independent CPU1 counter at HVMAP_DBGTOOLS_BASE was climbing throughout).
# Fixed 2026-08-20 by routing every liveness consumer through the new
# bmc_client.health_fresh(); the same run then reported ALIVE in 2.5 s.
#
# Only two records carry this defect (both 2026-08-20, both mine), so unlike
# the chimpd case above this barely moves the gate number -- but the same rule
# applies: a boot-watch ok=False older than this timestamp says nothing about
# the board.
BOOTWATCH_FIX_TS = 1787254200.0    # 2026-08-20
BOOTWATCH_SOURCE = "bzdctl-boot-watch"

# ── the second gate that lives in this same ledger ─────────────────────────
# ROADMAP §2 also wants "20 survived break-glass resets in a row, auto-
# recovered". breakglass_cycle.py records one entry here per ATTEMPT (source
# "breakglass"), for the same reason the boot streak lives here: one durable,
# append-only file that accrues from real board work, not a parallel scoreboard
# that has to be reconciled later.
#
# Those entries are deliberately EXCLUDED from stats() -- the clean-boot streak.
# A break-glass attempt is not a reload attempt: the reload *inside* an attempt
# is already recorded separately by reliable_load.py, so counting the attempt
# too would double-count the good case and, worse, let a failed *recovery*
# truncate the boot streak for something that was never a boot. Both gates are
# then readable from one file without either polluting the other. (No historical
# entry has a "breakglass" source, so this changes no existing number.)
BREAKGLASS_SOURCE_PREFIX = "breakglass"
BREAKGLASS_GATE = 20


def _is_breakglass(rec):
    return str(rec.get("source") or "").startswith(BREAKGLASS_SOURCE_PREFIX)


def record(ok, isol_pass=None, sha=None, source="reliable_load", note=""):
    """Append one reload outcome and refresh the summary. Never raises — a
    ledger write must never be able to break the reload path that calls it."""
    try:
        rec = dict(ts=time.time(), ok=bool(ok), isol_pass=isol_pass,
                   sha=sha, source=source, note=note)
        with open(LEDGER, "a") as f:
            f.write(json.dumps(rec) + "\n")
        _rewrite_summary()
        return rec
    except Exception:
        return None


def _read_all():
    if not os.path.exists(LEDGER):
        return []
    out = []
    for line in open(LEDGER):
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except Exception:
            pass
    return out


def stats():
    recs = [r for r in _read_all() if not _is_breakglass(r)]
    total = len(recs)
    passes = sum(1 for r in recs if r.get("ok"))
    isol_ok = sum(1 for r in recs if r.get("isol_pass") == 1)
    isol_seen = sum(1 for r in recs if r.get("isol_pass") is not None)
    cur = best = 0
    for r in recs:
        if r.get("ok"):
            cur += 1
            best = max(best, cur)
        else:
            cur = 0
    pre_fix = sum(1 for r in recs if r.get("ts", 0) < SUPERVISOR_FIX_TS)
    # Failures written by the pre-fix boot-watch: not board events. Counted
    # separately rather than filtered out, so the raw file stays the record and
    # the summary stays honest about what is in it.
    bw_false = sum(1 for r in recs
                   if r.get("source") == BOOTWATCH_SOURCE
                   and not r.get("ok")
                   and r.get("ts", 0) < BOOTWATCH_FIX_TS)
    return dict(total=total, passes=passes, fails=total - passes,
                isol_ok=isol_ok, isol_seen=isol_seen,
                current_streak=cur, best_streak=best,
                pre_supervisor_fix=pre_fix,
                bootwatch_false_fails=bw_false,
                first_ts=recs[0]["ts"] if recs else None,
                last_ts=recs[-1]["ts"] if recs else None)


def breakglass_stats():
    """The "20 break-glass resets in a row" gate, from the same ledger.

    Only entries written by breakglass_cycle.py (source "breakglass...") are
    counted, so this number cannot be inflated by ordinary reloads."""
    recs = [r for r in _read_all() if _is_breakglass(r)]
    cur = best = 0
    for r in recs:
        if r.get("ok"):
            cur += 1
            best = max(best, cur)
        else:
            cur = 0
    passes = sum(1 for r in recs if r.get("ok"))
    return dict(total=len(recs), passes=passes, fails=len(recs) - passes,
                current_streak=cur, best_streak=best,
                gate=BREAKGLASS_GATE,
                first_ts=recs[0]["ts"] if recs else None,
                last_ts=recs[-1]["ts"] if recs else None)


def _rewrite_summary():
    s = stats()
    bg = breakglass_stats()
    def when(ts):
        if not ts:
            return "n/a"
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts))
    gate = "✅ CLOSED" if s["current_streak"] >= 100 else \
           f"{s['current_streak']}/100 (need {100 - s['current_streak']} more in a row)"
    lines = [
        "bzdOS cumulative boot ledger (v1 '100 clean boots' gate)",
        "",
        f"100-in-a-row gate : {gate}",
        f"current streak    : {s['current_streak']}",
        f"best streak ever  : {s['best_streak']}",
        "",
        f"total reloads     : {s['total']}",
        f"clean boots (ok)  : {s['passes']}",
        f"failures          : {s['fails']}",
        f"isolation proven  : {s['isol_ok']}/{s['isol_seen']} boots where the "
        f"A1 ISOL check was read",
        "",
        f"first entry       : {when(s['first_ts'])}",
        f"last entry        : {when(s['last_ts'])}",
    ]
    bg_gate = ("✅ CLOSED" if bg["current_streak"] >= BREAKGLASS_GATE else
               f"{bg['current_streak']}/{BREAKGLASS_GATE}"
               + (f" (need {BREAKGLASS_GATE - bg['current_streak']} more in a row)"
                  if bg["total"] else " (no attempts recorded yet — run "
                                       "breakglass_cycle.py)"))
    lines += [
        "",
        f"break-glass gate  : {bg_gate}",
        f"  attempts        : {bg['total']}  ({bg['passes']} recovered, "
        f"{bg['fails']} failed)",
        f"  best streak     : {bg['best_streak']}",
        "  (source=breakglass entries only; excluded from the boot counts "
        "above by design)",
    ]
    if s["pre_supervisor_fix"]:
        lines += [
            "",
            f"⚠ {s['pre_supervisor_fix']} of {s['total']} entries predate the "
            f"supervisor fix of 2026-08-05 (df611e3) and were scored by a chimpd",
            "  that condemned healthy boots after 40 s of console silence. Those",
            "  entries UNDERSTATE reliability (healthy boots recorded as failures,",
            "  streaks truncated), so treat this gate number as pessimistic but",
            "  not trustworthy. Re-run the streak with the fixed supervisor before",
            "  citing it in a release claim.",
        ]
    if s["bootwatch_false_fails"]:
        lines += [
            "",
            f"⚠ {s['bootwatch_false_fails']} failure(s) above were written by "
            f"`bzdctl boot-watch` BEFORE its 2026-08-20 fix and are",
            "  instrument error, not board events: it judged liveness from an",
            "  unrefreshed DRAM snapshot, so it could never observe motion and",
            "  always timed out. Same direction of error as the chimpd case --",
            "  reliability understated, streaks truncated.",
        ]
    with open(SUMMARY, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    if "--tail" in sys.argv:
        i = sys.argv.index("--tail")
        n = int(sys.argv[i + 1]) if i + 1 < len(sys.argv) else 20
        recs = _read_all()[-n:]
        for r in recs:
            ts = time.strftime("%H:%M:%S", time.localtime(r.get("ts", 0)))
            print(f"{ts}  ok={r.get('ok')}  isol={r.get('isol_pass')}  "
                  f"src={r.get('source')}  {r.get('note','')}")
        return
    _rewrite_summary()
    print(open(SUMMARY).read())


if __name__ == "__main__":
    main()
