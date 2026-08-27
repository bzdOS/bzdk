#!/usr/bin/env bash
# dual-qemu-ci.sh — dual-guest Step 1 board-free proof: CPU0 and CPU3 run two
# INDEPENDENT payloads GENUINELY CONCURRENTLY, under two different stage-2
# regimes, before ever attempting real Zephyr or real hardware.
#
# This name was deliberately left free by smp-qemu-ci.sh (dual-guest "Step 0",
# commit c78280d), which proved only that the REAL, unmodified smp.c/start.S
# PSCI CPU_ON bring-up works under QEMU with 4 cores online -- no guest was
# ever entered there. This script builds on that foundation (main_dual2_qemu.c
# reuses smp_init()/start_secondary_qemu.o exactly as Step 0 proved them) and
# on the real dual-guest mechanism itself (commit 7e5303c: smp.c's
# zephyr_cpu3_run() hook, zguest_cpu3.c/zload2.c/stage2_zephyr.c) to prove the
# actual claim: not "boots, then the other boots" sequentially, but both
# cores advancing their own independent counters during the SAME wall-clock
# interval.
#
# WHAT A PASS HERE PROVES:
#   - CPU0 runs guest.c's already-proven guest_demo_el1() (stage-2 disabled,
#     flat physical -- the exact payload main_dbg.c/qemu-ci.sh already
#     exercise elsewhere), preempted every 100 ms by the EL2 GICv2 tick.
#   - CPU3 runs the REAL zguest_cpu3.c code path -- zload2_parse_and_place()
#     genuinely parsing an ELF64/AArch64 header out of DRAM,
#     stage2_zephyr_init()/_enable() programming a SECOND, disjoint stage-2
#     table set on CPU3's own banked VTCR_EL2/VTTBR_EL2,
#     stage2_zephyr_isolation_selfcheck() proving in hardware (AT S12E1W)
#     that CPU3 cannot reach MMIO or FreeBSD's DRAM gigabyte, then
#     guest_config()/kload_enter() dropping to EL1 -- against a hand-built
#     trivial ELF (zg3_trivial_payload.S/.ld, built by THIS script, staged
#     via the same "-device loader,...,force-raw=on" technique
#     zephyr-qemu-ci.sh/snapshot-qemu-ci.sh already use for their own guest
#     images). This is the REAL code path end to end, not a QEMU-only bypass
#     of it -- see zg3_trivial_payload.S's header for why that was both
#     feasible and preferred over a synthetic CPU3 entry variant.
#   - el2_exc_dual2_qemu.c's tick handler (running only on CPU0, which is the
#     only core that ever arms a timer/unmasks IRQ on this target) samples
#     BOTH payloads' own progress counters at tick 2 (~200 ms in) and again
#     at tick 12 (~1.2 s in), entirely from the outside -- neither payload
#     cooperates with or even knows about the other -- and requires BOTH to
#     have strictly increased across that interval before printing PASS.
#     Two samples with a real gap, both climbing, is what distinguishes
#     genuine concurrency from "one after the other"; this script re-checks
#     that arithmetic itself (not just the firmware's own verdict string) by
#     parsing the printed before/after values.
#
#     CPU3's counter (the "cpu3 before=.../after=..." half of that line) is
#     vconsole.c's channel-1 "total_bytes" counter, bumped ONLY when
#     el2_exc_dual2_qemu.c's el2_trap() actually services a stage-2 data
#     abort CPU3 took on a UART0-THR write (zg3_trivial_payload.S writes
#     'h','b','\n' to it every loop iteration; stage2_zephyr.c maps zero
#     real MMIO for CPU3, so that write always faults). This is a
#     FAULT-OBSERVED signal, not a DRAM word read from another core -- see
#     docs/dual-guest.md, "Retrying a failed zboot ... and a probe that
#     lied": an earlier version of this counter (a plain DRAM word CPU1 read
#     directly) was reliable under QEMU but unsound on real hardware, where
#     the payload's Device-typed stage-1-off writes never invalidate another
#     core's cached copy. The DRAM word is still bumped by the payload and
#     still printed (informational only, see el2_exc_dual2_qemu.c's
#     "cpu3_dram=" field) but no longer decides PASS/FAIL.
#
# WHAT A PASS HERE DOES NOT PROVE:
#   - Nothing about real Zephyr booting under QEMU (a separate, later step —
#     zg3_trivial_payload.elf is a hand-written 4-instruction program, not
#     Zephyr) or about anything on the real board.
#   - Nothing about CPU1 (the EMAC/debug core, deliberately parked here via
#     dbg_core_enable=0 -- see main_dual2_qemu.c) or CPU2 (async eMMC
#     offload, deliberately idle) -- same scope carve-out Step 0 already
#     documented.
#
# No SKIP path: everything needed (the cross-compiler, the trivial payload
# source, qemu-system-aarch64) is in this repo / already required by ci.sh.
# Either PASS or FAIL.
#
# Wired into ci.sh as of 2026-08-11 (stage 7-10). Still runnable standalone,
# which is the faster loop while iterating on this target specifically.
#
# Usage:   ./dual-qemu-ci.sh
# Exit:    0 = PASS, 1 = build/run/assert failure.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
CROSS=${CROSS:-aarch64-linux-gnu-}
ELF=microkernel-dual2-qemu.elf
PAYLOAD_SRC=zg3_trivial_payload.S
PAYLOAD_LD=zg3_trivial_payload.ld
PAYLOAD_ELF=zg3_trivial_payload.elf
# ZG3_ELF_STAGE_PA from zguest_cpu3.c: ZSTAGE2_DRAM_BASE (0xBE000000) +
# ZSTAGE2_DRAM_SIZE/2 (16 MiB) == 0xBF000000 -- the upper half of Zephyr's own
# 32 MiB stage-2 slice, disjoint from the lower-half placement destination
# (ZG3_PA_BASE, 0xBE000000) so the raw-ELF staging buffer and the
# relocated/placed image never overlap during zload2's copy.
STAGE_ADDR=0xBF000000
# ZSTAGE_LOW_PA from zstage.h: the low-DRAM window U-Boot TFTPs a guest image
# into on the real board (0x4E000000, the first round boundary above the HDMI
# framebuffer, 32 MiB, verified unclaimed). Pass A stages the payload HERE
# instead of at STAGE_ADDR, so that zstage.c's copy-in is the only thing that can put it where
# zload2_parse_and_place() looks -- see zstage.h for why the image cannot
# simply be TFTP'd straight to STAGE_ADDR (U-Boot relocates itself to the top
# of DRAM, which is exactly that neighbourhood).
LOW_ADDR=0x4E000000
# -m 2048 is required: Zephyr's stage-2 slice sits in the high GiB
# (0x80000000-0xC0000000), which only physically exists with at least 2 GiB
# of QEMU RAM -- same requirement snapshot-qemu-ci.sh already established for
# the same high-GiB window (see that script's own comment).
#
# IDLE_TIMEOUT/ABS_TIMEOUT replace the old flat TIMEOUT=30: see
# qemu-patient-run.sh for why a fixed wall-clock budget flaked under host
# load and what replaces it. IDLE_TIMEOUT is generous against real scheduling
# delay (the run normally reaches its verdict in a couple of seconds, and a
# deliberately-provoked 40s scheduling gap in testing still passed cleanly --
# see the verification notes this shipped with); ABS_TIMEOUT is the backstop
# against a run that keeps producing output forever without ever reaching
# its sample target.
IDLE_TIMEOUT=60
ABS_TIMEOUT=300

