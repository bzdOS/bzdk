#!/usr/bin/env bash
# dual-zephyr-qemu-ci.sh — dual-guest Step 2 board-free proof: swap Step 1's
# (fc20b63) hand-built trivial ELF on CPU3 for a REAL Zephyr RTOS image,
# still running genuinely concurrently with CPU0's FreeBSD-shaped
# guest_demo_el1(), before ever attempting the real board (Step 3, later).
#
# Follows zephyr-qemu-ci.sh's own SKIP-if-no-Zephyr-tree convention exactly
# (same guest-image discovery order, same SKIP wording, same exit-0-on-SKIP
# contract) -- see that script's header for the rationale: the Zephyr tree
# is 565 MB and deliberately not vendored into this repo, so a checkout on a
# fresh host genuinely cannot run this, and that is a missing input, not a
# regression.
#
# ============================================================================
# WHY THIS SCRIPT BUILDS A DIFFERENT ZEPHYR IMAGE THAN zephyr-qemu-ci.sh DOES
# ============================================================================
# zephyr-qemu-ci.sh's guest is linked for the `bpi_m64_hv` board, entry
# 0x5100100c -- correct for CPU0's main_zephyr_qemu.c (which stages/places it
# at 0x51000000, exactly where it is linked) but WRONG for this script's CPU3
# target: stage2_zephyr.c's table leaves FreeBSD's entire low-DRAM gigabyte
# (which includes 0x51000000) completely UNMAPPED -- that unmapping IS the
# cross-guest isolation boundary this milestone exists to prove. Two options
# were considered for getting Zephyr into CPU3's actual 0xBE000000-0xC0000000
# slice instead:
#
#   1. Keep the `bpi_m64_hv` image and RELOCATE its bytes at HV load time
#      (zload2_parse_and_place()'s pa_base parameter already supports a
#      general positional shift, not just identity placement -- see
#      zguest_cpu3.c, which always passes ZG3_PA_BASE regardless of the
#      ELF's own p_vaddr). Tried first, since it needs no rebuild. RESULT,
#      confirmed empirically (reproduced twice, once with an 80s/800-tick
#      observation window specifically to rule out "just slow"): CPU3
#      genuinely reaches the relocated entry point (zguest_cpu3.c's own
#      breadcrumbs confirm zload2 placed one clean segment and the
#      isolation selfcheck passed) and then produces ZERO console bytes and
#      never traps back to EL2 again, for the entire window. Root cause:
#      the `bpi_m64_hv` board's Kconfig sets CONFIG_ARM_MMU=y, so Zephyr's
#      own arm64 SoC layer builds STATIC STAGE-1 PAGE TABLES from that
#      board's devicetree (sram0 @ 0x51000000, the GIC) at BUILD time --
#      those are baked-in absolute data, unlike the PC-relative code that
#      makes up most of a typical AArch64 binary, and a byte-for-byte
#      positional shift after the fact does not move what they SAY. The
#      guest's own MMU-enable then walks against now-wrong tables and
#      (consistent with the observed symptom) silently wedges in its own
#      fatal-error path before ever reaching a UART write.
#   2. REBUILD Zephyr for a board linked at the address it will actually run
#      at. This is what this script does: `bpi_m64_hv_dual`
#      (zephyr-guest/boards/bzdos/bpi_m64_hv_dual/), a new board derived
#      from `bpi_m64_hv` that differs in exactly one devicetree node (sram0
#      @ 0xBE000000, 32 MiB, matching stage2_zephyr.h's ZSTAGE2_DRAM_BASE/
#      SIZE) -- every MMIO address (GIC, UART0) is unchanged, since those
#      are trap-emulated/absorbed by the hypervisor regardless of the
#      guest's own RAM placement. The real `bpi_m64_hv` board -- the one
#      actually loaded onto hardware -- is NOT touched by this at all.
#
# GUEST_STAGE_ADDR below is therefore ZG3_ELF_STAGE_PA (zguest_cpu3.c:
# ZSTAGE2_DRAM_BASE + ZSTAGE2_DRAM_SIZE/2, 0xBF000000) -- Zephyr's own
# 32 MiB slice's upper half, disjoint from zload2's placement destination
# (ZG3_PA_BASE, the lower half) -- NOT zephyr-qemu-ci.sh's 0x44000000 (that
# address is inside FreeBSD's gigabyte, which this target's CPU3 cannot
# reach at all).
#
# ============================================================================
# WHAT A PASS HERE ASSERTS (see el2_exc_dual_zephyr_qemu.c's header for the
# full three-part verdict design)
# ============================================================================
#   1. CPU0's guest_demo_el1() loop counter strictly increases across a
#      sampled interval -- Step 1's own CPU0-side proof, unchanged.
#   2. CPU3's REAL vconsole channel-1 byte counter strictly increases across
#      the SAME interval -- Step 1's concurrency argument, generalised to
#      real Zephyr's own console traffic instead of a synthetic counter.
#   3. The real "*** Booting Zephyr OS build v4.4.1 ***" / "heartbeat 1"
#      text was actually seen in that byte stream (byte-for-byte substring
#      match, same technique zephyr-qemu-ci.sh's own el2_exc_zephyr_qemu.c
#      uses) -- the guard against "the byte counter went up because garbage
#      is being emitted", which (1) and (2) alone cannot rule out.
#
# All three, together, in ONE run, printed via "DUAL-ZEPHYR-QEMU-CI: PASS".
#
# Usage:   ZEPHYR_BASE=<zephyr-4.4.x> ./dual-zephyr-qemu-ci.sh
# Exit:    0 = PASS or SKIP, 1 = build/run/assert failure.
#
# Wired into ci.sh as of 2026-08-11 (stage 10), with the same SKIP-vs-FAIL
# handling the single-guest Zephyr and Linux stages already use: a missing Zephyr
# tree must not be able to turn the gate red. Still runnable standalone, which is
# the faster loop while iterating on this target specifically.
#
# Override the guest image with ZEPHYR_DUAL_GUEST_ELF=/path/to/zephyr.elf.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
CROSS=${CROSS:-aarch64-linux-gnu-}
ELF=microkernel-dual-zephyr-qemu.elf
GUEST_BOARD=bpi_m64_hv_dual
# ZG3_ELF_STAGE_PA from zguest_cpu3.c: ZSTAGE2_DRAM_BASE (0xBE000000) +
# ZSTAGE2_DRAM_SIZE/2 (16 MiB) == 0xBF000000 -- see dual-qemu-ci.sh's own
# copy of this same comment (Step 1 used the identical address for its
# trivial payload).
GUEST_STAGE_ADDR=0xBF000000
# ZSTAGE_LOW_PA from zstage.h: the low-DRAM window U-Boot's third TFTP drops a
# guest image into on the real board. Staging a REAL Zephyr image here (rather
# than only the 144-byte trivial payload dual-qemu-ci.sh uses) is the point of
# pass A below: it is the first thing that exercises zstage_span() + the copy
# against an image of realistic size and shape -- many program headers, and
# large .debug_* sections past the last PT_LOAD that must NOT be copied.
GUEST_LOW_ADDR=0x4E000000
# -m 2048 -smp 4: same requirement dual-qemu-ci.sh (Step 1) already
# established -- Zephyr's stage-2 slice sits in the high GiB, which only
# physically exists with >=2 GiB of QEMU RAM, and all 4 cores must be
# online for CPU3 to exist at all.
#
# IDLE_TIMEOUT/ABS_TIMEOUT replace the old flat TIMEOUT=90: see
# qemu-patient-run.sh for why a fixed wall-clock budget flaked under host
# load (this is one of the two scripts that measurably did, 2026-08-27) and
# what replaces it.
IDLE_TIMEOUT=60
ABS_TIMEOUT=450

