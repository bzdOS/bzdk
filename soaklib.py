#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""soaklib.py — shared plumbing for the two long, unattended v1-gate runs.

The two criteria in ROADMAP §2 that are still open are both *numbers nobody
has produced*, and they keep not happening for the same reason: they are long
(72 h / ~20 board resets), they will be interrupted, and there was no harness
you could start and walk away from. This module is the part both of them need:

  * `EventLog`  — append-only JSONL, fsync'd per line, so a crash **mid-soak**
    is still diagnosable afterwards. Nothing here ever rewrites history.
  * `RunState`  — a small atomically-replaced JSON file that makes a run
    RESUMABLE and IDEMPOTENT: relaunching the same command after a host
    reboot, an OOM kill or a Ctrl-C continues the run instead of restarting
    the count from zero.
  * `Board`     — one narrow facade over the board, so both harnesses talk to
    the hardware through ~12 named operations instead of ad-hoc subprocesses.
  * `FakeBoard` — the same 12 operations, simulated, which is what makes
    `--dry-run` able to exercise every code path (including the failure paths
    you cannot ask real hardware to produce on demand) with no board at all.
  * `classify_sample()` — the project's accumulated knowledge of *which
    alarming-looking events are benign here*, in one auditable place.

WHAT IS REUSED (this file implements no board mechanics of its own):
  bzdctl.collect_status()      fresh-vs-stale-latch liveness (the trap this
                               project has fallen into four times: diffing two
                               samples of a latch nobody updates)
  supervise.break_glass()      the `\\x00~BZRST\\x00` bytes, IMPORTED never
                               retyped, so they cannot drift from usbacm.c
  supervise.emac_alive()       flap-tolerant EMAC liveness
  soak.run_reliable_load()     bounded reload subprocess + tail extraction
  soak.capture_counters()      VBK1/EBIO/GIC counter snapshot (read_words_stable)
  soak.read_isolation()        A1 STG2[18] isolation self-check
  a2_cycle.fs_state()          "root rw? did fsck repair anything?" over ssh
  a2_cycle.guest()             the guest ssh invocation
  guest_sh.run()               the lossy-console command channel (frame+retry)
  bzd_board.*                  every board constant
  boot_ledger.record()         the ONE cumulative ledger both gates feed

HARD RULE THIS FILE ENFORCES: only one process may hold /dev/ttyACM0. Every
console operation goes through `Board.console()`, which refuses to open the tty
while another process holds it (see `tty_holder()`), because two readers steal
each other's bytes and a healthy channel then looks dead.
"""
import contextlib
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

GUEST_IP = "192.168.88.82"
GATEWAY = "192.168.88.1"
ROOT_DEV = "/dev/vtbd0p3"
HIGH_TEMP_C = 85.0

# Severity vocabulary. Only "reset", "recover" and "fail" make a harness act;
# "benign" and "warn" are recorded and moved past. Keeping the vocabulary this
# small is deliberate: an unattended run that cries wolf is a run whose owner
# stops reading the log.
BENIGN, WARN, RESET, RECOVER, FAIL = "benign", "warn", "reset", "recover", "fail"


class HarnessStop(Exception):
    """Raised to stop a run LOUDLY. The brief for both harnesses is explicit:
    if the board stops coming back, stop and say so — never keep churning."""


# ── clock (real / virtual) ────────────────────────────────────────────────
class Clock:
    """Real time. Every sleep in both harnesses goes through a Clock so that
    `--dry-run` can simulate 72 hours in milliseconds and still traverse the
    exact same branches, rather than testing a special short-run code path
    that the real run never takes."""

    virtual = False

    def now(self):
        return time.time()

    def sleep(self, s):
        if s > 0:
            time.sleep(s)


class FakeClock(Clock):
    virtual = True

    def __init__(self, t0=1_800_000_000.0):
        self.t = float(t0)
        self.slept = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        s = max(0.0, float(s))
        self.t += s
        self.slept += s


# ── append-only structured log ────────────────────────────────────────────
class EventLog:
    """One JSON object per line, flushed AND fsync'd immediately.

    Why fsync per event and not per run: the whole point of this log is to
    survive the failure it is recording. A soak that wedges the host, or a
    board that takes the harness's ssh session down with it, must still leave
    the last event on disk. Events are appended forever — a resumed run adds
    to the same file, so the file is the run's whole history across restarts.
    """

    def __init__(self, path, clock, run_id, stdout=True):
        self.path = path
        self.clock = clock
        self.run_id = run_id
        self.stdout = stdout
        self.seq = 0
        self.counts = {}
        d = os.path.dirname(os.path.abspath(path))
        if d:
            os.makedirs(d, exist_ok=True)

    def emit(self, kind, severity="info", **fields):
        self.seq += 1
        self.counts[severity] = self.counts.get(severity, 0) + 1
        ts = self.clock.now()
        rec = {"ts": round(ts, 3),
               "iso": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(ts)),
               "run": self.run_id, "seq": self.seq,
               "kind": kind, "severity": severity}
        rec.update(fields)
        line = json.dumps(rec, default=str)
        try:
            with open(self.path, "a") as f:
                f.write(line + "\n")
                f.flush()
                os.fsync(f.fileno())
        except Exception as e:                       # pragma: no cover
            print(f"!! event log write failed: {e}", file=sys.stderr)
        if self.stdout:
            mark = {"fail": "✖", "warn": "⚠", "reset": "↻", "recover": "⚕",
                    "benign": "·", "info": " "}.get(severity, " ")
            extra = " ".join(f"{k}={v}" for k, v in fields.items()
                             if k in ("detail", "note", "attempt", "phase",
                                      "load_h", "streak", "elapsed_s"))
            print(f"[{rec['iso']}] {mark} {kind}"
                  + (f"  {extra}" if extra else ""), flush=True)
        return rec


def read_events(path, kinds=None, severities=None):
    """Read an event log back. Used by the `--report` view and by the dry-run
    assertions — a log nobody can parse is not a log."""
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except Exception:
            continue                     # a torn last line is expected on crash
        if kinds and r.get("kind") not in kinds:
            continue
        if severities and r.get("severity") not in severities:
            continue
        out.append(r)
    return out


# ── resumable state ───────────────────────────────────────────────────────
class RunState:
    """Atomically-replaced JSON state, so an interrupted run resumes.

    `defaults` seeds a NEW run. On an existing file the stored values win, and
    any key added to `defaults` since the file was written is filled in — so a
    harness that grows a counter mid-run does not invalidate a 40-hour state
    file. `label` guards against pointing two different runs at one file.
    """

    def __init__(self, path, clock, label, defaults, restart=False):
        self.path = path
        self.clock = clock
        self.resumed = False
        self.d = dict(defaults)
        self.d.setdefault("label", label)
        self.d.setdefault("run_id", time.strftime("%Y%m%d-%H%M%S"))
        self.d.setdefault("created", clock.now())
        self.d.setdefault("restarts", [])
        if os.path.exists(path) and not restart:
            try:
                old = json.load(open(path))
            except Exception as e:
                raise HarnessStop(
                    f"state file {path} exists but is unreadable ({e}). Move it "
                    f"aside to start fresh, or fix it to resume — refusing to "
                    f"silently restart a long run from zero.")
            if old.get("label") not in (None, label):
                raise HarnessStop(
                    f"state file {path} belongs to run label "
                    f"{old.get('label')!r}, not {label!r}. Use --state to pick "
                    f"a different file.")
            for k, v in old.items():
                self.d[k] = v
            self.resumed = True
            self.d["restarts"] = list(self.d.get("restarts", [])) + [clock.now()]
        self.save()

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.d, f, indent=2, default=str)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)

    def bump(self, key, by=1):
        self.d[key] = self.d.get(key, 0) + by
        return self.d[key]


# ── guest-side commands, named once ───────────────────────────────────────
# Kept as module constants rather than inline strings so the docs can quote
# them, the fake can be checked against them, and there is exactly one place
# to fix when the guest changes.

# The sustained load. Realistic for THIS board's risk profile: the disk write
# path (virtio-blk -> eMMC) is where every stability bug in this project has
# lived, so the load writes, syncs, re-reads and VERIFIES, and churns metadata
# (inode/dir operations are what the corruption saga actually damaged). md5 and
# /dev/urandom keep a core busy at the same time. Bounded footprint: the blob
# is deleted every generation, so a 72 h run cannot fill the root filesystem.
LOAD_DIR = "/var/tmp/bzdsoak"
LOAD_SCRIPT = r"""#!/bin/sh
# bzd-soakload.sh -- sustained mixed disk/CPU/metadata load. Written by
# soaklib.py; safe to kill at any time. Progress and errors are files so the
# host can sample them over EITHER ssh or the console.
D=%(dir)s
SZ=%(size_mb)s
IDLE=%(idle_s)s
mkdir -p $D/tree
echo $$ > $D/pid
: > $D/errors
gen=0
while :; do
  gen=$((gen+1))
  dd if=/dev/urandom of=$D/blob bs=1m count=$SZ 2>/dev/null
  a=`md5 -q $D/blob 2>/dev/null`
  sync
  b=`md5 -q $D/blob 2>/dev/null`
  if [ -z "$a" ] || [ "$a" != "$b" ]; then
    echo "`date '+%%Y-%%m-%%dT%%H:%%M:%%S'` VERIFY-MISMATCH gen=$gen a=$a b=$b" >> $D/errors
  fi
  i=0
  while [ $i -lt 200 ]; do echo x > $D/tree/f$i; i=$((i+1)); done
  rm -f $D/tree/f*
  rm -f $D/blob
  echo "gen=$gen ts=`date +%%s`" > $D/progress
  sleep $IDLE
