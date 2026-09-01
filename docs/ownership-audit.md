# Ownership-class completeness audit — internal-note

**Date:** 2026-09-01. **SPEC ref:** SPEC_chimp_hal.md §2, §3.3.

## 1. board-config.xml entries: ALL 14 marked

| Entry | owner | Notes |
|---|---|---|
| **Devices (virtio-mmio)** | | |
| vblk_emmc | hv | EL2 owns eMMC HW, guest sees virtio-blk |
| vnet_emac | hv | EL2 owns EMAC HW, guest sees virtio-net |
| scanout | hv | EL2 owns HDMI/TCON1, guest sees doorbell |
| vinput | hv | EL2 owns input path |
| vblk_sd | hv | EL2 owns SD HW |
| **SoC nodes (DTB)** | | |
| /soc/usb@1c1b000 | guest | EHCI1, PHY1 (electrically independent) |
| /soc/usb@1c1b400 | guest | OHCI1, PHY1 |
| /soc/mmc@1c10000 | guest | SMHC1/WiFi (was none, flipped with guest_wifi_sdio) |
| /soc/codec@1c22e00 | guest | Digital codec (register control only) |
| /soc/codec-analog@1f015c0 | guest | Analog codec |
| /soc/dsi@1ca0000 | guest | MIPI DSI (no FreeBSD driver yet) |
| /soc/d-phy@1ca1000 | guest | MIPI D-PHY |
| /soc/csi@1cb0000 | guest | MIPI CSI (no FreeBSD driver yet) |
| /soc/ir@1f02000 | guest | Infrared remote |

**Completeness: 14/14 (100%).** No entry lacks owner=.

## 2. SoC blocks NOT in board-config.xml

These are SoC-level, not DTB feature-gated. They live in EL2 C code directly,
not in the board-config.xml `<devices>` or `<soc-nodes>` sections.

| Block | SPEC owner | Why not in board-config.xml |
|---|---|---|
| Timers/GIC/PLL_CPUX | EL2 | SoC-level, gic_timer.c/sched.c — no DTB flip |
| PMIC AXP803 (RSB) | EL2 | axp803.c/rsb.c — EL2-owned, no guest driver |
| Thermal THS | EL2 | Not yet implemented (SPEC §9 phase 2) |
| Watchdog WDOG | EL2 | wdt.c — EL2 pets, guest cannot disarm |
| GPIO | EL2 + guest allow-list | Stage2 trap-based, not DTB-gated |
| DMA controller | EL2 | dai@1c22c00 (has dmas) in FORBIDDEN_DTB_NODES |
| I2C/TWI | none | No consumers on M64 — not built |

These blocks don't need owner= in board-config.xml because they aren't
feature-gated DTB nodes. Their ownership is enforced by code structure
(linker markers, FORBIDDEN_DTB_NODES, stage2 traps).

## 3. hv-rt class status

**gen_config.py:** hv-rt is in VALID_OWNERS since c380b6a. Validation passes.

**Actual usage:** NO block currently uses hv-rt. The spec (§3.3) reserves
this class for:
- Watchdog tick (periodic WDOG pet as WCET task)
- EMAC/dbgmon channel (periodic poll as WCET task)

These are currently classified as plain `hv` because the WCET integration
(spec §9 phase 2-3) is not yet implemented. When wcet.c gets its first
real consumer (THS periodic read), the watchdog pet and EMAC poll should
be reclassified to hv-rt.

**Recommendation:** Keep hv-rt unused for now. Assign it when wcet.c
tasks are actually declared with period/budget in sched.c. Prematurely
marking blocks hv-rt would be speculative — the class exists for the
future, not as a current label.

## 4. Classification correctness

| Check | Result |
|---|---|
| codec@1c22e00 owner=guest | CORRECT — register control only; dai@1c22c00 (with dmas) is FORBIDDEN |
| codec-analog@1f015c0 owner=guest | CORRECT — analog codec, no DMA |
| mmc@1c10000 owner=guest | CORRECT — WiFi enabled, SMHC1 exposed to guest |
| All hv devices in known address ranges | CORRECT — cross-check passes (virtio-mmio block + soc_a64.h) |
| No guest device backs onto EL2-critical addr | CORRECT — cross-check passes |

## 5. SPEC §3.3 update needed

The spec's §3.3 step 2 says:

> Добавить классы `hv`, `none` и `hv-rt`

**Actual state:** All three classes exist in gen_config.py. `hv` and `guest`
are actively used (5 hv + 9 guest). `none` was used for WiFi before the
flip. `hv-rt` is valid but unused — reserved for WCET-task blocks.

The spec's description of `shares="phy0"` attribute is NOT yet implemented
in board-config.xml or gen_config.py. This is a future enhancement for
blocks that share silicon (e.g. MUSB + USB host0 share PHY0).