command -v "$QEMU" >/dev/null 2>&1 || { echo "dual-zephyr-qemu-ci: FAIL — $QEMU not installed"; exit 1; }
command -v "${CROSS}gcc" >/dev/null 2>&1 || { echo "dual-zephyr-qemu-ci: FAIL — ${CROSS}gcc not installed"; exit 1; }
. "$(dirname "$(readlink -f "$0")")/qemu-patient-run.sh"

# ---- 1. the hypervisor firmware (CPU0 skeleton + real dual-guest mechanism,
# now with a CPU3 dispatch that can service real Zephyr's UART/GIC faults) --
echo "dual-zephyr-qemu-ci: building $ELF ..."
# GUEST_DRAM_2G=0 explicitly: board-config.xml turns that flag ON tree-wide (the
# guest needs the board's whole 2 GiB to build Mesa), and it is a COMPILE ERROR
# in stage2_zephyr.h for any dual build, because Zephyr's guest slice lives in
# the high GiB the flag hands to FreeBSD. The flag is a whole-tree build choice,
# so the target that needs the opposite choice says so at its own build site
# rather than relying on nobody having enabled it.
if ! make GUEST_DRAM_2G=0 -s dual-zephyr-qemu >/dev/null 2>&1; then
    echo "dual-zephyr-qemu-ci: FAIL — 'make GUEST_DRAM_2G=0 dual-zephyr-qemu' did not build"
    make GUEST_DRAM_2G=0 dual-zephyr-qemu 2>&1 | tail -40
    exit 1