done
"""

# The 4-vCPU mixed load (--profile mixed). Same file contract as LOAD_SCRIPT
# (pid / progress / errors), so sampling, classification and the verdict are
# unchanged; the SD verify loop is still the one that advances `gen`. Around it
# run workers that each stress one path the single-disk load never touched:
#   cpu   bursts of 5-40 s then 0-20 s idle, so powerd keeps changing the
#         operating point (the RSB-trapped DVFS path, see guest-dvfs-works-on-dc)
#   emmc  re-reads random 64 MB windows of the READ-ONLY root and compares each
#         with the md5 it got the first time (root is ro, so any change is a
#         read-path corruption)
#   net   pulls a blob with a known md5 from the host's TFTP (vnet RX, EMAC)
#   gpu   limabench: pixel-checked lima renders; non-zero exit is a failure
# A worker that dies is an error, and so is any verify mismatch. The master
# traps TERM and takes the workers with it, so the idempotent restart in
# guest_start_load() never leaves orphans behind.
LOAD_SCRIPT_MIXED = r"""#!/bin/sh
D=%(dir)s
SZ=%(size_mb)s
IDLE=%(idle_s)s
NET_URL='%(net_url)s'
NET_MD5='%(net_md5)s'
GPU_CMD='%(gpu_cmd)s'
EMMC_DEV=%(emmc_dev)s
mkdir -p $D/tree $D/emmc
echo $$ > $D/pid
: > $D/errors
err() { echo "`date '+%%Y-%%m-%%dT%%H:%%M:%%S'` $*" >> $D/errors; }
rnd() { echo $((`od -An -N2 -tu2 /dev/urandom` %% $1)); }
w_cpu() {
  while :; do
    n=$((5 + `rnd 36`))
    timeout $n sh -c 'while :; do dd if=/dev/zero bs=1m count=256 2>/dev/null | md5 >/dev/null; done'
    sleep `rnd 21`
  done
}
w_emmc() {
  nmb=$((`diskinfo $EMMC_DEV | awk '{print $3}'` / 1048576 / 64))
  while :; do
    w=`rnd $nmb`
    h=`dd if=$EMMC_DEV bs=1m skip=$((w*64)) count=64 2>/dev/null | md5`
    if [ -f $D/emmc/$w ]; then
      [ "`cat $D/emmc/$w`" = "$h" ] || err "EMMC-MISMATCH win=$w got=$h want=`cat $D/emmc/$w`"
    else
      echo $h > $D/emmc/$w
    fi
    echo "win=$w ts=`date +%%s`" > $D/emmc.progress
    sleep 2
  done
}
w_net() {
  # NET_URL is "host:file" on the host's TFTP (69/udp is the one port the
  # host firewall opens to the LAN -- the board already boots from it)
  while :; do
    printf "binary\nblocksize 1428\nget %%s $D/net.blob\nquit\n" "${NET_URL#*:}" |
      timeout 300 tftp "${NET_URL%%%%:*}" >/dev/null 2>&1
    h=`md5 -q $D/net.blob 2>/dev/null`
    rm -f $D/net.blob
    [ "$h" = "$NET_MD5" ] || err "NET-MISMATCH got=$h want=$NET_MD5"
    echo "ts=`date +%%s`" > $D/net.progress
    sleep 10
  done
}
w_gpu() {
  while :; do
    $GPU_CMD > $D/gpu.last 2>&1 || err "GPU-FAIL rc=$? `tail -1 $D/gpu.last`"
    echo "ts=`date +%%s`" > $D/gpu.progress
    sleep 5
  done
}
W=""
w_cpu & W="$W $!:cpu"
w_cpu & W="$W $!:cpu"
[ -c $EMMC_DEV ] && { w_emmc & W="$W $!:emmc"; }
[ -n "$NET_URL" ] && { w_net & W="$W $!:net"; }
[ -n "$GPU_CMD" ] && { w_gpu & W="$W $!:gpu"; }
echo "$W" > $D/workers
# started by daemon(8) in a session of its own: one signal to that process
# group takes every worker AND their dd/md5/tftp children (and the daemon
# supervisor, which has nothing left to supervise)
PG=`ps -o pgid= -p $$ | tr -d ' '`
trap 'trap - TERM INT; kill -TERM -$PG 2>/dev/null; exit 0' TERM INT
gen=0
while :; do
  gen=$((gen+1))
  dd if=/dev/urandom of=$D/blob bs=1m count=$SZ 2>/dev/null
  a=`md5 -q $D/blob 2>/dev/null`
  sync
  b=`md5 -q $D/blob 2>/dev/null`
  if [ -z "$a" ] || [ "$a" != "$b" ]; then
    err "VERIFY-MISMATCH gen=$gen a=$a b=$b"
  fi
  i=0
  while [ $i -lt 200 ]; do echo x > $D/tree/f$i; i=$((i+1)); done
  rm -f $D/tree/f*
  rm -f $D/blob
  for p in $W; do
    kill -0 ${p%%:*} 2>/dev/null || { err "WORKER-DIED ${p#*:} pid=${p%%:*}"; W=`echo $W | sed "s|$p||"`; }
  done
  echo "gen=$gen ts=`date +%%s`" > $D/progress
  sleep $IDLE
