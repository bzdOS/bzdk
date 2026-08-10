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
python3 soak72.py --dry-run              # 36 cases
python3 breakglass_cycle.py --dry-run    # 13 cases
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

### Hardware status, 2026-08-10 — both harnesses have now met the board

The smoke runs the previous revision of this section asked for have been done,
under supervision, and both gates closed:

```
soak72.py --restart --hours 0.05 --poll-s 20 --size-mb 16
  bzdOS 72h SOAK — GATE CLOSED
  healthy load time : 0.06 h of 0.05 h        samples: 7 (7 healthy, 0 warnings)
  resets: 0   break-glass sent: 0   reloads: 0   max temperature: 36.2 C

breakglass_cycle.py --restart --attempts 1
  bzdOS BREAK-GLASS gate — CLOSED
  streak: 1/1     magic bytes sent: 1     fsck: 1 attempt needed a repair
  reset observed 20.0 s after the magic bytes; reload 49.9 s; ssh up 0.7 s after
  recovery; root rw
```

**Verified on hardware.** The guest-side load script really runs under FreeBSD
and the here-document install survives the console; `GUEST_LOAD_PROBE` /
`GUEST_SYSINFO_PROBE` parse real `df -k` / `sysctl` / `dmesg` output; load
generations accumulate (~71-79 gens/h at `--size-mb` 16-24) and cumulative
healthy load time advances; benign events are classified benign rather than
failed — `cpu2-idle` once per boot, and `emmc-retry-absorbed` repeatedly
(`ebio_fails` climbing to 61 while `g_ioerrs` stayed 0, exactly the
retry-covered case). For break-glass: the magic byte sequence, the USB-identity
flip that proves the watchdog really fired, the reload, `fsck_ffs -y` with a real
repair, the remount rw, and the ssh health verification — the whole chain.

**A real bug each, found by the first hardware contact** (which is what smoke
tests are for):

1. `bzd_board.USB_NODE` was hard-coded to `/sys/bus/usb/devices/1-4` while the
   board was on `2-4`, so `soak72.py` aborted instantly with *"board-off-usb —
   the ONE case with no remote recovery path"* against a perfectly healthy
   board. A false GONE is worse than no check, because it kills exactly the
   unattended runs these harnesses exist for. Fixed: `usb_find_node()` resolves
   the board by vid:pid, and `supervise.py` / `reliable_load.py` had the same
   latent fault.
2. The verdict printed **"GATE CLOSED"** and **"failures recorded : 1"** in the
   same box, because the state is resumable and `failures` accumulates across
   restarts. A mixed verdict is the worst output for something read by whoever
   comes back later. A recorded failure now disqualifies the gate outright and
   says so; use `--restart` to deliberately re-measure.

### Board-free coverage pass, 2026-08-10 — closing the dry-run gaps

The hardware smoke runs above closed both gates at tiny scale, but left a
specific hole: `early_rate` was `None` in both real runs, and — this is the
part that actually mattered — it had *never* been `None` for a good reason: no
dry-run case ran long enough to lock it either. A degradation check that has
never fired, on hardware or in simulation, is not a tested criterion. This
pass went through `classify_sample()` kind by kind, drove every reachable
branch from `soaklib.FakeBoard`, and fixed what it found broken.

**The degradation detector now actually fires, in simulation.** Two new
dry-run cases in `soak72.py` (`_dry_degradation_case`, `_dry_fluctuation_case`)
give `FakeBoard` a `rate_schedule` knob (`[(poll_threshold, gen_period), ...]`)
that genuinely slows the load's generation counter from a given poll onward,
long enough for the trailing rate window to lock in on it:

- a **sustained** slowdown (1/4 rate, never recovers) reports **not closed**
  with the `<-- DEGRADED` marker and `verdict.degraded == True`.
- a **transient** dip that fully recovers before the run ends reports
  **closed**, `degraded == False` — proving the check does not cry wolf on
  ordinary variance, which would be exactly as damaging as never firing.

Both reduce `--early-window-s` from the production 1800 s to 300 s so the dry
run stays fast; the mechanism exercised (lock an early rate, compare the
trailing window to it) is identical. **The real 1800 s / 50 % thresholds are
still unmeasured against hardware** — this closes the "cannot fire at all"
gap, not the "is 50 % the right number" question.

