# Loading a Zephyr guest image onto real hardware

Not executed from this worktree (no board access here) — this is the
procedure for whoever has it, mirroring the existing FreeBSD load flow
(`reliable_load.py` / `chimpd.py` / `main_fbsd.c`) as closely as possible.
Where it differs from the FreeBSD flow, the difference and why is called
out explicitly.

## 1. Build both halves

**a) The hypervisor firmware**, now with Zephyr-guest support built in:

```
make zephyr        # -> microkernel-zephyr.elf / microkernel-zephyr.bin
```

(Mirrors `make fbsd` exactly — see `main_zephyr.c`'s header comment for
what its object list is and why it's identical to `FBSD_OBJS` bar the one
source file.

**RESOLVED, 2026-08-04.** This paragraph used to record that `make zephyr`
did not link, with undefined references to `musb_puts` /
`emac_link_watchdog` from `smp.c` — symbols that live in `musb.c`/`emac.c`,
missing from the object list in the worktree this file was written in. That
was fixed by commit `203247f` ("build: fix stale OBJS lists so
repl/fbsd/zephyr link again", 2026-07-25), which added `musb.o`/`usbacm.o`/
`emac.o` to both `FBSD_OBJS` and `ZEPHYR_OBJS`; the note simply outlived the
bug. Re-verified from scratch (`rm -f *.o` before each target): `zephyr`,
`fbsd`, `repl`, `dbg`, `gdb` and `qemu` all link, `zephyr` at 49 883 B text.)

**b) The Zephyr guest image** (built and verified working in this pass —
see `../zephyr-guest/` and the accompanying report for full detail):

All of the below is now wrapped in one script — `./build.sh bpi_m64_hv`,
which prints the resulting ELF path as its last line. Use that; the manual
form is kept only so the flags stay explainable.

```
export ZEPHYR_BASE=/path/to/zephyr        # v4.4.1 tested
export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
export CROSS_COMPILE=/usr/bin/aarch64-linux-gnu-

cmake -GNinja \                            # "Unix Makefiles" also works
  -DBOARD=bpi_m64_hv \
  -DBOARD_ROOT=/path/to/zephyr-guest \
  -DEXTRA_CFLAGS=-mno-outline-atomics -DEXTRA_CXXFLAGS=-mno-outline-atomics \
  -S zephyr-guest/app -B build-zephyr
cmake --build build-zephyr
```

`build-zephyr/zephyr/zephyr.elf` is the artifact to load. It is a plain
`EXEC` ELF, single `PT_LOAD`, `p_vaddr == p_paddr == 0x51000000` (physical,
non-relocatable — see the board's `bpi_m64_hv.dts` file header for why that
address is safe), entry `0x5100100c`.

`-mno-outline-atomics` is required with this specific toolchain
(`aarch64-linux-gnu-gcc`, a *Linux*-hosted cross compiler, not a bare-metal
`-none-elf`/Zephyr-SDK one — neither was available in this environment; see
the report for detail) — without it, libgcc pulls in an atomics
runtime-detection thunk that calls `__getauxval`, which does not exist in a
freestanding image and fails the final link.

## 2. Stage the Zephyr ELF where the hypervisor expects it

