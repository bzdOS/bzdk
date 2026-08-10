#!/usr/bin/env bash
# dual-rearm-qemu-ci.sh — board-free regression guard for the dual-guest RETRY
# path: a `zboot` issued AFTER a `zunhalt` re-arm must produce a guest that
# actually RUNS, not merely one that reaches kload_enter.
#
# WHY THIS SCRIPT EXISTS. Real hardware (2026-08-10) showed a retry reaching
# kload_enter and then doing nothing at all: zguest_cpu3 breadcrumb state 3,
# attempts 2, zload2 clean (elf_valid 1, bytes_copied 24, fail_reason 0),
# isolation self-check passed, placed bytes verified byte-for-byte against
# objdump -- and the payload's counter never moved. The same firmware boots a
# real Zephyr image fine on a FIRST attempt. Every pre-existing dual-guest QEMU
# target only ever exercises a first attempt, so not one of them could have
# caught a second-attempt defect. This one does nothing but exercise a second
# attempt.
#
# WHAT A PASS PROVES:
#   - attempt #1 fails the way it does on the board (0xBAD1), because the
#     destination is genuinely empty -- this target deliberately skips the
#     boot-time copy-in so the failure is caused, not simulated;
#   - `zunhalt` returns CPU3 to an OBSERVABLE parked state with attempts
#     unchanged, rearms incremented and last_fail sticky (all asserted by the
#     firmware itself, see main_dual_rearm_qemu.c);
#   - the copy-in then stages a real ELF from the landing window;
#   - and the retry's guest genuinely RUNS -- established the same way every
#     other dual-guest target establishes it, by sampling BOTH cores' own
#     counters twice across a real interval and requiring both to climb.
#     state 3 means "about to enter", never "running", which is exactly the
#     distinction the board failure turned on.
#
# WHAT IT DOES NOT PROVE: anything about the real board. QEMU does not model
# caches, and the board's own failure involved staging over the debug channel
# straight to the destination -- a route that zstage_invalidate_dest() has since
# made unsupported. See docs/dual-guest.md.
#
# Usage:   ./dual-rearm-qemu-ci.sh
# Exit:    0 = PASS, 1 = build/run/assert failure.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
CROSS=${CROSS:-aarch64-linux-gnu-}
ELF=microkernel-dual-rearm-qemu.elf
PAYLOAD_SRC=zg3_trivial_payload.S
PAYLOAD_LD=zg3_trivial_payload.ld
PAYLOAD_ELF=zg3_trivial_payload.elf
# ZSTAGE_LOW_PA (zstage.h): the landing window. The payload MUST go here and not
# at the destination -- the whole point is that only the copy-in, run after the
# re-arm, can put it where zload2 reads.
LOW_ADDR=0x4E000000
TIMEOUT=60

command -v "$QEMU" >/dev/null 2>&1 || { echo "dual-rearm-qemu-ci: FAIL — $QEMU not installed"; exit 1; }
command -v "${CROSS}gcc" >/dev/null 2>&1 || { echo "dual-rearm-qemu-ci: FAIL — ${CROSS}gcc not installed"; exit 1; }

echo "dual-rearm-qemu-ci: building $ELF ..."
if ! make -s dual-rearm-qemu >/dev/null 2>&1; then
    echo "dual-rearm-qemu-ci: FAIL — 'make dual-rearm-qemu' did not build"
    make dual-rearm-qemu 2>&1 | tail -40
    exit 1
fi
[ -f "$ELF" ] || { echo "dual-rearm-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

echo "dual-rearm-qemu-ci: building $PAYLOAD_ELF ..."
if ! "${CROSS}gcc" -nostdlib -static -no-pie -Wl,--build-id=none \
        -Wl,-T,"$PAYLOAD_LD" -o "$PAYLOAD_ELF" "$PAYLOAD_SRC" 2>&1; then
    echo "dual-rearm-qemu-ci: FAIL — building $PAYLOAD_ELF failed"
    exit 1
fi
if ! "${CROSS}readelf" -h "$PAYLOAD_ELF" 2>/dev/null | grep -q "0xbe000000"; then
    echo "dual-rearm-qemu-ci: FAIL — $PAYLOAD_ELF entry point is not 0xBE000000"
    exit 1
fi

echo "dual-rearm-qemu-ci: running under $QEMU -M virt -smp 4 -m 2048, payload at $LOW_ADDR (timeout ${TIMEOUT}s) ..."
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 2048 -smp 4 -nographic \
        -kernel "$ELF" \
        -device "loader,file=$PAYLOAD_ELF,addr=$LOW_ADDR,force-raw=on" 2>&1)