**One real bug found and fixed in `soaklib.classify_sample()`: `timer-stalled`
was dead code.** The old check computed `imo1 = (tick_delta == 0)` from the
CURRENT sample and then, inside the `else` branch (i.e. only when
`tick_delta != 0`), re-tested `if tick_delta == 0` — a condition that branch
had already excluded. The FAIL could never fire, on hardware or in dry-run,
regardless of what tick_delta did. Fixed by threading a sticky
`ever_ticked` flag from `soak72.sample()` through to the classifier: a
build's IMO-ness cannot change mid-run, so once a real (non-zero) tick has
been seen, `tick_delta == 0` from then on can only mean the tick stopped, not
"this is an IMO=1 build". Scenarios `imo0-build` (WARN path) and
`imo0-timer-stalled` (the FAIL, now reachable) cover both sides.

**Classifier coverage table** — every `classify_sample()` kind, and how it is
now reached in `--dry-run`:

| kind | severity | dry-run scenario |
|---|---|---|
| `board-off-usb` | FAIL | `usb-vanishes-live` (classifier's own check) + `dead-board` (do_reload's separate check of the same name) |
| `board-in-uboot` | RESET | `watchdog-reset` |
| `unexpected-usb-id` | WARN | `unexpected-usb-id` |
| `emac-flap` | BENIGN | `wedge` (first dark sample) |
| `wedge` | RESET | `wedge` (second dark sample, past the grace period) |
| `liveness-unknown` | BENIGN | `liveness-unknown` |
| `cpu2-idle` | BENIGN | any scenario's first poll (e.g. `happy`) |
| `core-frozen` | FAIL | `core-frozen` (cpu0 only — the identical code path would fire the same way for cpu1; not separately exercised, deemed redundant) |
| `isolation-breach` | FAIL | `isolation-breach` |
| `imo1-build` | BENIGN | any scenario's first poll |
| `timer-stalled` | FAIL | `imo0-timer-stalled` (was unreachable dead code before this pass, see above) |
| `exception-count-increased` | WARN | `imo0-build`, `imo0-timer-stalled` |
| `silent-reset` | RESET | `silent-reset` |
| `high-temperature` | WARN | `high-temperature` |
| `emmc-lock-stuck` | FAIL | `emmc-lock-stuck` |
| `guest-io-error` | WARN | `guest-io-error` |
| `emmc-retry-absorbed` | BENIGN | `emmc-retry-absorbed` |
| `emmc-recovery-clk-timeout` | FAIL | `emmc-recovery-clk-timeout` |
| `guest-network-down` | WARN | `guest-network-down` |
| `guest-unreachable` | FAIL | `guest-down-board-up` |
| `root-readonly` | RECOVER | `root-readonly-live` |
| `data-verify-mismatch` | FAIL | `data-corruption` |
| `load-not-running` | RECOVER | `load-crashed` |
| `load-stalled` | FAIL | `load-stalled` |
| `load-slow` | WARN | `load-stalled` (same scenario, earlier poll) |
| `disk-filling` | WARN | `disk-filling` |
| `guest-log-errors` | WARN | `log-warnings` |
| `healthy` | BENIGN | `happy` |

Plus the kinds emitted outside the classifier: `fsck-second-pass` (any
reset/reload scenario), and do_reload's generic "reload failed twice" ending —
previously reachable only via `dead-board`, which always resolves to
"vanished" first; scenario `reload-always-fails` now reaches the *other*
ending (board stays present in U-Boot, reload just never lands).
`breakglass_cycle.py` got the matching treatment: `watchdog-never-fires`
("sent" bytes, watchdog never actually resets the board — distinct from
`dead-board`'s "vanished"), `baseline-in-uboot` / `baseline-guest-unhealthy`
(`_dry_baseline_repair_case`, since this harness barely calls `status()` at
all — once per attempt — so the usual poll-indexed scripting doesn't reach
them), and `recover_and_verify()`'s "reload after the reset failed" branch
(`_dry_reload_after_reset_fails_case`).

**Every one of the above needed new `soaklib.FakeBoard` knobs** (`usb_override`,
`moving_unknown`, `tick_delta`, `temp_mc`, `lock_giveups`, `g_ioerrs`,
`ebio_fails`, `ebio_settle_clkfail`, `avail_kb`, `force_readonly`,
`reload_always_fails`, `breakglass_ineffective`, `rate_schedule`), each
defaulting to the value that was previously hard-coded inline, so no existing
scenario's behaviour changed by adding them (`api-parity` and all 15+8
original cases still pass unmodified).

