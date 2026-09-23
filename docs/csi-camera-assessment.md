# A64 CSI (csi@1cb0000) under FreeBSD: feasibility assessment

Read-only research, done off the board. No hardware touched. Scope: whether
to bring up `csi@1cb0000` (currently `status = "disabled"`, owner=guest in
`board-config.xml`) for the FreeBSD 15.1 guest.

## 1. FreeBSD camera/CSI support: none found

Checked `/opt/bzdos/freebsd-src-earlyboot-wt`:

- `sys/arm/allwinner/a64/` has only `a64_padconf.c`, `a64_r_padconf.c`,
  `sun50i_a64_acodec.c`. No CSI driver.
- `grep -ril csi sys/arm/allwinner sys/dev` turns up nothing CSI-related —
  only unrelated hits (`aac_cam.c`/`aacraid_cam.c` = SCSI CAM layer,
  `acpi_asus.c`/`acpi_sony.c` = ACPI "camera button" hotkeys,
  `input-event-codes.h` = `KEY_CAMERA` define). No Allwinner camera driver
  exists under any name (`sun6i`, `sun4i-csi`, etc. — all zero hits).
- `sys/dev/video/` has exactly one file, `crtc_if.m` — a KMS/DRM CRTC method
  interface, unrelated to cameras. `sys/dev/usb/video/` has `udl.c`/`udl.h`
  — the DisplayLink USB *display* adapter, not a UVC webcam driver;
  `sys/dev/usb` has no `v4l2`/`videobuf` hits either.
- `sys/compat/linuxkpi`: no `v4l2`, `videobuf`, `media_device`, or
  `media_entity` hits anywhere. No `media_device`/`media_entity` symbols
  anywhere else in `sys/` except unrelated string collisions (SATA "media"
  in `sys/dev/pms/...sat.c`, an EDK2 UEFI header).

