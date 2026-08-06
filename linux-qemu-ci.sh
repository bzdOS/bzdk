#!/usr/bin/env bash
# linux-qemu-ci.sh — board-free gate for the LINUX GUEST target (ROADMAP D2).
#
# Sibling of zephyr-qemu-ci.sh, same shape: a real, mainline Linux/arm64
# `Image` (not a QEMU-shaped stand-in — the same artifact that would be
# TFTP'd to the real board, were this pass touching the board, which it does
# not) is handed off by main_linux_qemu.c's own Image-header loader, runs at
# EL1 under this hypervisor's real stage-2 map, and its console is served by
# the real vconsole.c 16550 trap-emulator — the same object FreeBSD and
# Zephyr use, unmodified.
#
# WHAT A PASS HERE DOES AND DOES NOT COVER — see docs/linux-guest.md's
# status table for the authoritative, row-by-row version; short form:
#   - DOES prove: the Image-header handoff (main_linux_qemu.c) is correct,
#     stage-2 + EL2->EL1 entry work for a second, independent guest OS, and
#     Linux's own earlycon 8250 driver drives vconsole.c's trap-emulated
#     UART0 correctly enough to print its own boot banner.
#   - Does NOT prove: GIC programming (identity-mapped, black hole under
#     QEMU — same limitation zephyr-qemu-ci.sh documents), anything past
#     whatever the guest console last printed, or anything about real
#     hardware timing.
#
# Usage:   ./linux-qemu-ci.sh
# Exit:    0 = PASS or SKIP, 1 = build/run/assert failure.
#
# SKIP (exit 0, with a "SKIP" line) when no Linux Image+DTB are available
# AND none can be built — a Linux source tree is a ~1.6 GB extracted
# checkout, deliberately not vendored into this repo (see linux-guest/
# build.sh's header), so a checkout on a fresh host genuinely cannot run
# this without first fetching one. That is a missing input, not a
# regression, and must not be able to turn ci.sh red — same contract
# zephyr-qemu-ci.sh already established.
#
# Override with LINUX_GUEST_IMAGE=/path/to/Image and
# LINUX_GUEST_DTB=/path/to/dtb (both, or neither — a mismatched pair is
# almost certainly not what the caller meant, so this script requires both
# together, same as ZEPHYR_GUEST_ELF being the single override zephyr's
# script accepts).
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
ELF=microkernel-linux-qemu.elf
LOAD_ADDR=0x51000000
DTB_ADDR=0x44000000
TIMEOUT=120

command -v "$QEMU" >/dev/null 2>&1 || { echo "linux-qemu-ci: FAIL — $QEMU not installed"; exit 1; }

# ---- 1. the hypervisor firmware --------------------------------------------
echo "linux-qemu-ci: building $ELF ..."
if ! make -s linux-qemu >/dev/null 2>&1; then
    echo "linux-qemu-ci: FAIL — 'make linux-qemu' did not build"
    make linux-qemu 2>&1 | tail -20
    exit 1
fi
[ -f "$ELF" ] || { echo "linux-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

# ---- 2. the guest artifacts (Image + DTB) ----------------------------------
# Order: explicit override, then the conventional out-of-tree build dir, then
# build them ourselves, then SKIP.
IMAGE=${LINUX_GUEST_IMAGE:-}
DTB=${LINUX_GUEST_DTB:-}
if [ -z "$IMAGE" ] || [ -z "$DTB" ]; then
    cand_image=/opt/bzdos/linux-work/build/arch/arm64/boot/Image
    cand_dtb=/opt/bzdos/linux-work/build/bpi_m64_hv-linux.dtb
    if [ -f "$cand_image" ] && [ -f "$cand_dtb" ]; then
        IMAGE=$cand_image
        DTB=$cand_dtb
    fi
fi
if [ -z "$IMAGE" ] || [ -z "$DTB" ]; then
    echo "linux-qemu-ci: no prebuilt guest artifacts; trying linux-guest/build.sh ..."
    if built=$(./linux-guest/build.sh 2>/dev/null) && [ -n "$built" ]; then
        IMAGE=$(echo "$built" | sed -n 's/^IMAGE=//p')
        DTB=$(echo "$built" | sed -n 's/^DTB=//p')
    fi
fi
if [ -z "$IMAGE" ] || [ ! -f "$IMAGE" ] || [ -z "$DTB" ] || [ ! -f "$DTB" ]; then
    echo "linux-qemu-ci: SKIP — no Linux Image+DTB and no Linux tree to build one."
    echo "linux-qemu-ci: SKIP —   LINUX_SRC=<linux-6.12.x checkout> ./linux-guest/build.sh"
    echo "linux-qemu-ci: SKIP —   or set LINUX_GUEST_IMAGE=/path/to/Image LINUX_GUEST_DTB=/path/to/dtb"
    exit 0
fi

# Fail loudly rather than mysteriously if the artifact was built for
# something else: main_linux_qemu.c's linux_image_check() enforces
# text_offset==0 and the "ARM\x64" magic at runtime already, but checking
# the magic here too turns a wrong-file mistake into an immediate, specific
# message instead of a QEMU timeout.
magic=$(od -An -tx4 -j56 -N4 --endian=little "$IMAGE" 2>/dev/null | tr -d ' ')
if [ "$magic" != "644d5241" ]; then
    echo "linux-qemu-ci: FAIL — $IMAGE has no arm64 Image magic at offset 56 (got 0x$magic)"
    exit 1
fi

echo "linux-qemu-ci: guest Image=$IMAGE DTB=$DTB"
echo "linux-qemu-ci: running under $QEMU -M virt (timeout ${TIMEOUT}s) ..."

# Same machine shape as zephyr-qemu-ci.sh/vgic-qemu-ci.sh: gic-version=2 +
# virtualization=on give the vCPU EL2 (the HV runs there), cortex-a53 matches
# the real board, -m 1024 matches stage2.h's 1 GiB identity map. Two loader
# devices, each dropping its file at the EXACT final address
# main_linux_qemu.c reads from — no relocation step exists on either side
# (see that file's header "WHY NO PLACE SEGMENTS STEP").
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 1024 -nographic \
        -kernel "$ELF" \
        -device "loader,file=$IMAGE,addr=$LOAD_ADDR,force-raw=on" \
        -device "loader,file=$DTB,addr=$DTB_ADDR,force-raw=on" 2>&1)

if echo "$OUT" | grep -q "LINUX-QEMU-CI: PASS"; then
    echo "linux-qemu-ci: PASS"
    echo "$OUT" | grep -E "Image header OK|entry_pa|Linux version|earlycon|LINUX-QEMU-CI:"
    exit 0
fi

echo "linux-qemu-ci: FAIL — never saw 'LINUX-QEMU-CI: PASS' within ${TIMEOUT}s. Full output:"
echo "$OUT" | tail -40
exit 1