done
"""

# Sampled every poll. One command, one line per field, so the lossy console
# only has to frame one reply. `|` separators are parsed positionally.
GUEST_LOAD_PROBE = (
    "cat %(dir)s/progress 2>/dev/null; echo '|'; "
    "wc -l < %(dir)s/errors 2>/dev/null || echo 0; echo '|'; "
    "(kill -0 `cat %(dir)s/pid 2>/dev/null` 2>/dev/null && echo RUNNING || "
    "echo STOPPED); echo '|'; "
    "tail -3 %(dir)s/errors 2>/dev/null; true")

GUEST_SYSINFO_PROBE = (
    "sysctl -n kern.boottime 2>/dev/null | tr -d '\\n'; echo '|'; "
    "df -k / | tail -1; echo '|'; "
    "dmesg 2>/dev/null | grep -ci 'error\\|fault\\|WARNING' || echo 0; true")

# The standard post-hard-reset recovery this project has needed EVERY time a
# running guest is stopped uncooperatively (documented in orient.py, in
# docs/zephyr-guest.md twice, and in docs/dual-guest.md). fsck first because a
# dirty filesystem is why root is read-only; `mount -u -o reload` before `-o rw`
# because the in-core superblock must be re-read after fsck repaired the
# on-disk one (orient.py prints exactly this pair).
RECOVER_FSCK = "fsck_ffs -y " + ROOT_DEV + " 2>&1 | tail -25"
RECOVER_MOUNT_RW = ("mount -u -o reload / 2>&1; mount -u -o rw / 2>&1; "
                    "mount | head -1")
RECOVER_NET = ("service netif restart 2>&1 | tail -4; "
               "route add default " + GATEWAY + " 2>&1; "
               "netstat -rn | grep -c '^default'")
RECOVER_SSHD = "service sshd start 2>&1 | tail -4; pgrep -q sshd && echo SSHD-UP"

# fsck sometimes needs a second pass: it reports FILE SYSTEM STILL DIRTY and a
# rerun then comes back clean. That is a documented normal outcome here, not a
# failure — so it must not be treated as one, and equally must not be hidden.
FSCK_DIRTY_MARKERS = ("FILE SYSTEM STILL DIRTY", "FILE SYSTEM MARKED DIRTY")
FSCK_REPAIRED_MARKERS = ("WAS MODIFIED", "MARKED CLEAN", "SALVAGE", "FIXED")


# ── the board facade ──────────────────────────────────────────────────────
def tty_holder(tty="/dev/ttyACM0"):
    """Who else has the tty open? Returns a string, or "" if free.

    This is the cheapest failure in this project to rule out and the most
    expensive to misdiagnose (a healthy channel looks completely dead when two
    processes read it). Both harnesses check it before every console use."""
    if not os.path.exists(tty):
        return f"{tty} MISSING (board off the USB bus, or usb_debug grabbed it)"
    try:
        r = subprocess.run(["fuser", "-v", tty], capture_output=True,
                           text=True, timeout=15)
        out = (r.stdout + r.stderr).strip()
    except Exception:
        return ""
    return out if tty in out else ""


class Board:
    """Real hardware. Every method delegates to the project's existing tool
    for that job; none of them reimplement board mechanics.

    NOT constructed by `--dry-run`, ever: a dry run must be incapable of
    reaching the board even by accident."""

    kind = "real"

    def __init__(self, iface="br0", log=None):
        self.iface = iface
        self.log = log
        self._ssh_ok_last = None
        # Imported lazily and defensively: a missing/at-fault helper must
        # degrade one capability, not prevent the harness from starting and
        # recording that fact.
        import bzd_board
        self.B = bzd_board
        self._sup = self._imp("supervise")
        self._bzdctl = self._imp("bzdctl")
        self._soak = self._imp("soak")
        self._a2 = self._imp("a2_cycle")
        self._gsh = self._imp("guest_sh")

    def _imp(self, name):
        try:
            return __import__(name)
        except Exception as e:
            if self.log:
                self.log.emit("helper-import-failed", WARN, module=name,
                              detail=f"{type(e).__name__}: {e}")
            return None

    # -- board-level state ------------------------------------------------
    def usb(self):
        """"uboot" | "hv" | "gone" | "other:<vid:pid>" — read off the OTG
        port's sysfs node (port-specific, unlike `lsusb`)."""
        # Resolve the board by IDENTITY, not by socket: a hard-coded port made
        # this read None and declared the board GONE while it sat there healthy
        # on a different port (2026-08-10). See bzd_board.usb_find_node().
        vp = self.B.usb_board_vidpid()
        if vp is None:
            return "gone"
        if vp == self.B.UBOOT_VIDPID:
            return "uboot"
        if vp == self.B.HVCON_VIDPID:
            return "hv"
        return "other:" + vp

    def emac_alive(self):
        return bool(self._sup and self._sup.emac_alive())

    def status(self):
        """bzdctl.collect_status(): reachable / health / moving / fresh.
        Never raises, and refuses to claim motion off a stale latch."""
        if not self._bzdctl:
            return {"reachable": False, "error": "bzdctl unavailable",
                    "moving": None}
        return self._bzdctl.collect_status(self.iface)

    def counters(self):
        return self._soak.capture_counters() if self._soak else {}

    def isolation(self):
        return self._soak.read_isolation() if self._soak else (None, {})

    # -- guest, over ssh (preferred) or the console (fallback) ------------
    def guest_alive(self):
        if not self._a2:
            return False
        rc, out, _ = self._a2.guest("echo UP", 20)
        ok = (rc == 0 and "UP" in out)
        self._ssh_ok_last = ok
        return ok

    def guest_fs_state(self):
        """a2_cycle.fs_state(): mount line, df, fsck-repair count, readonly.

        Since 2026-08-25 root is read-only BY DESIGN (fstab `ro`), so a
        read-only `/` is not the dirty-filesystem signal any more; when fstab
        says so, `readonly` reports /var -- where the load and every other
        write lands -- instead."""
        if not self._a2:
            return None
        fs = self._a2.fs_state()
        if not fs or not fs.get("readonly"):
            return fs
        rc, out, _ = self._a2.guest(
            "awk '$2==\"/\"{print $4}' /etc/fstab; echo '|'; "
            "mount -p | awk '$2==\"/var\"{print $4}'", 60)
        if rc != 0:
            return fs
        parts = [p.strip() for p in out.split("|")]
        if len(parts) == 2 and "ro" in parts[0].split(","):
            fs = dict(fs, root_ro_by_design=True,
                      readonly="rw" not in parts[1].split(","))
        return fs

    def console(self, cmd, timeout=60.0):
        """One command over the guest console. Refuses a busy tty."""
        busy = tty_holder(self.B.GUEST_ACM_TTY)
        if busy:
            raise HarnessStop(
                "refusing to open the guest console while another process "
                f"holds it: {busy}. One process per tty — kill the other one "
                "(by PID) and resume; the run state is on disk.")
        if not self._gsh:
            return None
        return self._gsh.run(cmd, timeout=timeout, quiet=True)

    def guest_exec(self, cmd, timeout=60.0):
        """Run one guest command over whichever channel is available.

        ssh when the guest has finished booting (fast, lossless); the console
        otherwise — which is the case that matters, because right after a hard
        reset root is read-only, /etc/rc never reached sshd, and the console is
        the ONLY way in. Returns text (possibly "") or None if neither channel
        produced a framed reply."""
        if self._a2:
            rc, out, _ = self._a2.guest(cmd, int(timeout) + 10)
            if rc == 0:
                return out
        return self.console(cmd, timeout=timeout)

    def guest_start_load(self, size_mb=48, idle_s=5, mixed=None):
        """(Re)start the sustained load inside the guest. Idempotent: kills a
        previous instance first, so calling it after every recovery is safe.
        `mixed` (a dict of LOAD_SCRIPT_MIXED's net_url/net_md5/gpu_cmd/
        emmc_dev) selects the 4-vCPU mixed load instead of the disk-only one."""
        if mixed is not None:
            body = LOAD_SCRIPT_MIXED % dict(mixed, dir=LOAD_DIR,
                                            size_mb=size_mb, idle_s=idle_s)
        else:
            body = LOAD_SCRIPT % {"dir": LOAD_DIR, "size_mb": size_mb,
                                  "idle_s": idle_s}
        # Written with a here-document so it survives both channels (no scp,
        # and the console cannot be trusted with long single lines).
        cmd = (f"mkdir -p {LOAD_DIR}; "
               f"if [ -f {LOAD_DIR}/pid ]; then P=`cat {LOAD_DIR}/pid`; G=; "
               # the group only when that pid IS our load.sh: a stale pid
               # file may name a reused pid (sshd's group, say)
               f"ps -o command= -p $P 2>/dev/null | grep -q {LOAD_DIR}/load.sh "
               f"&& G=`ps -o pgid= -p $P | tr -d ' '`; "
               f"kill $P 2>/dev/null; [ -n \"$G\" ] && [ \"$G\" != 0 ] && "
               f"kill -TERM -$G 2>/dev/null; sleep 1; fi; "
               f"cat > {LOAD_DIR}/load.sh <<'BZDEOF'\n{body}BZDEOF\n"
               f"chmod +x {LOAD_DIR}/load.sh; "
               f"daemon -o {LOAD_DIR}/load.out {LOAD_DIR}/load.sh; "
               f"sleep 2; echo STARTED-`cat {LOAD_DIR}/pid 2>/dev/null`")
        out = self.guest_exec(cmd, timeout=120)
        return bool(out and "STARTED-" in out and
                    out.split("STARTED-")[-1].strip()[:1].isdigit())

    def guest_load_progress(self):
        """{gen, ts, errors, running, error_tail} or None if unreadable."""
        out = self.guest_exec(GUEST_LOAD_PROBE % {"dir": LOAD_DIR}, timeout=60)
        return parse_load_probe(out)

    def guest_sysinfo(self):
        out = self.guest_exec(GUEST_SYSINFO_PROBE, timeout=60)
        return parse_sysinfo(out)

    def guest_recover(self, fsck_passes=2):
        """The standard post-hard-reset recovery, over the console.

        fsck (up to `fsck_passes`, because a STILL DIRTY first pass followed by
        a clean second is a documented normal outcome here) -> mount rw ->
        network -> sshd. Returns a dict of what each step said; the caller
        decides pass/fail from `ok`."""
        rec = {"fsck": [], "repaired": False, "still_dirty": False}
        for p in range(1, max(1, fsck_passes) + 1):
            out = self.console(RECOVER_FSCK, timeout=900) or ""
            rec["fsck"].append({"pass": p, "tail": out[-800:]})
            dirty = any(m in out.upper() for m in FSCK_DIRTY_MARKERS)
            if any(m in out.upper() for m in FSCK_REPAIRED_MARKERS):
                rec["repaired"] = True
            rec["still_dirty"] = dirty
            if not dirty:
                break
        rec["mount"] = self.console(RECOVER_MOUNT_RW, timeout=180) or ""
        rec["net"] = self.console(RECOVER_NET, timeout=240) or ""
        rec["sshd"] = self.console(RECOVER_SSHD, timeout=180) or ""
        rec["ok"] = evaluate_recovery(rec)
        return rec

    # -- destructive verbs ------------------------------------------------
    def break_glass(self):
        """supervise.break_glass(): writes the `\\x00~BZRST\\x00` magic to
        /dev/ttyACM0, which usbacm.c matches and turns into wdt_debug_hold=1,
        so the hardware watchdog resets the board to U-Boot within ~16 s.
        The bytes are imported, never retyped."""
        busy = tty_holder(self.B.GUEST_ACM_TTY)
        if busy:
            raise HarnessStop(f"cannot send break-glass: tty busy: {busy}")
        if not self._sup:
            raise HarnessStop("supervise.py unavailable — no break-glass path")
        return bool(self._sup.break_glass())

    def reload(self, log_path, timeout_s=1800, boot_to_shell=True,
               load_cycles=3, expect_vbk=False):
        """soak.run_reliable_load(): catch U-Boot -> TFTP -> loady+bootelf ->
        EMAC verify, as a subprocess bounded by an outer wall clock.
        Returns (ok, elapsed_s, tail, timed_out)."""
        if not self._soak:
            raise HarnessStop("soak.py unavailable — no reload path")
        ok, el, tail, to, _interrupted = self._soak.run_reliable_load(
            expect_vbk, load_cycles, timeout_s, log_path,
            boot_to_shell=boot_to_shell)
        return ok, el, tail, to


