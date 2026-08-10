# The two long unattended runs: 72 h soak and 20 break-glass resets

Two v1-gate criteria (`ROADMAP.md` §2) are still open and are pure numbers:

- **72 h soak** under continuous disk load, no degradation or leaks.
- **20 survived break-glass resets** in a row, auto-recovered.

They kept not happening for a mundane reason: they are long, they will be
interrupted, and there was no harness you could start and walk away from. These
two are that harness.

| Run | Command | Harness | Progress | State |
|---|---|---|---|---|
| 72 h soak | `python3 soak72.py --hours 72` | `soak72.py` | `python3 soak72.py --status` | `soak72-state.json`, `soak72-events.jsonl` |
| 20 break-glass | `python3 breakglass_cycle.py --attempts 20` | `breakglass_cycle.py` | `python3 breakglass_cycle.py --status` | `breakglass-state.json`, `breakglass-events.jsonl` |

Both share `soaklib.py` (event log, resumable state, the board facade, the
benign-vs-real classifier, and the fake board that `--dry-run` uses).

**Read `SESSION-RULES.md` first.** In particular R2: the hardware watchdog
resetting the board back into U-Boot is *deliberate anti-brick behaviour*. Both
harnesses treat a reset as an event to record and recover from, never as an
automatic failure — and both count them, because a soak that needed nine resets
is not the same result as one that needed none.

---

## 1. Starting them

One command each. Nothing else to set up; run them from `/opt/bzdos/microkernel`.

```sh
# the 72-hour soak
python3 soak72.py --hours 72

# the 20-in-a-row break-glass gate
python3 breakglass_cycle.py --attempts 20
```

Do a supervised smoke test first, always:

```sh
python3 soak72.py --hours 0.2                 # ~12 min of real soak
python3 breakglass_cycle.py --attempts 2      # two real resets
```

They print one line per event and are safe to run under `nohup`/`tmux`:

```sh
nohup python3 -u soak72.py --hours 72 > soak72-stdout.log 2>&1 &
```

**They must not run at the same time as each other**, or alongside `chimpd.py`,
`supervise.py`, `soak.py`, `orient.py` or a `guest_sh.py` session: only one
process may hold `/dev/ttyACM0`. `soaklib.Board.console()` refuses to open a
busy tty and stops the run with an explanation rather than corrupting a
channel — but the reload path shells out to `reliable_load.py`, which needs the
tty, so keep the board to one supervisor.

## 2. Checking progress mid-run

Both `--status` views read **only the state and event files**. They never open
the tty and never touch the board, so they are safe from a second terminal
while a run is live:

```sh
python3 soak72.py --status
python3 breakglass_cycle.py --status
tail -f soak72-events.jsonl | python3 -c 'import sys,json;[print(json.loads(l)["kind"]) for l in sys.stdin]'
python3 boot_ledger.py            # both gates, from the one cumulative ledger
```

`bzdctl.py status` and `bzdctl.py serve` are also still safe (EMAC-only) and
show the same board from the other side.

## 3. Interpreting the log

`soak72-events.jsonl` / `breakglass-events.jsonl` are **append-only JSONL,
fsync'd per line**, so a crash mid-run still leaves everything up to the last
event on disk. One object per line:

```json
{"ts": 1786360488.1, "iso": "2026-08-10T14:14:48", "run": "20260810-141448",
 "seq": 42, "kind": "board-in-uboot", "severity": "reset", "detail": "..."}
```

`severity` is the field to filter on. There are only five, deliberately:

| severity | meaning | harness reaction |
|---|---|---|
| `benign` | known-normal here (see §4) | recorded, counted, ignored |
| `warn` | worth knowing, not disqualifying | recorded; the sample still counts as healthy time |
| `reset` | the board reset (or must be reset) | counted, recovered from, run continues |
| `recover` | the guest needs the standard repair | repaired, run continues |
| `fail` | genuine failure | run **stops loudly** |

Useful greps:

```sh
grep -c '"kind":"healthy"'                soak72-events.jsonl
grep '"severity":"fail"'                  soak72-events.jsonl
grep '"severity":"reset"'                 soak72-events.jsonl
grep '"kind":"progress"'                  soak72-events.jsonl | tail -1
```

Per-reload logs from `reliable_load.py` land in `soak72-logs/reload-NNNN.log`
and `breakglass-logs/attempt-NNN-reload.log`. For anything deeper, the usual
tools apply unchanged: `triage.py` for hypervisor state, `bzdctl.py crash` for
a bundle.

## 4. What counts as pass, fail, and known-benign

### Pass