command -v "$QEMU" >/dev/null 2>&1 || { echo "dual-qemu-ci: FAIL — $QEMU not installed"; exit 1; }
command -v "${CROSS}gcc" >/dev/null 2>&1 || { echo "dual-qemu-ci: FAIL — ${CROSS}gcc not installed"; exit 1; }
. "$(dirname "$(readlink -f "$0")")/qemu-patient-run.sh"

# ---- 1. the hypervisor firmware (CPU0 skeleton + real dual-guest mechanism) -
echo "dual-qemu-ci: building $ELF ..."
# GUEST_DRAM_2G=0 explicitly: board-config.xml turns that flag ON tree-wide (the
# guest needs the board's whole 2 GiB to build Mesa), and it is a COMPILE ERROR
# in stage2_zephyr.h for any dual build, because Zephyr's guest slice lives in
# the high GiB the flag hands to FreeBSD. The flag is a whole-tree build choice,
# so the target that needs the opposite choice says so at its own build site
# rather than relying on nobody having enabled it.
if ! make GUEST_DRAM_2G=0 -s dual2-qemu >/dev/null 2>&1; then
    echo "dual-qemu-ci: FAIL — 'make GUEST_DRAM_2G=0 dual2-qemu' did not build"
    make GUEST_DRAM_2G=0 dual2-qemu 2>&1 | tail -40
    exit 1