if echo "$OUT" | grep -qiE "DUAL-REARM-CI: FAIL|DUAL-QEMU-CI2: FAIL|DUAL-QEMU-CI2: FAULT|UNEXPECTED TRAP|panic|Unhandled|abort"; then
    echo "dual-rearm-qemu-ci: FAIL — error line in output:"
    echo "$OUT" | grep -iE "DUAL-REARM-CI: FAIL|DUAL-QEMU-CI2: FAIL|DUAL-QEMU-CI2: FAULT|UNEXPECTED TRAP|panic|Unhandled|abort" | head
    echo "dual-rearm-qemu-ci: full output:"
    echo "$OUT"
    exit 1
fi

# The re-arm bookkeeping is asserted by the firmware, but assert the sequence
# HERE too: a silently-reordered or skipped step would otherwise still let the
# final counter check pass, and the sequence IS the thing under test.
for want in "after zboot #1: state=0x0000bad1 attempts=1" \
            "after zunhalt: state=0x00000001 attempts=1 last_fail=0x0000bad1 rearms=1" \
            "after zboot #2: state=0x00000003 attempts=2 last_fail=0x0000bad1 rearms=1"; do
    if ! echo "$OUT" | grep -qF "$want"; then
        echo "dual-rearm-qemu-ci: FAIL — expected step line not seen: '$want'"
        echo "dual-rearm-qemu-ci: full output:"
        echo "$OUT"
        exit 1
    fi
done

VERDICT=$(echo "$OUT" | grep "DUAL-QEMU-CI2: cpu0 before=" | tail -1)
if [ -z "$VERDICT" ]; then
    echo "dual-rearm-qemu-ci: FAIL — never saw the two-sample verdict line within ${TIMEOUT}s. Full output:"
    echo "$OUT"
    exit 1
fi

CPU0_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu0 before=\([0-9]*\).*/\1/p')
CPU0_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu0 before=[0-9]* after=\([0-9]*\).*/\1/p')
CPU3_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu3 before=\([0-9]*\).*/\1/p')
CPU3_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu3 before=[0-9]* after=\([0-9]*\).*/\1/p')

if [ -z "$CPU0_BEFORE" ] || [ -z "$CPU0_AFTER" ] || [ -z "$CPU3_BEFORE" ] || [ -z "$CPU3_AFTER" ]; then
    echo "dual-rearm-qemu-ci: FAIL — could not parse before/after out of: $VERDICT"
    exit 1
fi
if [ "$CPU0_AFTER" -le "$CPU0_BEFORE" ]; then
    echo "dual-rearm-qemu-ci: FAIL — CPU0's counter did not strictly increase ($CPU0_BEFORE -> $CPU0_AFTER)"
    exit 1
fi
if [ "$CPU3_AFTER" -le "$CPU3_BEFORE" ]; then
    echo "dual-rearm-qemu-ci: FAIL — the RE-ARMED CPU3 guest did not run: counter $CPU3_BEFORE -> $CPU3_AFTER. This is the board symptom (kload_enter reached, nothing executes)."
    echo "dual-rearm-qemu-ci: full output:"
    echo "$OUT"
    exit 1
fi
if ! echo "$OUT" | grep -q "DUAL-QEMU-CI2: PASS"; then
    echo "dual-rearm-qemu-ci: FAIL — arithmetic checked out but never saw PASS. Full output:"
    echo "$OUT"
    exit 1
fi

echo "dual-rearm-qemu-ci: PASS — retry after zunhalt produced a RUNNING guest (cpu3 $CPU3_BEFORE -> $CPU3_AFTER, cpu0 $CPU0_BEFORE -> $CPU0_AFTER)"
echo "$OUT" | grep -E "HV: (parked|after|step|copy-in)|DUAL-QEMU-CI2:"
exit 0
