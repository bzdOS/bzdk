#!/usr/bin/env bash
# run_holdtest.sh — build + run the throwaway "pause-before-entry TDE+BRK
# ordering" QEMU diagnostic (main_holdtest_qemu.c et al, `make holdtest`).
#
# NOT a CI gate (unlike qemu-ci.sh) — this is a one-off investigation aid
# for the 2026-07-26/27 board-hang session (see main_holdtest_qemu.c's file
# banner and memories dbgtools-infra-added / hold-gate-plus-breakpoint-
# crashes-board). Prints two "HOLDTEST: TEST A/B -> ..." verdict lines and
# cleanly PSCI-poweroffs qemu on success; hangs (caught by `timeout`) on a
# genuine hard failure of the mechanism under test.
#
# Usage: ./run_holdtest.sh
set -u
cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
ELF=microkernel-holdtest.elf
TIMEOUT=15

command -v "$QEMU" >/dev/null 2>&1 || { echo "run_holdtest: FAIL — $QEMU not installed"; exit 1; }

echo "run_holdtest: building $ELF ..."
make -s holdtest || { echo "run_holdtest: FAIL — 'make holdtest' did not build"; exit 1; }

echo "run_holdtest: running under $QEMU -M virt (timeout ${TIMEOUT}s) ..."
timeout "$TIMEOUT" "$QEMU" \
    -machine virt,gic-version=2,virtualization=on \
    -cpu cortex-a53 -m 256 -nographic \
    -kernel "$ELF"
RC=$?

if [ "$RC" -eq 124 ]; then
    echo "run_holdtest: TIMED OUT after ${TIMEOUT}s — the guest never reached RUN COMPLETE (a real hang)."
    exit 1
fi

echo "run_holdtest: qemu exited (rc=$RC) — see the HOLDTEST: TEST A/TEST B verdict lines above."
