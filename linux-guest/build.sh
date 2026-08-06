#!/usr/bin/env bash
# build.sh — build the Linux/arm64 guest artifacts (Image + DTB) this
# target's QEMU gate (../linux-qemu-ci.sh) loads.
#
# Mirrors zephyr-guest/build.sh's shape and the reason it exists: turn "fetch
# a large out-of-tree checkout, configure, build" into one command so the CI
# script can produce its own guest artifacts instead of depending on a build
# directory someone happened to leave lying around.
#
#   ./build.sh                # == ./build.sh linux-6.12
#   ./build.sh linux-6.12     # version tag, informational only — this script
#                             # does not itself fetch anything (see below);
#                             # it exists so a future second version and this
#                             # one can be told apart in output/logs.
#
# Environment:
#   LINUX_SRC    A Linux source checkout/extracted tarball (6.12.x tested).
#                Default /opt/bzdos/linux-work/linux-6.12. NOT vendored into
#                this repo: even a shallow single-version tarball extracts
#                to ~1.6 GB, same category of "too big to commit" as
#                zephyr-guest's ZEPHYR_BASE (565 MB).
#   BUILD_DIR    Kernel build objdir (make O=). Default
#                /opt/bzdos/linux-work/build, deliberately OUTSIDE this repo
#                (a defconfig arm64 Image-only build's objdir is several
#                hundred MB of .o files).
#   CROSS_COMPILE  Default aarch64-linux-gnu- (this host's only aarch64
#                cross toolchain — see TOOLCHAIN.md).
#
# What this script does NOT do, on purpose:
#   - Fetch the kernel source. Getting linux-6.12.tar.xz onto this host was a
#     ~4 s direct download from cdn.kernel.org (bypassing this host's
#     http_proxy, which throttles it to ~200 KB/s and times out — see
#     linux-guest/README in docs/linux-guest.md for the measured numbers);
#     that one-time step is cheap enough and disk-sensitive enough (this host
#     regularly runs <35 GB free) that it is a documented manual command, not
#     something this script retries silently on every CI run.
#   - Build modules, an initramfs, or anything beyond `Image`: this pass's
#     milestone is "the guest prints its own console output", which needs
#     the kernel image and nothing else. `make Image` (not `all`/`modules`)
#     is used specifically so a stray CONFIG_FOO=m in defconfig does not
#     silently drag in a multi-minute allmodconfig-sized build.
#
# Prints two "KEY=value" lines on stdout as its last output — IMAGE=<path>
# and DTB=<path> — so a caller can do:
#   eval "$(./build.sh | tail -2)"; echo "$IMAGE $DTB"
set -eu

TAG=${1:-linux-6.12}

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)

: "${LINUX_SRC:=/opt/bzdos/linux-work/linux-6.12}"
: "${BUILD_DIR:=/opt/bzdos/linux-work/build}"
: "${CROSS_COMPILE:=aarch64-linux-gnu-}"

if [ ! -f "$LINUX_SRC/Makefile" ] || [ ! -d "$LINUX_SRC/arch/arm64" ]; then
    echo "build.sh: no Linux/arm64 tree at LINUX_SRC=$LINUX_SRC ($TAG)" >&2
    echo "build.sh: fetch one, e.g.:" >&2
    echo "  mkdir -p /opt/bzdos/linux-work && cd /opt/bzdos/linux-work" >&2
    echo "  env -u http_proxy -u https_proxy curl -sS -o ${TAG}.tar.xz \\" >&2
    echo "      https://cdn.kernel.org/pub/linux/kernel/v6.x/${TAG}.tar.xz" >&2
    echo "  tar xf ${TAG}.tar.xz" >&2
    echo "build.sh: -u http_proxy -u https_proxy matters on this host — the" >&2
    echo "  default proxy measured ~200 KB/s + timeouts on cdn.kernel.org;" >&2
    echo "  bypassing it measured ~2 MB/s with no timeout (2026-08 numbers)." >&2
    exit 2