`main_zephyr.c` (already built into `microkernel-zephyr.elf` above) expects
the Zephyr ELF resident at physical `0x44000000` at the moment it runs —
the *same* staging address `main_fbsd.c` already uses for the FreeBSD
kernel ELF. **Unlike the FreeBSD flow, no DTB TFTP step is needed at all**
(Zephyr's devicetree is compiled into the image at build time — see the
board's `.dts` file header) — this saves one whole TFTP transfer per
cycle.

Concretely, in a `reliable_load.py`/`chimpd.py`-style flow:

1. Copy `build-zephyr/zephyr/zephyr.elf` into `/opt/bzdos/tftpboot/`
   (e.g. as `zephyr.elf`, alongside the existing `kernel`/
   `bananapi-min.dtb`).
2. `tftpboot 0x44000000 zephyr.elf` from U-Boot (this is exactly
   `chimpd.py`'s existing `tftp_retry(KADDR, ...)` step, just pointed at
   the new filename — the Zephyr image is currently ~33 KB for the
   `hello_world`/heartbeat apps built in this pass, vs. FreeBSD's ~16 MB
   kernel, so this transfer is seconds, not the ~55s `chimpd.py` budgets
   for the FreeBSD kernel).
3. **Skip** the DTB TFTP step (`tftpboot 0x4a000000 bananapi-min.dtb`) —
   not needed.
4. `loady` the **hypervisor firmware** itself
   (`microkernel-zephyr.elf`, from step 1a) to a staging address and
   `bootelf -p` it — identical mechanics to every other `main_*.c` variant
   in this tree (`loady_over_acm.py`'s `do_loady_and_go()`); U-Boot cannot
   `bootelf` a TFTP'd image directly (stale-dcache issue — see this
   project's own `uboot-tftp-bootelf-fails` note), which is exactly why
   this two-step loady(HV)+TFTP(guest) dance exists for FreeBSD already
   and is unchanged here.
5. The now-running `microkernel-zephyr.elf` firmware's `main()`
   (`main_zephyr.c`) does the rest itself, deterministically, with no
   further host interaction: parse the ELF at `0x44000000`
   (`kload_parse_elf`), copy/relocate its `PT_LOAD` segments to
   `0x51000000` (`kload_place_segments` — a no-op relocation here, see
   `main_zephyr.c`'s header comment point 4), program the guest CPU state
   (`guest_config()`, unchanged from the FreeBSD path), enable the stage-2
   identity map (`stage2_init()`/`stage2_enable()`, unchanged), arm the
   same ~6s bring-up watchdog `main_fbsd.c` uses, and `eret` into
   `0x5100100c` at EL1.

## 3. What to expect / how to verify

- **UART0** (the same vconsole capture ring `main_fbsd.c` already uses,
  0x50000f00+) should show, in order:
  ```
  *** Booting Zephyr OS build v4.4.1 ***

  bzdOS/Zephyr: hello from EL1 (board: bpi_m64_hv)
  heartbeat 0
  heartbeat 1
  ...
  ```
  **This exact text was reproduced board-free on 2026-08-04**, from THIS
  image (not an equivalent, not a QEMU-specific rebuild — the same
  `bpi_m64_hv` ELF this section tells you to TFTP), running under
  `make zephyr-qemu` on QEMU virt: `./zephyr-qemu-ci.sh`, ~1.6 s, 5/5 and
  8/8 runs PASS, now part of `ci.sh`. That covers the whole chain up to and
  including the vconsole 16550 emulation, so if the board shows nothing the
  suspects are narrowed to what QEMU cannot model: real UART0 wiring/timing,
  the watchdog `main_zephyr.c` arms, and the GIC (which QEMU silently
  swallows — see `../docs/zephyr-guest.md`, "What a pass therefore does NOT
  prove").
- A new breadcrumb window at physical `0x50008000` ("ZEP1", see
  `main_zephyr.c`) advances through stages 1–7 the same way
  `main_fbsd.c`'s `0x50000e00` ("FBS1") window does; word[1] climbing to 7
  with word[6] == `0x5100100c` means the hand-off point was reached
  cleanly. If it stops at `0xBAD1`/`0xBAD2`, the ELF failed to
  parse/place — check the TFTP'd file is actually the ELF (not truncated)
  at `0x44000000`.
- **No heartbeat / hang after stage 7**: the most likely real-hardware gap
  is NOT in this board port — see the "known gap" section of
  `main_zephyr.c`'s header comment and the board's `Kconfig.defconfig`.
  This build deliberately never waits on any interrupt (tickless,
  `CONFIG_SYS_CLOCK_EXISTS=n`), so a hang here would point at something in
  the UART0 trap-and-emulate path itself, not the (not-yet-relied-on)
  timer/vgic plumbing.

## 4. Next concrete step for whoever picks this up with board access

1. Run the procedure above once, confirm the breadcrumb + UART output
   match what QEMU showed in this pass.
2. Re-calibrate `CONFIG_BUSYWAIT_CPU_LOOPS_PER_USEC` in the board's
   `Kconfig.defconfig` against the real ~1.2 GHz Cortex-A53 clock (the
   400 shipped here is an untested guess, deliberately conservative/slow
   — see that file's comment).
3. When ready to give the guest a real scheduler tick (k_sleep, timeouts,
   a real shell): wire `vgic_init()` into `main_zephyr.c` (and
   `main_fbsd.c`, if desired) and add the stage-2 GICC-IPA → physical-GICV
   redirect `vgic.h` already documents — that piece needs `stage2.c`
   changes, which is exactly why it was left out of this pass (frozen for
   the parallel A1 isolation milestone). Once done, flip the board's
   `timer` node to `status = "okay"` and drop the `ARM_ARCH_TIMER`/
   `SYS_CLOCK_EXISTS` overrides in `Kconfig.defconfig`.
