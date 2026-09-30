# bzdk 0.2.0 "pirozhok"

**A bare-metal EL2 hypervisor for the Banana Pi M64 (Allwinner A64,
4x Cortex-A53, 2 GiB DRAM), running FreeBSD 15.1 arm64 as its guest.**
Written from scratch: no KVM, no Xen, no Jailhouse.

Tag: `v0.2.0`. Default build: `make dbg` (then `make hv-uimage`; build with
`LC_ALL=C` if you grep the output). Previous: `v0.0.2-prealpha` (see
`RELEASE-0.0.2.md`); this file covers what changed since, and — same rule as
before — what is still broken.

"pirozhok" — a small pie, for a small board: the BPI-M64 is a Banana **Pi**.
This is the release where the hypervisor's roadmap **for this board** is
done; what remains is guest work, or a second SoC.

## What changed since 0.0.2

### Storage: from a PIO bottleneck to DMA with IRQ completion

| | 0.0.2 | 0.2.0 |
|---|---|---|
| SD (vtbd1, /var /opt) write | 0.34 MB/s | 13.5 MB/s |
| SD raw read (64 KiB) | 2.07 MB/s | 20.1 MB/s |
| eMMC (vtbd0, root) sequential read | 4.4 MB/s | 10.9 MB/s |
| CPU time lost by 4 CPU-bound jobs to concurrent eMMC / SD I/O | — | halved (~0.24 -> ~0.14 s / ~0.2 -> ~0.09 s per 3.8 s run) |

- SD: 4-bit bus, SD High Speed 50 MHz (fail-safe both ways), CMD18/CMD25
  multi-block, one command per whole virtio request.
- eMMC: CMD18/CMD25 multi-block with hardware AUTO_STOP; 8-bit and HS50
  validated but not default (no measurable gain — the card is the limit).
- Both controllers move data with the IDMAC (descriptor-chain DMA). Rule
  learned the hard way: any buffer the IDMAC writes must be whole cache
  lines, or a neighbour's write-back silently corrupts guest data.
- **Requests complete from the IDMAC interrupt.** An eligible request is
  queued (16 deep) and the vCPU returns to the guest at once; the IRQ
  publishes it and starts the next transfer. 99% of transfers finish
  inside the IRQ; the tick and the controller-lock spin are backstops.
  SD gained the used-ring lock and lost-completion re-notify that async
  completion needs. Live switches `g_vblk_idma_on` / `g_vblk_sd_idma_on`.
- eMMC wear (EXT_CSD life estimates, pre-EOL) in `bzdctl status` (BMC v1.2).

### vzram: a compressed-RAM disk

`vtbd2`, LZ4 in 128 MiB of DRAM the guest is no longer told about (guest
RAM 1920 -> 1792 MiB), starting at 128 MiB and growing live to 1 GiB while
the data proves compressible. Verified as the only swap with a working set
larger than free RAM, every page checked. **Not swapped on by default** —
that is a guest policy decision, deliberately left open (`docs/vzram.md`).

### Display: hardware planes for the guest

