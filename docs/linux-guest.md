# The Linux guest (`make linux-qemu`) — ROADMAP D2, first pass

## Status

| Claim | True? | How it was checked |
|---|---|---|
| `make linux-qemu` **links** | ✅ | from-scratch, `microkernel-linux-qemu.elf`, ~14.6 KB text |
| A Linux/arm64 guest **Image builds** | ✅ | `linux-guest/build.sh` — mainline v6.12, defconfig with vendor SoC platforms/NET/ACPI/PCI/etc pruned (see "Why the config is trimmed" below), `arch/arm64/boot/Image` |
| The hypervisor **loads** it | ✅ | `main_linux_qemu.c`'s own Image-header handoff (magic/text_offset check, cache maintenance, `kload_enter()`), board-free under QEMU |
| It **boots** to its own early console output | ✅ | `./linux-qemu-ci.sh` — see "Result" below; boots deep into generic kernel init (RCU, PSCI, GIC, arch_timer, scheduler_clock) and stalls shortly after `cacheinfo`, not yet userland |
| It boots **on the board** | ❌ **not attempted, not in scope this pass** | ROADMAP D2 explicitly scopes this pass to board-free QEMU; the hard constraint for this work was "never touch the board" |

Those rows are deliberately separate, same discipline `docs/zephyr-guest.md` established: "links" is not "loads" and "loads" is not "boots".

## Deliverable 1: the entry-contract gap

Linux/arm64's boot protocol (`Documentation/arch/arm64/booting.rst`, fetched
and read in full for this pass — quoted below where it matters) is a
**different contract** from the one `kload.c` already implements for
FreeBSD/Zephyr. Concretely, at the moment of `eret` into EL1:

