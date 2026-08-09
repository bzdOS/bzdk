# Copyright (c) 2026 bzdOS project
# SPDX-License-Identifier: Apache-2.0
#
# Real hardware, not an emulator: no SUPPORTED_EMU_PLATFORMS entry, same as
# the sibling `bpi_m64_hv` board this one is derived from -- see that
# board's board.cmake for the full rationale (unchanged here).
#
# This board is NOT the one that gets loaded onto the real Banana Pi M64 --
# `bpi_m64_hv` (this board's sibling) is, and stays untouched by this pass.
# `bpi_m64_hv_dual` exists ONLY for the board-free QEMU dual-guest Step 2
# proof (../../../../dual-zephyr-qemu-ci.sh): CPU0 runs FreeBSD-shaped
# guest_demo_el1() while CPU3 runs a real Zephyr image concurrently, in the
# hypervisor's high-GiB "Zephyr private slice" (0xBE000000-0xC0000000, see
# stage2_zephyr.h) rather than the low-DRAM-gigabyte address the real board
# stages Zephyr at (0x51000000, `bpi_m64_hv`'s own sram0 node). The two
# addresses cannot be shared: CPU3's stage-2 table (stage2_zephyr.c)
# deliberately leaves FreeBSD's entire low-DRAM gigabyte UNMAPPED -- that IS
# the cross-guest isolation boundary this milestone exists to prove -- so a
# `bpi_m64_hv`-linked (0x51000000) image cannot run there at all, and an
# attempt to make it do so by RELOCATING that image's bytes at load time
# (rather than rebuilding for the right address) was tried and found to
# silently wedge the guest before it ever reaches its own console (see
# CONFIG_ARM_MMU=y in bpi_m64_hv_dual_defconfig's sibling: Zephyr's own
# static stage-1 page tables are built against the devicetree's sram0/GIC
# addresses at LINK time, and a naive positional relocation leaves those
# tables pointing at the OLD, now-unmapped addresses -- the guest's own MMU
# enable then faults immediately). This board fixes that at the source: its
# sram0 node names the ADDRESS THE IMAGE WILL ACTUALLY RUN AT, so every
# absolute pointer Zephyr's own build bakes in (page tables included) is
# correct from the start, with zero HV-side relocation trickery needed.
