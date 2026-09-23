# A64 Video Engine (Cedrus/VE) under FreeBSD: feasibility assessment

Read-only research, done off the board. No hardware touched. Scope: whether
to bring up `video-codec@1c0e000` (the Allwinner "Video Engine" / Cedrus VPU)
for the FreeBSD 15.1 guest.

## 1. FreeBSD Cedrus/VE support: none found

Checked `/opt/bzdos/freebsd-src-earlyboot-wt`:

- `grep -ril cedrus sys/` — zero hits, anywhere in the tree.
- `sys/arm/allwinner/` (full listing) has no `ve`, `cedrus`, or video-decode
  file. `files.allwinner`/`std.allwinner` list no such device; the only
  A64-adjacent media entry is `dev/clk/allwinner/ccu_de2.c`, the unrelated
  display-engine clock.
- `sys/dev/video/` has exactly one file, `crtc_if.m` — a DRM/KMS CRTC
  method interface, not V4L2.
- `sys/compat/linuxkpi/common/include/media/` has only `cec.h` and
  `cec-notifier.h` (HDMI CEC). No `v4l2-core`, `videobuf2`, `media-device`,
  or `media-entity` anywhere under `sys/compat/linuxkpi`.
- `sys/compat/linuxkpi/common/include/video/` and `dummy/include/video/`
  hold only display-adjacent stubs (`vga.h`, `mipi_display.h`, `cmdline.h`,
  empty `videomode.h`/`of_videomode.h`). `linuxkpi_videokmod.c` is a 7-line
  placeholder module (`MODULE_VERSION`/`MODULE_DEPEND` only, no code).
- `sys/arm64/conf/ALLWINNER` / `std.allwinner` enable no such device.

The DT node itself is real: `sys/contrib/device-tree/src/arm64/allwinner/
sun50i-a64.dtsi:528-530` has `video-codec@1c0e000 { compatible =
"allwinner,sun50i-a64-video-engine"; ... }` (mirrored verbatim from
upstream Linux). The deployed `/opt/bzdos/tftpboot/bananapi-min.dtb`
carries it too (`dtc -I dtb -O dts`): `reg 0x1c0e000/0x1000`, `clocks` =
ahb/mod/ram, `resets`, `interrupts`, an `allwinner,sram` phandle.
`gen_config.py` passes it through untouched. Silicon and DT description
exist; nothing in FreeBSD binds to them.

**Answer: no, FreeBSD has zero Cedrus/VE support today**, driver or framework.

## 2. What Linux does