fi
[ -f "$ELF" ] || { echo "dual-zephyr-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

# ---- 2. the real Zephyr guest image, for the bpi_m64_hv_dual board --------
# Same discovery order as zephyr-qemu-ci.sh: explicit override, then the
# conventional out-of-tree build dir, then build it ourselves, then SKIP.
GUEST=${ZEPHYR_DUAL_GUEST_ELF:-}
if [ -z "$GUEST" ]; then
    for cand in "/opt/bzdos/zephyr-work/build-$GUEST_BOARD/zephyr/zephyr.elf"; do
        [ -f "$cand" ] && GUEST=$cand && break
    done
fi
if [ -z "$GUEST" ]; then
    echo "dual-zephyr-qemu-ci: no prebuilt $GUEST_BOARD guest image; trying zephyr-guest/build.sh ..."
    if built=$(./zephyr-guest/build.sh "$GUEST_BOARD" 2>/dev/null | tail -1) && [ -f "$built" ]; then
        GUEST=$built
    fi
fi
if [ -z "$GUEST" ] || [ ! -f "$GUEST" ]; then
    echo "dual-zephyr-qemu-ci: SKIP — no Zephyr guest image and no Zephyr tree to build one."
    echo "dual-zephyr-qemu-ci: SKIP —   ZEPHYR_BASE=<zephyr-4.4.x> ./zephyr-guest/build.sh $GUEST_BOARD"
    echo "dual-zephyr-qemu-ci: SKIP —   or set ZEPHYR_DUAL_GUEST_ELF=/path/to/zephyr.elf"
    exit 0
fi

# Fail loudly rather than mysteriously if the image was built for the wrong
# board/address: this target's CPU3 slice starts at 0xBE000000, not
# zephyr-qemu-ci.sh's 0x51000000 -- see this file's header for why they
# differ. Same style of sanity check that script already applies to its own
# guest image.
if ! "${CROSS}readelf" -lW "$GUEST" 2>/dev/null | grep -q "0x00000000be000000"; then
    echo "dual-zephyr-qemu-ci: FAIL — $GUEST is not linked at 0xBE000000 (wrong board? expected $GUEST_BOARD)"
    "${CROSS}readelf" -lW "$GUEST" 2>/dev/null
    exit 1
fi

echo "dual-zephyr-qemu-ci: guest image $GUEST"

# ---- 3. run it, TWICE, once per staging route -------------------------------
# Same firmware binary. The payload's landing address differs and so does the
# correct outcome -- the arrangement dual-qemu-ci.sh (Step 1) documents in full.
#
#   Pass A (GUEST_LOW_ADDR)   -- the real bulk-loader chain, now with a REAL
#                                Zephyr image: U-Boot's third TFTP lands it in
#                                low DRAM and zstage.c's copy-in is the ONLY
#                                thing that can move it to where
#                                zload2_parse_and_place() reads. This is the
#                                first test anywhere that exercises the span
#                                computation against a realistically-shaped
#                                image rather than a 144-byte payload.
#   Pass B (GUEST_STAGE_ADDR) -- direct staging at the destination, the route
#                                commit 779153d proved and which is
#                                deliberately no longer supported. Zephyr must
#                                now FAIL to start: a boot that stages no image
#                                at the landing window actively zeroes the
#                                destination header, because a warm reset
#                                preserves DRAM and the previous boot's image
#                                would otherwise still be bootable as a ghost
#                                (observed live 2026-08-10, see
#                                zstage_invalidate_dest() in zstage.c).
# $3: "concurrent" = both guests must advance; "wiped" = Zephyr must NOT start,
#     proving the stale-image wipe.
run_pass() {
    pass_name=$1
    load_addr=$2
    expect=$3

    echo "dual-zephyr-qemu-ci: [$pass_name] running under $QEMU -M virt -smp 4 -m 2048, guest at $load_addr (idle timeout ${IDLE_TIMEOUT}s, absolute ceiling ${ABS_TIMEOUT}s) ..."
    qemu_patient_run OUT "$IDLE_TIMEOUT" "$ABS_TIMEOUT" -- \
            "$QEMU" \
            -machine virt,gic-version=2,virtualization=on \
            -cpu cortex-a53 -m 2048 -smp 4 -nographic \
            -kernel "$ELF" \
            -device "loader,file=$GUEST,addr=$load_addr,force-raw=on"

    # A fault/panic is never acceptable, in either pass. Firmware wording note:
    # el2_exc_dual_zephyr_qemu.c no longer prints "DUAL-ZEPHYR-QEMU-CI: FAIL"
    # for a cpu0/cpu3/marker-did-not-advance outcome (it can't tell
    # "concurrent" from "wiped" -- see that file's own comment on the
    # branch); it prints "DUAL-ZEPHYR-QEMU-CI: OUTCOME ..." instead, and this
    # script decides PASS/FAIL below from the parsed before=/after=/marker
    # values, per "expect". The things that can still print literal
    # "DUAL-ZEPHYR-QEMU-CI: FAIL" are genuine, expectation-independent
    # failures -- PSCI CPU_ON bring-up not completing
    # (main_dual_zephyr_qemu.c) or CPU0 itself faulting
    # (el2_exc_dual_zephyr_qemu.c's report_fault_cpu0()) -- which is why this
    # check is unconditional (not gated on "expect").
    if echo "$OUT" | grep -qiE "CPU3 FAULT|DUAL-ZEPHYR-QEMU-CI: FAIL|UNEXPECTED TRAP|panic|Unhandled"; then
        echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — fault/panic/bring-up error in output:"
        echo "$OUT" | grep -iE "CPU3 FAULT|DUAL-ZEPHYR-QEMU-CI: FAIL|UNEXPECTED TRAP|panic|Unhandled" | head
        echo "dual-zephyr-qemu-ci: full output:"
        echo "$OUT"
        return 1
    fi

    VERDICT=$(echo "$OUT" | grep "DUAL-ZEPHYR-QEMU-CI: cpu0 before=" | tail -1)
    if [ -z "$VERDICT" ]; then
        echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — never saw the 'DUAL-ZEPHYR-QEMU-CI: cpu0 before=...' sample line (qemu exited early, or was killed after ${IDLE_TIMEOUT}s with no new output / ${ABS_TIMEOUT}s total). Full output:"
        echo "$OUT"
        return 1
    fi

    # Re-derive the before/after numbers from the printed line and check the
    # arithmetic OURSELVES, same discipline dual-qemu-ci.sh (Step 1) applies.
    CPU0_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu0 before=\([0-9]*\).*/\1/p')
    CPU0_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu0 before=[0-9]* after=\([0-9]*\).*/\1/p')
    CPU3_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu3_bytes before=\([0-9]*\).*/\1/p')
    CPU3_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu3_bytes before=[0-9]* after=\([0-9]*\).*/\1/p')
    MARKER_SEEN=$(echo "$VERDICT" | sed -n 's/.*zephyr_marker_seen=\([01]\).*/\1/p')

    if [ -z "$CPU0_BEFORE" ] || [ -z "$CPU0_AFTER" ] || [ -z "$CPU3_BEFORE" ] || [ -z "$CPU3_AFTER" ] || [ -z "$MARKER_SEEN" ]; then
        echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — could not parse before/after/marker values out of: $VERDICT"
        return 1
    fi

    if [ "$CPU0_AFTER" -le "$CPU0_BEFORE" ]; then
        echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — CPU0's guest_demo_el1 counter did not strictly increase ($CPU0_BEFORE -> $CPU0_AFTER)"
        return 1
    fi
    if [ "$expect" = "wiped" ]; then
        # Zephyr must have produced NOTHING: the ghost image was zeroed, so
        # zload2_parse_and_place() rejected it and CPU3 never entered a guest.
        # CPU0 must be unharmed by the wipe.
        if [ "$CPU3_AFTER" -ne 0 ] || [ "$MARKER_SEEN" != "0" ]; then
            echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — Zephyr produced console output ($CPU3_BEFORE -> $CPU3_AFTER, marker=$MARKER_SEEN) from an image staged only at the destination. The stale-image wipe did NOT happen, so a warm reset can boot a ghost image (see zstage_invalidate_dest())."
            echo "dual-zephyr-qemu-ci: full output:"
            echo "$OUT"
            return 1
        fi
        if [ "$CPU0_AFTER" -le "$CPU0_BEFORE" ]; then
            echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — CPU0 stalled ($CPU0_BEFORE -> $CPU0_AFTER); the wipe must not cost the first guest anything"
            return 1
        fi
        echo "dual-zephyr-qemu-ci: [$pass_name] PASS — Zephyr correctly refused the destination-only image (0 bytes, no banner) while CPU0 stayed healthy ($CPU0_BEFORE -> $CPU0_AFTER)"
        echo "$OUT" | grep -E "smp_num_online|zguest_stage_copyin|baseline sample|DUAL-ZEPHYR-QEMU-CI:"
        return 0
    fi

    if [ "$CPU3_AFTER" -le "$CPU3_BEFORE" ]; then
        echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — CPU3's real Zephyr console byte counter did not strictly increase ($CPU3_BEFORE -> $CPU3_AFTER)"
        echo "dual-zephyr-qemu-ci: full output:"
        echo "$OUT"
        return 1
    fi
    if [ "$MARKER_SEEN" != "1" ]; then
        echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — the real 'heartbeat 1' banner text was never confirmed in the console stream"
        return 1
    fi

    if ! echo "$OUT" | grep -q "DUAL-ZEPHYR-QEMU-CI: PASS"; then
        echo "dual-zephyr-qemu-ci: FAIL [$pass_name] — arithmetic and marker checked out but never saw 'DUAL-ZEPHYR-QEMU-CI: PASS'. Full output:"
        echo "$OUT"
        return 1
    fi

    echo "dual-zephyr-qemu-ci: [$pass_name] PASS — cpu0 $CPU0_BEFORE -> $CPU0_AFTER, cpu3 console bytes $CPU3_BEFORE -> $CPU3_AFTER, real banner+heartbeat confirmed"
    echo "$OUT" | grep -E "smp_num_online|zguest_stage_copyin|baseline sample|Booting Zephyr|hello from EL1|heartbeat|DUAL-ZEPHYR-QEMU-CI:"
    return 0
}

run_pass "A: bulk-loader chain, real Zephyr at ZSTAGE_LOW_PA" "$GUEST_LOW_ADDR" concurrent || exit 1
run_pass "B: destination-only staging must be refused (stale-image wipe)" "$GUEST_STAGE_ADDR" wiped || exit 1

echo "dual-zephyr-qemu-ci: PASS — real Zephyr ran concurrently with CPU0 via the bulk-loader chain, and a destination-only (ghost) image was refused"
exit 0