# ── parsers (shared by real board and tests) ─────────────────────────────
def parse_load_probe(out):
    if out is None:
        return None
    parts = [p.strip() for p in out.split("|")]
    if len(parts) < 3:
        return None
    gen = ts = None
    for tok in parts[0].split():
        if tok.startswith("gen="):
            gen = _int(tok[4:])
        elif tok.startswith("ts="):
            ts = _int(tok[3:])
    errors = _int(parts[1].split()[0] if parts[1].split() else "0") or 0
    return {"gen": gen, "ts": ts, "errors": errors,
            "running": "RUNNING" in parts[2],
            "error_tail": parts[3].strip() if len(parts) > 3 else ""}


def parse_sysinfo(out):
    if out is None:
        return None
    parts = [p.strip() for p in out.split("|")]
    d = {"boottime": parts[0] if parts else ""}
    if len(parts) > 1:
        f = parts[1].split()
        # df -k /: Filesystem 1K-blocks Used Avail Capacity Mounted
        d["avail_kb"] = _int(f[3]) if len(f) > 4 else None
        d["capacity"] = f[4] if len(f) > 4 else None
    if len(parts) > 2:
        d["dmesg_errors"] = _int(parts[2].split()[0] if parts[2].split() else "0")
    return d


def _int(s):
    try:
        return int(str(s).strip())
    except Exception:
        return None


def evaluate_recovery(rec):
    """Did the documented recovery actually work? Text-based, because these
    commands are run over a channel that has no exit status."""
    if rec.get("still_dirty"):
        return False
    mount = (rec.get("mount") or "")
    if "read-only" in mount or "/ " not in mount:
        return False
    return True


