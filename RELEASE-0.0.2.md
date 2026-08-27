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

**Superseded the same day.** This table is the historical record of the first
jump, from 1 vCPU to 2 — it is not what a fresh boot of this tree reports
today. CPU2 joined as a genuine third vCPU later on 2026-08-27: `hw.ncpu=3`,
`kern.smp.cpus=3`, and it is now armed in the default build alongside CPU1.
See "A third guest vCPU, hardware-proven" below for the fixes it took, what it
cost, and a correction of my own that belongs on the record.

**And CPU1's own tick cost the guest something worth measuring separately: its
network.** `vnet_emac.c` states the guest's virtio-net RX is fed exclusively
from inside `emac_poll()` on CPU1 — true before this release (a tight loop)
and true after it (now `dbgmon_service()` called once per 10 ms EL2 tick,
`gic_timer.c`'s tick handler). A packet that arrives just after a tick fires
waits up to one full period before the guest's ring sees it. Measured: 40/40
ICMP packets to the guest, zero loss, RTT 0.47–9.4 ms, mean 4.2 ms — against
0.17–1.6 ms for the LAN gateway over the same link, for comparison. Not noise:
the spread is the shape a 10 ms polling period produces. This bounds guest
network latency, and probably throughput, architecturally — it is not a bug to
fix so much as a cost of the tick design to know about.

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

**Residual risk, and it is smaller than this section first claimed.** A
*software* hang of the guest on CPU1 can no longer starve the watchdog. An
earlier draft here went on to say a *hardware*-level wedge of that core still
can, "because if the core stops fetching instructions, EL2's tick handler
cannot run either" — which is backwards, and worth correcting rather than
softening: **if that core stops kicking the watchdog, the watchdog is exactly
what fires.** Not kicking it is the recovery path.

`wdt.c`'s layering closes the loop, and already documented it. There are two
feeds and only one is unconditional: `wdt_debug_kick()` from CPU1's tick,
unconditional; and `wdt_pet()` from `el2_trap()` on any core, **gated on the
guest having emitted a console byte within `WDT_TIMEOUT_S`** — so a board that
traps busily while producing nothing (the shape of a wedge) is not held alive by
it. So: CPU1 dies → the guest blocks on its first IPI to that vCPU → progress
goes stale → `wdt_pet()` stops re-arming → the hardware timer fires within 16 s
→ U-Boot → chimpd reloads. No human.

What genuinely remains is narrow: CPU1 hardware-dead *while the guest somehow
keeps printing* leaves the board resident but degraded — one vCPU gone, and no
debug channel, since dbgmon rides that core's tick. For an SMP guest that
combination is unlikely; it is not impossible.

That risk is live in the default build rather than opt-in.
`board-config.xml` holds the one attribute that disarms it (`vcpu1
enabled="false"` plus `python3 gen_config.py`, which flips the build flag and the
`cpu@1` DTB node together).

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

The heading above is about the driver, and stands. It is not a claim that
`/opt`'s filesystem itself has seen no further trouble since — it has, during
the three-vCPU work later in this file; see "`/opt` corruption during the
three-vCPU build" near the end.

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
  console, exhausting the 32-per-tick budget ~29 times/s. **Root-caused since:
  it is not inherited hardware behaviour, it is this release's own
  regression.** Commit `6cf4215` (arming CPU1 as vcpu1) enabled
  `REG_INTTXE`/`REG_INTRXE` so an IRQ-driven `usbacm_poll()` could see EP0/EP1
  activity — and nothing in this tree had ever read the matching status
  registers, `REG_INTTX`/`REG_INTRX`; the driver polls CSR-level bits
  directly instead. A latched endpoint bit — guaranteed during enumeration —
  holds the single OR'd "mc" SPI line asserted, and `gic_timer_irq()`'s
  EOI-then-recheck sequence re-presents it at once. Fixed by draining and
  writing back both registers in `musb.c`. **NOT hardware-validated, and the
  arithmetic does not fully close**: 646/s against a 32-per-tick ceiling at a
  100 Hz tick does not cleanly fit a permanently-asserted line. The mechanism
  is established from the code; the size of the effect still needs the
  measurement.
- **The vblank arrives at ~18 Hz, not the ~60 Hz** the old tight-loop polling
  measured (0.0.1 reported 60.04 Hz). NOT a throttle artifact —
  `hdmi_throttles` stays 0 and the ceiling is 3200/s. Unexplained. The most
  plausible candidate is the guest's own KMS/lima driver, which also reads
  `TCON_INT0` and may be consuming latches. **Do not treat vblank pacing as a
  finished 60 Hz feature.** The two counters cited to rule out the CPU1-side
  causes (`hdmi_wrong_core`, `hdmi_throttles`) had no live export — CPU1's
  vblank SPI and `IRQ_COUNTER_BC_BASE`'s CPU0-only window meant the numbers
  above were a one-off nobody could re-take. Both are now published from
  CPU1's tick into `BC_HDMI_BASE` words 13/14, diagnostic only. The leading
  explanation for the ~18 Hz figure is now same-GIC-priority contention on
  CPU1: INTID 119, 103 and 30 all sit at `TIMER_PRIORITY`, and a same-priority
  IRQ cannot preempt on GICv2, against a status bit that latches rather than
  counts.

One assumption remains unproven by construction: `TCON_INT0` bit30 as the
TCON1 vblank *enable* bit. It reads back as 1 and interrupts do arrive, which is
strong evidence, but the bit position itself comes from the register's
16-bit-offset enable/status symmetry (bits 30/31 enable what bits 14/15 report)
plus the cross-SoC-family Linux convention — not from this board's DTB.

## Known broken — changes since 0.0.1

Everything in 0.0.1's list still stands except where noted, plus:

- **FIXED during this release: `python3.12` on the guest segfaulted.** An earlier
  draft of this bullet said "`pkg check -s` passes, so this is not file
  corruption" — that was wrong twice over, and the correction is in its own
  section below ("The board's own build environment"). It *was* file corruption:
  70 python312 files and 6 meson files damaged at exactly their correct length,
  stale wreckage from the already-fixed `rd_cntpct()`-runs-backwards era. `pkg
  check -s` said nothing because that package's file list in pkg's database is
  empty. Repaired from intact references already on the board; a sweep of every
  package with an exact-version cached reference found no further damage.
- **The Mesa sources are staged and building, now on three vCPUs.**
  `/opt/src/mesa-26.2.0` (399 MB, complete). `meson setup` completes on the
  board (`Gallium drivers: lima`, EGL and GBM enabled) and `ninja` is working
  through 993 targets — resumable with `ninja -C /opt/build/mesa`, not
  restartable from scratch. The verified option set is in
  `../bsdOS/hal/lima/mesa/FEASIBILITY.md` — do not re-derive it. `PyYAML` and
  `ply` were missing and are staged under `/opt` on `PYTHONPATH` rather than
  installed, so they cost no root write. This build is also the one that hit
  the `/opt` filesystem panic below — recovered, cause not resolved.
- **`/opt` (the SD card) suffered a filesystem panic mid-Mesa-build and was
  recovered, but the cause is not resolved.** See "`/opt` corruption during
  the three-vCPU build" near the end of this file.
- **The RSP channel goes dark in long GDB sessions** — still unfixed. Note that
  the EMAC channel death described earlier in this file was a *different*,
  now-fixed bug (`dbg_core_active`); do not treat that fix as covering this one.
- **`holdtest` TEST B is INCONCLUSIVE**, unchanged: a documented QEMU-TCG
  self-modifying-code artifact on an already-executed page, not expected on real
  silicon. That target is not part of `ci.sh`; `run_holdtest.sh` runs it.
- **The "100 clean boots" gate is now MET on this build.** 100 cycles, zero
  failures, on the SoC-consolidated image with vcpu1 armed; the cumulative
  ledger reads current 110, best 130, 367 total. An earlier draft of this bullet
  recorded the streak as reset to 5 by reload cycles — that was true when
  written and is superseded. `boot_streak.py` drives it, deliberately via
  `board_ctl.force_to_uboot()`'s proven ladder rather than
  `reliable_load.py --cycles N`, whose EMAC self-reboot failed 3/3 when tried.
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
**22-file** mechanical change on a bare-metal image where a wrong address means
a board that does not boot, and physical access to press reset is the one
resource that disappears during a months-long pause. The green board-free gate
is what made it reviewable at all.

(Three different counts live in this section and it is worth pinning them,
because an earlier draft of this paragraph conflated two of them and a later
correction conflated two others. **48 `#define`s across 44 files** carried a
duplicate copy of some address — that is the size of the *problem*. The commit
itself, `88dea8c`, touches **24 files**: 22 migrated source files, plus the new
`soc_a64.h`, plus this release note. So **22** is the number that answers "how
much code was re-pointed", and it is the one this paragraph means.)

**Still hardcoded, and named rather than quietly left:** the load address
(`0x42000000`) and DRAM window in `link.ld`/`stage2.h`; RSB/AXP803 register
knowledge; the GIC SPI numbers, which stay with the drivers that cite them from
the DTB; and the HDMI/DE2 pipeline's assumption of an HDMI sink.

## Core allocation in the default build — CHANGED AGAIN

**CPU0** guest vCPU0 · **CPU1** guest vCPU1 *(was: debug/watchdog plane)*, with
the watchdog kick and dbgmon moved onto its unmaskable 10 ms tick · **CPU2**
**guest vCPU2** *(was: async eMMC I/O offload)*, hardware-proven 2026-08-27 ·
**CPU3** idle, or a concurrent Zephyr guest in `make dual`, or (wired but
**not** armed on hardware) a fourth FreeBSD vCPU via `vcpu3.c`.

`board-config.xml` now ships `vcpu1` and `vcpu2` both `enabled="true"` — the
guest is SMP across **three** cores by default, not two. CPU3 is the only core
still withheld, and for a specific, named reason: the three fixes that made
CPU2 work (`vgic_init()`+IRQ/FIQ unmask, CPU2's own tick, `own_cpu_mask()`
learning about every armed core generically) are all generic now, so CPU3 is
*expected* to behave the same way — but expected is not measured, and it is
mutually exclusive at link time with the `dual` build's Zephyr-on-CPU3 guest
regardless.

The trade CPU2 made: it no longer offloads eMMC I/O asynchronously.
`vblk_async_post()` gates on `g_vblk_async_ready`, and the board has always run
the synchronous fallback when that offload isn't available, so this is a
performance regression, not a correctness one. The risk to watch is
contention: three vCPUs can now all reach the synchronous eMMC path and
serialize on one unfair test-and-set lock, and two-way contention there has
already cost this project a root filesystem once. The mitigating fact for the
immediate use case is that root lives on eMMC read-only, while the Mesa build
below writes through `vblk_sd` (the SD card) instead — a different device, a
different lock. See "/opt corruption during the three-vCPU build" below for
why that distinction matters and is not, on its own, a full alibi.

## The board's own build environment: repaired, and a false alarm worth recording

The board has been its own build host since July. That stopped being true at
some point: `python3 -c "pass"` segfaulted, which blocked `meson`, which blocked
the native Mesa/lima build.

**It looked like our bug, and the trail was a good one.** `ktrace` put the
SEGV_MAPERR immediately after an anonymous `mmap()` that had itself *succeeded* —
and this hypervisor maps guest DRAM writable-but-execute-never, promoting pages
on demand from a fixed pool (`STAGE2_WX_DYNAMIC`). "The kernel granted a mapping
and the first touch faulted" is precisely the shape a bug in that mechanism
takes.

It was not that. The new `guest_memtest.c` says so by measurement rather than
argument: anonymous RW at four sizes, anonymous RWX, and — the step that matters,
because the first five would pass even with promotion entirely broken — write
bytes, `mprotect` them `R|X`, and *call* them. All six pass on hardware; the
called page returns 42. **The lesson is cheaper than the diagnosis: `ktrace`
names the last syscall before a SIGSEGV, not its cause.** A userland process
faults between syscalls, so adjacency in a trace is not evidence.

The real fault was on disk. Working back from the register state — `ob_type ==
NULL` on a static exception type, so CPython was raising an error before
`PyType_Ready` had run on the type it needed — the actual message recovered from
the core was `"non-string found in code slot"`: the *frozen* `importlib` bytecode
compiled into `libpython3.12.so.1.0` would not unmarshal. Comparing all 8027
files of the package against an intact reference already sitting in
`/var/cache/pkg`: **70 files corrupt at exactly their correct length**, scattered
across headers, `lib-dynload/*.so`, stdlib `.py`, test data and a bundled
`.whl` — the signature of the era when `rd_cntpct()` could run backwards. That
cause is fixed; this was its stale wreckage. `meson` had 6 more of the same.

`pkg check -s` had said python312 was fine, and that meant nothing: its file
list in pkg's database is *empty*, so there was nothing to check. Both packages
with an emptied list — python312 and meson — are ones an earlier `pkg install`
of mine was killed inside; the other 121 are intact, and a full sweep of every
package with an exact-version cached reference found **zero** further damaged
files. The blast radius really was those two packages.

Repaired from the local references, with `pkg` deliberately not involved (it is
the thing that crashes here). Both write windows put the read-only root back
with a `trap`, not a line at the end of the happy path — earlier the same day a
`pkg` crash inside such a window left root writable precisely because the
cleanup was unreachable. `fsck_ffs -n` clean before and after, identical file
counts. `PyYAML` and `ply` were missing outright and are staged under `/opt` on
`PYTHONPATH` instead of installed, so that need cost no root write at all.

Proven by the task rather than by version strings: `meson setup` now completes
on the board (`Gallium drivers: lima`, EGL and GBM enabled) and `ninja` is
building all 993 targets.

## A third guest vCPU, hardware-proven — three fixes, found in this order

Tried on hardware 2026-08-27, because the board is now the project's own build
host and two cores make that slow. It took three fixes, landed as three
separate attempts, and is now armed in the default build.

### Fix 1: the same `vgic_init()` gap vcpu1 already had

`vcpu2.c` had sat in the tree since 2026-08-21 doing the hard parts right —
`stage2_arm_secondary()` against the banked `VTTBR_EL2`, the banked
`HCR_EL2` IMO/FMO/TSC bits — and missing the two things that made vcpu1 work:
it never called `vgic_init()`, and it never unmasked EL2 IRQ/FIQ. Under this
build's `IMO=1` policy the only route from a physical interrupt to a guest core
is a GICH List Register, and GICH is banked per-PE: a core that skipped its own
`vgic_init()` has `nr_lr == 0`, so everything it takes — including the
rendezvous IPI FreeBSD sends immediately after `Release APs` — queues into a
list nothing drains. **0.0.1 recorded exactly this symptom for CPU2 and 0.0.2
root-caused it for CPU1; the fix was applied to `vcpu1.c` and never carried
across.** Two calls, derived from a hardware-proven pattern.

**This got the guest past the point every previous attempt died at.** Three
cores enumerated, and the line no earlier attempt had reached:

```
CPU  0: ARM Cortex-A53 r0p4 affinity:  0.
CPU  1: ARM Cortex-A53 r0p4 affinity:  2.
CPU  2: ARM Cortex-A53 r0p4 affinity:  1.
gic0: using for IPIs.
Release APs...done.
```

(The affinity order is not a bug: `gen_config.py` adds nodes with `fdtput -p`,
which PREPENDS, so `/cpus` read `cpu@2 cpu@1 cpu@0` and FreeBSD's logical
numbering followed DTB order rather than MPIDR. Same prepend footgun that once
swapped `vtbd0`/`vtbd1`.)

**Then it stalled at root mount, and not in the way anyone predicted.** Not the
eMMC-lock contention the trade was expected to risk — the console stayed alive,
EL2 kept ticking, the isolation self-check passed, `g_ioerrs` was 0 and the
block lock was free. What `triage.py` found (preserved in
`crash-20260827-133425/`):

```
INTID 137  en=1 pend=0 act=1 cfg=EDGE   virtio-blk (SPI 105)
GICH_LR0:  vINTID=27 state=1(pending) HW=0
```

**`act=1`.** Something read IAR for virtio-blk's SPI and nothing ever
deactivated it, and an Active SPI is never delivered again — hence exactly two
disk reads (`g_reads=2`, the last being the GPT backup header) and then silence
forever. Disarmed rather than chased live at that point, since the board was a
working build host and this was not yet understood.

### Fix 2: CPU2 had no periodic tick of its own

`vtimer_mask_watchdog()` — the rescue for FreeBSD's CNTV mask self-latch —
runs exclusively from the TIMER_INTID arm of `gic_timer_irq()`, which only
fires on a core that has armed its own CNTP comparator. CPU0 gets one from
`main_dbg.c`, CPU1 from `vcpu1.c`; `vcpu2.c` had neither, so its vCPU had no
path to that rescue at all. That is what left virtio-blk's SPI stuck `act=1`
above: without the rescue, CPU2's vCPU stopped servicing and the HW=1 List
Register it held was never EOI'd. Ported `vcpu1.c`'s
`gic_timer_arm_preserving_cntvoff()` arm verbatim (same 10 ms period, same
ordering — after `vgic_init()`, before `daifclr`).

Re-armed with this fix, `act=1` was gone (`act=0`), which also confirmed the
mechanism. But the guest still didn't finish booting — a new failure appeared
in its place, recorded live in `crash-20260827-vcpu2-livelock/finding.md`
along with a new breadcrumb window (`HVMAP_VGIC_BC_HI`) giving CPU2/CPU3 their
own vgic lanes for the first time, without which the next measurement could
not have been taken at all.

### Fix 3, and the one worth remembering: `own_cpu_mask()` never learned about CPU2

CPU2's new lane showed ~1174 vgic injections/second, every one succeeding,
zero drops, empty pending queue. **My first read of that number was wrong, and
the correction belongs on the record rather than being quietly folded in.** I
called it a livelock caused by CPU2, and went looking for a `CNTVOFF_EL2`
timebase disagreement between CPU0 and CPU2 to explain a storm. The hypothesis
was refuted by reading the source, not by burning a board cycle: `vgic_init()`
zeroes `CNTVOFF_EL2` unconditionally on every core that calls it — CPU0, CPU1
and CPU2 all do — and `gic_timer_arm_preserving_cntvoff()` only preserves
whatever was already there, which is already zero. There was no timebase to
disagree about.

The real cause was in `vgicd.c`. `own_cpu_mask()` — the function that polices
every guest write to `GICD_SGIR`/`GICD_ITARGETSR`, the only two paths that can
cross a core boundary — was extended on 2026-08-25 so CPU0/CPU1's rendezvous
IPIs could reach each other, and it hardcoded that exact pair. CPU2 became a
real third vCPU two days later and this function was never told: a rendezvous
SGI aimed at CPU2 still computed `{0,1}`, so **CPU2's bit was stripped from the
target list before the write ever reached the real distributor, in both
directions.** CPU2 was never spinning in a storm — it was running a perfectly
healthy per-CPU clock at a normal rate, simply cut off from the other two, which
were the ones actually stuck: CPU0 and CPU1, parked in `smp_rendezvous_action()`
waiting for an acknowledgement from a core nobody was able to ask. Their low
injection counts (71 and 3) were the real signature of "parked", exactly as
suspected — the reason was just on the other side of the wall.

**The lesson generalises past this one bug**: a busy core running normally next
to two parked ones should prompt "who cannot talk to it", not "the busy core is
the problem." Every number in the original measurement was correct; the story
built on top of it was not. Fixed by generalizing `own_cpu_mask()` to compute
its "same guest" group from every core's own `dbg_vcpuN` gate instead of a
hardcoded pair — CPU0 is always in the group, CPU1/2/3 join iff their own gate
is armed, and a build with nothing beyond CPU0 armed reduces to the old
behaviour byte-for-byte. This also covers `vcpu3.c` generically, without
touching the `dual` build's CPU3-as-Zephyr isolation.

### Result

**Hardware-proven, 2026-08-27, and armed in the default build**: `hw.ncpu=3`,
`kern.smp.cpus=3`, `cpu2:rendezvous` nonzero in `vmstat -i` (the counter that
was structurally impossible before — no cross-core SGI could reach CPU2 at
all), and three parallel spinners each completing identical work in the same
wall clock, which two cores cannot do.

`vcpu3.c` exists and is wired (PSCI filter, dispatch, breadcrumb window,
`VCPU3` flag, a `vcpu3` dbgmon command) and is **deliberately NOT armed**. All
three fixes above are generic now, so CPU3 is *expected* to work the same way
— but expected is not measured, and vcpu3 has no hardware track record at all
yet, unlike vcpu1/vcpu2 going into their respective fixes. Its exclusivity
with `dual`'s Zephyr-on-CPU3 is a **link error**, not a comment: both files
define `bzdos_cpu3_owner` incompatibly, and linking the two objects together
fails on purpose — verified directly (`ld --unresolved-symbols=ignore-all`
against both `.o` files), not asserted.

One tooling gap closed on the way out. `gen_config.py` knew about both
directions of the DTB/build-flag mismatch it exists to prevent and only acted
on one: a `cpu@N` node left behind by a disabled feature got a printed NOTE and
nothing more. Disarming vcpu2 mid-session therefore left `cpu@2` in the DTB
with `VCPU2=0` — FreeBSD would enumerate a third core, ask for it by PSCI, and
EL2 would refuse. A note you have to notice is not a safeguard; it removes the
node now.

## `/opt` corruption during the three-vCPU build — cause not resolved

Ten minutes into building Mesa on three cores, the guest panicked:
`ffs_valloc: dup alloc` on `/opt`, the SD card described in "The microSD story
is finished" above. `fsck` recovered 1555 orphaned files.

The likely cause is mine, not the hardware's: the board was reset four times
that day with `/opt` mounted read-write under an active build, and it was
never checked for damage afterward — the classic shape of an unclean unmount.
**But it cannot be cleanly attributed.** Some of the damaged inodes carry
mtimes from *during* the three-vCPU run itself, which keeps three-way
concurrency on the `vblk_sd` path alive as a second, unexcluded suspect — a
different device and a different lock from the eMMC contention risk named
above, but the same shape of bug this project has hit before. Stated as an open
question on purpose, not resolved in either direction here.

The experiment that would separate them is named, not run yet: a build that
goes start-to-finish with zero resets, then checking `/opt` for damage
afterward.
