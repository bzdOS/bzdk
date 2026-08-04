#!/usr/bin/env bash
# zephyr-qemu-ci.sh — board-free gate for the ZEPHYR GUEST target.
#
# Sibling of qemu-ci.sh. Where that one proves the hypervisor core (stage-2,
# EL1 entry, GICv2 tick) against a 40-line hand-written EL1 payload, this one
# proves the thing `make zephyr` exists to do and could never be checked
# without a board: that a real, third-party OS image — Zephyr RTOS v4.4.1,
# built out of tree — is parsed, placed and entered by kload.c, runs at EL1
# under our stage-2 map, and talks to a console that only exists because
# vconsole.c emulates it.
#
# The guest image is THE SAME FILE that gets flashed: `bpi_m64_hv`, the real
# board port, Allwinner MMIO addresses and all — not a QEMU-shaped stand-in.
# And it uses the SAME staging contract as real hardware: the guest ELF sits
# in DRAM at physical 0x44000000 and the hypervisor finds it there. On the
# board U-Boot's `tftpboot 0x44000000 zephyr.elf` puts it there; here QEMU's
# generic loader does, with force-raw=on so QEMU drops the file as an opaque
# memory image and does NOT interpret the ELF itself — kload.c does, exactly
# as on the board.
#
# WHAT A PASS HERE DOES NOT COVER (see main_zephyr_qemu.c's banner for the
# measurement behind this): the guest's GIC is identity-mapped, so its
# accesses go out to QEMU virt's memory system, where nothing answers at the
# A64's 0x01c81000 — QEMU silently drops them. Zephyr's arm_gic_init()
# therefore "succeeds" against a black hole. That is harmless for this
# tickless build (no interrupt is ever enabled by anyone) but it means this
# gate says nothing whatsoever about GIC programming, and it will need
# rethinking the day the guest gets a real timer tick.
#
# Usage:   ./zephyr-qemu-ci.sh
# Exit:    0 = PASS or SKIP, 1 = build/run/assert failure.
#
# SKIP (exit 0, with a "SKIP" line) is returned when no Zephyr guest image is
# available AND none can be built — the Zephyr tree is 565 MB and is
# deliberately not vendored into this repo, so a checkout on a fresh host
# genuinely cannot run this. That is a missing input, not a regression, and
# must not be able to turn ci.sh red; ci.sh distinguishes the two.
#
# Override the guest image with ZEPHYR_GUEST_ELF=/path/to/zephyr.elf.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
ELF=microkernel-zephyr-qemu.elf
GUEST_STAGE_ADDR=0x44000000
TIMEOUT=90

command -v "$QEMU" >/dev/null 2>&1 || { echo "zephyr-qemu-ci: FAIL — $QEMU not installed"; exit 1; }

# ---- 1. the hypervisor firmware --------------------------------------------
echo "zephyr-qemu-ci: building $ELF ..."
if ! make -s zephyr-qemu >/dev/null 2>&1; then
    echo "zephyr-qemu-ci: FAIL — 'make zephyr-qemu' did not build"
    make zephyr-qemu 2>&1 | tail -20
    exit 1
fi
[ -f "$ELF" ] || { echo "zephyr-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

# ---- 2. the guest image ----------------------------------------------------
# Order: explicit override, then the conventional out-of-tree build dir, then
# build it ourselves, then SKIP.
GUEST_BOARD=bpi_m64_hv
GUEST=${ZEPHYR_GUEST_ELF:-}
if [ -z "$GUEST" ]; then
    for cand in "/opt/bzdos/zephyr-work/build-$GUEST_BOARD/zephyr/zephyr.elf"; do
        [ -f "$cand" ] && GUEST=$cand && break
    done
fi
if [ -z "$GUEST" ]; then
    echo "zephyr-qemu-ci: no prebuilt guest image; trying zephyr-guest/build.sh ..."
    if built=$(./zephyr-guest/build.sh "$GUEST_BOARD" 2>/dev/null | tail -1) && [ -f "$built" ]; then
        GUEST=$built
    fi
fi
if [ -z "$GUEST" ] || [ ! -f "$GUEST" ]; then
    echo "zephyr-qemu-ci: SKIP — no Zephyr guest image and no Zephyr tree to build one."
    echo "zephyr-qemu-ci: SKIP —   ZEPHYR_BASE=<zephyr-4.4.x> ./zephyr-guest/build.sh $GUEST_BOARD"
    echo "zephyr-qemu-ci: SKIP —   or set ZEPHYR_GUEST_ELF=/path/to/zephyr.elf"
    exit 0
fi

# Fail loudly rather than mysteriously if the image was built for something
# else: the bzdOS guest board links at 0x51000000 with entry 0x5100100c, which
# is what main_zephyr_qemu.c's Z_PABASE assumes.
if ! aarch64-linux-gnu-readelf -lW "$GUEST" 2>/dev/null | grep -q "0x0000000051000000"; then
    echo "zephyr-qemu-ci: FAIL — $GUEST is not linked at 0x51000000 (wrong board?)"
    exit 1
fi

echo "zephyr-qemu-ci: guest image $GUEST"
echo "zephyr-qemu-ci: running under $QEMU -M virt (timeout ${TIMEOUT}s) ..."

# gic-version=2 + virtualization=on give the vCPU EL2 (the HV runs there) and
# a GICv2 at the addresses the guest board's devicetree names; cortex-a53
# matches the real board. -m 1024 is required: the guest's RAM node is
# 0x51000000 + 256 MiB. The HV issues PSCI SYSTEM_OFF on both PASS and FAIL,
# so QEMU exits on its own well inside the timeout either way.
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 1024 -nographic \
        -kernel "$ELF" \
        -device "loader,file=$GUEST,addr=$GUEST_STAGE_ADDR,force-raw=on" 2>&1)

if echo "$OUT" | grep -q "ZEPHYR-QEMU-CI: PASS"; then
    echo "zephyr-qemu-ci: PASS"
    echo "$OUT" | grep -E "self-check PASS|entry_pa|Booting Zephyr|hello from EL1|heartbeat|ZEPHYR-QEMU-CI:"
    exit 0
fi

echo "zephyr-qemu-ci: FAIL — never saw 'ZEPHYR-QEMU-CI: PASS' within ${TIMEOUT}s. Full output:"
echo "$OUT" | tail -30
exit 1