# ── benign-vs-real classification ────────────────────────────────────────
def classify_sample(s, prev, cfg):
    """Turn one health sample into a list of (severity, kind, detail).

    This function is where this project's hard-won "that alarming thing is
    normal here" knowledge lives. Each benign rule cites why, because the
    alternative — a harness that flags them — is a harness whose owner learns
    to ignore it, and then misses the one real failure.

    `s` is the sample dict built by the caller; `prev` the previous one (or
    None); `cfg` a dict of thresholds.
    """
    ev = []
    usb = s.get("usb")
    st = s.get("status") or {}

    # --- reachability ---------------------------------------------------
    if usb == "gone":
        ev.append((FAIL, "board-off-usb",
                   "board vanished from the USB bus — the ONE case with no "
                   "remote recovery path (supervise.py's GONE state)"))
        return ev
    if usb == "uboot":
        # SESSION-RULES R2: the hardware watchdog resetting the board back into
        # U-Boot is DELIBERATE anti-brick behaviour, not a fault. It must be
        # counted and recovered from, not called a soak failure.
        ev.append((RESET, "board-in-uboot",
                   "board is in U-Boot: the HW watchdog reset it (R2, by "
                   "design). Recoverable by reload."))
        return ev
    if usb and usb.startswith("other:"):
        ev.append((WARN, "unexpected-usb-id", usb))

    if not st.get("reachable"):
        dark = s.get("emac_dark_s", 0)
        if dark < cfg.get("wedge_grace_s", 60):
            # supervise.emac_alive()'s own docstring: the raw read flaps under
            # heavy guest I/O. A single dark sample is not a wedge.
            ev.append((BENIGN, "emac-flap",
                       f"EMAC did not answer (dark {dark:.0f}s < grace)"))
        else:
            ev.append((RESET, "wedge",
                       f"HV gadget present but EMAC dark for {dark:.0f}s — "
                       f"break-glass, exactly as supervise.py does live"))
        return ev

    h = st.get("health") or {}
    moving = st.get("moving")

    # --- liveness -------------------------------------------------------
    if moving is None:
        # bzdctl documents this: the BMC1 record is a LATCH. If the health verb
        # could not be poked, the record read says NOTHING about motion. Four
        # confidently-wrong diagnoses in this project came from ignoring that.
        ev.append((BENIGN, "liveness-unknown",
                   "health verb unreachable this poll; the record is a stale "
                   "latch, so motion is unknown (not frozen)"))
    else:
        for core, mv in sorted(moving.items()):
            if mv:
                continue
            if core == "cpu2":
                # CPU2 is the async eMMC I/O server: idle guest, nothing to do.
                # Once per boot, for the same reason as imo1-build below.
                if prev is None:
                    ev.append((BENIGN, "cpu2-idle",
                               "CPU2 posts no progress with no I/O outstanding "
                               "— benign, noted once per boot"))
            else:
                ev.append((FAIL, "core-frozen",
                           f"{core} heartbeat not advancing across two FRESH "
                           f"samples (cpu0=guest, cpu1=debug/watchdog core)"))

    # --- isolation ------------------------------------------------------
    iso = s.get("isol_pass")
    if iso is not None and iso != 1:
        ev.append((FAIL, "isolation-breach",
                   f"A1 STG2[18] ISOL self-check = {iso}: the guest may be "
                   f"able to reach HV memory"))

    # --- exceptions / timer, build-model aware --------------------------
    # Under the IMO=1 vGIC trunk EL2 runs no periodic tick (tick_delta==0 BY
    # DESIGN) and exc_count counts EVERY physical IRQ EL2 takes, millions per
    # second — it is not a fault counter there. soak.py calibrated this; if it
    # is not respected the run flags every poll of every cycle.
    #
    # tick_delta==0 ALONE cannot tell "IMO=1 build, always 0, benign" apart
    # from "IMO=0 build whose tick just stalled, FAIL" — both look identical
    # in a single sample. `s["ever_ticked"]` (threaded through by
    # soak72.sample()) is whether THIS run has ever seen a non-zero
    # tick_delta at all. A build's IMO-ness cannot change mid-run, so once a
    # real tick has been seen, tick_delta==0 from then on can only mean the
    # tick stopped. (An earlier version of this check computed `imo1` from
    # the CURRENT sample's tick_delta alone and then re-tested the same value
    # inside the branch that condition had already excluded — "timer-stalled"
    # was unreachable dead code as a result; fixed here, 2026-08-10.)
    td = h.get("tick_delta")
    ever_ticked = s.get("ever_ticked", False)
    if td == 0 and not ever_ticked:
        if prev is None:
            # Emitted ONCE per run/boot rather than every poll: a fact about the
            # build does not change between samples, and a 72 h run that repeats
            # it 4300 times is a log nobody reads.
            ev.append((BENIGN, "imo1-build",
                       "tick_delta=0 and a climbing exc_count are by design on "
                       "the IMO=1 vGIC build (no EL2 tick; exc_count counts "
                       "all IRQs) — this note is emitted once per boot"))
    elif td == 0 and ever_ticked:
        ev.append((FAIL, "timer-stalled",
                   "EL2 GIC-timer tick_delta=0 after this run already "
                   "observed a real (non-zero) tick — an IMO=0 build's timer "
                   "has stalled; this is not the IMO=1 by-design case"))
    else:
        pe = (prev or {}).get("exc_count")
        ce = h.get("exc_count")
        if pe is not None and ce is not None and ce > pe:
            ev.append((WARN, "exception-count-increased",
                       f"exc_count {pe} -> {ce} last_kind="
                       f"0x{h.get('last_exc_kind', 0):x} "
                       f"esr=0x{h.get('last_exc_esr', 0):08x}"))

    # --- uptime regression = something reset underneath us --------------
    pu, cu = (prev or {}).get("uptime_s"), s.get("uptime_s")
    if pu is not None and cu is not None and cu + 5 < pu:
        ev.append((RESET, "silent-reset",
                   f"HV uptime went backwards ({pu}s -> {cu}s): the board "
                   f"reset and came back without the harness driving it"))

    # --- thermals -------------------------------------------------------
    t = s.get("temp_c")
    if t and t > cfg.get("high_temp_c", HIGH_TEMP_C):
        ev.append((WARN, "high-temperature", f"{t:.1f} C"))

    # --- HV I/O counters ------------------------------------------------
    c = s.get("counters") or {}
    if c.get("lock_giveups"):
        ev.append((FAIL, "emmc-lock-stuck",
                   f"lock_giveups={c['lock_giveups']} — a leaked unlock, not "
                   f"contention; waiting cannot fix it"))
    if c.get("g_ioerrs"):
        ev.append((WARN, "guest-io-error",
                   f"g_ioerrs={c['g_ioerrs']} busy={c.get('ioerr_busy')} "
                   f"emmc={c.get('ioerr_emmc')} lba={c.get('ioerrN_lba')}"))
    if c.get("ebio_fails") and not c.get("g_ioerrs"):
        # Absorbed by the retry: healthy outcome, but must be visible, because
        # VBK1 alone reports it as silence.
        ev.append((BENIGN, "emmc-retry-absorbed",
                   f"ebio_fails={c['ebio_fails']} with g_ioerrs=0 — the eMMC "
                   f"misbehaved and the retry covered it"))
    if c.get("ebio_settle_clkfail"):
        ev.append((FAIL, "emmc-recovery-clk-timeout",
                   f"clk re-program timed out {c['ebio_settle_clkfail']}x — "
                   f"the controller reset is not completing, retries cannot help"))

    # --- guest ----------------------------------------------------------
    fs = s.get("fs")
    lp = s.get("load")
    if fs is None and not s.get("guest_alive"):
        if lp and lp.get("gen") is not None:
            # soak.py's rule: writing-but-unreachable is a networking problem,
            # not a boot failure. Do not conflate the two.
            ev.append((WARN, "guest-network-down",
                       "guest not reachable over ssh but its load is still "
                       "advancing — networking, not a dead guest"))
        else:
            ev.append((FAIL, "guest-unreachable",
                       "guest answered neither ssh nor the console, and no "
                       "load progress could be read"))
            return ev
    if fs and fs.get("readonly"):
        ev.append((RECOVER, "root-readonly",
                   "root is mounted read-only: the filesystem was left dirty, "
                   "so /etc/rc never finished. Standard fsck+remount recovery."))
    if lp is not None:
        if lp.get("errors"):
            ev.append((FAIL, "data-verify-mismatch",
                       f"the load's own read-back verify FAILED "
                       f"({lp['errors']} line(s)): {lp.get('error_tail', '')[:200]}"))
        if not lp.get("running"):
            ev.append((RECOVER, "load-not-running",
                       "the guest-side load process is not alive — restart it"))
        pg = (prev or {}).get("load_gen")
        cg = lp.get("gen")
        if pg is not None and cg is not None and cg == pg:
            stalled = s.get("load_stalled_s", 0)
            if stalled > cfg.get("load_stall_fail_s", 900):
                ev.append((FAIL, "load-stalled",
                           f"load generation stuck at {cg} for {stalled:.0f}s "
                           f"— the guest is not making progress"))
            elif stalled > cfg.get("load_stall_warn_s", 300):
                ev.append((WARN, "load-slow",
                           f"load generation stuck at {cg} for {stalled:.0f}s"))
    si = s.get("sysinfo")
    if si and prev:
        av, pav = si.get("avail_kb"), (prev or {}).get("avail_kb")
        if av is not None and pav is not None and av < cfg.get("min_avail_kb",
                                                              64 * 1024):
            ev.append((WARN, "disk-filling",
                       f"only {av} KiB free on / (was {pav})"))
        de, pde = si.get("dmesg_errors"), (prev or {}).get("dmesg_errors")
        if de is not None and pde is not None and de > pde:
            # Guest-side complaints (device errors, faults, warnings) that never
            # reach an EL2 counter. A warning, not a failure: FreeBSD logs plenty
            # of benign warnings, and the disqualifying conditions are measured
            # directly above.
            ev.append((WARN, "guest-log-errors",
                       f"guest dmesg error/fault/warning lines {pde} -> {de}"))
    if not ev:
        ev.append((BENIGN, "healthy", "all sampled signals nominal"))
    return ev


