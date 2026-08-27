# DMA bypass of stage-2 (finding H2(b)): what's fixable now vs. inherent

This is a clear-eyed analysis, in the spirit of `docs/security-notes.md` — it
documents a known, accepted gap precisely, cross-references the exact DTB
this board boots (`/opt/bzdos/build/bananapi-min.dts`, TFTP'd as
`bananapi-min.dtb`) to separate "theoretically reachable" from "the guest
actually uses this", and proposes ONE narrow, well-justified stage-2 denial
as a reviewed, build-verified, **NOT YET applied** draft patch. No change to
`stage2.c`/`el2_exc.c` was made as part of writing this file — see "What was
actually changed" at the bottom.

## The gap, precisely

Stage-2 (`stage2.c`) constrains **CPU** accesses only: `VTTBR_EL2`/`VTCR_EL2`
govern the translation a CPU (running at EL1/EL0, i.e. the guest) performs
when it issues a load/store. They say nothing about accesses issued by a
peripheral's own DMA engine — those go straight from the device to the
memory controller over the SoC's internal interconnect, never through the
CPU's stage-2 (or stage-1) MMU at all. The A64 (like most Cortex-A53-class
SoCs of its generation) has **no SMMU/IOMMU** in front of its peripheral bus
masters, so there is no stage-2-equivalent gate for DMA at all — this half of
the sentence is an inherent hardware limitation, not a bug in this tree.

The exploitable consequence: today the guest has RW CPU access to every SoC
MMIO register in `[0x00000000, 0x40000000)` except the two windows
`stage2_build_mmio_tables()` already carves out (the UART0 4 KiB page and the
`vblk`/`vnet` virtio-mmio 2 MiB block at `0x0A000000`, see stage2.c). If any
enabled MMIO block among the rest is a DMA-capable engine, a compromised or
buggy guest kernel can program a descriptor whose target address is
`hv-image` (`0x42000000`) or `hv-scratch` (`0x50000000`) — the exact windows
stage2.c's "hard boundary" comment (stage2.c:354-393) says are protected —
and the DMA engine will read/write there directly, no stage-2 fault, no
trace. `stage2.c`'s own comment over-promises here: it protects the CPU path
only.

## Inventory: what DMA-capable MMIO the guest's stage-2 map actually exposes

Cross-referencing every node in `bananapi-min.dts` against its `status` (a
node with no explicit `status` defaults to `"okay"`) and against whether any
*enabled* node actually references it (e.g. via a `dmas = <&phandle ...>`
property):

| Block (base, size) | DTB status | Real DMA master? | Guest genuinely uses it? |
|---|---|---|---|
| GIC (`0x1c81000`) | okay (implicit) | no (MMIO regs only) | **yes** — IRQ ack/EOI, cannot deny |
| CCU (`0x1c20000`) | okay (implicit) | no (clock/reset regs) | **yes** — HV itself pokes PC5 pinmux here too |
| MUSB OTG (`0x1c19000`) | okay | yes (has its own FIFO/DMA mode) | HV's OWN console (usbacm.c) owns it; guest DTB exposure is a known, separately-documented dual-ownership risk (see `usbacm.h`) |
| EHCI0/OHCI0 (`0x1c1a000`/`0x1c1a400`) | okay | **yes**, real USB host DMA | **yes** — the EHCI INTID-106 storm (see memory `ehci-intid106-storm`) proves the guest genuinely drives this |
| EHCI1/OHCI1 (`0x1c1b000`/`0x1c1b400`) | okay | **yes** | **yes**, same as above (2nd USB port) |
| MMC0 (SD, `0x1c0f000`) | okay | **yes** (IDMAC) | **yes** — real card slot |
| MMC1 (WiFi SDIO, `0x1c10000`) | okay | **yes** (IDMAC) | enabled, not confirmed actively used, but a legitimate driver could probe it |
| eMMC/MMC2 (`0x1c11000`) | okay | **yes** (IDMAC) | **yes** — this is the exact aw_mmc IDMAC-coherency device `el2_ncmap.c`/`docs/el2-nc-guest-dram.md` already treats specially |
| EMAC (`0x1c30000`) | okay | **yes** (TX/RX descriptor rings) | ambiguous — HV's OWN debug console (`emac.c`) polls this hardware directly; whether the guest's own DTB-declared EMAC driver also attaches is the same class of dual-ownership risk already flagged for MUSB (see `usbacm.h`) — NOT independently re-verified here |
| Crypto engine "CE" (`0x1c15000`) | **okay (no `status` property at all → defaults enabled)** | **yes** — Allwinner CE is descriptor/DMA-driven (AES/hash source+dest buffers) | **unconfirmed** — no evidence in this tree that the boot FreeBSD kernel carries a `sun8i-ce`-equivalent driver; FreeBSD's Allwinner arm64 support is comparatively thin. Cannot rule out. |
| System DMA controller "dma-controller@1c02000" | **okay (implicit — no `status` property at all)** | **yes** — general-purpose memory-to-memory DMA, the most dangerous block in the list | **no** — see below, all its clients are also disabled |
| SPDIF/I2S0-2/audio codec (`0x1c21000`-`0x1c22e00`) | disabled | only via the system DMA controller above (`dmas = <0x30 ...>`) | no |
| SPI0/SPI1 (`0x1c68000`/`0x1c69000`) | disabled | only via the system DMA controller above | no |
| Display Engine DE2 + mixer + rotate (`0x1000000`-`0x1400000`) | disabled | yes, large multimedia DMA | no |
| TCON-LCD, MIPI-DSI/D-PHY, CSI, deinterlace, HDMI (`0x1c0c000`, `0x1ca0000`-`0x1ca1fff`, `0x1cb0000`, `0x1e00000`, HDMI ctrl) | all disabled | yes | no |

