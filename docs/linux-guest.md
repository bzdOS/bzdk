# The Linux guest (`make linux-qemu`) — ROADMAP D2, first pass

## Status

| Claim | True? | How it was checked |
|---|---|---|
| `make linux-qemu` **links** | ✅ | from-scratch, `microkernel-linux-qemu.elf`, ~14.6 KB text |
| A Linux/arm64 guest **Image builds** | ✅ | `linux-guest/build.sh` — mainline v6.12, defconfig with vendor SoC platforms/NET/ACPI/PCI/etc pruned (see "Why the config is trimmed" below), `arch/arm64/boot/Image` |
| The hypervisor **loads** it | ✅ | `main_linux_qemu.c`'s own Image-header handoff (magic/text_offset check, cache maintenance, `kload_enter()`), board-free under QEMU |
| It **boots** to its own early console output | 🟡 **partially — see "Result" below** | `./linux-qemu-ci.sh` |
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

## Result (filled in 2026-08-06 — the run this doc's own "Status" table was
## still waiting on when its author ran out of budget)

`./linux-qemu-ci.sh` reports **FAIL** (never sees the harness's own
`LINUX-QEMU-CI: PASS`/success marker inside the 120 s window), but the console
log it captures is real, substantial mainline kernel boot, not an early
crash:

```
[guest] [    0.000000] pcpu-alloc: s56416 r8192 d29600 u94208 alloc=23*4096
[guest] [    0.000000] Detected VIPT I-cache on CPU0
[guest] [    0.000000] Kernel command line: earlycon=uart8250,mmio32,0x1c28000,...
[guest] [    0.000000] arch_timer: cp15 timer(s) running at 62.50MHz (virt).
[guest] [    0.000000] GIC: PPI11 is secure or misconfigured
[guest] [    0.000000] rcu: Preemptible hierarchical RCU implementation.
[guest] [    0.000000] NR_IRQS: 64, nr_irqs: 64, preallocated irqs: 0
[guest] [    0.075089] cacheinfo: Unable to detect cache hierarchy for CPU 0
```
(then times out — no further output, no panic message, no reboot)

That answers the GIC question two paragraphs up: Linux's GIC driver does
**not** panic against the identity-mapped black hole — it logs "PPI11 is
secure or misconfigured" (a warning, not a fatal path) and boot continues well
past it, through RCU/scheduler/cache-info init, matching Zephyr's
black-hole-tolerant behaviour on the same hardware model.

**A real, separate defect, found running this rather than assumed:** the
harness's own success marker (`el2_exc_linux_qemu.c`'s `PASS_MARKER`) is
literally the string `"THIS_STRING_SHOULD_NEVER_APPEAR_xyzzy"` — a sentinel
that cannot match anything, contradicting the comment directly above it, which
describes the marker as "the substring of init/main.c's start_kernel() banner
line" (i.e. `linux_banner`, "Linux version ..."), "expected to be the first
(or one of the first) lines this target's console ever shows." That comment
and that string disagree; this reads as an edit that was interrupted
mid-stream (this agent was killed by a session-limit API error while this
file was in progress) rather than a deliberate choice.

Fixing the string alone is not obviously sufficient, though, and I did not
guess: **"Linux version" does not appear anywhere in the captured output
above**, even though real arm64 `start_kernel()` prints `linux_banner` via
`pr_notice()` unconditionally, before every line actually captured here
(pcpu-alloc etc. all come later in a normal boot). Two explanations fit, and
distinguishing them is the concrete next step, not a fix to guess at:
  1. the banner DID print, but `vconsole.c`'s tee-ring/replay path drops the
     very earliest bytes of guest console output (a capture-window bug,
     independent of this target); or
  2. the banner genuinely never printed for a reason specific to this
     harness's earlycon/console wiring.

Until one of those is confirmed, do not just swap in `"Linux version"` and
declare the target green — that would risk trading an honest FAIL for a
false PASS, which is the one failure mode this project has spent the most
effort this session removing everywhere else.

## Gate status

`linux-qemu-ci.sh` is **not yet wired into `ci.sh`**. Per this project's own
rule ("a flaky addition to a green gate is worse than none — 5+ consecutive
runs before wiring anything in"), it needs that reliability run first; the
count and result are reported in the session write-up, not pre-committed
here.

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