def worst(events):
    order = {BENIGN: 0, "info": 0, WARN: 1, RESET: 2, RECOVER: 2, FAIL: 3}
    return max((order.get(sv, 0) for sv, _k, _d in events), default=0)


# ── the fake board (dry-run) ─────────────────────────────────────────────
class FakeBoard:
    """A board simulator with enough behaviour to walk every branch.

    It models what the real board actually does, including the parts a real
    run cannot be asked to do on demand: a watchdog reset mid-soak, an EMAC
    wedge, a filesystem that needs two fsck passes, a guest that stops making
    progress, an isolation-check regression, and a board that never comes back.

    Health dicts come from bzdctl._mkhealth(), which builds them through
    bmc_client's REAL wire-decode path — so the fake cannot drift into a shape
    the real decoder would never produce.
    """

    kind = "fake"

    def __init__(self, clock, scenario="happy", log=None):
        self.clock = clock
        self.scenario = scenario
        self.log = log
        self.calls = {}
        self.polls = 0
        self.phase = "hv"                 # hv | uboot | gone
        self.emac = True
        self.guest = "multiuser"          # multiuser | ro_root | down
        self.fs_dirty_passes = 0
        self.gen = 1
        # A freshly-booted guest has no load running yet: the harness is
        # expected to start it. (Set this True in a test to exercise the
        # resumed-run path where the load is already going.)
        self.load_running = False
        self.load_errors = 0
        self.isol = 1
        self.boot_epoch = clock.now()
        self.reloads = 0
        self.break_glasses = 0
        self.recoveries = 0
        # Extra knobs added for a board-free coverage pass (2026-08-10): every
        # one of these defaults to the value that was PREVIOUSLY HARD-CODED
        # inline below, so no existing scenario's behaviour changes just from
        # adding them. A scenario (or a dry-run test that pokes the board
        # directly, the way the resume/idempotent-load cases already do)
        # sets one to reach a classify_sample() branch that a dry run could
        # not otherwise exercise — see soak72.py's / breakglass_cycle.py's
        # DRY_CASES for which ones now use each.
        self.usb_override = None          # usb() returns this verbatim if set
        self.moving_unknown = False       # status(): moving=None (BMC1 latch)
        self.cpu1_frozen = False
        self.tick_delta = 0               # 0 == IMO=1 build, by design
        self.temp_mc = 52000
        self.lock_giveups = 0
        self.g_ioerrs = 0
        self.ebio_fails = 0
        self.ebio_settle_clkfail = 0
        self.avail_kb = 4_600_000
        self.force_readonly = False       # root goes ro WITHOUT a reboot
        self.reload_always_fails = False  # board stays put, reload never lands
        self.breakglass_ineffective = False  # bytes "sent", watchdog never fires
        self.rate_schedule = []           # [(poll_threshold, gen_period), ...]
        self.never_returns = (scenario == "dead-board")
        self.script = {}                  # poll index -> callable(self)
        self._build_scenario()
        try:
            import bzdctl
            self._mkhealth = bzdctl._mkhealth
        except Exception:                  # pragma: no cover
            self._mkhealth = None

    # -- scenarios -------------------------------------------------------
    def _build_scenario(self):
        s = self.scenario

        def wd_reset(b):
            b.phase = "uboot"
            b.emac = False
            b.guest = "down"

        def wedge(b):
            b.emac = False

        if s == "happy":
            pass
        elif s == "watchdog-reset":
            self.script[3] = wd_reset
        elif s == "repeated-resets":
            for i in (3, 6, 9, 12):
                self.script[i] = wd_reset
        elif s == "wedge":
            self.script[3] = wedge
        elif s == "dead-board":
            self.script[3] = wd_reset
        elif s == "isolation-breach":
            def breach(b):
                b.isol = 0
            self.script[3] = breach
        elif s == "load-stalled":
            def stall(b):
                # The process is still alive (so "restart the load" is not the
                # answer) but it stops making progress -- the shape of a guest
                # that is stuck in the block layer rather than crashed.
                b.load_running = True
                b.gen_frozen = True
                b.unstallable = True
            self.script[2] = stall
        elif s == "data-corruption":
            def corrupt(b):
                b.load_errors = 1
            self.script[3] = corrupt
        elif s == "fsck-still-dirty":
            self.script[2] = wd_reset
            self.fs_dirty_forever = True
        elif s == "log-warnings":
            # A WARN must be recorded and must NOT stop the run or forfeit the
            # healthy time -- otherwise a 72 h run dies on FreeBSD's ordinary
            # chatter.
            def warns(b):
                b.dmesg_errors = 7
            self.script[3] = warns
        elif s == "flaky-verify":
            # One attempt's post-recovery verification fails (sshd did not come
            # back that time) and later attempts are fine. This is the shape of
            # a single bad attempt on a board that is otherwise healthy -- the
            # case that separates "stop on the first failure" (the gate wants N
            # in a row) from "the board is dead".
            self.ssh_dead_while_reloads = 1
        elif s == "core-frozen":
            def freeze(b):
                b.cpu0_frozen = True
            self.script[3] = freeze
        elif s == "usb-vanishes-live":
            # Distinct from "dead-board": here the board disappears from the
            # USB bus DURING an ordinary health poll, not as the outcome of a
            # failed reload/break-glass retry. Exercises classify_sample()'s
            # OWN top-of-function board-off-usb branch, which do_reload()'s
            # separate (already-covered) board-off-usb check never reaches.
            def vanish(b):
                b.phase = "gone"
                b.emac = False
                b.guest = "down"
            self.script[3] = vanish
        elif s == "unexpected-usb-id":
            # A USB identity that is neither U-Boot's nor the HV gadget's --
            # some other device on the port -- while EMAC still answers.
            def other_id(b):
                b.usb_override = "other:1234:5678"
            self.script[3] = other_id
        elif s == "liveness-unknown":
            # The BMC1 health verb could not be polled this round even though
            # the board is otherwise reachable -- the record is a stale
            # LATCH, so classify_sample() must say "unknown", never "frozen".
            def unknown(b):
                b.moving_unknown = True
            self.script[3] = unknown
        elif s == "imo0-build":
            # A non-IMO1 build: the EL2 tick is real (tick_delta != 0) for the
            # whole run, so a climbing exc_count is an ordinary WARN, never
            # the IMO=1 build's once-per-boot benign note.
            self.tick_delta = 137
        elif s == "imo0-timer-stalled":
            # The tick was real (proving this is NOT an IMO=1 build) and then
            # stops: tick_delta==0 from here on means the timer actually
            # stalled, a hard failure, not the "by design" IMO=1 case.
            self.tick_delta = 137

            def stall_timer(b):
                b.tick_delta = 0
            self.script[4] = stall_timer
        elif s == "high-temperature":
            def hot(b):
                b.temp_mc = 90000
            self.script[3] = hot
        elif s == "emmc-lock-stuck":
            def stuck(b):
                b.lock_giveups = 3
            self.script[3] = stuck
        elif s == "guest-io-error":
            def ioerr(b):
                b.g_ioerrs = 2
            self.script[3] = ioerr
        elif s == "emmc-retry-absorbed":
            def absorbed(b):
                b.ebio_fails = 12      # g_ioerrs stays 0: the retry covered it
            self.script[3] = absorbed
        elif s == "emmc-recovery-clk-timeout":
            def clkfail(b):
                b.ebio_settle_clkfail = 1
            self.script[3] = clkfail
        elif s == "guest-network-down":
            # ssh is down (guest_alive() False) but the load's own generation
            # counter is still readable over the OTHER channel (the console)
            # -- soak.py's rule: writing-but-unreachable is a networking
            # problem, not a dead guest, so this is a WARN, not the FAIL
            # "guest-down-board-up" reaches.
            def net_down(b):
                b.guest = "ro_root"
            self.script[3] = net_down
        elif s == "guest-down-board-up":
            # The GUEST stops answering (neither ssh nor console, no load
            # progress) while the board/EMAC itself stays perfectly healthy
            # -- a real dead-guest failure, distinct from a board-level wedge.
            def guest_down(b):
                b.guest = "down"
            self.script[3] = guest_down
        elif s == "root-readonly-live":
            # Root remounts read-only on its own -- ssh stays reachable, no
            # reboot ever observed -- caught and repaired by the ordinary
            # poll loop, not a reset path.
            def ro(b):
                b.force_readonly = True
            self.script[3] = ro
        elif s == "load-crashed":
            # The LOAD PROCESS dies while the guest stays up, reachable and
            # rw -- a lighter recovery than a full guest_recover().
            def crash(b):
                b.load_running = False
            self.script[3] = crash
        elif s == "disk-filling":
            def filling(b):
                b.avail_kb = 30_000       # well under --min-avail-kb
            self.script[3] = filling
        elif s == "silent-reset":
            # Uptime goes backwards without the harness ever seeing usb flip
            # to uboot or EMAC go dark -- a reboot fast enough that neither
            # check caught it directly. (Backdated by 1s, not to exactly
            # clock.now(): sample()'s `if h.get("uptime") else None` guard
            # treats an exact 0 as "no reading", same as a missing key. Poll
            # 4, not 3: run()'s pre-loop load-progress check costs one status()
            # call before the main loop's own first sample, so poll 3 is only
            # the SECOND real sample and its prev's uptime is itself still the
            # falsy-zero "no reading" case -- poll 4 is the first with a
            # genuinely non-None previous uptime to regress against.)
            def silent(b):
                b.boot_epoch = b.clock.now() - 1.0
            self.script[4] = silent
        elif s == "reload-always-fails":
            # The board stays PRESENT and reachable in U-Boot (never "gone"),
            # but the reload itself never lands twice in a row -- exercises
            # do_reload()'s generic "reload failed twice" ending, which
            # "dead-board" never reaches (it always ends up "gone" first).
            def stuck_in_uboot(b):
                b.phase = "uboot"
                b.emac = False
                b.guest = "down"
                b.reload_always_fails = True
            self.script[3] = stuck_in_uboot
        elif s == "watchdog-never-fires":
            # break_glass() "sends" the bytes but the watchdog never actually
            # resets the board -- "sent" is not "done". Only meaningful to
            # breakglass_cycle.py's send_break_glass(), which waits for the
            # USB identity to flip and must fail loudly when it never does.
            self.breakglass_ineffective = True
        else:
            raise HarnessStop(f"unknown dry-run scenario {s!r}")

    def _tick(self):
        self.polls += 1
        fn = self.script.pop(self.polls, None)
        if fn:
            fn(self)
        if self.phase == "hv" and self.load_running and \
                not getattr(self, "gen_frozen", False):
            # rate_schedule lets a test slow (or restore) the generation rate
            # from a given poll onward -- e.g. [(40, 4)] means "from poll 40,
            # only 1 poll in 4 advances gen", a real and durable slowdown for
            # the degradation detector to catch. Defaults to period 1 (every
            # poll advances gen), i.e. unchanged behaviour for every scenario
            # that never sets it.
            period = 1
            for threshold, p in self.rate_schedule:
                if self.polls >= threshold:
                    period = p
            if self.polls % period == 0:
                self.gen += 1

    def _count(self, name):
        self.calls[name] = self.calls.get(name, 0) + 1

    # -- facade ----------------------------------------------------------
    def usb(self):
        self._count("usb")
        if self.usb_override is not None:
            return self.usb_override
        return {"hv": "hv", "uboot": "uboot", "gone": "gone"}[self.phase]

    def emac_alive(self):
        self._count("emac_alive")
        return self.phase == "hv" and self.emac

    def status(self):
        self._count("status")
        self._tick()
        if self.phase != "hv" or not self.emac:
            return {"reachable": False, "moving": None,
                    "error": "no BMC1 record (board down)"}
        up = int((self.clock.now() - self.boot_epoch) * 24_000_000)
        hb = self.polls * 10
        cpu0 = 0 if getattr(self, "cpu0_frozen", False) else hb
        cpu1 = 0 if self.cpu1_frozen else hb
        h = (self._mkhealth(uptime_lo=up & 0xffffffff, uptime_hi=up >> 32,
                            hb_cpu0=cpu0, hb_cpu1=hb, hb_cpu2=hb,
                            hb_cpu3=0xffffffff, tick_delta=self.tick_delta,
                            exc_count=self.polls, temp_mc=self.temp_mc)
             if self._mkhealth else
             {"tick_delta": self.tick_delta, "exc_count": self.polls,
              "temp_mc": self.temp_mc, "uptime": up})
        if self.moving_unknown:
            moving = None
        else:
            moving = {"cpu0": cpu0 != 0, "cpu1": cpu1 != 0, "cpu2": False}
        return {"reachable": True, "fresh": True, "health": h, "moving": moving,
                "console_advancing": True, "error": None}

    def counters(self):
        self._count("counters")
        if self.phase != "hv":
            return {"channel": "down"}
        return {"g_reads": 1000 * self.polls, "g_writes": 500 * self.polls,
                "g_ioerrs": self.g_ioerrs, "write_retries": 1,
                "lock_giveups": self.lock_giveups,
                "ebio_fails": self.ebio_fails,
                "ebio_settle_clkfail": self.ebio_settle_clkfail}

    def isolation(self):
        self._count("isolation")
        if self.phase != "hv":
            return None, {}
        return self.isol, {"hvimg_f": 0, "hvscr_f": 0, "ctrl_f": 0}

    def guest_alive(self):
        self._count("guest_alive")
        if self.reloads == getattr(self, "ssh_dead_while_reloads", None):
            return False          # scenario "flaky-verify": ssh dead this pass
        return self.phase == "hv" and self.guest == "multiuser"

    def guest_fs_state(self):
        self._count("guest_fs_state")
        if self.phase != "hv" or self.guest == "down":
            return None
        # `guest == "ro_root"` is the post-reset shape (ssh not up yet either,
        # so guest_alive() is False and this is never even called). scenario
        # "root-readonly-live" needs the OTHER shape -- ssh stays reachable
        # (guest_alive() True) while root has spontaneously gone read-only --
        # which is `force_readonly`, independent of the `guest` phase.
        ro = (self.guest == "ro_root") or getattr(self, "force_readonly", False)
        return {"mount": ("/dev/vtbd0p3 on / (ufs, local, read-only)" if ro else
                          "/dev/vtbd0p3 on / (ufs, local, soft-updates)"),
                "df": "/dev/vtbd0p3 5900000 900000 4600000 16% /",
                "repairs": 0, "readonly": ro}

    def console(self, cmd, timeout=60.0):
        self._count("console")
        return self._answer(cmd)

    def guest_exec(self, cmd, timeout=60.0):
        self._count("guest_exec")
        return self._answer(cmd)

    def _answer(self, cmd):
        if self.phase != "hv" or self.guest == "down":
            return None
        if "progress" in cmd:
            gen = self.gen
            return (f"gen={gen} ts={int(self.clock.now())}|"
                    f"{self.load_errors}|"
                    f"{'RUNNING' if self.load_running else 'STOPPED'}|"
                    f"{'VERIFY-MISMATCH gen=%d' % gen if self.load_errors else ''}")
        if "kern.boottime" in cmd:
            return (f"{{ sec = {int(self.boot_epoch)} }}|"
                    f"/dev/vtbd0p3 5900000 900000 {self.avail_kb} 16% /|"
                    f"{getattr(self, 'dmesg_errors', 0)}")
        if cmd.startswith("fsck_ffs"):
            self._count("fsck")
            if getattr(self, "fs_dirty_forever", False):
                return "***** FILE SYSTEM STILL DIRTY *****"
            if self.fs_dirty_passes > 0:
                self.fs_dirty_passes -= 1
                return ("SALVAGE? yes\n***** FILE SYSTEM MARKED DIRTY *****\n"
                        "***** FILE SYSTEM STILL DIRTY *****")
            return "5 files, 10 used, 100 free\n***** FILE SYSTEM IS CLEAN *****"
        if cmd.startswith("mount -u"):
            if getattr(self, "fs_dirty_forever", False):
                return "/dev/vtbd0p3 on / (ufs, local, read-only)"
            self.guest = "multiuser"
            self.force_readonly = False
            return "/dev/vtbd0p3 on / (ufs, local, soft-updates)"
        if cmd.startswith("service netif"):
            return "Stopping netif.\nStarting netif.\n1"
        if cmd.startswith("service sshd"):
            return "Starting sshd.\nSSHD-UP"
        if "load.sh" in cmd:
            self.load_running = True
            if not getattr(self, "unstallable", False):
                self.gen_frozen = False
            return "STARTED-4242"
        return ""

    def guest_start_load(self, size_mb=48, idle_s=5, mixed=None):
        self._count("guest_start_load")
        out = self._answer("cat > load.sh")
        return bool(out and "STARTED-" in out)

    def guest_load_progress(self):
        self._count("guest_load_progress")
        return parse_load_probe(self._answer("progress"))

    def guest_sysinfo(self):
        self._count("guest_sysinfo")
        return parse_sysinfo(self._answer("kern.boottime"))

    def guest_recover(self, fsck_passes=2):
        self._count("guest_recover")
        self.recoveries += 1
        rec = {"fsck": [], "repaired": False, "still_dirty": False}
        for p in range(1, max(1, fsck_passes) + 1):
            out = self._answer("fsck_ffs") or ""
            rec["fsck"].append({"pass": p, "tail": out})
            rec["still_dirty"] = any(m in out.upper() for m in FSCK_DIRTY_MARKERS)
            if any(m in out.upper() for m in FSCK_REPAIRED_MARKERS):
                rec["repaired"] = True
            if not rec["still_dirty"]:
                break
        rec["mount"] = self._answer("mount -u") or ""
        rec["net"] = self._answer("service netif") or ""
        rec["sshd"] = self._answer("service sshd") or ""
        rec["ok"] = evaluate_recovery(rec)
        return rec

    def break_glass(self):
        self._count("break_glass")
        self.break_glasses += 1
        if self.never_returns:
            self.phase = "gone"
            self.emac = False
            self.guest = "down"
            return True
        if self.breakglass_ineffective:
            # The bytes are accepted (return True: "sent") but the watchdog
            # never actually fires -- the board stays exactly as it was. This
            # is the "sent is not done" case send_break_glass() exists to
            # catch: nothing about board state changes here, on purpose.
            return True
        self.phase = "uboot"
        self.emac = False
        self.guest = "down"
        return True

    def reload(self, log_path, timeout_s=1800, boot_to_shell=True,
               load_cycles=3, expect_vbk=False):
        self._count("reload")
        self.reloads += 1
        self.clock.sleep(120)             # a real reload is minutes, not free
        if self.never_returns or self.phase == "gone":
            return False, 120.0, "(killed: board never returned)", True
        if self.reload_always_fails:
            # The board is still PRESENT (unlike never_returns/"gone") but the
            # reload itself never lands -- a bad on-disk image or a dead TFTP
            # server, not a vanished board. Exercises do_reload()'s OTHER
            # failure ending (the generic "reload failed twice" raise)
            # instead of the board-off-usb one.
            return (False, 120.0,
                    "(dry-run) simulated reload failure: TFTP timeout, board "
                    "stayed in U-Boot", False)
        self.phase = "hv"
        self.emac = True
        self.boot_epoch = self.clock.now()
        # A reload of a running guest is an UNCLEAN stop: root comes up
        # read-only with a dirty filesystem. That is the documented normal
        # outcome, and the reason the recovery sequence exists.
        self.guest = "ro_root"
        if not getattr(self, "fs_dirty_forever", False):
            # ...and the filesystem needs a pass of fsck, which reports STILL
            # DIRTY once and comes back clean on the rerun. That is what the
            # board actually does, so the fake does it too rather than
            # simulating an implausibly clean recovery.
            self.fs_dirty_passes = 1
        self.load_running = False
        self.gen_frozen = False
        try:
            with open(log_path, "w") as f:
                f.write("(dry-run) simulated reliable_load: LOADED & VERIFIED\n")
        except Exception:
            pass
        return True, 120.0, "LOADED & VERIFIED on cycle #1", False