Driver: `drivers/staging/media/sunxi/cedrus/` — staging not for quality but
because maintainers wanted to keep changing the controls ABI post-merge
([LWN](https://lwn.net/Articles/757773/),
[bootlin](https://bootlin.com/blog/wrapping-up-the-allwinner-vpu-crowdfunded-linux-driver-work/)).

- 16 files: `cedrus.c/.h`, `cedrus_dec.c/.h`, `cedrus_hw.c/.h`,
  `cedrus_video.c/.h`, `cedrus_regs.h`, one file per codec (`cedrus_mpeg2.c`,
  `cedrus_h264.c`, `cedrus_h265.c`, `cedrus_vp8.c`), Kconfig/Makefile/TODO.
  Sizes via GitHub API: source totals ~175 KB (`cedrus_h265.c` 32 KB,
  `cedrus_vp8.c` 31 KB, `cedrus_regs.h` 29 KB, `cedrus_h264.c` 22 KB,
  `cedrus.c` 17 KB, `cedrus_video.c` 15 KB, rest smaller) — roughly
  5,000-6,000 lines, same order of magnitude as this project's own `lima`
  GPU port (~6,500 lines, see §4).
- Hard dependency: the **V4L2 stateless decoder API + media controller +
  request API** ([kernel doc](https://lwn.net/Articles/786774/),
  [patch series](https://lore.kernel.org/all/1520590736.15946.1.camel@bootlin.com/T/)).
  It is not a self-contained decoder; per-frame decode parameters are
  attached to buffers as V4L2 controls via `MEDIA_REQUEST` file descriptors,
  driven by userspace (`ffmpeg`/`libva-v4l2-request`/GStreamer). Codecs:
  MPEG2, H.264, H.265/HEVC, VP8.
- DT binding matches what's already in this project's DTB: `compatible`,
  `reg`, `interrupts`, `clocks` named `ahb`/`mod`/`ram`, `clock-names`,
  `resets`, `allwinner,sram` for the shared SRAM block also used by the ARISC.
- Memory constraint: **the VPU can only DMA the first 256 MiB of DRAM**
  from the DRAM base — a hardware addressing limit, not a driver choice.

## 3. What FreeBSD lacks

Everything the driver sits on top of:

- No V4L2 core (`v4l2-core`), no `videobuf2` (contiguous-DMA buffer queue
  manager), no media-controller framework, no request API. None of these
  exist natively, and none exist as LinuxKPI shims — confirmed by the empty
  `sys/compat/linuxkpi/common/include/media` search above.
- FreeBSD's only V4L2-flavored userland story is **`webcamd`**: a
  **userspace** CUSE (`cuse(3)`) program linking `libusb`, porting Linux's
  *USB* UVC-class webcam drivers to expose a `/dev/video` node (confirmed by
  web search — no kernel V4L2 subsystem, no platform/MMIO support,
  USB-only). Cedrus is a memory-mapped platform device behind SoC
  clocks/resets and an interrupt; `webcamd`'s model does not apply to it.
- By contrast, DRM/KMS core **does** exist for this project, via the ported
  `drm-kmod` project (`/opt/bzdos/drm-kmod`, `/opt/bzdos/drm-kmod-src`) —
  that pre-existing core is what let this project's own `lima` GPU port
  succeed (see §4). No equivalent "v4l2-kmod" project exists anywhere;
  checked by web search, nothing beyond `webcamd` turned up.
- Generic FreeBSD primitives that *would* be reusable (`contigmalloc`,
  `bus_dmamem_alloc`, `busdma`, in `sys/vm/vm_contig.c`,
  `sys/kern/kern_malloc.c`) are generic memory allocators, not a
  V4L2/videobuf2-shaped buffer-queue-and-request abstraction — raw
  material for a new framework, not a substitute for one.

## 4. Realistic paths, honestly assessed

**a. Native FreeBSD driver.** Means inventing a FreeBSD-native decode ABI
from scratch — no existing ioctl surface to target, and no consuming
userland (`ffmpeg`, `gstreamer`, browsers) speaks anything but V4L2 for
stateless decode. A native ABI means also patching every consumer. This is
a new subsystem plus new consumer-side glue, not "a driver."

**b. LinuxKPI port**, following this project's own `lima` precedent
(`/opt/bzdos/bsdOS/hal/lima/`, ported from Linux's `lima` DRM driver onto
`drm-kmod`: ~6,520 lines across `lima_*.c`, one `drm_gem_shmem_helper.c`
fork, 9 patches across drm-kmod/freebsd-src/freebsd-ports, roughly five
weeks of hardware-in-the-loop work per the project's own 2026-07 to
2026-08-20 milestones). The enabling condition was that DRM core already
existed as a maintained upstream port (`drm-kmod`) — `lima` only had to
port the driver, not the subsystem beneath it. Cedrus has no equivalent
"v4l2-kmod." This path means porting V4L2 core + `videobuf2` +
media-controller + request API to LinuxKPI *first*, from a blank sheet —
larger and less precedented than DRM core — and only then porting the
~6k-line Cedrus driver on top. None of that subsystem work exists today,
upstream or in this project.

**c. Userland-only via some other route.** Does not apply here the way it
does for USB devices (`webcamd`/CUSE). The VE is memory-mapped, needs
clock/reset sequencing, an interrupt, and a bounded 256 MiB DMA window —
none of that is exposable safely through `/dev/mem` + `libusb`-style
userspace. Doing it anyway would still need a minimal kernel stub owning
clocks/resets/IRQ/DMA-window and handing the ring buffer to userspace over
a bespoke ioctl — most of a kernel driver anyway, and every consumer would
still need a bespoke (not V4L2) client library.

## 5. Effort estimate and recommendation

Effort: framework-scale kernel work — a full V4L2-core/media-controller/
request-API subsystem port — must land *before* any Cedrus-specific driver
work starts. That subsystem alone is larger than DRM core, which took years
of upstream `drm-kmod` maintenance and which this project only had to
*consume*, not build. The Cedrus driver itself (~6k lines) is comparably
sized to this project's own `lima` port, but `lima` succeeded by standing on
an existing DRM core; Cedrus has no such foundation. Estimate: multiple
months of subsystem work with zero hardware feedback until it's functional,
*before* board-specific driver work (comparable to `lima`'s own ~5 weeks)
even begins.

Strictly harder than the CSI camera case already rejected in
`docs/csi-camera-assessment.md` (same missing-framework blocker, but CSI's
driver is a smaller bridge module lacking Cedrus's request-API needs).

**Recommendation: not worth attempting**, at least not ahead of a generic
V4L2-core LinuxKPI port that would also unlock CSI capture — nobody
upstream has built that port either, so there is no external work to adopt.
This board has no camera sensor attached (`docs/csi-camera-assessment.md`
§3), so decode is the only plausible consumer of such a framework here. A
multi-month subsystem port for one codec engine does not compare favorably
to peripherals already working (GPU, DRM/KMS, HDMI, USB, SD/eMMC, WiFi).
Revisit only if a concrete decode need emerges with no software-decode
fallback acceptable.
