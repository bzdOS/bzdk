#!/usr/bin/env bash
# snapshot-qemu-ci.sh — board-free CI gate for guest checkpoint/restore
# (ROADMAP D1: "freeze the guest, dump RAM, restore it").
#
# Builds the `snapshot-qemu` target (main_snapshot_qemu.c /
# el2_exc_snapshot_qemu.c) and runs it under qemu-system-aarch64 -M virt,
# asserting the end-to-end "QEMU-SNAPSHOT-CI: PASS" line the harness prints
# only after it has: called the REAL, unmodified snapshot_save() against a
# live EL1 guest, called the REAL, unmodified snapshot_restore(), and
# observed the guest's own loop-counter breadcrumb actually REWIND to its
# snapshot-time value and then resume climbing — i.e. a genuine save+restore
# round trip, not just "no fault".
#
# WHY THIS SCRIPT EXISTS NOW (it did not before 2026-08): the naive
# whole-1-GiB dram_copy() used to overwrite the harness's own running image
# (link_qemu.ld places it at 0x40080000, inside the exact range being swept)
# mid-restore, corrupting its own live call stack and producing a garbled
# fault (ESR=0x02000000 ELR=0 FAR=0) every single time — snapshot-qemu could
# never reach a PASS, so wiring it into the gate would have meant either a
# permanently-red gate or a SKIP that hid the very thing this exercise exists
# to catch. Fixed via snapshot.c's dram_copy_excluding()/snapshot_excl_windows()
# (see snapshot.h's "EXCLUSION WINDOWS") — verified 5/5 clean runs before this
# script was added, per this project's own "5/5 before wiring into ci.sh" bar.
#
# TIMING: unlike qemu-ci.sh's ~1 s smoke test, this exercise moves ~1 GiB
# through dram_copy_excluding() TWICE (save + restore) under unaccelerated
# TCG (no KVM for a foreign-arch guest on a typical x86_64 CI host) — measured
# at 90-120 s wall-clock per full run on the host this was verified on. The
# harness self-powers-off (PSCI SYSTEM_OFF) right after printing its verdict,
# so a healthy run exits well before the timeout; only a genuinely wedged/
# faulted run burns the whole budget.
#
# Usage:   ./snapshot-qemu-ci.sh      # build + run + assert
# Exit:    0 = PASS, 1 = build/run/assert failure.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
ELF=microkernel-snapshot-qemu.elf
TIMEOUT=240

command -v "$QEMU" >/dev/null 2>&1 || { echo "snapshot-qemu-ci: FAIL — $QEMU not installed"; exit 1; }

echo "snapshot-qemu-ci: building $ELF ..."
if ! make -s snapshot-qemu >/dev/null 2>&1; then
    echo "snapshot-qemu-ci: FAIL — 'make snapshot-qemu' did not build"
    exit 1
fi
[ -f "$ELF" ] || { echo "snapshot-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

echo "snapshot-qemu-ci: running under $QEMU -M virt (timeout ${TIMEOUT}s; a healthy"
echo "snapshot-qemu-ci: run self-powers-off in well under a minute or two) ..."
# gic-version=2 + virtualization=on give the vCPU EL2 (the HV runs there);
# cortex-a53 + -m 2048 match the real board's 2 GiB layout exactly (see
# snapshot.h's "Snapshot STORE region" — the high GiB store window only
# exists at all with -m 2048). The HV issues PSCI SYSTEM_OFF right after the
# verdict line, so qemu exits on its own well inside the timeout on success.
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 2048 -nographic \
        -kernel "$ELF" 2>&1)

if echo "$OUT" | grep -qiE "QEMU-SNAPSHOT-CI: FAIL|FAULT kind=|FAILED rc=|panic|Unhandled|abort"; then
    echo "snapshot-qemu-ci: FAIL — error line in output:"
    echo "$OUT" | grep -iE "QEMU-SNAPSHOT-CI: FAIL|FAULT kind=|FAILED rc=|panic|Unhandled|abort" | head
    echo "snapshot-qemu-ci: full output tail:"
    echo "$OUT" | tail -20
    exit 1
fi

if echo "$OUT" | grep -q "QEMU-SNAPSHOT-CI: PASS"; then
    echo "snapshot-qemu-ci: PASS"
    echo "$OUT" | grep -iE "self-check PASS|snapshot_save\(\)|snapshot_restore\(\)|final guest loop_count|QEMU-SNAPSHOT-CI: PASS"
    exit 0
fi

echo "snapshot-qemu-ci: FAIL — never saw 'QEMU-SNAPSHOT-CI: PASS' within ${TIMEOUT}s. Tail:"
echo "$OUT" | tail -20
exit 1