Conclusion: FreeBSD has **zero** V4L2-equivalent, media-controller, or
videobuf2-equivalent framework, platform or USB. Matches the prior project
survey `docs/guest-hw-enablement.md` ("No camera/v4l2-equivalent framework
for this SoC anywhere in this tree"), independently reconfirmed here.

## 2. What Linux does for A64 CSI

Driver: `drivers/media/platform/sunxi/sun6i-csi/` (`sun6i_csi.c`/`.h`,
bridge + capture modules), upstream in mainline Linux.
- https://github.com/torvalds/linux/tree/master/drivers/media/platform/sunxi/sun6i-csi
- Binds compat strings `allwinner,sun6i-a31-csi`, `sun8i-a83t-csi`,
  `sun8i-h3-csi`, `sun8i-v3s-csi`, **`sun50i-a64-csi`**.
- Kconfig (`VIDEO_SUN6I_CSI`) depends on `VIDEO_DEV`, `ARCH_SUNXI`, `PM`,
  `COMMON_CLK`, `RESET_CONTROLLER`, `HAS_DMA`, and selects
  `MEDIA_CONTROLLER`, `VIDEO_V4L2_SUBDEV_API`, `VIDEOBUF2_DMA_CONTIG`,
  `V4L2_FWNODE`, `REGMAP_MMIO`. I.e. it is a thin bridge driver sitting on
  top of the full V4L2/media-controller/videobuf2 stack — none of which
  FreeBSD has.
- DT binding: `Documentation/devicetree/bindings/media/sun6i-csi.txt` —
  https://www.kernel.org/doc/Documentation/devicetree/bindings/media/sun6i-csi.txt
  Required: `reg`, `interrupts`, `clocks` (`bus`,`mod`,`ram`), `clock-names`,
  `resets`, and a `port`/`endpoint` child pointing at the sensor's own
  endpoint (`bus-width`, `hsync-active`, `vsync-active`, optional
  `pclk-sample`).
- **A64 CSI is a parallel/BT.656 interface, single channel, time-multiplexed
  — not MIPI CSI-2.** Confirmed from the DT pinctrl group in
  `sys/contrib/device-tree/.../sun50i-a64.dtsi:733-741` (mirrored verbatim
  from upstream Linux): `csi_pins` claims 11 GPIOs (`PE0,PE2-PE11`) for the
  parallel data/sync bus, plus `csi_mclk_pin` (`PE1`) for the sensor clock.
  No D-PHY/MIPI lane involved — project docs calling this "MIPI-CSI"
  (`board-config.xml:177`, `docs/ownership-audit.md:23`) are imprecise; the
  A64's actual DSI/D-PHY node (`dsi@1ca0000`/`d-phy@1ca1000`) is
  display-only and unrelated.
- The bridge driver alone captures nothing — it needs a sensor subdevice
  driver behind it (e.g. `ov5640.c`) wired via the `port/endpoint` and
  probed over I2C, plus that sensor's own power/mclk/reset sequencing.

## 3. Hardware reality on this board

`sys/contrib/device-tree/.../sun50i-a64-bananapi-m64.dts`:
- `&reg_aldo1` is named `regulator-name = "afvcc-csi"`, 2.8V,
  `regulator-always-on` (lines ~209-217) — a power rail to a CSI connector
  exists in the board design.
- `&i2c1 { status = "okay"; }` (lines 125-127) has no child device — the
  sensor control bus is wired but nothing is attached to it.
- `grep -n 'csi\|camera'` on this file matches **only** that regulator name
  — no `&csi` override, no `port`/`endpoint`, no sensor node (`ov5640` or
  otherwise). Upstream describes no populated sensor for this board; the
  `csi` node itself stays at its base `status = "disabled"`.

Matches the given fact that no camera module is attached: the connector
and power rail exist in board layout, but neither upstream Linux nor this
project's DTB describes an actual sensor. Nothing to probe, no
bus-width/timing to derive, no way to verify a driver without a module.

## 4. Minimal path, if attempted

Not a config flip. Would require, in order: (1) a FreeBSD
V4L2/media-controller/videobuf2-equivalent kernel framework — does not
exist today; either port linuxkpi shims for `v4l2-core`, `v4l2-subdev`,
`v4l2-fwnode`, `videobuf2-dma-contig`, `media-controller`, or write a
native framework from scratch — a new subsystem, not a driver; (2) a
ported or native bridge driver for the A64 CSI register block and its
BT.656 timing, based on `sun6i_csi.c`; (3) a sensor driver (e.g. `ov5640`)
plus I2C probe, GPIO reset/powerdown, and mclk sequencing — unspecified
anywhere in this board's own DT today, so authored, not adapted; (4)
`port`/`endpoint` DT nodes linking `csi@1cb0000` to the sensor, added to
the guest DTB by `gen_config.py` — absent even upstream; (5) a physical
camera module on the connector to bring any of this up against, since
nothing here can be verified without one.

Step 1 alone is a bigger lift than everything else in
`docs/guest-hw-enablement.md` combined (that survey's hardest finding, WiFi,
only needed enabling already-written LinuxKPI glue and a firmware blob —
the SDIO/V4L2 core code already existed). Here the core framework is
missing entirely.

## 5. Effort estimate and recommendation

Effort: framework-scale, multi-month kernel work with no prior art in this
tree, before any board-specific driver work even starts. Comparable Linux
V4L2 support for this SoC took years of upstream engineering, reusing
decades of existing V4L2/media infrastructure that FreeBSD lacks outright.
No sensor is attached to this board's CSI connector — even with unlimited
driver-writing effort, there is nothing to capture from and no way to
validate correctness on hardware.

**Recommendation: not worth attempting.** Keep `csi@1cb0000` at
`status = "disabled"`, or land the same zero-cost, zero-risk `status="okay"`
flag `guest_mipi_csi` already offers in `board-config.xml` (consistent with
the DSI precedent — flipping the DTB status costs nothing and nothing binds
to it either way, since no driver exists to attach). Do not invest
engineering time in a bridge or sensor driver absent (a) a FreeBSD
V4L2-equivalent framework upstream, and (b) a physical sensor module on
this board to develop and test against. Revisit only if both change.