def make_board(dry_run, scenario, clock, log, iface="br0"):
    """The ONE place a board object is constructed. A dry run can never end up
    with a real Board, whatever else goes wrong — and it also latches the
    DRY_RUN flag that keeps simulated outcomes out of the real ledger."""
    global DRY_RUN
    if dry_run:
        DRY_RUN = True
        return FakeBoard(clock, scenario=scenario, log=log)
    return Board(iface=iface, log=log)


FACADE_METHODS = ("usb", "emac_alive", "status", "counters", "isolation",
                  "guest_alive", "guest_fs_state", "console", "guest_exec",
                  "guest_start_load", "guest_load_progress", "guest_sysinfo",
                  "guest_recover", "break_glass", "reload")


def api_parity_problems():
    """Does FakeBoard still implement exactly the facade Board does?

    A dry run only means something if the fake and the real board answer the
    same calls. Checked on the CLASSES (never instantiated), so this costs
    nothing and cannot touch hardware."""
    problems = []
    for name in FACADE_METHODS:
        for cls in (Board, FakeBoard):
            if not callable(getattr(cls, name, None)):
                problems.append(f"{cls.__name__} is missing {name}()")
    extra_real = [n for n in vars(Board)
                  if not n.startswith("_") and n not in FACADE_METHODS
                  and not isinstance(vars(Board)[n], (str, int, float))]
    if extra_real:
        problems.append(f"Board has public methods outside the facade: "
                        f"{extra_real} (add them to FACADE_METHODS and to "
                        f"FakeBoard, or make them private)")
    return problems


