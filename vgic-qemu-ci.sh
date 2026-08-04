#!/usr/bin/env bash
# vgic-qemu-ci.sh — board-free gate for INTERRUPT VIRTUALIZATION (the vGIC).
#
# Third sibling of qemu-ci.sh / zephyr-qemu-ci.sh, and the one that closes the
# biggest hole those two leave:
#
#   qemu-ci.sh        proves EL2's OWN physical GIC use (GICD/GICC programming,
#                     CNTP IRQ taken at EL2 under HCR_EL2.IMO=1, EL1 guest
#                     preempted). Its guest never receives an interrupt, and
#                     vgic.c is not linked into that target at all.
#   zephyr-qemu-ci.sh boots a real Zephyr guest, but that guest's GIC is
#                     identity-mapped to the A64's addresses (0x01c81000...),
#                     where QEMU virt has NOTHING: the accesses are silently
#                     dropped, arm_gic_init() "succeeds" against a black hole,
#                     and the build is tickless so no interrupt is ever
#                     enabled. That gate says nothing about GIC emulation or
#                     interrupt delivery, by construction — its own banner
#                     says exactly that.
#   THIS one          runs the REAL vgic.c (recompiled with QEMU's GIC base
#                     addresses and nothing else changed — see vgic.h's #ifndef
#                     guards) against QEMU virt's REAL GICv2 virtualization
#                     extensions, and asserts a virtual interrupt is actually
#                     delivered to an EL1 guest, acknowledged via the virtual
#                     IAR, and retired via the virtual EOIR.
#
# WHAT A PASS PROVES (main_vgic_qemu.c's banner has the long form):
#   - vgic_init() talks to a real GICH: >=4 List Registers read out of
#     GICH_VTR, GICH_HCR.En sticks, GICH_VMCR reads back non-zero, CNTVOFF_EL2
#     written. Against the black hole above, every one of these reads 0.
#   - vgic_inject_cntv() builds a List Register word the HARDWARE accepts, and
#     the virtual CPU interface signals EL1 while EL2 keeps taking the physical
#     timer IRQ — two separated interrupt streams.
#   - the guest reads exactly the injected vINTID (27) from the virtual IAR,
#     and its virtual EOIR write frees the LR — proven by the 10th delivery
#     happening at all, which is impossible if any earlier LR stayed stuck.
#
# WHAT A PASS DOES *NOT* PROVE:
#   - anything about the A64 GIC-400 at its own addresses;
#   - anything about the stage-2 GICC->GICV redirect (this target has none —
#     the payload reaches GICV directly, see vgic.h);
#   - anything about HW=1 List Registers / physical deactivation on guest EOI
#     (this is the VGIC_CNTV_HW=0 software-vtimer path, vgic.h's default);
#   - anything about the LR-exhaustion pending queue or vgic_maintenance()
#     (never exercised with one vINTID in flight — see test_vgic_pendq).
#
# No SKIP path: unlike zephyr-qemu-ci.sh there is no 565 MB external input to
# be missing. Everything needed is in this repo, so this script is either PASS
# or FAIL.
#
# Usage:   ./vgic-qemu-ci.sh
# Exit:    0 = PASS, 1 = build/run/assert failure.
set -u

cd "$(dirname "$(readlink -f "$0")")"

QEMU=qemu-system-aarch64
ELF=microkernel-vgic-qemu.elf
# The firmware itself gives up after ~3 s of ticks and prints FAIL, so this
# timeout is only the backstop for "QEMU never even started / wedged".
TIMEOUT=60

command -v "$QEMU" >/dev/null 2>&1 || { echo "vgic-qemu-ci: FAIL — $QEMU not installed"; exit 1; }

echo "vgic-qemu-ci: building $ELF ..."
if ! make -s vgic-qemu >/dev/null 2>&1; then
    echo "vgic-qemu-ci: FAIL — 'make vgic-qemu' did not build"
    make vgic-qemu 2>&1 | tail -20
    exit 1
fi
[ -f "$ELF" ] || { echo "vgic-qemu-ci: FAIL — $ELF missing after build"; exit 1; }

echo "vgic-qemu-ci: running under $QEMU -M virt (timeout ${TIMEOUT}s) ..."
# virtualization=on is what gives the vCPU EL2 *and* the GICv2 virtualization
# extensions (GICH/GICV) this target is entirely about; gic-version=2 matches
# the A64's GIC-400 generation, and cortex-a53 matches the real board's core.
# The HV issues PSCI SYSTEM_OFF on BOTH verdicts, so QEMU exits on its own well
# inside the timeout either way and the timeout never decides the outcome.
OUT=$(timeout "$TIMEOUT" "$QEMU" \
        -machine virt,gic-version=2,virtualization=on \
        -cpu cortex-a53 -m 1024 -nographic \
        -kernel "$ELF" 2>&1)

if echo "$OUT" | grep -q "VGIC-QEMU-CI: PASS"; then
    echo "vgic-qemu-ci: PASS"
    echo "$OUT" | grep -E "vgic_init\(\)|self-check PASS|first physical tick|VGST\[|VGIC-QEMU-CI:"
    exit 0
fi

echo "vgic-qemu-ci: FAIL — never saw 'VGIC-QEMU-CI: PASS' within ${TIMEOUT}s. Full output:"
echo "$OUT" | tail -30
exit 1
