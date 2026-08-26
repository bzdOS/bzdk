# bzdk 0.0.2-prealpha

**A bare-metal EL2 hypervisor for the Banana Pi M64 (Allwinner A64, 4× Cortex-A53,
1 GiB DRAM), running FreeBSD 15.1 arm64 as its guest.** Written from scratch: no
KVM, no Xen, no Jailhouse.

Second intermediate snapshot, tagged for the same reason as the first: **so the
work can be put on hold at a known-good point.** See `RELEASE-0.0.1.md` for the
full picture of what the project is — this file covers what changed since it, and
is written to the same rule: a release that lists none of its broken parts is not
honest.

Tag: `v0.0.2-prealpha`. Default build: `make dbg`.

## The headline: the guest is genuinely SMP now

0.0.1 said this, under "SMP guest":

> **opt-in**: `make dbg VCPU2=1` gives CPU2 to the guest as a second vCPU.
> Verified — the guest enumerates `CPU 1 ... affinity: 2`, sets up IPIs,
> completes `Release APs`

That was as far as it went, and on CPU1 it went no further than that either: two
earlier attempts to hand CPU1 to the guest both wedged the WHOLE guest — both
cores — immediately after FreeBSD printed `Release APs...done.`

**Now, in the DEFAULT build:**

| | |
|---|---|
| `hw.ncpu` / `kern.smp.cpus` | 2 |
| IPIs actually delivered to CPU1 | `cpu1:preempt` 171472, `cpu1:rendezvous` 16 (`vmstat -i`) |
| Parallel throughput, idle guest | one job **3.95 s** wall / 3.89 s CPU; two jobs **3.92 s** wall / **7.80 s** CPU — two jobs in one job's wall time, ~1.92x |
| Continuous uptime with the debug plane still responsive | 4 h 09 m and counting |

### Why it used to wedge

`vcpu1_run()` never called `vgic_init()` on CPU1. Under this project's live
`HCR_EL2.IMO=1/FMO=1` policy, EL2 owns every physical interrupt and the ONLY
route to the guest is `vgic_inject_hw()` tying it to a GICH List Register — and
GICH is per-PE-banked, with `vgic_active()` reading the calling core's own slot.
So CPU1 had `GICH_HCR.En = 0`, `gic_timer_irq()`'s forwarding block was skipped
entirely, and that core's guest received **zero** interrupts: no timer, no IPI.

The console line was the trap. `Release APs...done.` is not the hang point —
FreeBSD's AP-release rendezvous is a pure shared-memory handshake needing no
interrupt, so CPU1 came up fine and printed. The guest died at the NEXT step,
`smp_after_idle_runnable()`'s `smp_rendezvous()`, whose IPI reached CPU1's
*physical* interface, trapped to EL2, and had nowhere to go.

Two wrong diagnoses were built and deployed before that one: a `GICD_SGIR`
masking bug in `vgicd.c`, and a CPU0↔CPU1 cache-coherency failure. The evidence
that rules out both is the same, and is worth remembering: **CPU1's EL2 side —
its tick, the watchdog kick, dbgmon over EMAC — stayed fully alive throughout
the wedge.** When a guest wedges while EL2 on the same core is healthy, suspect
interrupt delivery, not memory.

### What arming CPU1 cost, and what was done about it

CPU1 was this project's independent crash-recovery witness: a core that never
runs guest code, owns the hardware watchdog, and therefore survives any guest
wedge. Handing it to the guest gives that up. The mitigation is `HCR_EL2.IMO=1`:
a periodic CNTP tick on CPU1 traps to EL2 unconditionally, whatever the vCPU
there is doing, so the watchdog kick and `dbgmon_service()` still run.

Duties that lived only in CPU1's old tight loop had to be re-homed, and they
were, by two different mechanisms depending on what the duty actually needs:

- **On the 10 ms tick**: the eMMC PC5 pinmux enforcement (FreeBSD's own pinctrl
  leaves PC5 at gpio and kills the eMMC clock) and the HDMI PHY relock.
- **On the real hardware interrupt**, because a fixed period is the wrong shape
  for them: the USB-ACM console bridge, wired to MUSB's own "mc" SPI (INTID 103,
  cited from the live DTB). USB full-speed frames are ~1 ms apart; a 10 ms tick
  would miss ten of them between calls.

**Honest residual risk, unchanged by any of this:** a *software* hang of the
guest on CPU1 can no longer starve the watchdog. A *hardware*-level wedge of
that specific core still can — if the core stops fetching instructions, EL2's
tick handler cannot run either. That risk is now live in the default build
rather than opt-in. `board-config.xml` holds the one attribute that disarms it
(`vcpu1 enabled="false"` plus `python3 gen_config.py`, which flips the build
flag and the `cpu@1` DTB node together).

