# Peripheral status

State of the A64 peripherals as seen by the FreeBSD guest. Updated 2026-09-26.
Each "not done" entry says why, so the next person does not re-derive it.

## Working

| Device | Notes |
| --- | --- |
| SD / eMMC | via virtio-blk from the hypervisor |
| USB host | EHCI + OHCI on PHY1, hub enumerates |
| Networking | virtio-net |
| WiFi | BCM43430 SDIO, WPA2-PSK, real traffic. See the brcmfmac work |
| Bluetooth | AP6212 (BCM43438A1) on uart1: new `bcmbt(4)` + `ng_h4(4)` + `bcmbtattach`, up at boot, inquiry/name/l2ping to a laptop. See `bsdOS/hal/bluetooth/README.md` (2026-09-26) |
| GPU / DRM / KMS | Mali-400 via the lima port |
| HDMI | driven by the hypervisor, guest gets a framebuffer |
| Audio | analog codec, `pcm0` play/rec. Needed the DAI un-forbidden — see below |
| SPI | `aw_spi`, PIO only |
| I2C | four buses |
| PWM | `pwm0`/`pwmbus0`/`pwmc0`. Driver is a module, `kldload aw_pwm` |
| CPU DVFS | cpufreq_dt 648-1152 MHz + powerd; **needs the DC supply** (micro-USB browns out at 1152x4). aw_thermal throttles at 85 C. Feature `guest_dvfs` (2026-09-26) |
| PMU | `pmu0` on SPIs 116-119, all four vCPUs; hwpmc counting and overflow sampling work. Feature `guest_pmu` (2026-09-26) |
| Header UARTs | uart2 (PB0/PB1), uart3 (PD0/PD1), uart4 (PD2/PD3) as `/dev/cuau2..4`; pinctrl + pull-ups from gen_config (`guest_header_uarts`); internal-loopback 8/8 each (2026-09-26) |
| R_PWM, R_I2C | `pwmc1.0` (PL10): 1 ms / 25 % programmed and read back from EL2; `iic3` (PL8/PL9) scans clean, nothing attached on this board (2026-09-26) |
| LEDs | gpio-leds: `/dev/led/bananapi-m64:{red:pwr,green:user,blue:user}` (PD24, PE14, PE15); state verified in the PIO data register from EL2 (2026-09-26) |
| UART, GPIO, IR, thermal, RTC | stock drivers, nothing special |

## Not done: no FreeBSD driver exists

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
| `rsb@1f03400` | shared, not given: the guest runs aw_rsb + axp8xx_pmu, but its controller is emulated by `rsbtrap.c` (one bus lock for guest and EL2; writes that would cut the CPU/DRAM/PHY/HDMI rails or power the PMIC off are refused). Since 2026-09-26 |

## Enabled but with a cost worth knowing

`dai@1c22c00` is the audio DAI, and it is the only node in the audio chain
with a `dmas` property. Enabling it arms the SoC's general-purpose DMA engine,
which no SMMU gates — a guest that can program it can reach hypervisor memory
regardless of stage-2. Audio does not work without it, so it is enabled behind
an explicit `dev_relax_dma_isolation` feature in `board-config.xml`.

**While that feature is on, this board's stage-2 isolation result does not
hold, and an isolation test run in this configuration measures nothing.** Do
not ship a build with it enabled.

## Checked 2026-09-26 and left off, with the reason

| Device | Why |
| --- | --- |
| `crypto@1c15000` (CE) | no FreeBSD driver; the A64 CE has a PRNG but no TRNG, and `armv8crypto` (CPU AES/SHA instructions) already outruns it for bulk crypto |
| `i2s@1c22000/400/800` | no pin groups in the DT and nothing on this board to talk to (the analog codec has its own DAI, which is on) |
| `spdif@1c21000` | no A64 SPDIF driver in FreeBSD; PH8 goes nowhere useful on this board |
| `lradc@1c21800` | no FreeBSD driver, and the Banana Pi M64 wires no buttons to it (power key is the PMIC's) |

## Could be enabled, simply not needed yet

`i2s@1c22400/1c22800`
and `spdif@1c21000` (the analog path already carries audio).