fi

echo "build.sh: LINUX_SRC=$LINUX_SRC BUILD_DIR=$BUILD_DIR" >&2

mkdir -p "$BUILD_DIR"

if [ ! -f "$BUILD_DIR/.config" ]; then
    make -C "$LINUX_SRC" O="$BUILD_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        defconfig >&2

    # Trim: disable every CONFIG_ARCH_* vendor SoC platform select (this
    # synthetic DTB names none of their compatible strings, so building
    # their clk/pinctrl/gpio/soc driver trees is pure disk/CPU waste on a
    # disk-constrained host) plus subsystems this milestone's DTB gives the
    # kernel no way to use. See docs/linux-guest.md "Why the config is
    # trimmed" for the full rationale — this list must match that doc.
    DISABLE_CONFIGS="
        ARCH_ACTIONS ARCH_AIROHA ARCH_ALPINE ARCH_APPLE ARCH_BCM2835
        ARCH_BCMBCA ARCH_BCM_IPROC ARCH_BERLIN ARCH_BITMAIN ARCH_BRCMSTB
        ARCH_EXYNOS ARCH_HISI ARCH_INTEL_SOCFPGA ARCH_K3 ARCH_KEEMBAY
        ARCH_LAYERSCAPE ARCH_LG1K ARCH_MA35 ARCH_MEDIATEK ARCH_MESON
        ARCH_MVEBU ARCH_MXC ARCH_NPCM ARCH_PENSANDO ARCH_QCOM ARCH_REALTEK
        ARCH_RENESAS ARCH_ROCKCHIP ARCH_S32 ARCH_SEATTLE ARCH_SPARX5
        ARCH_SPRD ARCH_STM32 ARCH_SUNXI ARCH_SYNQUACER ARCH_TEGRA
        ARCH_TESLA_FSD ARCH_THUNDER ARCH_THUNDER2 ARCH_UNIPHIER
        ARCH_VEXPRESS ARCH_VISCONTI ARCH_XGENE ARCH_ZYNQMP
        NET ACPI PCI SCSI ATA WLAN INPUT FB SOUND USB_SUPPORT MMC
        VIRTIO_MENU CRYPTO_HW
    "
    DISABLE_ARGS=""
    for c in $DISABLE_CONFIGS; do
        DISABLE_ARGS="$DISABLE_ARGS --disable $c"
    done
    # shellcheck disable=SC2086
    "$LINUX_SRC/scripts/config" --file "$BUILD_DIR/.config" $DISABLE_ARGS >&2

    make -C "$LINUX_SRC" O="$BUILD_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
        olddefconfig >&2
fi

# Deliberately capped, not a bare nproc: this host has run near-zero free
# disk/memory from wide-parallelism full kernel builds before (see the
# ROADMAP D2 session brief) — even a trimmed Image-only build stays modest
# here rather than assuming every core is free to spend.
NPROC=$(command -v nproc >/dev/null 2>&1 && nproc || echo 2)
[ "$NPROC" -gt 4 ] && NPROC=4
make -C "$LINUX_SRC" O="$BUILD_DIR" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$NPROC" Image >&2

IMAGE="$BUILD_DIR/arch/arm64/boot/Image"
if [ ! -f "$IMAGE" ]; then
    echo "build.sh: FAIL — $IMAGE missing after 'make Image'" >&2
    exit 1
fi

DTB="$BUILD_DIR/bpi_m64_hv-linux.dtb"
command -v dtc >/dev/null 2>&1 || { echo "build.sh: FAIL — dtc not installed" >&2; exit 1; }
dtc -I dts -O dtb -o "$DTB" "$HERE/bpi_m64_hv-linux.dts" >&2

echo "build.sh: IMAGE=$IMAGE ($(stat -c%s "$IMAGE") bytes)" >&2
echo "build.sh: DTB=$DTB ($(stat -c%s "$DTB") bytes)" >&2

echo "IMAGE=$IMAGE"
echo "DTB=$DTB"
