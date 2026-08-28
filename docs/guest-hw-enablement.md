# Handing more A64 hardware to the guest: what's dark, what's landed, what isn't

2026-08-27 survey of the six device classes `ORIENTATION.md`'s "Background" section
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
| **USB host, port 1** | `usb@1c1b000` (EHCI1) + `usb@1c1b400` (OHCI1), PHY1 ("pmu1", independent lane) | Flip `status` on 2 already-correct nodes | **Proven, built in.** `sys/dev/usb/controller/generic_ehci_fdt.c` binds `"generic-ehci"`, `generic_ohci.c` binds `"generic-ohci"`, `sys/arm/allwinner/aw_usbphy.c` binds `"allwinner,sun50i-a64-usb-phy"` — all in `files.arm64`. Empirically confirmed on THIS board today: `awusbphy0` already attaches on the live guest, so this driver family is compiled into the running kernel right now, not just present in source. | Low — physically independent PHY from the debug channel; and the EHCI/INTID-106 storm this SoC is known for was a symptom of an incomplete `IMO=1` vGIC (List Registers never populated for device IRQs), not of USB itself. The **current** default (`main_dbg.c`, "interrupt-virtualization milestone") is complete `HCR_EL2.IMO=1/FMO=1` HW-mode forwarding — `vgic_inject_hw()` ties every physical INTID to a GICH List Register so the guest's own EOI deactivates the physical source, with an LR-exhaustion pending queue backing up GIC-400's 4 hardware LRs so a burst is queued, not dropped (`vgic.c`). This is hardware-verified for the CNTV timer and the virtio-mmio SPIs (105-108) and for cross-core SGIs (vcpu1/vcpu2), but **not yet for a real external device SPI under sustained storm conditions** — EHCI/OHCI have been disabled the whole time this architecture has existed, so the specific "will the new forwarding survive the same 145 kHz pathological case that broke the old one" question is architecturally answered (yes, by design) but not board-measured. **EHCI1 is `usb@1c1b000`, SPI 74 = INTID 106 — the EXACT INTID that stormed before.** The first hardware measurement on this feature is therefore GIC SPI-74/INTID-106's rate (`dbgmon`'s `GICD_ISPENDR`/`GICC_HPPIR`, or `preempt_cnt`'s climb rate), not just whether `uhub` shows up in the guest's own `dmesg` — a quiet-looking guest console does not by itself rule out a storm EL2 is silently absorbing. | **Land it, try it first.** Highest confidence of the six, matches the user's own instinct — but check the SPI-74 rate specifically, not just guest `dmesg`. |
| **USB host, port 0** | `usb@1c1a000` (EHCI0) + `usb@1c1a400` (OHCI0), PHY0 ("pmu0") | N/A — deliberately no lever | same drivers as port 1 | **High — do not enable.** PHY0 is the SAME silicon `usb@1c19000` (MUSB OTG) uses, and MUSB is this hypervisor's OWN CDC-ACM debug/break-glass gadget (`usbacm.c`, bit-banged live from CPU1) — this is exactly usbacm.h's pre-existing "KNOWN OPEN RISK". Enabling this pair hands the guest's own PHY driver write access to the same PHY0 registers the recovery channel depends on, untested, with no stage-2 trap protecting it. | **No feature offered.** `gen_config.py`'s `FORBIDDEN_DTB_NODES` makes listing either path a hard `validate()` error, not just a comment — same treatment as `watchdog@1c20ca0`. |
| **WiFi** (AP6212/BCM43430, SDIO) | `mmc@1c10000` (SMHC1) + `wifi@1` child (`brcm,bcm4329-fmac`, no independent status) | Flip `status` on 1 node | **Source exists, does nothing as shipped.** `sys/contrib/dev/broadcom/brcm80211/brcmfmac/*` is a real LinuxKPI port with `sdio.c`/`bcmsdh.c`/`of.c`, but `sys/modules/brcm80211/brcmfmac/Makefile` sets **`BRCMFMAC_SDIO=0` and `BRCMFMAC_OF=0`** — neither the SDIO bus glue nor the FDT/OF binding is actually compiled into the module as shipped. Firmware is absent from the FreeBSD tree entirely; the exact board-matched blob (`brcmfmac43430-sdio.sinovoip,bananapi-m64.txt`/`.bin`) exists only in this HOST's own Linux firmware tree (`/usr/lib/firmware/brcm/`) — copying it into the guest would need its own licensing check, not evaluated here. | Low, DTB-wise — SMHC1 is its own controller instance, not shared with anything the HV touches | **Landed, flagged insufficient on its own.** The DTB flip is necessary but nowhere near sufficient — this needs `BRCMFMAC_SDIO=1 BRCMFMAC_OF=1`, a guest kernel/module rebuild (on the guest itself, its own build host — see `ORIENTATION.md`), and a firmware file placed where `brcmfmac` looks for it. **Hardest of the six, confirming the user's own instinct** — the DTB machinery alone will not produce a `wlan0`. |
| **Bluetooth** (BCM43438, UART) | none — child of `serial@1c28400` (uart1), already `status = "okay"` today | **None at all.** There is no `status` to flip; the `bluetooth {}` node is metadata under an already-enabled UART. | **Structurally absent.** `sys/netgraph/bluetooth/drivers/` has only `ubt` and `ubtbcmfw`, both USB HCI transports. There is no `ng_h4` (or any other) UART/H4 HCI transport anywhere in this tree — not a missing config option, a missing transport. | None — uart1 is already exposed, stable, unrelated to any HV-owned peripheral | **Nothing to land.** No DTB lever exists (nothing to flip) and none would help — a `status` flip cannot supply a transport driver that plain doesn't exist. This is a "needs a driver someone would have to write" finding, not a "needs a board cycle" one. |
| **Audio codec** | `codec@1c22e00` (digital) + `codec-analog@1f015c0` (analog); **`dai@1c22c00` (codec-i2s DAI) deliberately excluded** | Flip `status` on 2 of the 3 nodes the full path needs | **Real, SoC-specific, and already compiled in.** `sys/arm/allwinner/a33_codec.c` matches the digital codec's *second* compat string (`"allwinner,sun8i-a33-codec"`); `sys/arm/allwinner/a64/sun50i_a64_acodec.c` matches the analog codec's primary string exactly; `sys/arm/allwinner/aw_i2s.c` matches `"allwinner,sun50i-a64-codec-i2s"` (the internal DAI ONLY — the three general-purpose `i2s0-2` blocks have no driver at all, consistent with them staying disabled and out of scope here). All pulled in via `std.allwinner` + `device sound`; empirically confirmed live today — `pcm0: <simple-audio-card>` already attaches on the guest, proving `device sound` is compiled into the exact kernel on the board right now. So this is a **DTB-node problem, not a driver problem** — a real reassessment upward from a first-pass "probably no driver" guess. | The DAI is the ONLY enabled-or-disabled node in the whole audio chain with a `dmas` property — it is `dma-controller@1c02000`'s one path back to being armed. That controller is the SoC's general-purpose memory-to-memory DMA engine, has no SMMU in front of it, and `docs/dma-bypass-stage2.md`'s inventory calls it out as the single genuinely dangerous block that is "provably unused" today **specifically because every possible consumer is disabled** (that doc had the controller's OWN status wrong — corrected in this pass, see below — but the "every consumer disabled" argument, which is what actually matters, was and remains correct). Re-arming it with zero stage-2 mitigation (a draft denial patch exists in that doc, not wired in or board-validated) is not a risk this survey will spend without a deliberate decision. | **Partial land, and the good half is bigger than expected.** `codec@1c22e00`/`codec-analog@1f015c0` get a flag and WILL attach for real (not just register-probe theater — these are the actual production drivers for this exact silicon); the DAI does not, so there is no I2S data path and therefore no *sound* regardless — intentional, not a bug in the config, and worth a real board cycle specifically to confirm the attach (see the one-line check below), even without audio output yet. |
| **MIPI-DSI** | `dsi@1ca0000` + `d-phy@1ca1000` | Flip `status` on 2 nodes | **Absent.** No DSI/D-PHY/panel driver anywhere under `sys/arm/allwinner` or `sys/dev/drm` in this tree. | None — neither node carries a `dmas` property or shares a PHY/register block with anything the HV depends on | Land the flag anyway (cheap, safe, zero conflict) so the DTB half is ready; expect it to do nothing until a driver exists — do not spend a board cycle expecting a device to appear. |
| **MIPI-CSI** | `csi@1cb0000` | Flip `status` on 1 node | **Absent.** No camera/v4l2-equivalent framework for this SoC anywhere in this tree. | None, same reasoning as DSI | Land the flag anyway, same reasoning as DSI. |
| **IR receiver** | `ir@1f02000` | Flip `status` on 1 node | **MEASURED 2026-08-27: driver ABSENT from the booting kernel image.** The DTB side is fully proven: node enables cleanly (irq 68, pinctrl processed, both compat strings present), but `simplebus` prints `(no driver attached)` and `strings /boot/kernel/kernel` contains ZERO `sun6i-a31-ir`/`sun50i-a64-ir` hits — the `aw_cir` compat table never made it into this build (the earlier "built in" claim above was source-survey inference, now retracted by measurement). `sys/arm/allwinner/aw_cir.c` exists in the tree (`optional evdev aw_cir fdt` in `files.arm64`), so this needs a guest kernel rebuild with `device evdev` + `device aw_cir`, same class as WiFi. Note the compat table only lists `"allwinner,sun6i-a31-ir"`, NOT the A64-specific string first on this node — it would bind via the fallback-list walk | None — isolated block, no `dmas` property, no shared PHY | Kernel rebuild, then the flag is live. DTB side needs nothing further. |

