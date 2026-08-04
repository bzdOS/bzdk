# Copyright (c) 2026 bzdOS project
# SPDX-License-Identifier: Apache-2.0
#
# Real hardware, not an emulator: no SUPPORTED_EMU_PLATFORMS entry, so
# `west build -t run` has nothing to do here (correctly -- there is no QEMU
# machine model matching this MMIO map: real GICv2 @ 0x01c81000/0x01c82000
# plus a 16550 UART @ 0x01c28000 is not any of QEMU's built-in "virt"
# variants). Loading onto real hardware is via the bzdOS hypervisor's own
# TFTP + loady/bootelf U-Boot flow -- see docs/index.rst in this board's
# directory for the exact procedure.
#
# That said, this image CAN be run board-free -- just not by `west -t run`,
# and not on its own. Under the bzdOS hypervisor's QEMU-virt target it boots
# unmodified (the UART page is trap-emulated, so its address needing to be
# the A64's is irrelevant; the GIC's is silently dropped by QEMU, which is
# harmless for this tickless build). From the hypervisor tree:
#
#     ./zephyr-qemu-ci.sh
#
# See that script's header and main_zephyr_qemu.c's banner for exactly what
# such a run does and does not prove.
