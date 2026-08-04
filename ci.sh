#!/usr/bin/env bash
# ci.sh — the complete board-free regression gate for bzdOS
# (ROADMAP v1 gate: "хостовые юнит-тесты в CI" + "вторая цель грузится").
#
# Runs everything that can be checked WITHOUT the physical Banana Pi M64:
#   1. make test    — the hosted unit suites (virtqueue ring parser, stage-2
#                     table builder, vnet ring, kload modinfo, vconsole UART),
#                     pure host-gcc, catch logic regressions with no board.
#   2. ./qemu-ci.sh — the QEMU-virt second target boots end to end (EL2 vectors
#                     + stage-2 + GICv2 timer + EL1 guest + timer preemption).
#   3. ./zephyr-qemu-ci.sh
#                   — the ZEPHYR GUEST target boots end to end: a real Zephyr
#                     RTOS image parsed/placed by kload.c, entered at EL1 under
#                     stage-2, console served by the real vconsole.c 16550
#                     trap-emulator. SKIPs (does not fail) if no Zephyr tree is
#                     available to build a guest image from — see that script's
#                     header for why that is a missing input, not a regression.
#
# One command, one exit code — the gate to run on every commit. Hardware-only
# checks (100-boot streak, soak, break-glass) live in the cumulative boot
# ledger (boot_ledger.py) and soak.py, fed by real board work.
#
# Usage:  ./ci.sh
# Exit:   0 = all board-free checks pass, 1 = first failure.
set -u
cd "$(dirname "$(readlink -f "$0")")"

fail=0

echo "════════ ci: hosted unit tests (make test) ════════"
if make test; then
    echo "ci: hosted unit tests PASS"
else
    echo "ci: hosted unit tests FAIL"
    fail=1
fi

echo "════════ ci: QEMU-virt second target (qemu-ci.sh) ════════"
if ./qemu-ci.sh; then
    echo "ci: qemu second target PASS"
else
    echo "ci: qemu second target FAIL"
    fail=1
fi

echo "════════ ci: Zephyr guest on QEMU-virt (zephyr-qemu-ci.sh) ════════"
# Distinguish SKIP from PASS: the script exits 0 for both (a missing 565 MB
# Zephyr tree must not be able to turn this gate red), so read its verdict
# from the output rather than only from $?. A FAIL is still a FAIL.
zout=$(./zephyr-qemu-ci.sh 2>&1)
zrc=$?
echo "$zout"
if [ "$zrc" -ne 0 ]; then
    echo "ci: zephyr guest target FAIL"
    fail=1
elif echo "$zout" | grep -q "zephyr-qemu-ci: SKIP"; then
    echo "ci: zephyr guest target SKIPPED (no Zephyr tree — not a regression)"
else
    echo "ci: zephyr guest target PASS"
fi

echo "════════════════════════════════════════════════════"
if [ "$fail" -eq 0 ]; then
    echo "ci: ALL BOARD-FREE CHECKS PASS ✅"
else
    echo "ci: FAILURES ABOVE ❌"
fi
exit "$fail"
