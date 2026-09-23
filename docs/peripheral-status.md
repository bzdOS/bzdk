# Peripheral status

State of the A64 peripherals as seen by the FreeBSD guest. Updated 2026-09-23.
Each "not done" entry says why, so the next person does not re-derive it.

## Working

| Device | Notes |
| --- | --- |
| SD / eMMC | via virtio-blk from the hypervisor |
| USB host | EHCI + OHCI on PHY1, hub enumerates |
| Networking | virtio-net |
| WiFi | BCM43430 SDIO, WPA2-PSK, real traffic. See the brcmfmac work |
| GPU / DRM / KMS | Mali-400 via the lima port |
| HDMI | driven by the hypervisor, guest gets a framebuffer |
| Audio | analog codec, `pcm0` play/rec. Needed the DAI un-forbidden — see below |
| SPI | `aw_spi`, PIO only |
| I2C | four buses |
| PWM | `pwm0`/`pwmbus0`/`pwmc0`. Driver is a module, `kldload aw_pwm` |
| UART, GPIO, IR, thermal, RTC, crypto | stock drivers, nothing special |

## Not done: no FreeBSD driver exists

**Bluetooth.** The AP6212's BT side is on uart1 and the DT binding
(`brcm,bcm43438-bt`) is already present upstream — but FreeBSD has no UART HCI
transport at all. `ng_h4` was removed in 79a100e28e3c (2021-11-10), having been
broken since the MPSAFE TTY rewrite; only `ubt`/`ubtbcmfw` remain, both USB.
Writing one is roughly 1500-2500 lines plus a Broadcom patchram firmware
loader. This is the only remaining item with value beyond this board: it would
serve every SBC with UART-attached Bluetooth, the Raspberry Pi family included.
See `bluetooth-uart-assessment.md`.

**LRADC.** No driver anywhere in FreeBSD (only DT bindings docs). Drives
resistor-ladder buttons. Not worth writing.

## Not done: needs a subsystem FreeBSD does not have

**Camera (`csi@1cb0000`)** and **video decode (`video-codec@1c0e000`, Cedrus)**
both require V4L2, videobuf2 and the media controller. FreeBSD has none of
them, natively or under LinuxKPI. Each driver is small; the framework beneath
it is not. There is also no camera sensor attached to this board. See
`csi-camera-assessment.md` and `cedrus-assessment.md`.

**DSI display** is disabled. HDMI already works through the hypervisor, so
there is no need pulling on it.

### These three are one project, not three

Camera, video decode and DSI all sit on the same side of the same missing
work: the guest does not own a display/media pipeline. The hypervisor drives
HDMI and hands over a framebuffer, which is why display works at all today.
Giving the guest the display engine, DSI and a V4L2 stack would be one
coordinated effort against the same silicon block and the same absent
frameworks — and is a far larger undertaking than the sum of the individual
drivers suggests. Do not start any one of them in isolation.

## Not given to the guest, by design

The hypervisor owns these and handing them over would break something
specific, not merely be untidy:

| Device | Why |
| --- | --- |
| `ethernet@1c30000` | EMAC is the hypervisor's debug and BMC channel |
| `usb@1c19000` | MUSB is the USB-ACM break-glass console |
| `usb@1c1a000/400` | share PHY0 with that console |
| `watchdog@1c20ca0` | the board's only unattended recovery path |
| `hdmi@*`, `lcd-controller@*` | hypervisor drives the display |
| `mmc@1c0f000`, `mmc@1c11000` | passed to the guest as virtio-blk instead |
| `rsb@1f03400` | hypervisor drives the AXP803 PMIC through it |

## Enabled but with a cost worth knowing

`dai@1c22c00` is the audio DAI, and it is the only node in the audio chain
with a `dmas` property. Enabling it arms the SoC's general-purpose DMA engine,
which no SMMU gates — a guest that can program it can reach hypervisor memory
regardless of stage-2. Audio does not work without it, so it is enabled behind
an explicit `dev_relax_dma_isolation` feature in `board-config.xml`.

**While that feature is on, this board's stage-2 isolation result does not
hold, and an isolation test run in this configuration measures nothing.** Do
not ship a build with it enabled.

## Could be enabled, simply not needed yet

`serial@1c28800/1c28c00/1c29000` (three spare UARTs), `i2s@1c22400/1c22800`
and `spdif@1c21000` (the analog path already carries audio), `pwm@1f03800`
(R_PWM), `i2c@1f02400` (R_I2C, in the PMIC's domain — check against the
hypervisor's RSB use first).