("okay" rows above without a status column entry in the DTS were checked
directly — `grep -n 'status = "disabled"'` around each node's line range in
`bananapi-min.dts` came up empty.)

## Verifying the one clean case: the system DMA controller

**CORRECTED 2026-08-27** (`docs/guest-hw-enablement.md`'s hardware-enablement
survey caught this): `dma-controller@1c02000` (`compatible =
"allwinner,sun50i-a64-dma"`, `phandle = <0x30>`) has **no `status` property at
all** — `okay` by spec default, not `"disabled"` as this section originally
claimed. Re-checked directly against a scratch `dtc -I dtb -O dts` decompile
of the live `bananapi-min.dtb`: the node's block runs straight from
`compatible` to `phandle` with no `status` line. The "provably unused"
conclusion below still holds, just for the narrower reason that survives the
correction — every consumer, not the controller itself, is what is disabled.

Grepping the whole DTS for `dmas = <0x30` (the only way any node could reach
this controller) finds exactly seven consumers — SPDIF, I2S0, I2S1, I2S2, the
`codec-i2s` DAI (`dai@1c22c00`), SPI0, SPI1 — and **every single one of them
is independently `status = "disabled"`** too. That means: with this exact
DTB, there is no code path (not even an accidental one via a sibling driver)
through which the FreeBSD guest would ever issue a CPU access to
`0x1c02000-0x1c02fff`. This is the one block in the table above that is both
(a) a genuinely dangerous general-purpose DMA engine, enabled itself, and (b)
provably unused, not just "probably unused" — because every consumer that
could arm it is off. `gen_config.py`'s `FORBIDDEN_DTB_NODES`
(`docs/guest-hw-enablement.md`) now enforces the consumer half of this
structurally: `dai@1c22c00` — the only consumer with any live path to being
re-enabled through this project's own tooling — cannot be flipped back to
`"okay"` without a hard `validate()` error, precisely so this paragraph's
"provably unused" claim cannot be silently invalidated by a future
`board-config.xml` edit the way the controller's own `status` value already
was by whatever produced this file's original (wrong) claim.

This 4 KiB page happens to sit inside the SAME 2 MiB block
(`UART_L2_IDX == 14`, IPA `0x1c00000-0x1dfffff`) that `stage2.c` already
splits to page granularity for the UART0 trap — `stage2_l3_uart[]` already
exists and is already indexed by exactly this arithmetic
(`(addr & 0x1fffff) >> 12`). Denying the DMA controller's page is therefore,
mechanically, one more `if` arm in the same loop that already builds that
table — the smallest possible diff, reusing infrastructure already proven on
real hardware (the UART trap has been working since well before this pass).

### Draft patch (NOT applied — see rationale below)

```c
/* in stage2_build_mmio_tables()'s L3-building loop, alongside UART_L3_IDX: */
#define DMA_CTRL_BASE   0x01C02000UL   /* dma-controller@1c02000, DTB-enabled
                                         * (no status property, okay by
                                         * default) but zero enabled
                                         * consumers (see
                                         * docs/dma-bypass-stage2.md) */
#define DMA_CTRL_L3_IDX ((unsigned)((DMA_CTRL_BASE & (STAGE2_L2_BLOCK_SIZE - 1u)) \
                                     >> STAGE2_L3_PAGE_SHIFT))   /* == 2 */

	for (unsigned j = 0; j < STAGE2_L3_ENTRIES; j++) {
		if (j == UART_L3_IDX) {
			stage2_l3_uart[j] = 0;
			continue;
		}
#if STAGE2_DENY_DMA_CTRL          /* NEW, default undefined/0 -- see below */
		if (j == DMA_CTRL_L3_IDX) {
			stage2_l3_uart[j] = 0;   /* INVALID: unused general DMA engine */
			continue;
		}
#endif
		...
	}
```

