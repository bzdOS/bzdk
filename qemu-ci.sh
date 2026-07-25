#!/usr/bin/env bash
# qemu-ci.sh — board-free CI smoke test for the bzdOS hypervisor core
# (ROADMAP v1 gate: "вторая цель собирается и грузится (минимум QEMU-virt)").
#
# Builds the QEMU-virt target and runs it under qemu-system-aarch64 -M virt,
# then asserts the end-to-end "QEMU-CI: PASS" line the HV prints only after it
# has: installed its EL2 vectors, programmed + enabled stage-2 (AT S12E1R
# self-check), armed the GICv2 timer, dropped to an EL1 guest, and had that
# guest preempted by the timer tick. No physical board, fully scriptable —
# this is the regression gate that catches stage-2 / EL1-entry / GICv2 breakage
# on every commit without touching hardware.
#
# Usage:   ./qemu-ci.sh            # build + run + assert
# Exit:    0 = PASS, 1 = build/run/assert failure.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
ELF=microkernel-qemu.elf
TIMEOUT=30

command -v "$QEMU" >/dev/null 2>&1 || { echo "qemu-ci: FAIL — $QEMU not installed"; exit 1; }

echo "qemu-ci: building $ELF ..."
if ! make -s qemu >/dev/null 2>&1; then
    echo "qemu-ci: FAIL — 'make qemu' did not build"
    exit 1
fi
[ -f "$ELF" ] || { echo "qemu-ci: FAIL — $ELF missing after build"; exit 1; }

echo "qemu-ci: running under $QEMU -M virt (timeout ${TIMEOUT}s) ..."
# gic-version=2 + virtualization=on give the vCPU EL2 (the HV runs there);
# cortex-a53 matches the real board. The HV issues PSCI SYSTEM_OFF after the
# PASS line, so qemu exits on its own well inside the timeout on success.
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 1024 -nographic \
        -kernel "$ELF" 2>&1)

if echo "$OUT" | grep -qiE "FAIL|panic|Unhandled|abort"; then
    echo "qemu-ci: FAIL — error line in output:"
    echo "$OUT" | grep -iE "FAIL|panic|Unhandled|abort" | head
    exit 1
fi

if echo "$OUT" | grep -q "QEMU-CI: PASS"; then
    echo "qemu-ci: PASS"
    echo "$OUT" | grep -iE "self-check PASS|first tick|tick [0-9]+|QEMU-CI: PASS"
    exit 0
fi

echo "qemu-ci: FAIL — never saw 'QEMU-CI: PASS' within ${TIMEOUT}s. Tail:"
echo "$OUT" | tail -15
exit 1
