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

No other compiler/binutils version has been tested against this tree.

## Checking your toolchain

```sh
make toolchain-check
```

Reports the versions actually found against the verified ones above. It is a
dependency of `make test`, so `./ci.sh` exercises it on every gate run.

Its failure policy is deliberate: it **fails only when a tool is missing**, and
merely **warns** when a version differs from the verified one. Hard-failing on a
version mismatch would pin the project to one Fedora release with no measured
justification — the useful content of this file is "check here first before
assuming the source regressed", which is advice. A warning delivers that advice
at the moment it matters; an error would just get the check patched out on other
distros.

`CROSS` still defaults to the `aarch64-linux-gnu-` prefix on `$PATH`; override it
(`make CROSS=my-prefix-`) and the check follows.

## Build flags (for reference — see `Makefile` for the authoritative list)

`-ffreestanding -nostdlib -mgeneral-regs-only -march=armv8-a
-fno-stack-protector -fno-pic -fno-pie -g -O2`, linked `-nostdlib -static
-no-pie --build-id=none` against `link.ld`, then `objcopy -O binary` to
produce the raw, position-fixed image U-Boot's `loady`/`bootelf` loads.

## Host-only tests

A set of test programs builds with the **plain host `gcc`** (not
`$(CROSS)`/`$(CC)`) and runs entirely on the dev host, unit-testing pure
parsing/arithmetic/table-building logic with no board involved. They are not
part of the AArch64 cross-build, and any recent host `gcc` (e.g. Fedora 43's
system one) suffices.

`make test` is the authoritative list — do not trust an enumeration here to stay
current. As of 2026-08-05 it runs nine C suites (virtqueue ring parsing and
partial-sector stitching, stage-2 tables, the vnet ring, kload modinfo, the
vconsole UART, two gdbstub suites, the vGIC pending queue) plus three Python
ones (`test_automount.py`, and the offline `selftest` modes of
`coredump-recv.py` and `snapshot_net.py`).

That warning is not boilerplate: an earlier version of this section named
exactly two programs and was left behind as the suite grew, and a "these targets
are pre-broken" note elsewhere in the docs outlived the bug by two weeks and
sent sessions hunting a failure that no longer existed. Point at `make test`,
not at a hand-maintained list.
