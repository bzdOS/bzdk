# The Zephyr guest (`make zephyr` / `make zephyr-qemu`)

## Status, 2026-08-04

| Claim | True? | How it was checked |
|---|---|---|
| `make zephyr` **links** | ✅ | from-scratch build, `rm -f *.o` first: 49 883 B text |
| A Zephyr guest image **builds** | ✅ | `zephyr-guest/build.sh bpi_m64_hv` → `zephyr.elf`, EXEC, one `PT_LOAD` @ `0x51000000`, entry `0x5100100c` |
| The hypervisor **loads** it | ✅ | `kload_parse_elf` + `kload_place_segments` on the real image, board-free under QEMU |
| It **boots** to its own `main()` | ✅ | `./zephyr-qemu-ci.sh` — banner, `hello from EL1`, heartbeats 0 and 1 |
| It boots **on the board** | ❓ **not yet** | needs hardware; the image is staged at `/opt/bzdos/tftpboot/zephyr.elf`, procedure in `../zephyr-guest/LOADING.md` |

Those five rows are deliberately separate. "Links" is not "loads" and
"loads" is not "boots"; this project has burned time before on a report that
blurred them.

## The two halves

**The guest image** is built out of tree from `zephyr-guest/` against a
Zephyr v4.4.1 checkout (565 MB, deliberately *not* vendored into this repo):

```sh
ZEPHYR_BASE=/opt/bzdos/zephyr-work/zephyr ./zephyr-guest/build.sh bpi_m64_hv
```

It is a plain `EXEC` ELF with a single `PT_LOAD` at physical `0x51000000`
(`p_vaddr == p_paddr`, non-relocatable) and entry `0x5100100c`. See
`zephyr-guest/boards/bzdos/bpi_m64_hv/` for the board port and
`zephyr-guest/LOADING.md` for the full build/flash procedure, including why
`-mno-outline-atomics` is required with this host's Linux-hosted cross
compiler.

**The hypervisor firmware** comes in two flavours that run the *same*
guest-loading sequence:

| Target | Runs on | Guest console | Progress readable via |
|---|---|---|---|
| `make zephyr` (`main_zephyr.c`) | real board | vconsole 16550 emulation | EMAC debug channel / breadcrumb `0x50008000` |
| `make zephyr-qemu` (`main_zephyr_qemu.c`) | QEMU `virt` | vconsole 16550 emulation | PL011, straight to stdout |

Sequence, identical in both: `kload_parse_elf(0x44000000)` →
`kload_place_segments(…, 0x51000000)` → `guest_config()` → `stage2_init()` /
`stage2_enable()` → `kload_enter(entry, 0, …)`. `kload.c`, `stage2.c`,
`guest.c`, `vconsole.c` and `exceptions.S` are used **unmodified** and are
the same objects the board build links — that reuse is itself the evidence
that they are guest-OS-agnostic rather than FreeBSD-specific.

## Running it without a board

```sh
./zephyr-qemu-ci.sh      # ~1.6 s; also part of ./ci.sh
```

Expected tail:

```
HV: segments placed at 0x51000000, entry_pa=0x000000005100100c
HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)
[guest] *** Booting Zephyr OS build v4.4.1 ***
[guest] bzdOS/Zephyr: hello from EL1 (board: bpi_m64_hv)
[guest] heartbeat 0
[guest] heartbeat 1
ZEPHYR-QEMU-CI: PASS …
```

The guest ELF is staged at physical `0x44000000` by QEMU's generic loader
(`-device loader,file=…,addr=0x44000000,force-raw=on`). `force-raw=on`
matters: QEMU drops the file as an opaque memory image and does **not**
interpret the ELF — `kload.c` does, exactly as on the board, where
`tftpboot 0x44000000 zephyr.elf` fills the same role.

### Why the *real board's* image runs under QEMU at all

The image is not a QEMU port. Its MMIO addresses are Allwinner A64
addresses. Two independent reasons it still works:

- **UART0 (`0x01c28000`) never reaches QEMU's memory system.** stage-2
  leaves exactly that 4 KiB page invalid (`stage2.h`, `UART0_BASE`), so
  Zephyr's stock `ns16550` driver faults into EL2 on every access and
  `vconsole.c` emulates a 16550. Whether QEMU has a device there is
  irrelevant. This is the *real* console mechanism, and it is the most
  valuable thing this gate exercises: a third-party driver, written with no
  knowledge of us, driving our emulation.
- **The GIC (`0x01c81000`) is a black hole.** It *is* identity-mapped, so
  guest accesses do go out to the memory system, where QEMU virt has
  nothing. Measured on QEMU 10.1.5: dropped silently (reads return 0), no
  external abort, so `arm_gic_init()` completes and boot continues.

### What a pass therefore does NOT prove

- **Nothing about GIC programming** (see above). Harmless today — this build
  is tickless and no interrupt is ever enabled by anyone — but the day the
  guest gets a real timer tick, this gate needs a GIC that answers.
- **Nothing about real-hardware timing**: no eMMC, no EMAC, no USB gadget,
  no watchdog, and `CONFIG_BUSYWAIT_CPU_LOOPS_PER_USEC=400` is an untested
  guess at a ~1.2 GHz Cortex-A53 that only ever gets exercised against TCG.
- **Nothing about the A64 watchdog path** `main_zephyr.c` arms and
  `main_zephyr_qemu.c` omits.

## Next step (needs the board)

1. `tftpboot 0x44000000 zephyr.elf` (already staged at
   `/opt/bzdos/tftpboot/zephyr.elf`); **no DTB transfer needed** — Zephyr's
   devicetree is compiled into the image.
2. `loady` + `bootelf -p` `microkernel-zephyr.elf` (`make zephyr`).
3. Expect the same console text the QEMU run prints, and breadcrumb
   `0x50008000` word[1] climbing to 7 with word[6] == `0x5100100c`.
4. Then re-time `CONFIG_BUSYWAIT_CPU_LOOPS_PER_USEC`, and only after that
   consider wiring `vgic_init()` + the GICC-IPA → GICV stage-2 redirect to
   give the guest a real tick.

Full procedure and failure signatures: `zephyr-guest/LOADING.md`.