| Requirement (booting.rst §4) | FreeBSD/Zephyr via `kload_enter()` | Linux via `main_linux_qemu.c` | Gap? |
|---|---|---|---|
| Image format | ELF (`kload_parse_elf`/`kload_place_segments`) | **Raw binary** (`arch/arm64/boot/Image`) with its own 64-byte header (magic `0x644d5241`/"ARM\x64", `text_offset`, `image_size`) at a fixed offset — not an ELF at all | **Real, load-bearing gap.** `kload_parse_elf()` cannot open this file format; nothing to fix in `kload.c` — a different loader was needed, hence this file (see below). |
| x0 at entry | modinfo blob PA (FreeBSD's own convention; DTB is *inside* that blob) | **physical address of the DTB directly** | Different convention, same *mechanism* — `kload_enter(entry, x0, sp)`'s second argument is generic, so this is a call-site difference, not a `kload.c` change. |
| x1–x3 at entry | not architected by FreeBSD's loader ABI | **must be 0** (booting.rst, "Primary CPU general-purpose register settings") | **Known, currently-inert deviation.** `kload_enter()` only ever sets x0 before `eret`; x1–x3 hold whatever the compiler left in those registers. Real, stock arm64 `head.S` (`primary_entry`) does not read x1–x3 in the non-EFI path, so this has no observed effect — but it is a spec violation, not a verified-safe shortcut, and it cannot be fixed without touching `kload.c` (out of scope for this pass; noted for whoever owns that file). |
| SP_EL1 at entry | Zephyr/FreeBSD's own reset code sets its own SP before any stack use | **Not specified by booting.rst at all** — no SP entry in its "Primary CPU general-purpose register settings" list, unlike x0–x3, which get one | **No gap.** The doc's *silence* on SP is the evidence: arm64 `head.S` sets up its own boot SP from a static array in the kernel image before the first stack-relative access, exactly like Zephyr's reset code. `SP_EL1_PLACEHOLDER` (a named, recognisable constant, not a bare 0) is passed for the same "never actually used" reason `main_zephyr_qemu.c` gives. |
| PSTATE.DAIF at entry | `KLOAD_GUEST_SPSR_EL2` = EL1h \| D=1 \| A=1, **I=0, F=0** | same (reused verbatim) | **Real, documented deviation from booting.rst** ("All forms of interrupts must be masked in PSTATE.DAIF"). Currently inert on *every* QEMU target in this tree that uses `kload_enter()` (Zephyr included) because nothing arms a physical interrupt source on any of them — no `gic_timer_qemu_init()`, no vGIC. It stops being inert the day this target (or Zephyr's) grows a real tick; flagged here so it is not rediscovered the hard way. Not fixed here: `kload.c` is read-only for this pass. |
| MMU/cache state | `SCTLR_EL1` = RES1-bits-only (M=0, C=0, I=0) via `guest.c`/`guest_config()` | same, reused verbatim | No gap — booting.rst requires MMU off, which this already is. |
| Cache maintenance over the loaded image | `kload_place_segments()`'s own 64-byte-line `dc cvac`+`ic ivau` loop | **New loop in `main_linux_qemu.c`** (`lx_cache_clean_to_pou()`), same idiom, over the Image (`image_size` bytes) and the DTB (2 MiB, the spec's own upper bound) | Not a gap once written — but had to be written, since there is no ELF "place" step to piggyback it onto (see below). |
| DTB placement | N/A (Zephyr bakes its DT into the build at compile time and never parses a runtime blob at all — see `zephyr-guest`'s board `.dts` file header) | **Mandatory, runtime, load-bearing** — 8-byte aligned, ≤2 MiB, not inside a 2 MiB region needing special attributes (booting.rst §2) | Real, new requirement this target has that neither existing guest does. `linux-guest/bpi_m64_hv-linux.dts` is new; see "Why a new DTB, not a reuse" below. |
| `CONFIG_ARM64_VHE` implications, given the guest runs at EL1 with `HCR_EL2.IMO=0` on this target | — | **None.** VHE (`ARM64_VHE`) governs code that itself *runs at EL2* (a Type-1/KVM-style host) — it is irrelevant to code entered at EL1, which is what every `kload_enter()` call in this tree does for every guest, Linux included. A stock distro/defconfig Linux kernel entered at EL1 under someone else's EL2 takes the ordinary "guest under a hypervisor" boot path (the same path it takes under Xen or KVM as a non-privileged guest) — `head.S`'s `el2_setup` stub checks `CurrentEL` and simply skips the EL2-only setup work (VHE detection, `CPTR_EL2`, etc.) when entered already at EL1. `IMO=0` (physical IRQs not routed to EL2, see `guest.c`) affects interrupt *routing*, not exception-level *identity*, and this target arms no physical interrupt source at all (see the DAIF row above), so the two questions don't currently interact. | N/A |

**Net assessment**: the gap is real but narrow, and it is exactly where the
brief predicted it would be — "the loading path, not the HV core". Nothing
in `stage2.c`, `guest.c`, `vconsole.c`, or `exceptions.S` needed to know
anything Linux-specific; the entire new surface is (a) a ~250-line
Image-header-and-cache-maintenance loader (`main_linux_qemu.c`, new) that
does for a raw binary what `kload_parse_elf`/`kload_place_segments` already
do for an ELF, calling the one guest-agnostic primitive `kload.c` exports
(`kload_enter()`) exactly as-is, and (b) a devicetree, because unlike Zephyr,
Linux actually parses one at runtime.

## Why a new DTB, not a reuse of zephyr-guest's

`zephyr-guest/boards/bzdos/bpi_m64_hv/bpi_m64_hv.dts` documents that Zephyr
never parses a runtime DTB at all — it is compiled into the image at build
time and `x0` is a don't-care from Zephyr's own reset code's point of view.
For Linux, `x0` (the DTB pointer) is the *entire* mechanism by which the
guest learns anything about its environment, so `linux-guest/bpi_m64_hv-linux.dts`
is new, load-bearing, and has requirements Zephyr's never needed:

- `/memory` (Zephyr gets its RAM extent from Kconfig/the linker script
  instead) — set to match `stage2.h`'s identity-mapped
  `[0x40000000, 0x80000000)` intersected with QEMU's `-m 1024`.
- `/chosen/bootargs` + `/chosen/stdout-path` + an `earlycon=` argument, since
  Linux's console only exists once something tells it to use one — Zephyr's
  is wired at compile time via `zephyr,console`.
- A standard-binding-`compatible` UART/GIC/timer node set that Linux's
  *upstream, unmodified* drivers will actually bind against, rather than
  Zephyr's own `"ns16550"` binding.

The MMIO addresses (UART0 `0x01c28000` reg-shift 2, GIC `0x01c81000`/
`0x01c82000`) are byte-for-byte the same as `zephyr-guest`'s `.dts` — both
are the real Allwinner A64 addresses this hypervisor's C code already uses
(`stage2.h`, `vconsole.c`), cross-checked the same way.

## Why the config is trimmed (and what that cost)

A first attempt at this pass built `defconfig` unmodified. That is the
*wrong* default for this milestone specifically because of how arm64's
`defconfig` is structured: it is not one board's config, it is roughly 180
`CONFIG_ARCH_*` vendor SoC platform selects all turned on simultaneously
(Qualcomm, Rockchip, Broadcom, Allwinner's own real driver stack, dozens
more), each pulling in its own tree of `clk`/`pinctrl`/`gpio`/`soc` drivers —
none of which this synthetic, no-real-SoC target will ever probe against,
since this DTB names none of their platform compatible strings. Building all
of it anyway meant compiling `drivers/{clk,pinctrl,gpio,soc,dma,...}` trees
for boards that will never run, which is exactly the disk/CPU a
disk-constrained CI host (this one regularly runs in the 25–35 GB free
range) should not spend on every run.

`linux-guest/build.sh`'s `.config` therefore disables every `CONFIG_ARCH_*`
platform select plus `NET`/`ACPI`/`PCI`/`SCSI`/`ATA`/`WLAN`/`INPUT`/`FB`/
`SOUND`/`USB_SUPPORT`/`MMC`/`VIRTIO_MENU`/`CRYPTO_HW` and a few more
subsystems this milestone's DTB gives the kernel no way to use, then runs
`olddefconfig` to resolve the fallout, then builds `Image` only (never
`modules`/`all`) — never a full `make`. What survives is core kernel + VFS +
the generic 8250 serial / ARM GIC / architected-timer / PSCI drivers this
milestone actually exercises. This is *not* the same config as a real
distro kernel; it is deliberately the smallest config that can still boot
and print its own banner on this synthetic platform, and any future work
extending this target (a real rootfs, virtio-blk/net for the guest) will
need to re-enable the relevant subsystem deliberately, not inherit it by
accident from `defconfig`.

## Placement / memory map

```
0x40000000 ─── DRAM base (stage2.h STAGE2_DRAM_BASE, QEMU -m 1024 RAM base)
0x40080000 ─┬─ this hypervisor's OWN image under QEMU (link_qemu.ld, 32 MiB)
0x42080000 ─┘
0x44000000 ─── DTB staged here (QEMU -device loader, force-raw=on)
0x50000000 ─┬─ hv-scratch: breadcrumbs, vconsole capture ring, etc.
0x50200000 ─┘
0x51000000 ─── Linux Image staged here (2 MiB aligned; text_offset asserted 0)
0x60000000 ─── STAGE2_SELFTEST_IPA (stage2_at_check(), transient, not guest RAM)
0x80000000 ─── end of the 1 GiB identity map
```

Both the Image and the DTB are dropped by QEMU's generic loader device
**directly at their final resting addresses** — unlike the Zephyr/FreeBSD
ELF path, there is no separate "stage here, then copy/relocate there" step,
because a raw `Image`'s only placement rule is "any 2 MiB-aligned base,
chosen by the loader" (booting.rst) rather than an ELF's fixed `p_vaddr`.
See `main_linux_qemu.c`'s header, section "WHY NO PLACE SEGMENTS STEP".

## Running it without a board

```sh
./linux-qemu-ci.sh
```

First run builds the guest artifacts via `linux-guest/build.sh` (one-time,
~a few minutes; see that script's header for exactly what it does and does
not build) unless `LINUX_GUEST_IMAGE`/`LINUX_GUEST_DTB` are already set or a
prebuilt pair exists at the conventional `/opt/bzdos/linux-work/build/...`
path — same SKIP/build/override precedence `zephyr-qemu-ci.sh` established
for the Zephyr tree, for the same reason: a Linux source checkout is ~1.6 GB
extracted, deliberately not vendored into this repo.

### Result

*(Filled in once the trimmed-config build in progress at the time of writing
completes and `linux-qemu-ci.sh` has actually been run — see the session's
final report for the measured outcome: either the exact console tail up to
and including `LINUX-QEMU-CI: PASS`, or the exact fault this target hits and
where.)*

## PSCI SMC passthrough — the one new EL2 behaviour this target needed

`linux-guest/bpi_m64_hv-linux.dts` advertises `psci { method = "smc"; }`,
which Linux's PSCI client probes early in boot regardless of whether this
milestone cares about CPU hotplug/suspend/reboot. Absent an implemented
EL3, an `smc` instruction executed at EL1 traps unconditionally to EL2 —
there is nowhere else for it to go. `el2_exc_linux_qemu.c` handles this by
**re-issuing the identical `smc` while running at EL2** (where this
hypervisor is the top implemented EL, exactly the context every other QEMU
CI target's own `qemu_poweroff()` already relies on to reach QEMU's
built-in PSCI firmware emulation) and copying the x0–x3 result back into
the trapped guest's frame before returning past the instruction. This is
the standard PSCI-passthrough shape any bare hypervisor without a trusted
firmware layer of its own needs, and it is new (Zephyr's target never
needed it — its board's `method` binding is absent since it makes no PSCI
call at all in this build).

## What a pass would/does NOT prove

Same limitation `docs/zephyr-guest.md` documents for its own target, for the
identical reason:

- **Nothing about GIC programming.** The GIC (`0x01c81000`) is
  identity-mapped by `stage2.c` and, under QEMU, a black hole — nothing
  answers there, reads return 0, no abort (measured against QEMU 10.1.5 for
  the Zephyr target; not re-measured against this one independently, but
  the mechanism is identical). Linux's `irq-gic.c` driver *will* probe this
  during boot; whether that probe "succeeds against a black hole" the way
  Zephyr's did, or fails a sanity check on the zeroed `GICD_TYPER` and
  panics, is exactly the kind of thing that has to be read off the actual
  boot log, not asserted in a comment — see "Result" below for what
  actually happened.
- **Nothing about real-hardware timing.** No eMMC, no EMAC, no USB gadget,
  no watchdog.
- **Nothing past whatever the last line of guest console output is.** This
  is a first pass at the entry-contract gap and a board-free loader, not a
  working Linux port with a rootfs.

## Result (corrected 2026-08-06 — supersedes this section's own immediately
## prior revision, which shipped `PASS_MARKER` broken; see "Corrected"
## below for exactly what was wrong and how the correction was verified,
## not guessed)

`./linux-qemu-ci.sh` reports **PASS**. Verified directly, four separate
times in one sitting, not asserted on one green run:

1. `./linux-qemu-ci.sh` itself, unmodified — PASS, exits via the harness's
   own `qemu_poweroff()` (clean PSCI SYSTEM_OFF, not a `timeout` kill) after
   101 guest console bytes:
   ```
   HV: Image entry_pa=0x0000000051000000
   [guest] [    0.000000] Linux version
   LINUX-QEMU-CI: PASS (mainline Linux/arm64 Image loaded by main_linux_qemu.c's own Image-header handoff, running at EL1 under stage-2, console via vconsole 16550 trap-emulation)
   LINUX-QEMU-CI: guest console bytes emulated: 101
   ```
2–4. Three longer, unbounded diagnostic runs (marker deliberately disabled so
   the guest keeps running rather than powering off at first match; output
   forced to a plain file with `stdbuf -oL -eL ... > file`, sidestepping a
   real pipe-buffering trap described in "Corrected" below), at 30 s, 70 s and
   240 s. All three show, from the very first guest byte:

```
[guest] [    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd034]
[guest] [    0.000000] Linux version 6.12.0 (root@workstation) (aarch64-linux-gnu-gcc ...) #1 SMP PREEMPT ...
[guest] [    0.000000] Machine model: Banana Pi M64 (bzdOS EL2 hypervisor EL1 guest, Linux, QEMU)
[guest] [    0.000000] earlycon: uart8250 at MMIO32 0x0000000001c28000 (options '115200n8')
[guest] [    0.000000] printk: legacy bootconsole [uart8250] enabled
...
[guest] [    0.000000] psci: PSCIv1.1 detected in firmware.
[guest] [    0.000000] psci: Using standard PSCI v0.2 function IDs
[guest] [    0.000000] WARNING: x1-x3 nonzero in violation of boot protocol:
[guest] [    0.000000] 	x1: 0000000044000000
[guest] [    0.000000] 	x2: 0000000000000305
[guest] [    0.000000] 	x3: 000000000000000a
[guest] [    0.000000] This indicates a broken bootloader or old kernel
...
[guest] [    0.000000] NR_IRQS: 64, nr_irqs: 64, preallocated irqs: 0
[guest] [    0.000000] Root IRQ handler: gic_handle_irq
[guest] [    0.000000] GIC: PPI11 is secure or misconfigured
[guest] [    0.000000] arch_timer: cp15 timer(s) running at 62.50MHz (virt).
[guest] [    0.000000] clocksource: arch_sys_counter: mask: 0x1ffffffffffffff ...
[guest] [    0.009640] Console: colour dummy device 80x25
[guest] [    0.014990] Calibrating delay loop (skipped) ... 125.00 BogoMIPS (lpj=250000)
[guest] [    0.020203] LSM: initializing lsm=capability
[guest] [    0.072600] cacheinfo: Unable to detect cache hierarchy for CPU 0
```

Three real findings, each checked against the log rather than assumed:

- **The x1–x3 gap this doc's own table predicted as "known, currently-inert"
  is now measured, not just theorised.** The kernel's own boot-protocol
  checker caught it (`WARNING: x1-x3 nonzero in violation of boot
  protocol`), printed the exact values, and **continued booting anyway** —
  "currently-inert" was the right call, now with real register values on
  record (`x1==0x44000000` is exactly `LX_DTB_PA`: it is `kload_enter()`'s
  second AAPCS64 argument register, never clobbered before `eret` — a fully
  explained leftover, not a mystery).
- **PSCI SMC passthrough (`forward_psci_smc()`) works exactly as designed**:
  `psci: PSCIv1.1 detected in firmware` / `Using standard PSCI v0.2 function
  IDs` is Linux's PSCI client successfully round-tripping a probe SMC through
  EL2 to QEMU's own firmware emulation and back.
- **The GIC-black-hole question this doc asks earlier is answered**: Linux's
  `irq-gic.c` does **not** panic against the identity-mapped black hole. It
  registers `gic_handle_irq` as the root IRQ handler, warns once
  (`PPI11 is secure or misconfigured` — GICD registers reading all-zero look
  like a security misconfiguration to the arch_timer driver), and the virtual
  counter/clocksource still registers successfully — the same
  black-hole-tolerant outcome Zephyr's tickless build got, now confirmed for
  a real, unmodified mainline driver too.

**The actual boundary of this pass**: all runs reach `cacheinfo: Unable to
detect cache hierarchy for CPU 0` reliably; progress *past* it is real but
inconsistent run-to-run (the 70 s run reached two more RCU init lines within
its window, the 240 s run did not advance further in over three minutes of
wall-clock time). Guest timestamps barely move during that stall (tens of
microseconds of guest time for tens of seconds of wall time), which is the
signature of a busy-wait calibrated against `lpj=250000` (BogoMIPS from the
timer, not a real calibration loop) rather than a true deadlock — most likely
something waiting on a jiffies-driven timeout that a real timer tick would
resolve instantly and this build's TCG-speed busy-wait resolves only after
enormous emulated instruction counts. **Not root-caused further this pass**:
the next step for whoever picks this up is exactly what "Next step" below
already said before this correction — wire `vgic_init()` + the GICC→GICV
stage-2 redirect (a `stage2.c` change, out of scope here) and see whether the
stall clears.

### Corrected: what the immediately prior revision of this file got wrong

The revision of this file this replaces reported `linux-qemu-ci.sh` **FAIL**
and left `PASS_MARKER` set to a sentinel that could never match
(`"THIS_STRING_SHOULD_NEVER_APPEAR_xyzzy"`), reasoning — correctly, as a
methodology — that swapping in `"Linux version"` without first confirming it
actually appears in the captured output would risk trading an honest FAIL for
a false PASS. It reported that string was **absent** from its captured
console log entirely.

That reasoning was sound; the observation it was reasoning from was not
reproducible. Restoring `PASS_MARKER = "Linux version"` in this pass was not
a guess made in spite of that finding — it followed FOUR independent,
directly-observed captures (enumerated above) that all show the banner
clearly, including one using the exact unmodified `linux-qemu-ci.sh` script
and capture mechanism the prior revision itself ran.

The most likely explanation for the discrepancy, though not conclusively
root-caused (rebuilding to test it further was blocked this pass by an
unrelated, severe host-wide disk-full event — see the session report): this
pass independently hit a real, reproducible pipe-buffering trap while
building its own diagnostic captures — piping `qemu-system-aarch64`'s
`-nographic` stdout through `| tail` with no forced line-buffering produced
**zero bytes of output** for a run that was killed by `timeout` (SIGTERM)
before a clean exit, versus full output once `stdbuf -oL -eL` forced
line-buffering. Every run the prior revision's own broken marker could ever
produce necessarily hit `timeout`'s SIGTERM path (a sentinel that cannot
match never lets the harness reach its own clean `qemu_poweroff()`), which is
exactly the scenario this pass demonstrated can lose buffered console output
depending on how the caller captures QEMU's stdout. This is a real
robustness gap in how output gets captured on a FAIL/timeout path generally,
independent of this one string — worth hardening (e.g. `linux-qemu-ci.sh`
switching its capture to a plain file rather than a `$(...)` pipe) before
leaning on a FAIL run's captured tail as evidence of anything again.

## Gate status

`linux-qemu-ci.sh` is **not yet wired into `ci.sh`**. Per this project's own
rule ("a flaky addition to a green gate is worse than none — 5+ consecutive
runs before wiring anything in"), it needs that reliability run first — 1
clean PASS is on record (see "Result"), not yet 5. The reliability run
itself could not be completed in this pass: a severe, host-wide disk-full
event (unrelated to this target — see the session report) forced deleting
the built `Image`/DTB partway through verification, and rebuilding a second
copy was not attempted while the host was still critical. `linux-qemu-ci.sh`
already SKIPs (not FAILs) when no guest artifacts are available, so this is
a safe, honest state to leave it in — whoever runs the 5x trial next just
needs `./linux-guest/build.sh` (or `LINUX_SRC=`) once the host has headroom
again.

## Next step

1. Read whatever `linux-qemu-ci.sh` actually prints (see "Result") and
   triage the first real blocker past the console banner — almost certainly
   GIC- or timer-related, per "What a pass would/does NOT prove" above.
2. If GIC programming turns out to matter for the very next milestone (e.g.
   Linux's `arch_timer` driver refusing to register a clocksource against a
   black-hole GIC, unlike Zephyr's tickless build which never asked), the
   real fix is the same one `docs/zephyr-guest.md` already names for its
   own target: wire `vgic_init()` + the GICC-IPA → physical-GICV stage-2
   redirect. That is a `stage2.c` change and therefore out of scope for
   this pass's file-scope constraints; it belongs to whoever owns that file
   next.
3. A minimal initramfs (busybox `init` that just prints something and
   halts) would turn "boots to a console line" into "boots to userland",
   the same distance FreeBSD/Zephyr have already covered on their guests —
   not attempted this pass; scoping call for whoever picks this up.