## Cost, risk, and an ordered recommendation

1. **USB host, port 1** (`guest_usb_host1`). Cheapest node work, most mature
   driver (already partially attached today), lowest conflict risk of
   anything that does something real. The one open question — the new
   IMO=1 vGIC forwarding under a genuine device-IRQ storm — is answered by
   a single, specific measurement (GIC SPI-74/INTID-106 rate), not a
   guess. Try this first, alone.
2. **Audio codec** (`guest_audio_codec`). Moved up from an initial
   "probably no driver, low value" assumption once the source survey found
   real, already-compiled SoC-specific drivers — cheaper to verify than
   WiFi and with no PHY-sharing risk, only the (structurally enforced) DMA
   consideration. Worth a board cycle specifically to confirm the digital
   and analog codec attach for real, even with no sound yet.
3. **IR receiver** (`guest_ir`). Cheapest single-node flip with a real,
   already-compiled driver and zero conflict surface — low ceiling (an IR
   receiver is a minor feature) but essentially free to confirm.
4. **WiFi** (`guest_wifi_sdio`). Confirmed hardest, matching the original
   instinct behind this survey: the DTB flip is necessary but the driver
   module ships with both `BRCMFMAC_SDIO` and `BRCMFMAC_OF` off, and
   firmware is absent from FreeBSD entirely. This is a guest-kernel-rebuild
   project (on the guest, its own build host) plus a firmware-sourcing
   decision, not a board-cycle-sized task — but it remains the most
   valuable single item on this list for the PinePhone direction `soc_a64.h`
   already calls out, so it is landed (flag ready) rather than dropped.