**Why this is NOT wired in (`STAGE2_DENY_DMA_CTRL` stays undefined/0) this
pass**, even though the DTB analysis is about as clean as this kind of case
gets:

1. **Fault-handling gap.** `el2_exc.c`'s stage-2 data-abort dispatch
   (`vconsole_handle_fault` → `vblk_mmio_fault` → `vnet_mmio_fault` → generic
   fault record) has no handler for this new IPA. If the static analysis
   above is wrong in some way not visible from the DTS alone — e.g. FreeBSD's
   SoC-identification/erratum-workaround code touching a "disabled" block
   directly before any driver instantiation, which this tree cannot rule out
   without reading the actual kernel build that's loaded — the guest would
   take a **real, unhandled stage-2 abort** instead of the benign real-
   hardware read/write it gets today. That is a strictly worse failure mode
   than the status quo for a case I have not run.
2. **The absolute hard constraint for this pass is no hardware access.**
   Every other stage-2 table change in this codebase's history (UART0 trap,
   virtio-mmio carve-out, the vGIC GICC→GICV redirect attempt that was later
   *reverted* after a live regression — see stage2.c:279-286) was validated
   by booting the real guest through it. This one hasn't been, and per the
   task's own instruction ("Only implement a stage-2 denial if you can show
   it won't break the working guest" / "erring toward a solid design ...
   is better than a big risky change"), a DTB-only argument is a strong
   *design* case, not hardware proof.

**What DOES make this safe to merge once someone has board access**: flip
`STAGE2_DENY_DMA_CTRL` to 1, `make dbg`, reload once, confirm the guest still
reaches userland exactly as today (root mount, vtbd0p3, virtio-net — the
existing `reliable_load.py`/`chimpd.py` smoke-boot is sufficient), and the
change graduates from "designed" to "landed" — same staged-rollout pattern
this tree already uses for `DBG_NCMAP_ENABLE` in `main_dbg.c`.

## The rest of the "disabled, zero enabled consumers" group

DE2/mixer/rotate, TCON-LCD, MIPI-DSI/D-PHY, CSI, deinterlace, and HDMI are
*also* all `status = "disabled"` with no enabled consumer referencing them,
by the same DTB-grep method. Each is, in principle, exactly as safe to deny
as the DMA controller above, and each would need its own small L2/L3 split
(most of them sit in L2 blocks stage2.c doesn't currently split at all, so
each is a slightly bigger diff than the DMA controller case, which reuses an
existing split). Not enumerated as individual draft patches here to keep
this document's one concrete patch focused and reviewable; the same
methodology applies directly if/when someone wants to extend the denial
list.

## What's genuinely inherent (not fixable by stage-2 alone)

- **No SMMU on the A64.** This is a hardware fact, not a software gap in this
  tree — there is no register anywhere that lets EL2 gate a DMA engine's bus
  transactions by target address. The only mitigations available at the
  software layer are (a) deny the CPU path to a device's *control* registers
  (what the draft patch above does — the guest can't arm a transfer it can't
  configure) or (b) don't give the guest a device at all (DTB `disabled`,
  which is convention only and unenforceable against a compromised guest —
  exactly the reason stage-2 CPU-side denial has value even though it can't
  reach the DMA engine's actual bus-master path).
- **Engines the guest genuinely needs** (GIC, CCU, both EHCI/OHCI pairs, all
  three MMC/eMMC controllers) **cannot be stage-2-denied at all** without
  breaking the working guest — this is the hard boundary of what stage-2 can
  offer here, documented so nobody proposes closing these particular
  "holes" later without re-deriving why they can't be.
- **Ambiguous-ownership devices** (MUSB, and now — newly identified by this
  pass — EMAC and the crypto engine CE) are a THIRD category: not clearly
  needed, not clearly unneeded, because it depends on whether the specific
  FreeBSD kernel build loaded onto this board carries a driver for them.
  MUSB's version of this risk is already documented in `usbacm.h`. EMAC and
  CE are the same shape of question and are called out here for the first
  time; resolving them needs either reading the actual kernel config that
  produced the boot ELF, or a live `dmesg`/`sysctl dev` check on the booted
  guest — both out of scope for a hardware-access-free pass.

## What was actually changed as part of H2(b)

Nothing in `stage2.c`/`el2_exc.c`. This document is the deliverable: a
precise inventory (table above), one fully-designed and reviewed draft patch
for the single case that's both dangerous and provably safe to deny, gated
behind a compile-time flag so it can be flipped on and hardware-validated
without anyone having to re-derive the analysis, and an honest list of what
remains open (the ambiguous-ownership devices) or structurally unfixable
(no SMMU) at this layer.
