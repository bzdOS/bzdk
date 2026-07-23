# Toolchain

This project is freestanding AArch64 (no libc, no OS ABI) and is built with a
plain cross-`gcc` + GNU `binutils`, invoked via the `Makefile`'s
`CROSS ?= aarch64-linux-gnu-` prefix (`$(CROSS)gcc`, `$(CROSS)objcopy`,
`$(CROSS)size`).

## Verified-working versions

Confirmed working in the environment this v1 milestone (FreeBSD guest booting
to an interactive root shell) was built and tested in:

- **Host OS:** Fedora Linux 43
- **`aarch64-linux-gnu-gcc`:** 15.2.1 20250808 (Red Hat Cross 15.2.1-1)
  — Fedora package `gcc-aarch64-linux-gnu-15.2.1-1.fc43`
- **`aarch64-linux-gnu-ld` / `objcopy` (binutils):** 2.45-1.fc43
  — Fedora package `binutils-aarch64-linux-gnu-2.45-1.fc43`

Install on Fedora with:

```sh
dnf install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

No other compiler/binutils version has been tested against this tree. The
`Makefile` does not pin or version-check the toolchain (`CROSS` just defaults
to the `aarch64-linux-gnu-` prefix on `$PATH`) — if you hit a build error on a
different distro/version combination, check here first before assuming the
source itself regressed.

## Build flags (for reference — see `Makefile` for the authoritative list)

`-ffreestanding -nostdlib -mgeneral-regs-only -march=armv8-a
-fno-stack-protector -fno-pic -fno-pie -g -O2`, linked `-nostdlib -static
-no-pie --build-id=none` against `link.ld`, then `objcopy -O binary` to
produce the raw, position-fixed image U-Boot's `loady`/`bootelf` loads.

## Host-only test binaries

Two test programs (`test_stage2_tables.c`, `test_vblk_ring.c`) build with the
**plain host `gcc`** (not `$(CROSS)`/`$(CC)`) — they run entirely on the dev
host to unit-test pure parsing/table-building logic without a board. Any
recent `gcc` on the host (e.g. the Fedora 43 system `gcc`) is sufficient for
those; they are not part of the AArch64 cross-build.
