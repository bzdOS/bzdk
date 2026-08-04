#!/usr/bin/env bash
# build.sh — build the Zephyr guest image for one of the two bzdOS boards.
#
# Turns the procedure LOADING.md §1b describes in prose into one command, so
# the QEMU gate (../zephyr-qemu-ci.sh) can produce its own guest image instead
# of depending on a build directory someone happened to leave lying around.
#
#   ./build.sh                     # == ./build.sh bpi_m64_hv
#   ./build.sh bpi_m64_hv          # the image to TFTP to real hardware, and
#                                  # also exactly what the QEMU gate runs
#
# Environment:
#   ZEPHYR_BASE   Zephyr checkout to build against (v4.4.1 tested). If unset,
#                 falls back to /opt/bzdos/zephyr-work/zephyr, this host's
#                 existing tree. NOT vendored into this repo: it is 565 MB.
#   BUILD_DIR     where to build (default: $ZEPHYR_WORK/build-<board>, i.e.
#                 alongside the Zephyr tree, deliberately OUTSIDE this repo —
#                 a Zephyr build directory is ~15 MB of generated files).
#
# Prints the absolute path of the resulting zephyr.elf on stdout as its last
# line, so a caller can do  ELF=$(./build.sh qemu_virt_hv | tail -1).
#
# -mno-outline-atomics is required with a *Linux*-hosted aarch64-linux-gnu-gcc
# (the only cross compiler on this host; no bare-metal -none-elf or Zephyr SDK
# toolchain is installed): without it libgcc pulls in an atomics
# runtime-detection thunk that calls __getauxval, which does not exist in a
# freestanding image and fails the final link. See LOADING.md §1b.
set -eu

BOARD=${1:-bpi_m64_hv}

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)

: "${ZEPHYR_BASE:=/opt/bzdos/zephyr-work/zephyr}"
if [ ! -f "$ZEPHYR_BASE/VERSION" ]; then
    echo "build.sh: no Zephyr tree at ZEPHYR_BASE=$ZEPHYR_BASE" >&2
    echo "build.sh: set ZEPHYR_BASE to a Zephyr v4.4.x checkout" >&2
    exit 2
fi
export ZEPHYR_BASE

: "${BUILD_DIR:=$(dirname "$ZEPHYR_BASE")/build-$BOARD}"

export ZEPHYR_TOOLCHAIN_VARIANT=${ZEPHYR_TOOLCHAIN_VARIANT:-cross-compile}
export CROSS_COMPILE=${CROSS_COMPILE:-/usr/bin/aarch64-linux-gnu-}

GEN=Unix\ Makefiles
command -v ninja >/dev/null 2>&1 && GEN=Ninja

echo "build.sh: board=$BOARD zephyr=$ZEPHYR_BASE build=$BUILD_DIR" >&2

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -G"$GEN" \
        -DBOARD="$BOARD" \
        -DBOARD_ROOT="$HERE" \
        -DEXTRA_CFLAGS=-mno-outline-atomics \
        -DEXTRA_CXXFLAGS=-mno-outline-atomics \
        -S "$HERE/app" -B "$BUILD_DIR" >&2
fi
cmake --build "$BUILD_DIR" >&2

echo "$BUILD_DIR/zephyr/zephyr.elf"