## Two more real bugs found on the way there

**The debug channel died a few minutes into every guest boot.** `smp.c` calls
`vcpu1_run()` BEFORE its own `dbg_core_active = 1`, and `vcpu1_run()` never
returns — so that flag stayed 0 for the life of the board. It is not a status
bit but the `emac_poll()` **single-owner mutex**: `el2_exc.c` has six
`if (!dbg_core_active) dbgmon_service(frame);` guards on the guest's MMIO trap
paths, so CPU0 kept servicing dbgmon while CPU1's tick did the same — running
`emac_poll()` concurrently on two cores and permanently corrupting `g_rx_slot`
and the console ring. `emac.c`'s own header states the RX side is safe *only*
because that cannot happen, citing this very flag.

Signature worth recognising: fine during guest boot (few traps), dead once the
guest is active, never recovers however quiet the link goes — while stateless
paths serviced *inside* `emac_poll()` itself keep answering. That asymmetry is
what localises it: `hvdbg.read_words()` goes through dbgmon's `exec_line()`,
`wdt_reset()` does not.

The same stuck flag also disabled `el2_exc.c`'s guest-frame snapshot, so
`gr`/`sr`/gdbstub would have been reporting stale registers.

Measured after the fix: **0 read errors in ~600 EMAC samples over 10 minutes**
under guest CPU load and bulk traffic — the exact conditions that used to kill
it. (The three errors seen in a later 12-minute run were all my own concurrent
probes on the same channel.)

**`vgicd.c` sent the guest a self-IPI it never asked for.** The `GICD_SGIR`
`filter == 0b01` ("all except self") path masked the target list to
`own_cpu_mask()`, which with `dbg_vcpu1` armed *includes the sender*. Now masked
with `~(1u << me)`.

## The board-free CI gate had been entirely red

Every one of the ten sections of `ci.sh`'s QEMU half was failing, and had been
for some time. `stage2.h`'s `STAGE2_WX_DYNAMIC` defaults to 1, which maps guest
DRAM writable-but-execute-never and relies on a stage-2 permission fault to
promote each page on first fetch. The board's `el2_exc.c` routes those to
`stage2_wx_fault()`; all **nine** QEMU targets link their own `el2_trap()` and
**none** had the hook, while all of them link `stage2.o`. Every target died on
its guest's first instruction fetch (`ESR=0x8200000e`).

Confirmed against a pristine HEAD checkout in a throwaway worktree, which is the
check that proved it was nobody's work-in-progress. That matters: while the gate
is red, every change goes straight to hardware with no board-free safety net —
which is exactly what `SESSION-RULES.md` says not to do.

Fixed by **wiring the hook, not** by building the targets with
`-DSTAGE2_WX_DYNAMIC=0`. A regression gate that goes green by disabling the
policy under test is worse than a red one.

Two further failures were hiding behind that one:

- **zephyr-qemu**: the real Zephyr image is built for `bpi_m64_hv`, whose DTS
  describes the **A64** GIC (GICD `0x01C81000`) that QEMU virt does not have, so
  `arm_gic_init()` faulted forever. Reused `mmio_absorb.c`'s catch-all from the
  dual targets — but **gated on the IPA being outside guest DRAM**, because
  `mmio_absorb_fault()` never returns 0 and absorbing unconditionally under
  `stage2.c`'s identity map would swallow a genuine guest bug and turn a real
  failure into a green run.