def ledger_fingerprint():
    """(size, mtime) of the real boot ledger, or None. Used by the dry-run
    matrices to PROVE a simulated run left it untouched — see ledger_record()'s
    guard for why that assertion is not paranoia."""
    p = os.path.join(HERE, "boot-ledger.jsonl")
    try:
        st = os.stat(p)
        return (st.st_size, st.st_mtime)
    except FileNotFoundError:
        return None


def banner(lines, char="━"):
    w = max(len(x) for x in lines) + 2
    out = [char * w] + [" " + x for x in lines] + [char * w]
    return "\n".join(out)


DRY_RUN = False      # set by make_board(); see ledger_record()


def ledger_record(ok, source, note="", isol_pass=None):
    """Feed the ONE cumulative ledger (boot_ledger.py). Never raises: a ledger
    write must not be able to break a 72-hour run.

    HARD GUARD: a dry run must never write to boot-ledger.jsonl. This is not
    theoretical — the first dry-run of soak72.py in development appended eight
    fabricated "clean boot" entries from a simulated board and inflated the
    already-closed 100-boot streak from 100 to 108 before they were removed.
    Relying on every caller to patch this function out was the bug; the flag is
    checked here, at the one place that writes."""
    if DRY_RUN:
        return None
    try:
        import boot_ledger
        return boot_ledger.record(ok, isol_pass=isol_pass, source=source,
                                  note=note)
    except Exception:
        return None


@contextlib.contextmanager
def patched(obj, name, value):
    """Swap one attribute for the duration of a block (dry-run tests only)."""
    orig = getattr(obj, name)
    setattr(obj, name, value)
    try:
        yield
    finally:
        setattr(obj, name, orig)
