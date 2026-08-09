#!/usr/bin/env bash
# smp-qemu-ci.sh — Step-0 board-free proof for PSCI CPU_ON multi-core bring-up
# (dual-guest effort foundation — see main_dual_qemu.c's banner for the full
# argument).
#
# This is deliberately NOT named dual-qemu-ci.sh: that name is reserved for a
# LATER script the parallel dual-guest effort will add once it actually boots
# a second guest on top of this foundation. This script proves ONLY the
# foundation itself: that qemu-system-aarch64's PSCI CPU_ON emulation brings
# up 3 secondary vCPUs and lands each one in the REAL, UNMODIFIED
# smp_secondary_main() (smp.c) via the REAL, UNMODIFIED start.S bring-up logic
# (copied into its own translation unit only to avoid a duplicate `_start`
# symbol with start_qemu.S — see start_secondary_qemu.S's header for why zero
# logic changed).
#
# WHAT A PASS HERE PROVES:
#   - qemu-system-aarch64 -machine virt,gic-version=2,virtualization=on with
#     -smp 4 gives 4 vCPUs and a working PSCI CPU_ON conduit from EL2 (no real
#     EL3/secure firmware needed — same SMC-to-QEMU-internal-PSCI mechanism
#     every other target in this tree already relies on for SYSTEM_OFF).
#   - smp_init() (smp.c, byte-for-byte unmodified) successfully PSCI CPU_ON's
#     affinities 1..3 pointed at _start_secondary.
#   - Each secondary's _start_secondary (start.S's own routine, copied
#     unmodified into start_secondary_qemu.S — see that file's header for
#     why no logic needed to change under QEMU's MMU-off boot contract)
#     reaches stage 0x04 ("about to enter C") and calls the REAL, unmodified
#     smp_secondary_main(cpuid), which atomically sets its online bit and
#     SEVs CPU0's wait.
#   - smp_num_online() (the existing accessor in smp.h) reads back 4, and the
#     SMP1 breadcrumb's online bitmap reads back 0xf.
#
# WHAT A PASS HERE DOES *NOT* PROVE:
#   - Nothing about a second GUEST running on any of these cores — no guest
#     is ever entered by this target. That is the LATER dual-guest step's own
#     proof, built on top of this one.
#   - Nothing about the CPU1 debug-core loop, CPU2's eMMC async-I/O offload,
#     or any of the real board's per-core dispatch in smp_secondary_main() —
#     main_dual_qemu.c deliberately sets dbg_core_enable=0 so CPU1 parks in
#     WFI like CPU2/CPU3 do by default, specifically so this proof is about
#     PSCI CPU_ON reachability alone, not about board-only MMIO surviving
#     under QEMU (which it can't, and isn't the point here).
#
# No SKIP path: everything needed is in this repo. Either PASS or FAIL.
#
# Usage:   ./smp-qemu-ci.sh
# Exit:    0 = PASS, 1 = build/run/assert failure.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
ELF=microkernel-dual-qemu.elf
# The harness self-powers-off (PSCI SYSTEM_OFF) within ~100ms of a healthy
# run (no timer, no guest, nothing to wait on) — this timeout is purely the
# backstop for "QEMU never even started / a secondary wedged before reaching
# smp_secondary_main() and CPU0's bounded ~100ms wait somehow never returned".
TIMEOUT=30

command -v "$QEMU" >/dev/null 2>&1 || { echo "smp-qemu-ci: FAIL — $QEMU not installed"; exit 1; }

echo "smp-qemu-ci: building $ELF ..."
if ! make -s dual-qemu >/dev/null 2>&1; then
    echo "smp-qemu-ci: FAIL — 'make dual-qemu' did not build"
    make dual-qemu 2>&1 | tail -30
    exit 1
fi
[ -f "$ELF" ] || { echo "smp-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

echo "smp-qemu-ci: running under $QEMU -M virt -smp 4 (timeout ${TIMEOUT}s) ..."
# -smp 4 is the one flag this target adds over every sibling QEMU-CI script:
# 4 vCPUs so PSCI CPU_ON has 3 real secondaries to target. gic-version=2 +
# virtualization=on + cortex-a53 match every other script in this tree
# exactly (see qemu-ci.sh/vgic-qemu-ci.sh).
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 1024 -smp 4 -nographic \
        -kernel "$ELF" 2>&1)

if echo "$OUT" | grep -qiE "DUAL-QEMU-CI: FAIL|UNEXPECTED TRAP|panic|Unhandled|abort"; then
    echo "smp-qemu-ci: FAIL — error line in output:"
    echo "$OUT" | grep -iE "DUAL-QEMU-CI: FAIL|UNEXPECTED TRAP|panic|Unhandled|abort" | head
    echo "smp-qemu-ci: full output:"
    echo "$OUT"
    exit 1
fi

if echo "$OUT" | grep -q "DUAL-QEMU-CI: PASS" && \
   echo "$OUT" | grep -q "online-bitmap=0x0000000f" && \
   ! echo "$OUT" | grep -q "online=NO"; then
    echo "smp-qemu-ci: PASS"
    echo "$OUT" | grep -E "smp_num_online|PSCI CPU_ON rc|stage markers|cpu[0-9] online|DUAL-QEMU-CI:"
    exit 0
fi

echo "smp-qemu-ci: FAIL — never saw a clean 'DUAL-QEMU-CI: PASS' (all 4 cores online) within ${TIMEOUT}s. Full output:"
echo "$OUT"
exit 1
