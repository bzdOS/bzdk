# Handing more A64 hardware to the guest: what's dark, what's landed, what isn't

2026-08-27 survey of the six device classes `CLAUDE.md`'s "Background" section
lists as dark in the FreeBSD guest, done entirely off the board: a scratch
`dtc -I dtb -O dts` decompile of the LIVE `/opt/bzdos/tftpboot/bananapi-min.dtb`
(copied to a scratch path, never edited in place — see `SESSION-RULES.md`/this
project's hard rule against writing to `tftpboot`), diffed against the upstream
`sun50i-a64.dtsi` / `sun50i-a64-bananapi-m64.dts` under
`/opt/bzdos/build/u-boot/arch/arm/dts/`, plus a dedicated FreeBSD `releng/15.1`
source survey for driver reality. Board time was not spent on any of this.

## The one finding that changes the whole shape of this survey

**Every node below already exists in the live DTB, fully wired, copied
verbatim from the upstream board `.dts`** — correct `clocks`/`resets`/`phys`/
`pinctrl-0`/regulator-supply references, the same ones the same silicon uses
successfully today for `mmc0`/`mmc2`/`uart0`/`uart1`. The upstream vendor tree
(`sun50i-a64-bananapi-m64.dts`) itself turns ON `ehci0`, `ehci1`, `ohci0`,
`ohci1`, `usb_otg`, `usbphy`, `mmc1` (WiFi), `codec`, `codec_analog`, `dai`,
and `sound` for this exact board (`grep -n '^&' sun50i-a64-bananapi-m64.dts`) —
i.e. the board vendor considers all of that real, populated hardware. It has
NO override at all for `dsi`/`csi`/`ir`, meaning upstream doesn't even
consider those wired on this specific board (no connector/antenna route),
independent of any FreeBSD driver question.

So for every device below except DSI/CSI/IR, **"what DTB work is needed" has
one, uniform, already-verified answer: flip one `status` property from
`"disabled"` to `"okay"` on already-complete, already-correct node(s).**
Nothing needs a clock reference added, a regulator wired, or a pinctrl group
created — that was the expected hard part going in, and it turned out to
already be done. The real questions turned out to be entirely about (a)
whether FreeBSD's driver exists for the far side of that flip, and (b)
whether flipping it can hurt the hypervisor's own hardware.

Structural check used throughout: `fdtget -l <dtb> /` reports 21 nodes and
`fdtget -l <dtb> /soc` reports 62 both before and after every edit tested
here (matches `docs/guest-dtb.md`'s own convention) — nothing added, nothing
removed, only `status` values change.

## Per-device table

| Device | DTB node(s) | Node work needed | FreeBSD 15.1 driver | Conflict risk | Verdict |
|---|---|---|---|---|---|
| **USB host, port 1** | `usb@1c1b000` (EHCI1) + `usb@1c1b400` (OHCI1), PHY1 ("pmu1", independent lane) | Flip `status` on 2 already-correct nodes | `generic-ehci`/`generic-ohci` (`compatible` includes the generic fallback) + `aw_usbphy`/awusbphy0, which the guest ALREADY attaches today with nothing hooked to it | Low — physically independent PHY from the debug channel; and the EHCI/INTID-106 storm this SoC is known for was a symptom of an incomplete `IMO=1` vGIC (List Registers never populated for device IRQs), not of USB itself. The **current** default (`main_dbg.c`, "interrupt-virtualization milestone") is complete `HCR_EL2.IMO=1/FMO=1` HW-mode forwarding — `vgic_inject_hw()` ties every physical INTID to a GICH List Register so the guest's own EOI deactivates the physical source, with an LR-exhaustion pending queue backing up GIC-400's 4 hardware LRs so a burst is queued, not dropped (`vgic.c`). This is hardware-verified for the CNTV timer and the virtio-mmio SPIs (105-108) and for cross-core SGIs (vcpu1/vcpu2), but **not yet for a real external device SPI under sustained storm conditions** — EHCI/OHCI have been disabled the whole time this architecture has existed, so the specific "will the new forwarding survive the same 145 kHz pathological case that broke the old one" question is architecturally answered (yes, by design) but not board-measured. | **Land it, try it first.** Highest confidence of the six, matches the user's own instinct. |
| **USB host, port 0** | `usb@1c1a000` (EHCI0) + `usb@1c1a400` (OHCI0), PHY0 ("pmu0") | N/A — deliberately no lever | same drivers as port 1 | **High — do not enable.** PHY0 is the SAME silicon `usb@1c19000` (MUSB OTG) uses, and MUSB is this hypervisor's OWN CDC-ACM debug/break-glass gadget (`usbacm.c`, bit-banged live from CPU1) — this is exactly usbacm.h's pre-existing "KNOWN OPEN RISK". Enabling this pair hands the guest's own PHY driver write access to the same PHY0 registers the recovery channel depends on, untested, with no stage-2 trap protecting it. | **No feature offered.** `gen_config.py`'s `FORBIDDEN_DTB_NODES` makes listing either path a hard `validate()` error, not just a comment — same treatment as `watchdog@1c20ca0`. |
| **WiFi** (AP6212/BCM43430, SDIO) | `mmc@1c10000` (SMHC1) + `wifi@1` child (`brcm,bcm4329-fmac`, no independent status) | Flip `status` on 1 node | *(see FreeBSD driver survey below)* | Low, DTB-wise — SMHC1 is its own controller instance, not shared with anything the HV touches (that's exactly why `soc_a64.h`, which lists only addresses EL2 C code itself touches, has no SMHC1 entry — correct, not a gap) | *(pending driver verdict)* |
| **Bluetooth** (BCM43438, UART) | none — child of `serial@1c28400` (uart1), already `status = "okay"` today | **None at all.** There is no `status` to flip; the `bluetooth {}` node is metadata under an already-enabled UART. | *(see FreeBSD driver survey below)* | None — uart1 is already exposed, stable, unrelated to any HV-owned peripheral | *(pending driver verdict; no machinery needed regardless — see below)* |
| **Audio codec** | `codec@1c22e00` (digital) + `codec-analog@1f015c0` (analog); **`dai@1c22c00` (codec-i2s DAI) deliberately excluded** | Flip `status` on 2 of the 3 nodes the full path needs | *(see FreeBSD driver survey below)* | The DAI is the ONLY enabled-or-disabled node in the whole audio chain with a `dmas` property — it is `dma-controller@1c02000`'s one path back to being armed. That controller is the SoC's general-purpose memory-to-memory DMA engine, has no SMMU in front of it, and `docs/dma-bypass-stage2.md`'s own inventory calls it out as the single genuinely dangerous block that is "provably unused" today **specifically because every possible consumer is disabled**. Re-arming it with zero stage-2 mitigation (a draft denial patch exists in that doc, not wired in) is not a risk this survey will spend without a deliberate decision. | **Partial land.** `codec@1c22e00`/`codec-analog@1f015c0` get a flag (drivers can ATTACH, register-level only) so the DTB half is honest and testable; the DAI does not, so there is no I2S data path and therefore no sound regardless of driver support — this is intentional, not a bug in the config. |
| **MIPI-DSI** | `dsi@1ca0000` + `d-phy@1ca1000` | Flip `status` on 2 nodes | *(see FreeBSD driver survey below — expected absent)* | None — neither node carries a `dmas` property or shares a PHY/register block with anything the HV depends on | Land the flag anyway (cheap, safe, zero conflict) so the DTB half is ready; expect it to do nothing until a driver exists |
| **MIPI-CSI** | `csi@1cb0000` | Flip `status` on 1 node | *(see FreeBSD driver survey below — expected absent)* | None, same reasoning as DSI | Land the flag anyway, same reasoning as DSI |
| **IR receiver** | `ir@1f02000` | Flip `status` on 1 node | *(see FreeBSD driver survey below — expected absent)* | None | Land the flag anyway |

## What was landed

`gen_config.py` gained a third artifact class alongside `cpu@N` nodes and
`virtio_mmio@...` nodes: a `<soc-nodes>` section in `board-config.xml` that
maps a `dtb-only="true"` feature (no `mkvar`, nothing written to
`config.mk` — there is no EL2 C code gated by any of these) to one or more
real silicon node paths, and `ensure_node_status()` flips each one's
`status` between `"okay"`/`"disabled"` with a plain `fdtput`, both
directions, self-healing exactly like `reconcile_drift()` already does for
`cpu@N` nodes. `FORBIDDEN_DTB_NODES` is a structural allow-list violation
check (hard `validate()` error, not a comment) for the two nodes no feature
may ever re-enable: `usb@1c19000`/`usb@1c1a000`/`usb@1c1a400` (MUSB debug
channel + its shared PHY0) and `watchdog@1c20ca0` (already protected, see
`CLAUDE.md`), joined now by `dma-controller@1c02000` and `dai@1c22c00`
(its one live-consumer path).

Six `dtb-only` features, all default `enabled="false"`:

| Feature | Turns on |
|---|---|
| `guest_usb_host1` | EHCI1 + OHCI1 (USB-A port on PHY1) |
| `guest_wifi_sdio` | SMHC1 (mmc1, the AP6212 WiFi/BT module's SDIO side) |
| `guest_audio_codec` | digital + analog codec registers (no DMA/sound path) |
| `guest_mipi_dsi` | DSI controller + D-PHY |
| `guest_mipi_csi` | CSI controller |
| `guest_ir` | IR receiver |

Turn one on: edit `board-config.xml`, flip the feature's `enabled` to
`"true"`, run `python3 gen_config.py --dtb <path>` (use `--dry-run` first;
never point it at `/opt/bzdos/tftpboot/bananapi-min.dtb` without a human
about to test on the board, per this project's hard rule). Turn it back off
the same way — the script self-heals the DTB back to `disabled` on the next
run regardless of which direction changed.

Validated entirely off the board, against a scratch copy of the live DTB:
- `--dry-run` against the real `board-config.xml` reports every one of the
  12 managed `soc-node` paths as "already disabled" (matches the live DTB
  exactly — proof the paths are correct, not guessed).
- Enabling `guest_usb_host1` + `guest_audio_codec` together flips exactly
  the 4 nodes expected (`usb@1c1b000`, `usb@1c1b400`, `codec@1c22e00`,
  `codec-analog@1f015c0`) and touches nothing else; `fdtget` confirms
  `usb@1c1a000` and `dai@1c22c00` stay `disabled` throughout. Disabling
  again flips all 4 back. `fdtget -l /` node count: 21 before and after.
  `dtc -I dts -O dtb` round-trips the current live DTB byte-length-stable
  (43060 bytes both directions) — the "`dtc` can't rebuild this blob"
  caveat in `docs/guest-dtb.md` (2026-08-11) is stale; `gen_config.py`'s
  own `reorder_virtio_nodes()` already relies on exactly this round-trip in
  production, hardware-verified during the 2026-08-27 vcpu2 bring-up.
- Listing a `FORBIDDEN_DTB_NODES` path in a test copy of `board-config.xml`
  makes `gen_config.py` exit 1 with a clear error before touching anything.

## The one-line hardware check per device, once a feature is flipped on and the board is rebooted

| Feature | It worked | It broke something else |
|---|---|---|
| `guest_usb_host1` | `guest_sh.py 'dmesg \| grep -E "usbus\|uhub"'` shows a new `usbus`/`uhub` attach, and `usbconfig list` (or plugging a drive) shows a device on port 1 | **Debug channel check, mandatory**: confirm `/dev/ttyACM0` / `chimpd`'s U-Boot-gadget catch window still behaves normally on the NEXT reload cycle. This feature does not touch PHY0, so it should be a no-op on the channel — any change there means the port-0/port-1 PHY independence assumed above was wrong and must be re-examined before trying anything else |
| `guest_wifi_sdio` | `dmesg \| grep -i bcm` or `sysctl -a \| grep -i wlan`; a `wlan0`-capable device probing at all (even failing firmware load) is progress — see the driver-reality table for what "attach" even means here | none expected — SMHC1 is not shared with anything HV-owned |
| `guest_audio_codec` | `dmesg \| grep -iE "codec\|sun8i-a33"` shows the digital/analog codec attaching (register probe only) | none expected |
| `guest_mipi_dsi` / `guest_mipi_csi` / `guest_ir` | `dmesg` after boot — expect NO new device (no known driver); confirms the flag is inert as predicted rather than silently wrong | none expected — no `dmas`, no shared PHY |

## What this survey could not determine from this host

FreeBSD driver reality for WiFi/Bluetooth/audio-codec/DSI/CSI/IR needed a
dedicated source-tree search (`/opt/bzdos/freebsd-src-earlyboot-wt`,
`/opt/bzdos/freebsd-src`) rather than the DTB work above — see the driver
survey findings folded into the per-device table. Not independently
verified here: whether `dma-controller@1c02000`'s draft stage-2 denial
patch (`docs/dma-bypass-stage2.md`) is safe to land ahead of ever wanting
real audio — that is a separate, deliberate decision, not a DTB question,
and this survey does not make it.
