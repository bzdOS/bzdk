#!/usr/bin/env python3
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

CLI:
    python3 boot_ledger.py            # print the cumulative summary
    python3 boot_ledger.py --tail 20  # last 20 entries
"""
import json, os, sys, time

HERE    = os.path.dirname(os.path.abspath(__file__))
LEDGER  = os.path.join(HERE, "boot-ledger.jsonl")
SUMMARY = os.path.join(HERE, "boot-ledger.txt")


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
    recs = _read_all()
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
    return dict(total=total, passes=passes, fails=total - passes,
                isol_ok=isol_ok, isol_seen=isol_seen,
                current_streak=cur, best_streak=best,
                first_ts=recs[0]["ts"] if recs else None,
                last_ts=recs[-1]["ts"] if recs else None)


def _rewrite_summary():
    s = stats()
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