scanout grew from one guest window to a small compositor surface:
- **v2** — two overlay planes (the HDMI mixer's spare UI layers), XRGB or
  ARGB, global alpha. Verified on the panel.
- **v3** — the mixer's VI channel as a video plane: NV12/NV21/YUYV/XRGB,
  hardware-scaled, drawn over everything. Scaler coefficients and the
  BT.601/709 matrices are computed, not copied from GPL/vendor tables, and
  checked against the hardware's own format. Verified on the panel:
  NV12 colour bars 320x180 upscaled 3x, correct colours
  (`hdmi_video_selftest()`).

This is the hypervisor half of an HWC-style compositor; the other half
(which surface gets which plane) belongs in the guest's bzkms. VE
hardware decode needs no hypervisor work at all — a guest driver.

### The board is not fragile any more

- The PMIC's RSB bus had two unarbitrated masters (guest + EL2); the
  guest's RSB controller is now trap-emulated under one lock. This was the
  cause of the PHY rail being cut and the board going dark.
- EMAC-dark detection that no longer reboots a healthy but unpolled board,
  a three-strike reboot budget that survives the reset, and an SD-card
  black box written on the way into every reset.
- A U-Boot with a hardware watchdog armed is on the eMMC (chain-load
  tested first, never written blind); chimpd is retired, the board boots
  itself over TFTP.
- Guest DVFS (648-1152 MHz on DC power), the A53 PMU for all four vCPUs,
  header UARTs, R_PWM/R_I2C and Bluetooth handed to the guest; CPU3's
  missing periodic tick fixed.
- WiFi works in the guest (BCM43430 over SDIO) with guest-side FreeBSD
  fixes that live in `bzdOS/freebsd-brcmfmac-sdio`, not here.

### Tooling

- `reliable_load.py` wrote `1` into every 0x40 of 0x4201d000..0x42030000
  of the freshly booted hypervisor on every reload (a stale-symbol
  fallback). Fixed; anything odd after a reload before `7a1f302` is
  suspect.
- `repl`, `fbsd`, `gdb`, `hdmi` link again; `board-config.xml` ownership
  classes are cross-checked at build time.

## Soak

`soak72.py --hours 3 --profile mixed --gpu-cmd ''`, 2026-09-30 19:37-22:38,
on the hypervisor this tag builds, guest CPU held at 816 MHz: **gate closed,
3.00 h of healthy load, 155 of 155 samples healthy, 0 warnings, 0 resets
(watchdog 0, wedge 0), 0 reloads, 0 guest recoveries, 0 fsck repairs.**
The load was SD write-verify, CPU bursts, a read-only eMMC read-verify and
a TFTP pull, on all four vCPUs at once (load average ~3.7), with every
transfer going through the IDMAC completion queues; rate 334 -> 337
generations/h early to late (no degradation), maximum temperature 66.2 C.
The GPU worker was off (see below).

## Known broken or open

- **Not the v1 72-hour soak.** See above for what was run.
- **GPU workload (guest side): `limabench` crashes and can hang.** A
  SIGSEGV in Mesa's CPU-side shader compiler, always at the same site
  (`set_search_or_add` <- `nir_lower_io`), roughly every 12-15 minutes of
  back-to-back runs; separately it can spin at ~95% CPU waiting on the GPU,
  pinned to one CPU or with Mesa's threading off alike, which looks like a
  wedged Mali that only a guest reboot clears. Reproduced at 816 MHz, so the
  old "CPU unstable at 1152 MHz near 85 C" suspicion is ruled out; it also
  predates this release's storage work. Mali is guest-owned; unexplained.
  The soak above therefore ran without its GPU worker. One `ntpd` SIGSEGV
  seen earlier is also unexplained.
- vzram is not swapped on at boot; FreeBSD has no swap priorities (vzram
  and eMMC swap would interleave) and a grown vzram disk is only used
  after `swapoff`/`swapon`.
- WiFi modules are not loaded at boot and join via
  `compat.linuxkpi.80211.connect_*` sysctls; `ifconfig wlan0 scan` is not
  supported by this port.
- The SD IDMAC line still fires for synchronous transfers on other cores;
  it is masked at once and costs nothing, but it is not zero.
- `dual` (Zephyr on CPU3) does not build with the default 2 GiB guest, and
  snapshot/restore is not linked in it — both need the high GiB.
- U-Boot's MUSB gadget can still wedge after a catch; root cause needs a
  ~5 h catch soak. The board no longer depends on it to boot.
- scanout's `screenshot.py` composes only the HUD and the guest window;
  overlay and video planes are not in its picture.

## Where to read next

`HANDOFF.md` (top section) for the state and the numbers behind this
file; `docs/vzram.md`; `docs/zero-copy-scanout.md` §9; `SESSION-RULES.md`
before touching the board.