**`--size-mb 48` (the default)** — checked what can be checked without the
board: `LOAD_SCRIPT` renders correctly at 48 (and 16/24, for comparison) and
`sh -n` syntax-checks clean; the only difference from the already-hardware-run
16-24 MB is the `SZ=` constant, no new shell syntax. No timeout in the harness
scales with `--size-mb` — `guest_start_load()`'s 120 s and the load probe's
60 s just start/sample the background process, and the load-stall
thresholds (300 s warn / 900 s fail) are wall-clock, not size-aware. Real
per-generation time is genuinely unmeasured at 48 MB: the hardware runs at
16-24 MB averaged ~45-50 s/generation; a naive linear extrapolation to 48 MB
lands around 90-150 s, comfortably under the 300 s warn threshold, but that is
an extrapolation, not a measurement — eMMC throughput under 2-3x the sustained
write volume could be non-linear (wear-leveling, garbage collection) in either
direction.

**Resume/idempotence under the new "a failure disqualifies the gate" rule** —
now explicitly tested, not just inferred:
- `_dry_resume_case` (soak72) now also asserts that an interrupted-then-resumed
  run reaches the **same closed/not-closed verdict** as an equivalent
  straight-through run, not just that the raw hour counter accumulates.
- `_dry_resume_carries_failure_case` (soak72) proves the exact shape of the
  hardware-found bug: a real failure recorded in leg 1 still disqualifies leg
  2's verdict even though leg 2 itself did nothing wrong, and `--restart`
  (leg 3) is what clears it.
- `_dry_restart_case` (breakglass) proves `--restart` zeroes the streak and
  attempt count instead of carrying the previous run's numbers forward.
- `_dry_status_readonly_case` (both harnesses) proves `--status` leaves the
  state file and event log byte-identical — snapshotted before and after,
  not just assumed from reading the code.

**Lose-work audit (72 h unattended, memory/log growth, non-atomic writes)** —
reviewed, nothing needed fixing in the files this pass owns:
- `RunState.save()` was already atomic (`tmp` + `os.replace()`); confirmed, not
  changed.
- `gen_window` was already capped at 120 entries (`del w[:-120]`); the
  degradation cases above are the first thing to actually rely on that cap
  doing its job (trimming the early-rate samples out of the trailing window).
- `state.d["failures"]` cannot grow past one entry within a single process
  lifetime: the very first FAIL raises `HarnessStop` and the run exits before
  a second one could ever be appended. It only grows further across *manual*
  restarts without `--restart` — bounded by an operator's actions, not by run
  duration.
- `EventLog` never holds events in memory; each `emit()` opens, writes,
  fsyncs and closes the file, so the append-only log growing across 72 h (an
  estimated few thousand lines at the default 60 s poll cadence) costs disk,
  not RAM.
- The one subprocess whose output *is* accumulated in a Python list —
  `soak.run_reliable_load()`'s `lines` — is out of this pass's file scope
  (`soak.py`) and, on inspection, is bounded per-call (one reload's runtime,
  freed on return), not across the 72 h run, so it is not a "dies at hour 60"
  risk. Noted, not touched.

### Still unverified (read this before quoting any number)

- **Duration.** The longest real run so far is minutes, not hours. Nothing is
  known about hardware behaviour over 72 h.
- **The degradation check's real thresholds.** The mechanism now fires (see
  above), in simulation, with a shortened window. The production 1800 s
  early-rate window and 50 % fraction remain reasoned, not measured against
  real load-rate variance — `early_rate` still has never been non-`None` on
  hardware.
- **20 break-glass resets in a row.** One has been proven. The gate wants 20, and
  this project has a documented history of the *second* or *third* cycle being
  the one that misbehaves.
- **The failure paths' real-world timing.** `--max-resets`, the 60 s
  wedge-detection grace and the 15 min load-stall timeout are now all reached
  in dry-run (see the coverage table above), but only against `FakeBoard`'s
  timing, not the real board's.
- **`--size-mb 48`'s real throughput.** Script correctness and arithmetic are
  checked (see above); actual generations/hour at 48 MB — and whether that
  stays comfortably clear of the load-stall thresholds — is unmeasured. Both
  hardware smoke runs used 16-24 MB.

So the harnesses are no longer untested code — but "the gate closed" above means
minutes of load and one reset, and must not be quoted as the v1 numbers.
