# qemu-patient-run.sh — shared helper, sourced by dual-qemu-ci.sh and
# dual-zephyr-qemu-ci.sh: run a QEMU-CI subprocess with a PROGRESS-based wait
# instead of a single flat `timeout N`.
#
# THE BUG THIS FIXES (see the two callers' own headers for the incident):
# both scripts decide PASS/FAIL by sampling a firmware-side progress counter
# at GICv2 tick 2 and again at a later tick, and the firmware only advances
# that tick count when its own vCPU thread actually gets host CPU time. A
# flat `timeout N qemu-system-aarch64 ...` cannot distinguish "genuinely
# wedged" from "correct, just slow because the host is busy with something
# else" -- it kills both alike. Measured 2026-08-27: the same commit, same
# script, PASSED on an idle run and FAILED on a run that raced unrelated
# video-processing load (~7 on an 8-core host); running the "failing" script
# alone (i.e. off the load) passed immediately. That is a flaky gate, not a
# real regression signal, and a flaky gate trains people to re-run until
# green -- which is how a real regression eventually gets waved through.
#
# WHY A PROGRESS-BASED WAIT AND NOT A BIGGER CONSTANT: raising TIMEOUT is a
# guess at "how slow is the host allowed to get", re-guessed forever as
# hosts and hosting change, and it still eventually flakes at whatever load
# exceeds the new guess -- it does not fix the underlying inability to tell
# "slow" from "stuck". The firmware already prints an "HV: tick N" heartbeat
# line every 4 GICv2 ticks (~400 ms of GUEST virtual time) UNCONDITIONALLY --
# it does not depend on either test payload doing anything right, only on
# CPU0's vCPU thread being scheduled by the host often enough to service its
# own timer interrupt. So: as long as new bytes keep landing in the output,
# the run is making progress and is left alone, no matter how slow the host
# is being about it. Only a stretch of REAL wall-clock time with *zero* new
# output -- IDLE_TIMEOUT -- is treated as "this looks hung", because that is
# a host-load-independent signal: a genuinely wedged HV (dead loop, missed
# IRQ, un-serviced fault, the exact class of bug these scripts exist to
# catch) stops producing the heartbeat entirely, whereas a merely slow host
# just spaces the same heartbeats further apart in real time without ever
# silencing them for long. A second, much larger ABS_TIMEOUT still bounds
# the total wall-clock budget, so a firmware bug that keeps printing forever
# without ever reaching its sample target (logic that can never satisfy its
# own exit condition) cannot hang the gate indefinitely either.
#
# Both callers still independently re-derive the before/after arithmetic
# from the captured text (their own "belt and suspenders" -- see their
# headers) and still require every assertion they required before. This
# helper only changes WHEN the subprocess is killed, never what is checked
# once it stops.
#
# Usage:   qemu_patient_run OUTVAR IDLE_TIMEOUT ABS_TIMEOUT -- <argv...>
# Sets $OUTVAR (via the caller's own variable name) to the captured combined
# stdout+stderr of <argv...> and returns:
#   <qemu's own exit status> - it exited on its own before either timeout
#   124 - killed: no new output for IDLE_TIMEOUT seconds (looks hung)
#   125 - killed: exceeded ABS_TIMEOUT despite ongoing output (refuse to
#         wait forever even for a "slow but still producing output" run)
qemu_patient_run() {
    local __outvar=$1 idle_timeout=$2 abs_timeout=$3
    shift 3
    [ "${1:-}" = "--" ] && shift

    local outfile
    outfile=$(mktemp "${TMPDIR:-/tmp}/qemu-patient-run.XXXXXX") || return 1

    "$@" >"$outfile" 2>&1 &
    local pid=$!

    local start now size last_size last_growth rc=0
    start=$(date +%s)
    last_size=0
    last_growth=$start

    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        now=$(date +%s)
        size=$(stat -c%s "$outfile" 2>/dev/null || echo 0)
        if [ "$size" -gt "$last_size" ]; then
            last_size=$size
            last_growth=$now
        fi
        if [ $(( now - last_growth )) -ge "$idle_timeout" ]; then
            kill -9 "$pid" 2>/dev/null
            echo "qemu-patient-run: no new output for ${idle_timeout}s -- treating as hung, not just slow" >>"$outfile"
            rc=124
            break
        fi
        if [ $(( now - start )) -ge "$abs_timeout" ]; then
            kill -9 "$pid" 2>/dev/null
            echo "qemu-patient-run: exceeded the absolute ${abs_timeout}s ceiling despite ongoing output -- refusing to wait forever" >>"$outfile"
            rc=125
            break
        fi
    done
    if [ "$rc" -eq 0 ]; then
        wait "$pid"
        rc=$?
    else
        wait "$pid" 2>/dev/null
    fi

    printf -v "$__outvar" '%s' "$(cat "$outfile")"
    rm -f "$outfile"
    return "$rc"
}
