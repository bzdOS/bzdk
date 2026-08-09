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
# NOT wired into ci.sh yet, per the task that added this script -- run
# standalone until proven reliable (see the task's own verification
# requirement: 5+ standalone runs).
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
# -m 2048 -smp 4: same requirement dual-qemu-ci.sh (Step 1) already
# established -- Zephyr's stage-2 slice sits in the high GiB, which only
# physically exists with >=2 GiB of QEMU RAM, and all 4 cores must be
# online for CPU3 to exist at all.
TIMEOUT=90

command -v "$QEMU" >/dev/null 2>&1 || { echo "dual-zephyr-qemu-ci: FAIL — $QEMU not installed"; exit 1; }
command -v "${CROSS}gcc" >/dev/null 2>&1 || { echo "dual-zephyr-qemu-ci: FAIL — ${CROSS}gcc not installed"; exit 1; }

# ---- 1. the hypervisor firmware (CPU0 skeleton + real dual-guest mechanism,
# now with a CPU3 dispatch that can service real Zephyr's UART/GIC faults) --
echo "dual-zephyr-qemu-ci: building $ELF ..."
if ! make -s dual-zephyr-qemu >/dev/null 2>&1; then
    echo "dual-zephyr-qemu-ci: FAIL — 'make dual-zephyr-qemu' did not build"
    make dual-zephyr-qemu 2>&1 | tail -40
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
echo "dual-zephyr-qemu-ci: running under $QEMU -M virt -smp 4 -m 2048 (timeout ${TIMEOUT}s) ..."
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 2048 -smp 4 -nographic \
        -kernel "$ELF" \
        -device "loader,file=$GUEST,addr=$GUEST_STAGE_ADDR,force-raw=on" 2>&1)

if echo "$OUT" | grep -qiE "DUAL-ZEPHYR-QEMU-CI: FAIL|CPU3 FAULT|UNEXPECTED TRAP|panic|Unhandled"; then
    echo "dual-zephyr-qemu-ci: FAIL — error line in output:"
    echo "$OUT" | grep -iE "DUAL-ZEPHYR-QEMU-CI: FAIL|CPU3 FAULT|UNEXPECTED TRAP|panic|Unhandled" | head
    echo "dual-zephyr-qemu-ci: full output:"
    echo "$OUT"
    exit 1
fi

VERDICT=$(echo "$OUT" | grep "DUAL-ZEPHYR-QEMU-CI: cpu0 before=" | tail -1)
if [ -z "$VERDICT" ]; then
    echo "dual-zephyr-qemu-ci: FAIL — never saw the 'DUAL-ZEPHYR-QEMU-CI: cpu0 before=...' sample line within ${TIMEOUT}s. Full output:"
    echo "$OUT"
    exit 1
fi

# Re-derive the before/after numbers from the printed line and check the
# arithmetic OURSELVES, same discipline dual-qemu-ci.sh (Step 1) applies.
CPU0_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu0 before=\([0-9]*\).*/\1/p')
CPU0_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu0 before=[0-9]* after=\([0-9]*\).*/\1/p')
CPU3_BEFORE=$(echo "$VERDICT" | sed -n 's/.*cpu3_bytes before=\([0-9]*\).*/\1/p')
CPU3_AFTER=$(echo  "$VERDICT" | sed -n 's/.*cpu3_bytes before=[0-9]* after=\([0-9]*\).*/\1/p')
MARKER_SEEN=$(echo "$VERDICT" | sed -n 's/.*zephyr_marker_seen=\([01]\).*/\1/p')

if [ -z "$CPU0_BEFORE" ] || [ -z "$CPU0_AFTER" ] || [ -z "$CPU3_BEFORE" ] || [ -z "$CPU3_AFTER" ] || [ -z "$MARKER_SEEN" ]; then
    echo "dual-zephyr-qemu-ci: FAIL — could not parse before/after/marker values out of: $VERDICT"
    exit 1
fi

if [ "$CPU0_AFTER" -le "$CPU0_BEFORE" ]; then
    echo "dual-zephyr-qemu-ci: FAIL — CPU0's guest_demo_el1 counter did not strictly increase ($CPU0_BEFORE -> $CPU0_AFTER)"
    exit 1
fi
if [ "$CPU3_AFTER" -le "$CPU3_BEFORE" ]; then
    echo "dual-zephyr-qemu-ci: FAIL — CPU3's real Zephyr console byte counter did not strictly increase ($CPU3_BEFORE -> $CPU3_AFTER)"
    exit 1
fi
if [ "$MARKER_SEEN" != "1" ]; then
    echo "dual-zephyr-qemu-ci: FAIL — the real 'heartbeat 1' banner text was never confirmed in the console stream"
    exit 1
fi

if echo "$OUT" | grep -q "DUAL-ZEPHYR-QEMU-CI: PASS"; then
    echo "dual-zephyr-qemu-ci: PASS — cpu0 $CPU0_BEFORE -> $CPU0_AFTER, cpu3 console bytes $CPU3_BEFORE -> $CPU3_AFTER, real banner+heartbeat confirmed"
    echo "$OUT" | grep -E "smp_num_online|baseline sample|Booting Zephyr|hello from EL1|heartbeat|DUAL-ZEPHYR-QEMU-CI:"
    exit 0
fi

echo "dual-zephyr-qemu-ci: FAIL — arithmetic and marker checked out but never saw 'DUAL-ZEPHYR-QEMU-CI: PASS'. Full output:"
echo "$OUT"
exit 1