fi
[ -f "$ELF" ] || { echo "dual-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

# ---- 2. CPU3's trivial payload — a genuine, tiny ELF64/AArch64 binary, built
# standalone (NOT part of the Makefile — see zg3_trivial_payload.S/.ld's own
# headers for why) with the SAME cross-compiler the rest of this tree uses. --
echo "dual-qemu-ci: building $PAYLOAD_ELF (standalone, not part of the Makefile) ..."
if ! "${CROSS}gcc" -nostdlib -static -no-pie -Wl,--build-id=none \
        -Wl,-T,"$PAYLOAD_LD" -o "$PAYLOAD_ELF" "$PAYLOAD_SRC" 2>&1; then
    echo "dual-qemu-ci: FAIL — building $PAYLOAD_ELF failed"
    exit 1
fi
[ -f "$PAYLOAD_ELF" ] || { echo "dual-qemu-ci: FAIL — $PAYLOAD_ELF missing after build"; exit 1; }

# Sanity-check the payload is exactly what zload2_parse_and_place() expects:
# ET_EXEC, entry == ZG3_PA_BASE (0xBE000000) — a wrong-looking payload here
# would make a later PASS meaningless (see zephyr-qemu-ci.sh's own analogous
# link-address sanity check).
if ! "${CROSS}readelf" -h "$PAYLOAD_ELF" 2>/dev/null | grep -q "0xbe000000"; then
    echo "dual-qemu-ci: FAIL — $PAYLOAD_ELF entry point is not 0xBE000000 (ZG3_PA_BASE)"
    "${CROSS}readelf" -h "$PAYLOAD_ELF" 2>/dev/null
    exit 1
fi

# ---- 3. run it, TWICE, once per staging route -------------------------------
# Both passes use the SAME firmware binary. What differs is where the payload is
# placed -- and, because of that, what the correct outcome IS: pass A must
# succeed, pass B must be refused.
#
#   Pass A (LOW_ADDR)   -- the real bulk-loader chain: the image lands where
#                          U-Boot's third TFTP puts it, and zstage.c's
#                          copy-in is the ONLY thing that can move it to where
#                          zload2_parse_and_place() reads. If the copy-in is
#                          broken or mispositioned, CPU3's counter never moves.
#   Pass B (STAGE_ADDR) -- payload staged DIRECTLY at the destination, which
#                          was the original Step 1 route (commit fc20b63) and
#                          is deliberately NO LONGER a supported one. CPU3 must
#                          now FAIL, because a boot that stages no image at the
#                          landing window actively zeroes the destination
#                          header. That wipe exists because a warm reset
#                          preserves DRAM, so the previous boot's image would
#                          otherwise still be there and `zboot` would silently
#                          boot a ghost -- observed live on 2026-08-10, see
#                          zstage_invalidate_dest() in zstage.c. Asserting the
#                          FAILURE here is what keeps that wipe honest; a pass
#                          B that "worked" would mean the ghost is bootable
#                          again.
#
# Running A first is deliberate: it is the new, unproven path, and a failure
# there should not be masked by B passing.
# $3: "concurrent" = both cores must advance; "wiped" = CPU3 must NOT advance,
#     proving the stale-image wipe.
run_pass() {
    pass_name=$1
    load_addr=$2
    expect=$3

    echo "dual-qemu-ci: [$pass_name] running under $QEMU -M virt -smp 4 -m 2048, payload at $load_addr (idle timeout ${IDLE_TIMEOUT}s, absolute ceiling ${ABS_TIMEOUT}s) ..."
    qemu_patient_run OUT "$IDLE_TIMEOUT" "$ABS_TIMEOUT" -- \
            "$QEMU" \
            -machine virt,gic-version=2,virtualization=on \
            -cpu cortex-a53 -m 2048 -smp 4 -nographic \
            -kernel "$ELF" \
            -device "loader,file=$PAYLOAD_ELF,addr=$load_addr,force-raw=on"

    # A fault/panic is never acceptable, in either pass. Firmware wording note:
    # el2_exc_dual2_qemu.c no longer prints "DUAL-QEMU-CI2: FAIL" for a
    # cpu0/cpu3-did-not-advance outcome (it can't tell "concurrent" from
    # "wiped" -- see that file's own comment on the branch); it prints
    # "DUAL-QEMU-CI2: OUTCOME cpu0_advanced=.. cpu3_advanced=.." instead, and
    # this script decides PASS/FAIL below from the parsed before=/after=
    # numbers, per "expect". The ONLY things that can still print literal
    # "DUAL-QEMU-CI2: FAIL" are genuine, expectation-independent failures --
    # PSCI CPU_ON bring-up not completing (main_dual2_qemu.c) -- which is why
    # this check is unconditional (not gated on "expect").
    if echo "$OUT" | grep -qiE "DUAL-QEMU-CI2: FAULT|DUAL-QEMU-CI2: FAIL|UNEXPECTED TRAP|panic|Unhandled|abort"; then
        echo "dual-qemu-ci: FAIL [$pass_name] — fault/panic/bring-up error in output:"
        echo "$OUT" | grep -iE "DUAL-QEMU-CI2: FAULT|DUAL-QEMU-CI2: FAIL|UNEXPECTED TRAP|panic|Unhandled|abort" | head
        echo "dual-qemu-ci: full output:"
        echo "$OUT"
        return 1
    fi

    VERDICT=$(echo "$OUT" | grep "DUAL-QEMU-CI2: cpu0 before=" | tail -1)
    if [ -z "$VERDICT" ]; then
        echo "dual-qemu-ci: FAIL [$pass_name] — never saw the 'DUAL-QEMU-CI2: cpu0 before=...' sample line (qemu exited early, or was killed after ${IDLE_TIMEOUT}s with no new output / ${ABS_TIMEOUT}s total). Full output:"
        echo "$OUT"
        return 1
    fi

    # Re-derive the before/after numbers from the printed line and check the
    # arithmetic OURSELVES — belt and suspenders on top of the firmware's own
    # PASS string, same discipline smp-qemu-ci.sh applies to Step 0's verdict.
    CPU0_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu0 before=\([0-9]*\).*/\1/p')
    CPU0_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu0 before=[0-9]* after=\([0-9]*\).*/\1/p')
    CPU3_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu3 before=\([0-9]*\).*/\1/p')
    CPU3_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu3 before=[0-9]* after=\([0-9]*\).*/\1/p')

    if [ -z "$CPU0_BEFORE" ] || [ -z "$CPU0_AFTER" ] || [ -z "$CPU3_BEFORE" ] || [ -z "$CPU3_AFTER" ]; then
        echo "dual-qemu-ci: FAIL [$pass_name] — could not parse before/after values out of: $VERDICT"
        return 1
    fi

    if [ "$CPU0_AFTER" -le "$CPU0_BEFORE" ]; then
        echo "dual-qemu-ci: FAIL [$pass_name] — CPU0's guest_demo_el1 counter did not strictly increase ($CPU0_BEFORE -> $CPU0_AFTER)"
        return 1
    fi
    if [ "$expect" = "wiped" ]; then
        # CPU3 must have gone nowhere: the ghost image was zeroed, so
        # zload2_parse_and_place() rejected it and CPU3 never entered a guest.
        # CPU0 must still be perfectly healthy -- the wipe must not cost the
        # first guest anything.
        if [ "$CPU3_AFTER" -ne 0 ] || [ "$CPU3_BEFORE" -ne 0 ]; then
            echo "dual-qemu-ci: FAIL [$pass_name] — CPU3 advanced ($CPU3_BEFORE -> $CPU3_AFTER) from an image staged only at the destination. The stale-image wipe did NOT happen, so a warm reset can boot a ghost image (see zstage_invalidate_dest())."
            echo "dual-qemu-ci: full output:"
            echo "$OUT"
            return 1
        fi
        echo "dual-qemu-ci: [$pass_name] PASS — CPU3 correctly refused the destination-only image (0 -> 0) while CPU0 stayed healthy ($CPU0_BEFORE -> $CPU0_AFTER)"
        echo "$OUT" | grep -E "smp_num_online|zguest_stage_copyin|baseline sample|final sample|DUAL-QEMU-CI2:"
        return 0
    fi

    if [ "$CPU3_AFTER" -le "$CPU3_BEFORE" ]; then
        echo "dual-qemu-ci: FAIL [$pass_name] — CPU3's zguest_cpu3/zload2 trivial-payload counter did not strictly increase ($CPU3_BEFORE -> $CPU3_AFTER)"
        echo "dual-qemu-ci: full output:"
        echo "$OUT"
        return 1
    fi

    if ! echo "$OUT" | grep -q "DUAL-QEMU-CI2: PASS"; then
        echo "dual-qemu-ci: FAIL [$pass_name] — arithmetic checked out but never saw 'DUAL-QEMU-CI2: PASS'. Full output:"
        echo "$OUT"
        return 1
    fi

    echo "dual-qemu-ci: [$pass_name] PASS — cpu0 $CPU0_BEFORE -> $CPU0_AFTER, cpu3 $CPU3_BEFORE -> $CPU3_AFTER (both strictly increased over the same sampled interval)"
    echo "$OUT" | grep -E "smp_num_online|zguest_stage_copyin|baseline sample|final sample|DUAL-QEMU-CI2:"
    return 0
}

run_pass "A: bulk-loader chain, payload at ZSTAGE_LOW_PA" "$LOW_ADDR" concurrent || exit 1
run_pass "B: destination-only staging must be refused (stale-image wipe)" "$STAGE_ADDR" wiped || exit 1

echo "dual-qemu-ci: PASS — bulk-loader chain runs concurrently on CPU0+CPU3, and a destination-only (ghost) image is refused"
exit 0