**Soak.** The measured quantity is **cumulative healthy load time**, not wall
time: only intervals between two consecutive healthy samples are credited, and
each credit is capped at `2 × --poll-s`. So time the harness was not running,
or the board spent recovering, can never be counted toward the 72 h. The gate
closes when that total reaches `--hours` **and** the load rate has not degraded
(recent generations/hour ≥ 50 % of the early rate).

**Break-glass.** The gate closes when the **streak** reaches `--attempts`
(20) — 20 in a row, not 20 out of 30. One attempt passes only if all of this
holds: the magic bytes were accepted, the USB identity actually flipped to
U-Boot (proof the watchdog really fired — "sent" is not "done"), the reload
verified, the standard recovery cleaned the filesystem, and **ssh** then
answered with root mounted `rw`. That last check deliberately uses a *different
channel* than the recovery used (recovery over the console, verification over
the network), so a recovery that only appeared to work cannot pass.

### Fail — the run stops, loudly

- `board-off-usb` — the board left the USB bus. The one failure with no remote
  recovery path (`supervise.py`'s GONE state); nothing to retry.
- `core-frozen` — CPU0 or CPU1 not advancing across two **fresh** samples.
- `isolation-breach` — A1 `STG2[18]` ISOL self-check ≠ 1.
- `data-verify-mismatch` — the load's own write→sync→re-read verify failed.
  This is the thing a soak exists to catch.
- `load-stalled` — the load's generation counter stuck > `load_stall_fail_s`
  (15 min) while the process is still alive.
- `emmc-lock-stuck` (`lock_giveups`), `emmc-recovery-clk-timeout` — documented
  unrecoverable eMMC states; waiting cannot fix either.
- `timer-stalled` — `tick_delta == 0` on an IMO=0 build.
- a reload that fails twice with a break-glass reset in between.
- recovery that cannot get root back to `rw` / cannot clean the filesystem.
- more resets than `--max-resets` (default 20). Each is individually by design;
  needing many is not a 72 h stability claim.

The break-glass harness stops after the **first** failed attempt by default,
because the gate is N in a row and continuing would only churn the board (and
give the guest filesystem more chances to be damaged). `--keep-going` continues
for diagnosis runs and the verdict says so.

### Known-benign — recorded, never a failure

Each of these has cost this project real time in the past, which is why they
are enumerated in `soaklib.classify_sample()` with their justifications:

| event | why it is benign |
|---|---|
| `board-in-uboot` | the HW watchdog reset the board — **by design** (R2). Counted as a reset, recovered by reload. |
| `emac-flap` | `supervise.emac_alive()`'s own note: the raw read flaps under heavy guest I/O. One dark sample is not a wedge; the grace period is 60 s. |
| `liveness-unknown` | the BMC1 record is a **latch**. If the health verb could not be poked, the record says *nothing* about motion — "unknown", never "frozen". Four confidently-wrong diagnoses in this project came from ignoring that. |
| `cpu2-idle` | CPU2 is the async eMMC I/O server; with no I/O outstanding it legitimately posts no progress. |
| `imo1-build` | on the IMO=1 vGIC trunk EL2 runs no periodic tick, so `tick_delta == 0` is by design and `exc_count` counts *every* physical IRQ (millions/s), not faults. |
| `fsck-second-pass` | `fsck_ffs -y` reporting `FILE SYSTEM STILL DIRTY` and coming back clean on the rerun is a documented normal outcome here. |
| `emmc-retry-absorbed` | `ebio_fails > 0` with `g_ioerrs == 0`: the card misbehaved and the retry covered it. Healthy, but shown, because VBK1 alone reports it as silence. |
| `guest-network-down` | a guest that is unreachable over ssh but still advancing its load is a networking problem, not a dead guest. `soak.py`'s rule; do not conflate the two. |

## 5. Resumability and idempotence

A 72-hour run will be interrupted — host reboot, Ctrl-C, OOM, a session ending.

- State is a small JSON file replaced atomically after every step; the event log
  is only ever appended to.
- **Re-run the exact same command to resume.** Accumulated healthy hours, reset
  counts, attempt counts and the streak all carry over, and the event log keeps
  growing (a resumed run writes a second `run-start`, so restarts are visible).
- `--restart` is the only way to zero the counters.
- Restarting against a guest whose load is already running **adopts** it rather
  than killing and restarting it (which would reset the generation counter and
  lose the stall detector's baseline).
- `SIGINT`/`SIGTERM` finish the current poll, write state, and exit 2 with a
  reminder that re-running resumes.
- Corrupt or foreign state files are refused with an explanation instead of
  being silently overwritten — losing 40 hours to a "helpful" fresh start is
  worse than an error message.

## 6. The recovery sequence the break-glass gate performs

Exactly what this project has needed every single time a running guest is
stopped uncooperatively (documented in `orient.py`, `docs/zephyr-guest.md`,
`docs/dual-guest.md`), run over the **console**, because right after a hard
reset root is read-only, `/etc/rc` never reached sshd, and the console is the
only way in:

```sh
fsck_ffs -y /dev/vtbd0p3            # up to --fsck-passes (default 2)
mount -u -o reload / ; mount -u -o rw /
service netif restart ; route add default 192.168.88.1
service sshd start
```

`mount -u -o reload` before `-o rw` because the in-core superblock must be
re-read after fsck repaired the on-disk one. Then, over ssh (the other
channel): root is `rw` and the filesystem state is readable.

## 7. Both gates live in the one ledger

`breakglass_cycle.py` records every attempt in **`boot-ledger.jsonl`** — the
same cumulative, append-only ledger that carries the "100 clean boots" streak —
with `source="breakglass"`. `python3 boot_ledger.py` now prints both gates:

```
100-in-a-row gate : ✅ CLOSED
current streak    : 100
...
break-glass gate  : 0/20 (no attempts recorded yet — run breakglass_cycle.py)
  attempts        : 0  (0 recovered, 0 failed)
```

Break-glass entries are **excluded from the boot-streak arithmetic** by design:
an attempt is not a reload attempt (the reload *inside* it is already recorded
by `reliable_load.py`), so counting it would double-count the good case and let
a failed *recovery* truncate the boot streak for something that was never a
boot. No historical entry has a `breakglass` source, so this changed no
existing number. `soak72.py` tags its own reloads `soak72-reload` /
`soak72-breakglass`, and `reliable_load.py` grew a `--ledger-source` flag so a
long run's boots are distinguishable from manual ones.

## 8. Dry run (no board) — and what that does and does not prove

```sh
python3 soak72.py --dry-run              # 15 cases
python3 breakglass_cycle.py --dry-run    # 8 cases
python3 soak72.py --dry-run --scenario watchdog-reset --hours 0.25   # watch one
```

Both are in `make test`. They run the **real harness logic** against
`soaklib.FakeBoard` on a virtual clock, so a simulated 72 h run finishes in
milliseconds and every branch is walked — including the ones hardware cannot be
asked to produce on demand (a watchdog reset mid-soak, an EMAC wedge, a
filesystem fsck cannot clean, an isolation regression, a board that never comes
back, an attempt whose sshd does not return).

Two guards keep dry runs honest:

- **`api-parity`** — `FakeBoard` must implement the same 15-method facade as the
  real `Board`, checked on the classes. A dry run only means something if the
  fake answers the same calls.
- **`ledger-untouched`** — `boot-ledger.jsonl` must be byte-identical after the
  matrix. This is not paranoia: the first development dry run of `soak72.py`
  appended eight fabricated "clean boot" entries from a simulated board and
  inflated the already-closed 100-boot streak from 100 to 108 before they were
  removed. `soaklib.ledger_record()` now refuses to write when the dry-run flag
  is latched, at the one place that writes.

### Still unverified (read this before quoting any number)

**Neither harness has been run against the real board.** They were written while
another session owned the hardware, so:

- Every board-facing mechanic they drive is existing, board-proven code
  (`supervise.break_glass`, `reliable_load`, `bzdctl.collect_status`,
  `soak.capture_counters`, `guest_sh`, `a2_cycle`). The **harness logic around
  it** has been exercised only against the fake.
- Specifically unproven on hardware: the guest-side load script actually running
  under FreeBSD (`md5 -q`, `jot`-free loop, `/dev/urandom` throughput at
  `--size-mb 48`); the here-document that installs it surviving the lossy
  console; `GUEST_LOAD_PROBE`/`GUEST_SYSINFO_PROBE` parsing real output
  (`df -k`, `sysctl kern.boottime`, `dmesg`); the real timings behind
  `--uboot-timeout-s`, `--settle-s` and `--guest-timeout-s`; and whether a real
  `fsck_ffs -y` tail matches `FSCK_DIRTY_MARKERS`/`FSCK_REPAIRED_MARKERS`.
- The classifier's thresholds (60 s wedge grace, 15 min load-stall,
  `--max-resets 20`, the 50 % degradation fraction) are reasoned, not measured.

So: run `soak72.py --hours 0.2` and `breakglass_cycle.py --attempts 2` under
supervision, confirm the events look like the board you know, and only then
start the long runs.