5. **MIPI-DSI / MIPI-CSI**. Landed as flags anyway — cheap, zero conflict,
   and the DTB half should not be the reason a driver-day-one bring-up is
   slower — but confirmed no FreeBSD driver exists for either; do not
   expect a board cycle here to show anything.
6. **Bluetooth**. Not landed as a feature at all — there is no DTB lever to
   flip (`uart1` is already fully enabled) and no UART/H4 HCI transport
   anywhere in FreeBSD's Bluetooth stack to receive it. This is the one
   item that is a "someone has to write a driver" finding, categorically
   different from the other five.

This mostly matches, and where it differs it's evidence-driven: USB stays
the clear first move, and WiFi stays hardest despite being the most
valuable long-term target — both agree with the instinct that opened this
survey. Audio's ranking moved up from "lowest value" specifically because
the driver-reality assumption behind that ranking turned out to be wrong;
it is now a legitimate second try, just still capped short of real sound by
a structural decision (the DMA controller) rather than a driver gap.

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
`ORIENTATION.md`), joined now by `dma-controller@1c02000` and `dai@1c22c00`
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
| `guest_usb_host1` | `guest_sh.py 'dmesg \| grep -E "usbus\|uhub"'` shows a new `usbus`/`uhub` attach, and `usbconfig list` (or plugging a drive) shows a device on port 1 | **Two checks, both mandatory.** (1) Debug channel: confirm `/dev/ttyACM0` / `chimpd`'s U-Boot-gadget catch window still behaves normally on the NEXT reload cycle — this feature does not touch PHY0, so it should be a no-op on the channel; any change there means the port-0/port-1 PHY independence assumed above was wrong. (2) **INTID-106 storm check, specific to this device**: sample `dbgmon`'s `GICD_ISPENDR`/`GICC_HPPIR` or `preempt_cnt`'s climb rate right after boot — EHCI1 IS the SPI-74/INTID-106 device this SoC's storm history is about, and a quiet guest console does not rule out EL2 silently absorbing a storm via the new HW-mode forwarding path |
| `guest_wifi_sdio` | `dmesg \| grep -i bcm` shows SOME probe attempt from `mmc1`/`wifi@1` (even one that then fails on the missing SDIO/OF glue or firmware) — that is still useful signal, distinct from "nothing probed at all" | none expected — SMHC1 is not shared with anything HV-owned. Do not expect `wlan0` — the driver survey found `BRCMFMAC_SDIO=0`/`BRCMFMAC_OF=0` in the shipped module build, so a clean probe failure (not a `wlan0`) is the CORRECT and expected outcome of this flag alone |
| `guest_audio_codec` | `dmesg \| grep -iE "a33codec\|a64codec\|acodec"` shows the digital AND analog codec actually attaching (real drivers, confirmed present and built in — not a register-probe guess) | none expected. Do not expect `/dev/sndstat` to show a channel — the DAI (`dai@1c22c00`) is deliberately not enabled, so there is no I2S data path; a silent-but-attached codec is the correct outcome, not a failure |
| `guest_mipi_dsi` / `guest_mipi_csi` | `dmesg` after boot — expect NO new device (confirmed no driver exists in this FreeBSD tree for either); confirms the flag is inert as predicted rather than silently wrong | none expected — no `dmas`, no shared PHY |
| `guest_ir` | `dmesg \| grep -i aw_cir` shows the driver attaching (it binds via the node's SECOND compat string, `"allwinner,sun6i-a31-ir"`, not the A64-specific one listed first — worth confirming that fallback match actually fires), then `/dev/input/eventN` appears | none expected |

## What this survey could not determine from this host

- **Whether `dma-controller@1c02000`'s draft stage-2 denial patch
  (`docs/dma-bypass-stage2.md`, `STAGE2_DENY_DMA_CTRL`) is safe to land
  ahead of ever wanting real audio** — that is a separate, deliberate
  decision (whether to spend engineering effort closing a hole that is
  today provably unused, before there is any actual consumer pressuring it
  open again), not a DTB question, and this survey does not make it.
- **Whether `brcmfmac`'s exact runtime firmware-loading path in FreeBSD's
  `firmware(9)`/LinuxKPI shim would even accept a copied-over Linux
  firmware blob** — no literal firmware path reference was found in
  `sys/contrib/dev/broadcom/brcm80211/brcmfmac/firmware.c`'s search list
  during the source survey; this needs either reading that code more
  closely or a real attempt on the guest (its own build host), not
  something this DTB-side survey established.
- **Which exact `KERNCONF` produced the kernel currently booting on the
  board.** ANSWERED 2026-08-27 by `config -x /boot/kernel/kernel` on the
  guest: `ident GENERIC`, `options CONFIG_AUTOGENERATED`,
  `makeoptions MODULES_EXTRA="dtb/allwinner"` — i.e. plain GENERIC from the
  `freebsd-src-earlyboot-wt` lineage (kernel version string
  `134a4b503a7b-dirty`). The audio/USB driver-presence claims therefore rest
  on that config plus the observed live attaches.
- **MMC1 (WiFi SDIO)'s IDMAC cache-coherency behavior under this project's
  `el2_ncmap.c` remap.** The remap covers all guest DRAM by construction
  (so it should apply here too), but only `mmc0`/`mmc2` have an actual
  hardware track record proving the remap handles their IDMAC descriptor
  writes correctly — `mmc1` would be new, unmeasured territory for that
  interaction specifically, on top of everything else WiFi already needs.

## Two corrections this survey made to existing docs, not just new findings

- **`docs/dma-bypass-stage2.md`** stated `dma-controller@1c02000` itself is
  `status = "disabled"`. Re-checked directly against a scratch decompile of
  the live DTB: it has **no `status` property at all** (`okay` by spec
  default). The doc's "provably unused" conclusion still holds, corrected
  to rest on the actual reason — every one of its seven potential
  consumers, not the controller, is what's disabled — and that doc has
  been updated in this same pass (including the draft patch's own
  comment, which had the same error).
- **This project's own initial framing of Audio as low-value** (matching
  the task's own working assumption) undersold it: the digital and analog
  codec drivers are real, SoC-specific, and already compiled into the
  running kernel — this is a DTB-node problem, not a driver problem, same
  shape as USB and cheaper than WiFi. The genuine limiter is the
  DMA-controller hazard on the DAI, not driver absence, so it moves up in
  practical value while staying capped below "real sound" until that
  separate decision gets made.