- **vgic-qemu flaked ~1 in 3**: `guest_config()` leaves `VBAR_EL1 = 0` and the
  self-test installs its vectors a few instructions in, but `vgic_init()` has
  already opened the virtual interface — so a vIRQ in that window was delivered
  against VBAR=0 and the guest branched to IPA `0x280` (an AArch64 vector
  table's Current-EL-SPx IRQ slot) and died. Injection is now gated on
  `VGST_VBAR_SET`, published in the instruction right *after* `write_vbar_el1()`
  — not on the magic, which the payload writes *before*. 8/8 clean after.

### And the structural fix, so it cannot recur the same way

Nine hand-written `el2_trap()`s each re-implemented PSCI SYSTEM_OFF, the fault
dump, and the guest lower-EL sync chain. Nine copies of a dispatch chain is
*why* a policy change in `stage2.h` reached zero of them.
`el2_exc_qemu_common.{c,h}` now owns the shared half; each variant keeps its own
`el2_trap()`, tick schedule, pass criteria and PASS/FAIL strings — a gate's
verdict logic belongs in the gate.

Behaviour was preserved deliberately, **including things that are arguably
wrong**: `hvc_advance_elr` is per-target because `el2_exc_dual2_qemu.c`'s
documented finding (ELR_EL2 already points past an HVC, so `el2_exc_qemu.c`'s
`+= 4` is a latent bug that is merely dead code there) must not be silently
harmonised away. Four variants keep their own fault reporter, because their CI
scripts grep literally for `FAIL`, `CPU3 FAULT` or `UNEXPECTED TRAP` while the
shared helper prints `FAULT` — delegating those would have broken the gate while
looking like a cleanup.

An independent audit pass compared all nine files against their pre-refactor
versions specifically on the **fault paths CI never exercises** (terminal
halt-vs-poweroff, greppable markers, dropped fields, dispatch order, weak
references against each target's object list) and found **no functional
regression**; its two findings were inaccurate comments, since corrected.

Result: `ci: ALL BOARD-FREE CHECKS PASS` across all ten sections.

Two things about running it, both learned by getting them wrong:

- Run `ci.sh` **with nothing else on the host.** Concurrent `make` in one
  directory races on shared objects, and — separately — `dual-rearm-qemu-ci` and
  `dual-zephyr-qemu-ci` are genuinely load-sensitive: they bring up all four
  cores by PSCI `CPU_ON` under QEMU TCG, and under host CPU contention a core
  can miss its window (`smp_num_online()=3 online-bitmap=0x0000000b`, i.e. CPU2
  absent). Both pass **6/6** run alone and failed once during a run that had a
  `make dbg` alongside it. That is a property of the harness, not a regression —
  do not go hunting for a code bug when you see it.
- `vgic-qemu-ci` used to be flaky ~1 in 3 for a real reason (the VBAR race
  above), and is now 8/8. If it starts flaking again, that is a genuine signal.

## The microSD story is finished

0.0.1 left this open: *"Real capacity is a hardcoded 1 GiB stub (`sd_bio.c`
discards the CSD response instead of parsing it) against an actual 64 GiB
card."* Now:

- The CSD **is** parsed. The word order was the trap: `RESP0`=LSB … `RESP3`=MSB
  (the SDHCI convention for this SMHC IP), not the naive opposite — real 58 GiB
  detected and grown into.
- The card is GPT-partitioned instead of one whole-disk filesystem: `/var`
  (8 GiB) and `/opt` (~50 GiB, 44 GiB free) for build trees and sources.
- **A real bug was found by the first broad LBA sweep**: `sd_serve_data()` had a
  3-try bounded retry on the WRITE path and **zero** on READ — both the plain
  read and the read-modify-write step of a partial write. A single transient
  failure returned `SD_RC_IOERR` immediately, and a background `fsck` hit it and
  got `/opt` force-unmounted (`vtbd1: hard error cmd=read`). Symmetric retry
  now. If another path on this hardware is added later, default to symmetric
  retry unless there is a specific reason reads are more reliable than writes —
  there wasn't one here, and assuming so is what caused this.

## `gr1`/`sr1`: CPU1's vCPU is now readable — verified live

CPU1's own guest register state had no reader at all; `gr`/`sr` deliberately
show CPU0's, and `el2_exc.c` gates its snapshot to CPU0 precisely so two vCPUs'
registers can never be mixed into one report. There is now a second,
independent frame snapshot with its own seqlock, captured under the mirror image
of that gate, plus `gr1`/`sr1` in dbgmon. "Never captured" is deliberately
distinguishable from "captured, and the registers really are zero".

Verified on the board, and the interesting part is the independence proof —
sampling alternately:

    gr   elr(pc)=ffff000000931bc0 spsr=600000c5
    gr1  elr(pc)=ffff00000091ebd8 spsr=60000005
    gr   elr(pc)=ffff000000584990 spsr=20000005      <- CPU0 moved
    gr1  elr(pc)=ffff00000091ebd8 spsr=60000005      <- CPU1's own, unchanged

`gr` tracks CPU0's guest as it traps; `gr1` holds CPU1's own value and never
shows CPU0's PC. `sr1` reads sane live EL1 state for that core
(`VBAR_EL1=ffff000000924800` — the guest's real vectors, not the 0 that
`guest_config()` leaves).

## HDMI vblank as a real interrupt — wired, verified, and partly disappointing

The HUD/vblank duty was the last of CPU1's old tight-loop jobs with no
equivalent after that core became a guest vCPU. It is now driven by the real
TCON1 interrupt, located from evidence rather than guessed:
`lcd-controller@1c0d000`, `interrupts = <0 0x57 0x04>` -> GIC SPI 87 -> **INTID
119**, whose `reg` matches `hdmi.c`'s `TCON1_BASE` exactly and which `/aliases`
confirms as `tcon1` feeding `hdmi_out_tcon1`.

**Verifying it is what made this section worth writing.** Reading the
distributor back showed neither the new HDMI path *nor the supposedly-proven
MUSB one* was reaching CPU1 at all:

    CPU0 irq_counter[103] +133/s    irq_counter[119] +80/s
    CPU1 irq_counter[103] frozen at 3783, [119] frozen at 97
    CPU0 musb_throttles 47168 — one throttle per interrupt taken

The guest's own GIC driver resets `GICD_ITARGETSR` across the SPI range to the
boot CPU during attach, so a one-shot "target CPU1 only" does not survive guest
boot. Both lines fired on CPU0 — the guest's primary vCPU — which is precisely
the hardware race `usbacm.h`'s "KNOWN OPEN RISK" section warns about, silently
stealing guest cycles. And because the per-tick budget reset lives in the
CPU1-only tick block, CPU0's counter grew forever, stayed over the ceiling, and
masked the line on *every* interrupt, which CPU1's tick then re-enabled: an
enable/mask ping-pong at tick rate.

Fixed the way this tree already handles "the guest's driver keeps undoing our
setting" for the PC5 eMMC pinmux: re-assert the targeting from CPU1's tick,
write only when it drifted. Plus a core gate on both dispatch arms — a
wrong-core arrival is EOI'd and counted, never serviced and never allowed to
reach `vgic_inject_hw()`. After: `ITARGETSR` byte `0x02`, CPU0 taking zero of
both, `wrong_core` counters flat at 0, CPU1 taking INTID 119 with
`g_vblank_count` advancing at exactly the same rate.

**Two measurements that did not come out as hoped:**

- **MUSB genuinely storms**: ~646 interrupts/s on a completely idle CDC-ACM
  console, exhausting the 32-per-tick budget ~29 times/s. So that budget is
  containing a real defect in `usbacm_poll()`'s source de-assertion, not
  hypothetical hardening. Bounded and harmless where it is, but it is a real
  open bug in `musb.c`/`usbacm.c`, not a clean path.
- **The vblank arrives at ~18 Hz, not the ~60 Hz** the old tight-loop polling
  measured (0.0.1 reported 60.04 Hz). NOT a throttle artifact —
  `hdmi_throttles` stays 0 and the ceiling is 3200/s. Unexplained. The most
  plausible candidate is the guest's own KMS/lima driver, which also reads
  `TCON_INT0` and may be consuming latches. **Do not treat vblank pacing as a
  finished 60 Hz feature.**

One assumption remains unproven by construction: `TCON_INT0` bit30 as the
TCON1 vblank *enable* bit. It reads back as 1 and interrupts do arrive, which is
strong evidence, but the bit position itself comes from the register's
16-bit-offset enable/status symmetry (bits 30/31 enable what bits 14/15 report)
plus the cross-SoC-family Linux convention — not from this board's DTB.

## Known broken — changes since 0.0.1

Everything in 0.0.1's list still stands except where noted, plus:

- **NEW: `python3.12` on the guest segfaults.** `python3 -c "pass"` dies with
  SIGSEGV inside `Py_InitializeFromConfig` (`python3 --version` "works" only
  because it exits before running bytecode). `ktrace` shows SEGV_MAPERR right
  after an `mmap` that itself succeeds; `pkg check -s` passes, so this is not
  file corruption; a core dump dated **2026-08-23** proves it predates this
  release. **Consequence: `meson` cannot run, so no meson-based build — the
  native Mesa build included — is possible on the board at all.** This narrows
  "the board is also a build host" considerably: `cc`/`ninja`/`make` still work,
  anything Python-dependent does not. Repair needs `pkg install
  --force-reinstall`, which needs `mount -uw /` on a deliberately read-only
  root — treat it as a maintenance window, not a side quest.
- **NEW: the Mesa sources are staged but unbuilt.** `/opt/src/mesa-26.2.0`
  (399 MB, complete) is in place for a native build that cannot start until the
  above is fixed. The verified `meson setup` option set already exists in
  `../bsdOS/hal/lima/mesa/FEASIBILITY.md` — do not re-derive it.
- **The RSP channel goes dark in long GDB sessions** — still unfixed. Note that
  the EMAC channel death described earlier in this file was a *different*,
  now-fixed bug (`dbg_core_active`); do not treat that fix as covering this one.
- **`holdtest` TEST B is INCONCLUSIVE**, unchanged: a documented QEMU-TCG
  self-modifying-code artifact on an already-executed page, not expected on real
  silicon. That target is not part of `ci.sh`; `run_holdtest.sh` runs it.
- **The "100 clean boots" streak was reset** by this session's reload cycles
  (best 130, current 5). The gate's own reasoning in `ROADMAP.md` is unchanged
  and it stays closed; noted only so nobody reads the current counter as a
  regression.
- **One board.** Unchanged, and still the largest caveat in this file.

## Portability: the first real step, taken deliberately now

The stated product direction is the Allwinner Banana Pi family and the
PinePhone. One fact makes half of that much cheaper than it sounds: **the
PinePhone is the same Allwinner A64 die as the BPI-M64** — same GIC-400, CCU,
PIO, watchdog, SMHC, MUSB and PHY, same DE2 and Mali-400, same AXP803 PMIC. It
is not a new platform, it is a second board on a known SoC.

"The whole Banana Pi family" is not one family, though: M64 is A64, M2+ is H3,
M3 is A83T, M2 Zero is H2+, M2 Berry is V40 — but M5 is Amlogic, R2 is
MediaTek, and F3 is RISC-V. The coherent target is the Allwinner subset; the
rest are different projects wearing the same brand.

**Two findings that matter more than the address consolidation itself:**

- **The PinePhone has no Ethernet.** This project's differentiator — a debug
  plane that outlives the guest: dbgmon, the GDB stub, coredumps and snapshots
  over the network, `hvdbg` — is built entirely on EMAC. On the PinePhone it
  would have to move to USB. That promotes the MUSB storm measured above from a
  bounded nuisance to a defect on the critical path of the stated direction.
- **The display path genuinely differs** even on the same die: MIPI-DSI off
  TCON0 rather than HDMI off TCON1. DE2, Mali-400, zero-copy and KMS carry over;
  the output stage does not.

Those two converge on one conclusion worth writing down: the single investment
that serves both the feature axis and the portability axis is making the debug
plane **transport-independent and reliable**. The feature axis needs it stable
(source-level GDB, the one genuinely broken headline, is blocked behind channel
reliability); the portability axis needs it non-Ethernet. Same work.

### What was actually done here

`soc_a64.h` now holds every A64 peripheral address in one place. Before it, 48
`#define`s across 44 files each carried their own copy, and the same address was
often spelled several ways — the GIC distributor at `0x01C81000` had **eight
names across 19 files** (`GICD_BASE`, `GICD_CTLR_ADDR`, `VGICD_BASE`,
`VGIC_GICD_BASE`, `VBLK_GICD_BASE`, `VBLK_SD_GICD_BASE`, `VINPUT_GICD_BASE`,
`VNET_GICD_BASE`); MUSB, CCU and SRAMC had two each; `WDOG_CTRL`/`CFG`/`MODE`
were defined twice each in different letter case. That is not untidiness, it is
the mechanism by which a port breaks: change seven of eight and the eighth
silently keeps the old address.

Deliberately a *pure consolidation*, not a HAL: local names are kept and simply
re-pointed at the canonical ones, so no use site changed and nothing is selected
at runtime. Verified the only way that claim can be verified — **the compiled
binary is byte-for-byte identical**, same md5 before and after — plus the full
board-free gate, plus a boot on hardware.

Done now rather than after the pause for a specific reason: it is a
44-file mechanical change on a bare-metal image where a wrong address means a
board that does not boot, and physical access to press reset is the one resource
that disappears during a months-long pause. The green board-free gate is what
made it reviewable at all.

**Still hardcoded, and named rather than quietly left:** the load address
(`0x42000000`) and DRAM window in `link.ld`/`stage2.h`; RSB/AXP803 register
knowledge; the GIC SPI numbers, which stay with the drivers that cite them from
the DTB; and the HDMI/DE2 pipeline's assumption of an HDMI sink.

## Core allocation in the default build — CHANGED

**CPU0** guest vCPU0 · **CPU1** guest vCPU1 *(was: debug/watchdog plane)*, with
the watchdog kick and dbgmon moved onto its unmaskable 10 ms tick · **CPU2**
async eMMC I/O · **CPU3** idle, or a concurrent Zephyr guest in `make dual`.

Giving the guest all four cores is still not done: CPU2's async I/O is
load-bearing, and CPU3 is architecturally exclusive with the second-guest build.
