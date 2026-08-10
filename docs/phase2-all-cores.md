# Phase 2 — all four cores usable by guests

**Design document. Nothing here is implemented.** No `.c`/`.h`/`.S` file and no
Makefile line was changed by the pass that wrote this. The predecessor is
`docs/dual-guest.md` (Phase 1: FreeBSD on CPU0 + a genuinely concurrent second
guest on CPU3, hardware-confirmed 2026-08-10).

## 0. How to read the claims in this document

Every non-trivial statement is tagged with how it was established. This project
has repeatedly been damaged by confident text that turned out to be a guess (see
`docs/war-stories.md`, and `ORIENTATION.md`'s rule 4 on banked per-PE registers).

| Tag | Meaning |
|---|---|
| **[code]** | Read directly in the cited source this pass. |
| **[inferred]** | A conclusion drawn from cited code, not itself written anywhere. |
| **[assumption]** | Believed, not checked. Must be checked before it is relied on. |
| **[unknown]** | Openly not established. Do not build on it without measuring. |

No claim in this document was checked against the running board. **The board was
untouched by this pass on purpose** — the primary session owned it.

### 0.1 Working-tree caveat (important for the line numbers below)

At the time of writing, the tree has **uncommitted work in progress** from
another session **[code]** (`git status`): `Makefile`, `dbgmon.c`, `gdbstub.c`,
`gdbstub_hw.c`, `hv_addrmap.h`, `kload.c`, `main_dbg.c`, `main_gdb.c`,
`zguest_cpu3.c`, `zguest_cpu3.h` are modified, and `zstage.c`, `zstage.h`,
`test_zstage.c` are new/untracked. Line citations into **those** files are
against the working tree as of 2026-08-10 and may shift. Citations into
`smp.c`, `wdt.c`, `vblk_emmc.c`, `emmc_bio.c`, `gic_timer.c`, `vgic.c`,
`el2_exc.c`, `stage2.c`, `stage2_zephyr.c`, `vconsole.c`, `snapshot.h`,
`vblk_async.c` are against clean, committed files.

Two of `docs/dual-guest.md`'s "What's still open" items are already stale
because of that in-flight work — see §6.

---

## 1. CPU1: everything that today depends on "CPU1 never runs guest/EL1 code"

`ORIENTATION.md`'s core layout calls CPU1 "the EMAC/debug core; owns the hardware
watchdog". That undersells it substantially. The single `for (;;)` loop at
`smp.c:545-683` is doing **nine** distinct jobs, and at least two of them are on
the critical path of the *FreeBSD* guest, not just of the debugger.

### 1.1 The complete inventory

Everything below is inside, or reached only from, `smp_secondary_main()`'s
`if (cpu == SMP_DEBUG_CPU && dbg_core_enable)` branch (`smp.c:538`).
`SMP_DEBUG_CPU == 1` (`smp.h:43`).

| # | Mechanism | Where | What it does | What breaks if CPU1 runs a guest instead |
|---|---|---|---|---|
| 1 | **Unconditional HW watchdog kick** | `wdt_debug_kick()` `wdt.c:134-139`; called `smp.c:546-547` | Pets the A64 WDOG every loop pass, **not** gated on guest health, unless `wdt_debug_hold != 0` (`wdt.c:132`) | The project's only automatic crash recovery. Detail in §1.2 |
| 2 | **EMAC RX polling — sole owner** | `console_poll()` = `emac_poll()` (`main_dbg.c:90`), called from `dbgmon_service()` at `dbgmon.c:1167`, reached at `smp.c:617` | EMAC has **no interrupt path at all**: `emac.c` contains zero occurrences of `irq`/`interrupt`. Everything RX is polled | The debug channel, `netcon`, `dbgtools`' raw `ETHERTYPE_DBGRAW` peek, `snapshot_net` RX, **and the FreeBSD guest's virtio-net RX** all stop. See §1.3 |
| 3 | **eMMC clock pinmux enforcement** | `smp.c:553-558` | Re-forces PIO `PC5` (`0x01C20848` nibble 5) to function 3 (MMC2 CLK) every loop pass, because FreeBSD's pinctrl muxes every *other* mmc2 pin but leaves PC5 at gpio | Per project memory (`emmc-pc5-pinmux-fix`) PC5 at gpio means no eMMC clock, `OCR=0`, no root filesystem. `emmc_bio_init()` (`emmc_bio.c:317-318`) also forces it, but only **once**, at HV boot, before FreeBSD's pinctrl runs. **[inferred]** the periodic re-enforcement is what makes it stick; **[unknown]** whether FreeBSD actually re-clears it after the initial mux, i.e. whether the periodic pass is load-bearing or merely belt-and-braces. **This is measurable board-free-ish and must be measured before removing it** |
| 4 | **EMAC link self-heal** | `emac_link_watchdog()`, called `smp.c:622-623` | Bounded, rate-limited re-init when EMAC has never accepted an RX frame; optionally escalates to `wdt_debug_hold = 1` | Loss of the "dead-EMAC boot recovers itself" path |
| 5 | **USB-ACM console** | `usbacm_poll()` `smp.c:672-677` | The **only** channel proven alive when EMAC is dark (project memory `breakglass-usb-acm...`). `usbacm.h` documents a single-owner discipline: this is the only place in the tree that touches MUSB registers once the HV is up | The break-glass channel dies. Loses the recovery path that does not depend on EMAC |
| 6 | **EMAC-dark diagnostics** | `emac_status_line_maybe()` `smp.c:128-171`, called `smp.c:678` | Injects an `[EMAC-WDT]` status line into the USB-ACM stream after 30 s of EMAC silence | Diagnostics for the exact failure that is hardest to debug |
| 7 | **dbgtools heartbeat** | `smp.c:572-578` writing `HVMAP_DBGTOOLS_HEARTBEAT` | Protocol-independent proof that CPU1 is alive, answerable from inside `emac_poll()` itself | "Is CPU1 spinning but not answering, or actually stopped?" becomes unanswerable again — the exact gap `dbgtools.h` was added to close |
| 8 | **`dbgmon` command service** (`gr`, `sr`, `r`, `w`, `bc`, `call`, `bmc …`, `zboot`, `zunhalt`) | `dbgmon_service(&snap)` `smp.c:614-617` | The whole live-inspection surface, plus `bmc_dispatch()` (`dbgmon.c:1120`) and arbitrary HV function invocation via `call` | See §1.4 |
| 9 | **GDB-stub RSP hosting** | `smp.c:583-613` | `gdbstub_on_debug_event()` / `gdbstub_poll()`, plus `wdt_debug_kick()` from inside the stub's own spin (`gdbstub.c:890-894`) | The stub loses its host core; see §1.5 |
| 10 | **HDMI HUD + PHY relock** (`HV_HDMI` builds only) | `smp.c:625-663` | `hud_update()` repaint and `hdmi_relock()` (which reclaims RSB and re-enables AXP803 dldo1) | HDMI output dies ~1 s into guest boot when FreeBSD's PMIC driver drops dldo1 |

### 1.2 The watchdog specifically

`wdt.c` implements two independent feeding policies **[code]**:

* `wdt_pet()` (`wdt.c:100-112`) — called from `el2_trap()` (`el2_exc.c:606`) on
  *every* EL2 exception, but **progress-gated**: it only re-arms while the guest
  emitted a console byte within `WDT_TIMEOUT_S = 180 s` (`wdt.c:53`,
  `wdt_note_progress()` fed from `vconsole.c:485`, channel 0 only). A busy wedge
  therefore still reboots.
* `wdt_debug_kick()` (`wdt.c:134-139`) — called only from `smp.c:546-547`,
  **unconditional**. `wdt.c:120-131` states the intent plainly: *"a
  wedged-but-inspectable board is MORE useful than an auto-rebooting one, and I
  trigger resets myself over EMAC"*. And the catastrophic case is still covered:
  *"If CPU1 itself ever dies, petting stops on its own and the same ≤16 s HW fire
  recovers the board"*.

That last sentence is the whole safety argument, and it is a **liveness proof by
construction**: the entity that pets the dog is the entity whose death must be
detected. Any replacement has to preserve that property, not just "still pet the
dog somewhere".

`wdt_debug_hold` is honoured by both paths (`wdt.c:107-108`, `wdt.c:137`), which
is how a remote reset is triggered (`bmc.c:574-583`).

### 1.3 The finding that most changes Phase 2's shape

**CPU1 is not only the debug core; it is the FreeBSD guest's network receive
engine.** `vnet_emac.c:75-96` says it explicitly **[code]**:

> `CPU1` EXCLUSIVELY owns `emac_poll()` (RX drain + link maintenance) today,
> polled from its own tight loop … `vnet_emac_rx_frame()` is called FROM INSIDE
> that CPU1 loop.

So the guest's `vtnet0` receive path — the thing that makes
`ssh root@192.168.88.82` work (project memory `guest-ssh-access-works`) — runs
on CPU1's loop passes. **[inferred, high confidence]** Handing CPU1 to a guest
without replacing the poller costs the FreeBSD guest its networking, not merely
the debug channel. `docs/dual-guest.md` does not mention this; §6 records it as
a correction.

TX is already cross-core-safe (`EMAC_TX_LOCK_PA` at `emac.c:390`, taken around
all of `tx_frame_raw()`), and `emac.c:42-50` notes RX is **single-owner by
assumption**, not by lock: *"`emac_poll()` is only ever invoked from ONE core at
a time"*. Moving the poll to a second core therefore also needs an RX-side lock
or a strict single-owner re-argument **[code]**.

### 1.4 `dbgmon`'s `sr` / `sr2` / `gr`, and `call`

* `cmd_sr()` (`dbgmon.c:254-270`) issues **direct `mrs` reads** of `ELR_EL1`,
  `SCTLR_EL1`, `TTBR0/1_EL1`, `CNTV_CTL_EL0`, … from whatever core is executing
  it **[code]**. Because that is CPU1, and CPU1 has no EL1 context, `sr` has
  always reported garbage/zeros — which `docs/dual-guest.md:35` already records
  as a pre-existing limitation. **Phase 2 changes the failure mode from useless
  to actively misleading**: if CPU1 hosts a guest, `sr` starts returning *that
  guest's* real EL1 state while still being labelled as "the guest's". This is
  precisely the class of error `ORIENTATION.md` rule 4 exists to prevent.
  **Replacement:** make `sr` take an explicit core/guest selector and read the
  target core's bank via the same cross-core request handshake `gdbstub_hw.c`
  already uses (§1.5), or delete it in favour of `gr`.
* `cmd_sr2()` (`dbgmon.c:593`) dumps EL2 registers (`HCR`, `VTCR`, `VTTBR`, …).
  Same problem, larger: with three or four guests, `VTTBR_EL2`/`VTCR_EL2` are
  per-PE banked (`stage2_zephyr.h:12-19`) and CPU1's copy describes *its own*
  guest. **Replacement:** print the core id alongside every value, and add a
  per-core variant.
* `gr` reads `g_last_guest_frame`, which `el2_exc.c:685` already gates to
  `smp_cpu_id() == 0` **[code]** — Phase 1 did this correctly. For Phase 2 the
  snapshot must become an array indexed by core, with `gr [core]`.
* **`call`** (`dbgmon.c:638-670`, dispatch `dbgmon.c:1093`, recovery path
  `el2_exc.c:700-710`) invokes arbitrary HV functions *on CPU1, at EL2*. Host
  tooling depends on it: `emmc_raw.py:120-170` resolves
  `vblk_emmc_trylock`/`vblk_emmc_unlock` by `nm` and `call`s them around every
  `emmc_bio_*` call **[code]**. A guest on CPU1 removes the EL2 execution
  context these calls need. **Replacement:** none cheap. Either keep an EL2
  sliver on CPU1 (§1.6 option a) or move `call` to a core that still has one.

### 1.5 `gdbstub.c` address resolution and `gdbstub_hw.c`'s cross-core queue

Both files are *explicitly built around* "the stub runs on a core with no guest"
**[code]**:

* `gdbstub.c:157-218` — `resolve()` must never do a live stage-1 walk, because
  `AT S1E1R` reflects the *executing* core's banked `TTBR*_EL1`. The fix was to
  use only static arithmetic over `kload.c`'s `pa_base`/`kernbase`
  (`kload_va_to_pa()`, `gdbstub.c:188-200`), plus "treat sub-canonical addresses
  as flat PAs" (`gdbstub.c:213-216`), and to **refuse** anything else.
  * **Effect of a guest on CPU1:** `AT S1E1R` from CPU1 would now translate
    through *CPU1's guest's* page tables. The current code never does that walk,
    so it does not break — but the "flat PA" fallback becomes wrong in a new
    way: a low address is no longer unambiguously "our own EL2 structure", it
    could be another guest's IPA. **Replacement:** make `resolve()` take an
    explicit guest id and resolve against that guest's `kload` bookkeeping /
    stage-2 slice bounds.
* `gdbstub_hw.c:26-58` and `hwop_run()` (`gdbstub_hw.c:105-…`) — `DBGBVR`/
  `DBGBCR`/`DBGWVR`/`DBGWCR` are per-PE banked, so CPU1 posts a request
  (`gdb_hw_op_*`, defined in `el2_exc.c`) and **CPU0 executes it**, but only
  while CPU0 is already parked in the stop loop; otherwise it refuses with `-2`
  (`gdbstub_hw.c:107-108`). `gdbstub_hw.c:47` records why there is no
  interrupt-driven version: *"would need a new SGI/IPI path (this board's GIC has
  none today) — DELIBERATELY NOT built this pass"*.
  * **Effect:** the mechanism is a 2-party handshake with hard-coded roles
    ("CPU1 asks, CPU0 does"). With 3-4 guests it must become "any service context
    asks core N". That is a mechanical generalisation of existing, working code —
    the cheapest item in this whole section.

### 1.6 `gic_timer.c`'s CNTV-mask watchdog — the premise needs correcting

The task framing lists this among things that depend on CPU1 never running guest
code. **The dependency is the opposite way round, and it matters** **[code]**:

`vtimer_mask_watchdog()` (`gic_timer.c:398-421`) runs from the **EL2 CNTP tick
on the guest's own core (CPU0)**, and `gic_timer.c:364-368` says why that is
mandatory: *"the same read issued by the CPU1 debug core would report CPU1's bank
and be worthless"*. So it is not endangered by CPU1 gaining a guest.

What *is* endangered is far more general, and applies to **every** core that
starts taking EL2 interrupts:

1. **`gic_timer.c`'s module state is single-instance.** `gt_period_ticks`,
   `gt_next_deadline`, `gt_ticks`, `gt_mismatches` (`gic_timer.c:496-499`),
   `irq_counter[160]` (`gic_timer.c:505`), and the CNTV-watchdog trio
   `gt_cntv_el2_masked` / `gt_cntv_masked_ticks` / `gt_cntv_rescues`
   (`gic_timer.c:391-393`) are file-scope statics. `smp.c:417-419` already
   flags this: the per-core tick in `smp.c` keeps its deadline private
   *"unlike gic_timer.c's shared global"*. A second core entering
   `gic_timer_irq()` corrupts CPU0's timer bookkeeping **and** its
   CNTV-rescue state machine.
2. **`vgic.c`'s state is single-instance too** **[code]**: `vg_pendq[]`,
   `vg_pendq_head/tail/count` (`vgic.c:280-285`), `vg_active` (`vgic.c:252`),
   `vg_nr_lr` (`vgic.c:247`), the injection counters (`vgic.c:248-255`), and
   `vgic_gicd_shadow[0x1000]` (`vgic.c:606`). The GICH List Registers themselves
   are banked per PE, so *hardware* isolation is fine — but the software queue
   that feeds them is shared. Two cores injecting concurrently would interleave
   pushes/pops on one queue.
3. **`sched_tick()`** is called from the IRQ path (`el2_exc.c:725`) and operates
   on a single ready-list (`sched.c:333`) **[code]**.

**Consequence for the whole of Phase 2, and this is the single biggest hidden
cost:** *any* design in which a second core takes EL2 interrupts — whether to
run a periodic service sliver on CPU1, or to deliver a virtio-blk completion SPI
to a guest on CPU2 — requires `gic_timer.c` and `vgic.c` to be made
per-core-instanced first. That is a prerequisite step, not a detail.

### 1.7 The watchdog replacement: four options compared

| | Option | Mechanism | What it preserves | What is **lost** / risked |
|---|---|---|---|---|
| **(a)** | **Thin non-guest sliver on CPU1** — recommended | Arm CPU1's own banked CNTP (INTID 30) and unmask `DAIF` on CPU1, so EL2 preempts CPU1's guest every N ms and runs a bounded service pass: `wdt_debug_kick()` + `emac_poll()` + PC5 pinmux + `dbgmon`/gdbstub service. **The code for the arming half already exists and is currently dead**: `smp_timer_init_secondary()` (`smp.c:363-399`) programs the banked PPI 30, `GICC_PMR`/`GICC_CTLR`, and `HCR_EL2.IMO`, and has **no caller** **[code]** | *Everything* in §1.1, watchdog included, at reduced duty cycle. The death-detection property survives intact: if CPU1 stops (or its guest wedges EL2 out), the tick stops, petting stops, the ≤16 s HW window fires | The guest on CPU1 is **not a full-fat guest**: it loses N% of its core and, more importantly, it must tolerate EL2 stealing time at unpredictable points. Requires §1.6's per-core `gic_timer`/`vgic` work **first**. Requires `emac_poll()` to become safe under a bounded time budget (today it is called from a free-running loop with no deadline) — **[unknown]** what its worst-case pass costs. Risk: a service pass that overruns starves CPU1's guest; a guest that masks/disables things EL2 needs breaks the sliver |
| **(b)** | **Move the unconditional kick to CPU2** | CPU2's loop (`vblk_async.c:40-72`) already runs free with IRQs masked (`vblk_async.c:59-64`); add `wdt_debug_kick()` to it | The watchdog, cleanly. One-line change, trivially verifiable | **Does not help Phase 2 at all**, because Phase 2 wants CPU2 for a guest too. It also silently changes the death-detection subject from "CPU1 alive" to "CPU2 alive" — and CPU2's loop is *idle by default* (`g_vblk_async_ready`-gated producer, `vblk_emmc.c:163`), so "CPU2 alive" is a weaker statement than "CPU1 servicing EMAC". Useful only as a stepping stone, or in a build where CPU2 stays non-guest |
| **(c)** | **Move it into EL2's own trap/timer path on CPU0** | Make `wdt_pet()` unconditional, or add an unconditional kick to `gic_timer_irq()`'s CNTP branch | Zero new cores needed; CPU0's tick already exists and is armed (`main_dbg.c:375`) | **Destroys the property the current design was built for.** `wdt.c:24-37` and `el2_exc.c:596-605` spell out the failure this re-introduces: CPU0 trapping constantly with no forward progress (the "busy wedge" / poll-storm case) would keep the dog fed forever. Worse, CPU0 is the core most likely to be *stuck inside a guest*, which is exactly when you need the reboot. This inverts the safety argument: the entity being watched becomes the watcher |
| **(d)** | **External / BMC-side kick** | A host-side keepalive over EMAC (or the USB-ACM break-glass channel) that pokes the WDOG, e.g. via the existing `bmc.c` `wdt arm/hold/release` verbs (`bmc.c:567-583`) | Frees all four cores completely — the only option that does | Turns an *autonomous* recovery into one that **depends on the host being up, the network being up, and EMAC being alive** — and EMAC going dark while the board is otherwise healthy is a documented, repeated failure in this project (project memory `breakglass-usb-acm-emac-dark-recovery`, `emac-flakiness` hypothesis #5 cited at `wdt.c:102-106`). It also depends on the very poller (§1.3) that a guest on CPU1 would have displaced — circular. Rejected |

**Recommendation: (a), with (b) as the interim step.**

Reasoning, in order of weight:

1. Options (c) and (d) both break the *structure* of the safety argument, not
   just its implementation. (c) makes the watched thing the watcher; (d) makes
   an autonomous property depend on an external, historically flaky channel.
   Neither is worth four cores.
2. (a) is the only option that keeps **all nine** of CPU1's jobs, and §1.3 shows
   at least two of them (EMAC poll, PC5 pinmux) are not optional if FreeBSD is
   to keep working. Any plan that "frees CPU1" must re-home them somewhere, and
   the cheapest somewhere is a sliver on CPU1 itself, where they already are.
3. (a) has an unusually good cost profile because the missing piece is
   **already-written dead code** (`smp.c:363-399`), authored with the correct
   banked-register rationale at `smp.c:219-226`.
4. **Be honest about what (a) means for the user's stated goal.** "All four cores
   usable by guests" becomes, under (a), *"three cores run unrestricted guests;
   the fourth runs a guest that shares its core with a bounded EL2 service
   sliver"*. That is a real fourth guest — but it is a cooperative-grade one, best
   suited to an RTOS (Zephyr) rather than anything with hard real-time deadlines
   or its own tick assumptions. If the requirement is genuinely four
   *unrestricted* guests, the answer is that this board cannot have both that and
   autonomous crash recovery, and the honest thing is to say so rather than to
   quietly pick (c).

---

## 2. CPU2 for a third guest with real disk I/O

### 2.1 GIC SPI routing: this tree has never programmed it. At all.

**`GICD_ITARGETSR` (GICD + 0x800) is never written, never read, and never even
mentioned anywhere in the tree** **[code]** — verified by an exhaustive sweep:
zero hits for `ITARGETSR`/`itarget` across all `.c`/`.h`/`.S`/`.py` (including
`docs/`, `attic/`, and the Python tooling), and every GICD address expression in
the tree resolves to one of only five offsets: `0x000` (CTLR), `0x080`
(IGROUPR), `0x100` (ISENABLER), `0x200` (ISPENDR), `0x400` (IPRIORITYR).

The complete set of GICD address sites, for the record **[code]**:
`gic_timer.c:205-210`, `smp.c:230-232`, `gic_timer_qemu.c:30-33`,
`vgic.c:176-180`, `vgic.c:642` (bounds check only), `vblk_emmc.c:613`,
`vnet_emac.c:414`.

Corollaries that Phase 2 must build on:

* **The tree is GICv2/GIC-400 only.** No `GICD_IROUTER`, no `GICR_*`, no
  `ICC_*`, no redistributor anywhere **[code]** (`vgic.c:110`, `vgic.h:17-18`,
  `gic_timer.c:14-21`, `stage2.c:299`).
* **No SPI is ever enabled by the HV.** The only two `ISENABLER` writes on the
  board are PPIs — INTID 30 (`gic_timer.c:630`) and INTID 25
  (`vgic.c:408`) **[code]**. Enabling SPIs is explicitly delegated to the guest;
  `vblk_emmc.c:616-619` states it: *"this assumes the guest has already ENABLED
  `VBLK_INTID` at the distributor … as part of `bus_setup_intr()`"*. And
  `stage2.c:319-320` leaves the GICD page **writable** to the FreeBSD guest, so
  FreeBSD sets its own targeting and the HV neither observes nor constrains it.
* **`GICD_ICFGR` (edge vs level) is never written either** — read-only, and only
  from Python (`triage.py:331`) **[code]**. Discussed and deliberately delegated
  to the guest DTB (`vblk_emmc.c:624-629`).
* **There is no SGI/IPI path.** No `GICD_SGIR` write, no `0xF00` offset
  **[code]**; `gdbstub_hw.c:47` and `el2_exc.c:86` both record its absence as a
  deliberate choice. All inter-core coordination is shared memory + `sev`/`wfe`.
* **Which core do SPIs land on today?** **[inferred]** — CPU0, and the routing
  half of that is inference, not code: since nothing ever writes `ITARGETSR`,
  targeting is whatever the GIC reset default / ATF-BL31 left, conventionally
  CPU0 or all-CPUs. The *delivery* half is not inference: only CPU0 enables a
  CPU interface (`gic_timer_cpuif_init()` `gic_timer.c:541-561`, called only from
  `main_dbg.c:360`) and only CPU0 unmasks `PSTATE.I` (`main_dbg.c:396`,
  `msr daifclr, #3`); `vblk_async.c:59-64` and `smp.c:531-533` state that CPU1
  and CPU2 keep IRQs masked by design **[code]**.

### 2.2 What would have to be programmed for a guest on CPU2 with real I/O

New code, none of which exists **[code]**:

1. **`GICD_ITARGETSR[intid] = (1 << 2)`** — the tree's first ever write to that
   register, targeting the third guest's device SPI at CPU2 only (not 1-of-N).
2. **`GICC_PMR = 0xff; GICC_CTLR = 0x3 | EOImode`** on CPU2. The code exists,
   dead, at `smp.c:385-387`; note it must match CPU0's `EOImode=1` choice
   (`gic_timer.c:560`) or the EOI/DIR split in `gic_timer.c:767-768` breaks.
3. **`msr daifclr, #3` on CPU2** — no secondary does this today **[code]**.
4. **Per-core `gic_timer`/`vgic` instancing** — §1.6. Non-negotiable: without it,
   CPU2 entering `gic_timer_irq()` corrupts CPU0's timer and vGIC pending queue.
5. **A per-core vGIC init** on CPU2 (`GICH_HCR`, `GICH_VMCR`, LR count from
   `GICH_VTR`) — `vgic_init()`'s writes are per-PE-banked so the *register* half
   is fine, but `vg_active`/`vg_nr_lr` are shared statics.
6. **`GICD_ICFGR`** for the new SPI, if the third guest has no DTB of its own to
   declare edge-vs-level (a bare-metal or Zephyr guest will not).

**Why this is actually the clean part of the design [inferred]:** injection today
is a set-pending on the *real* distributor (`vblk_emmc.c:630`, `ISPENDR`),
forwarded into a GICH List Register by whichever core takes the physical
interrupt (`vgic_inject_hw()` at `gic_timer.c:885`). Because GICH is banked per
PE, "route SPI *n* to CPU2 with `ITARGETSR`" **automatically** lands the vIRQ in
CPU2's guest and nowhere else. Physical routing *is* the per-guest routing. No
VMID multiplexing, no software demux — the same architectural leverage
`stage2_zephyr.h:12-19` exploits for stage-2.

### 2.3 A genuinely per-guest `vblk_emmc.c`

Today there is exactly **one** device instance and **one** of everything above
the controller. The concrete work, with file:line:

**Must become per-guest (device model):**

| Item | Where | Note |
|---|---|---|
| `static struct vblk_dev g_blk` | `vblk_emmc.c:134` | The single device instance |
| `static struct vblk_async_req g_async` | `vblk_emmc.c:1105`, states `:1090-1091`, post `:1111-1142`, poll `:1147-1225` | **Single-slot** mailbox, one request in flight HV-wide |
| `static uint64_t g_bounce_q[64]` / `BOUNCE_PA` | `vblk_emmc.c:168-170` | One bounce buffer; safe today only because it is used strictly under the eMMC lock |
| `static uint16_t g_isr_scan_used_idx` | `vblk_emmc.c:590` | Per-guest ISR scan watermark, currently HV-global |
| `VBLK_MMIO_BASE` / `VBLK_SPI` / `VBLK_INTID` | `vblk_emmc.h:73`, `:108-109` | Compile-time constants (`0x0A000000`, SPI 105 → INTID 137) |
| `HVMAP_USED_LOCK` | `hv_addrmap.h:82`; acquire/release pairs at `vblk_emmc.c:1208/1214`, `1311/1313`, `1342/1344`, `1349/1351`, `1535/1537`, `1585/1588`, `1766/1804`, `1824/1828` | Guards `vq_push_used()` + `int_status` + inject; per-**virtqueue**, so per-device |
| `HVMAP_VBLK_BC` breadcrumb window | `hv_addrmap.h:57-58` (256 B), writer `vblk_emmc.c:124-129` | ~62 slots already used up to `bc[61]`; a second instance cannot share it |
| The ~35 `g_*` counters | `vblk_emmc.c:135-157`, `:206`, `:591`, `:690-702`, `:710`, `:994-1000`, `:1106` | Non-atomic by explicit decision (`vblk_emmc.c:986-993`); currently bumped from both CPU0 and CPU2 |

**Must stay single but become arbitrated (one physical controller):**

* **`HVMAP_EMMC_LOCK` = `0x50020100`** (`hv_addrmap.h:61-62`, alias
  `vblk_emmc.h:222`). Trylock `vblk_emmc.c:334-357`, unlock `:359-365`, bounded
  acquire `:482-499` with a **6000 ms** budget (`vblk_emmc.c:481`), used at
  `vblk_emmc.c:810-820` / released `:936`, with `VBLK_LOCK_RETRIES = 4`
  (`vblk_emmc.c:700`). Protocol: `ldaxr`/`stlxr` on one DRAM word, taken and
  released **once per 512-byte sector** **[code]**.
  * **This is the hard part, and the tree already measured it.**
    `vblk_emmc.c:800-809` is the sentence Phase 2 must design against **[code]**:
    the lock *"is a plain test-and-set with no queue, so under sustained two-core
    load … the loser can be starved for seconds on end"*, with
    `g_lock_retries`/`g_lock_giveups` published at `bc[59]`/`bc[60]`.
  * **Why starvation is a correctness bug, not a latency bug [inferred, but from
    a real observed failure]:** a lost lock race returns `VBLK_RC_BUSY`
    (`vblk_emmc.c:653`, returned `:816`) → `S_IOERR` to the guest → FreeBSD turns
    one failed metadata I/O into `vtbd0: hard error` and a dead root fs. That is
    exactly the bug that was root-caused and fixed as *"the sync fallback raced
    CPU2 for the eMMC lock"* (`vblk_emmc.c:1429-1447`, fix = call
    `vblk_async_drain_bounded()` at `:1447`; drain at `:1260-1282`). A third
    guest re-opens that race class with a *new* actor that cannot be drained,
    because it is not our mailbox — it is another guest.
  * Aggravating detail **[code]**: the write-retry loop (`vblk_emmc.c:904-933`)
    runs **with the lock still held** (noted at `:887-903`), so a stalling write
    can hold the eMMC for ~16 s.
* Unlocked shared hardware that two I/O-doing guests would newly contend:
  `PIO_PC_CFG0` RMW from CPU1 (`smp.c:553-558`) and from `emmc_bio_init()`
  (`emmc_bio.c:317-318`); `CCU_MMC2_CLK` at `emmc_bio.c:350` and `:952`; the
  EBIO failure counters `g_ebio_fails`/`g_settles`/`g_busy_timeouts`
  (`emmc_bio.c:425-428`) and window `HVMAP_EBIO_BC` (`hv_addrmap.h:65`), which
  would attribute a failure to "the eMMC" and not to a guest **[code]**.

**Two more things that are not optional:**

* **Guest-memory access has no per-guest notion at all.** `gmem_read/write/cmo`
  (`vblk_emmc.c:242-316`) are the **identity function** — the contract at
  `vblk_emmc.c:23-28` and `:221-226` is *"IPA==PA identity map ⇒ a guest PA is an
  EL2 pointer"* — and the only validity gate is `gpa_in_range()`
  (`vblk_emmc.c:208-219`) against a hard-coded `[0x40000000, 0x80000000)`
  (`GUEST_DRAM_BASE`/`SIZE`, `vblk_emmc.c:202-204`, deliberately duplicated from
  `stage2.h:87-88`) **[code]**. The **existing** second guest at `0xBE000000`
  (`stage2_zephyr.h:67`) would have **every** descriptor rejected as
  `VBLK_RC_BADPA` (`vblk_emmc.c:654`, `:787`). So the range check must become
  per-guest — and note the worse-with-two-guests hazard already flagged at
  `vblk_emmc.c:193-200`: guest A's descriptor pointing into guest B's DRAM would
  *pass* a naive check.
* **No LBA isolation whatsoever.** Capacity is the literal `15269888`
  (`vblk_emmc.c:1976`), exposed 1:1 — *"virtio sector N == eMMC LBA N"*
  (`vblk_emmc.h:190-195`) — with no offset/limit mechanism **[code]**. Two guests
  would share one namespace and be able to overwrite each other's filesystems,
  including FreeBSD's root. A per-instance `{lba_base, lba_count}` window,
  enforced in `serve_data()`, is mandatory before any second guest gets write
  access.

**The good news [code]:** the whole 2 MiB stage-2 block `0x0A000000..0x0A200000`
is already trapped (`stage2.c:337` sets the L2 descriptor to 0) and only two
0x200-byte slots are used — `0x0A000000` (blk) and `0x0A001000` (net,
`vnet_emac.h:87-88`). A second and third virtio-blk at `0x0A002000`/`0x0A003000`
need **no `stage2.c` change**, which `vblk_emmc.h:70-71` and `vnet_emac.h:69`
state explicitly. Since `stage2.c` is frozen, that matters.

### 2.4 The unavoidable trade: CPU2's guest vs. async I/O

`vblk_async.o` is linked into exactly two targets — `DBG_OBJS`
(`Makefile:243`) and `DUAL_OBJS` (`Makefile:281`) — and every other target keeps
the weak `wfi` park at `smp.c:461-465` **[code]**. The safety interlock is not
the linkage but `g_vblk_async_ready` (`vblk_emmc.c:163`, set only at
`vblk_async.c:56`, checked at `vblk_emmc.c:1118-1119` and `:1265-1266`), and
`vblk_emmc.h:361-371` / `vblk_async.h:119-136` / `smp.c:456-460` all forbid
adding a second independent gate.

**Giving CPU2 to a guest therefore removes the async eMMC offload entirely**
**[inferred, direct]**. Consequences, both directions:

* **Lost:** CPU0 goes back to doing eMMC PIO inline inside the guest's own trap.
  **[unknown]** how much that costs FreeBSD in practice — it was the pre-C2
  behaviour and the guest booted, but no measurement of the regression exists in
  the tree.
* **Gained:** the entire CPU0-vs-CPU2 race class disappears — including the
  single-slot mailbox limit (`vblk_emmc.c:1052-1055`) that would otherwise make
  guest B's every request take the sync fallback while guest A is in flight, i.e.
  **guest B's trap blocking up to 6 s on guest A's I/O** **[inferred from
  `vblk_emmc.c:481` + `:1429-1447`]**. That is a strong argument for *not*
  trying to keep both.

Recommended: **drop async when CPU2 becomes a guest core**, and keep it in the
`dbg`/`dual` targets by leaving those object lists untouched.

---

## 3. Memory partitioning for 3-4 concurrent guests

### 3.1 The map today

Board DRAM: **2 GiB, `0x40000000`–`0xC0000000`** **[code]** — confirmed by
`snapshot.h:91-109` (*"`0xC0000000`, the last byte of real, installed DRAM"*,
with a live fault at `FAR=0xC0000000`) and `hv_addrmap.h:236-246`.

| Window | Range | Owner | Cite |
|---|---|---|---|
| FreeBSD guest DRAM | `0x40000000`–`0x80000000` (1 GiB) | guest 1 | `stage2.h:87-88` |
| ├ HV image | `0x42000000`–`0x421FFFFF` | HV (stage-2-excluded) | `stage2.c:435`, `stage2.c:438` |
| ├ raw kernel ELF / placed kernel / DTB | `0x44000000` / `0x46000000` / `0x4A000000` | loader scratch | `main_dbg.c` `K_ELF`/`K_PABASE`/`DTB_SRC` |
| ├ HDMI framebuffer (`HV_HDMI`) | `0x4D000000`, 8 MiB | HV | `stage2.c:450-452` |
| ├ zstage landing window | `0x4E000000`–`0x50000000` (32 MiB) | 2nd-guest ELF landing pad | `zstage.h:45`, `:62` *(untracked, in flight)* |
| └ hv-scratch | `0x50000000`–`0x50200000` (2 MiB) | HV (stage-2-excluded) | `stage2.c:436`, `hv_addrmap.h` |
| High GiB, unassigned | `0x80000000`–`0xBE000000` (~992 MiB) | **free** | `stage2_zephyr.h:31` maps it INVALID |
| Zephyr guest slice | `0xBE000000`–`0xC0000000` (32 MiB) | guest 2 (CPU3) | `stage2_zephyr.h:67-68` |
| Snapshot DRAM mirror | `0x80000000`–`0xC0000000` (**the whole high GiB**) | `snapshot` feature | `snapshot.h:200-210` |

The last two rows are the documented mutual exclusion: `stage2_zephyr.h:39-54`
spells it out, and `Makefile:270-284` implements it by excluding
`snapshot.o`/`snapshot_net.o` from `DUAL_OBJS` **[code]**.

### 3.2 Proposed partitioning

**Design rule: do not move anything that is hardware-verified.** FreeBSD's
gigabyte and Zephyr's 32 MiB slice both stay **byte-identical**, so neither the
A1 isolation self-check (`stage2.c:944-950`) nor Phase 1's own self-check
(`stage2_zephyr.c:311-335`) needs re-proving. All new slices come out of the
~992 MiB of high GiB that is currently mapped INVALID by everyone.

| Guest | Core | IPA/PA slice | Size | Why |
|---|---|---|---|---|
| 1 · FreeBSD | CPU0 | `0x40000000`–`0x80000000` | 1 GiB | **unchanged** |
| 3 · new, with disk | CPU2 | `0xA0000000`–`0xB0000000` | 256 MiB | Big enough for a small Linux or a Zephyr with a filesystem; 2 MiB aligned; 1 GiB-block-aligned start keeps the L1/L2 arithmetic in a copy of `stage2_zephyr.c` trivial |
| 4 · new, sliver-core | CPU1 | `0xB8000000`–`0xBC000000` | 64 MiB | An RTOS-sized slice for the cooperative-grade guest of §1.7(a) |
| — reserved gap | — | `0xBC000000`–`0xBE000000` | 32 MiB | Deliberate slack. `hv_addrmap.h:177-195` and `:329-333` record that this project has already lost six windows to a neighbour grown without slack |
| 2 · Zephyr | CPU3 | `0xBE000000`–`0xC0000000` | 32 MiB | **unchanged** |
| — free | — | `0x80000000`–`0xA0000000` | 512 MiB | Left unassigned: room to grow guest 3, or to host a reduced snapshot store (§3.3) |

Total assigned to guests: 1 GiB + 256 MiB + 64 MiB + 32 MiB = **1376 MiB of
2048 MiB**, leaving 512 MiB free plus the gap. It fits with room to spare — DRAM
is *not* the binding constraint. The binding constraints are the four below.

### 3.3 What has to shrink, and what conflicts remain

1. **Snapshot is the casualty, and more so than today** **[code]**.
   `SNAP_DRAM_SIZE == SNAP_STORE_BASE`-to-`0xC0000000` == exactly 1 GiB
   (`snapshot.h:200-210`), deliberately equal to the guest's own window
   (`snapshot.h:77-78`) with **zero slack** — `hv_addrmap.h:236-246` records the
   live fault caused by trying to fit a 64 KiB header into it. Under §3.2 the
   store window overlaps guests 3 and 4 as well as Zephyr.
   * **Option A (recommended):** keep and *formalise* the mutual exclusion.
     Snapshot exists only in single-guest builds. Add a `_Static_assert` that
     `SNAP_STORE_BASE + SNAP_DRAM_SIZE <=` the lowest assigned guest slice, so
     the conflict becomes a compile error instead of a comment. Cost: no
     snapshot in any multi-guest build — the same trade `dual` already makes.
   * **Option B:** shrink `SNAP_DRAM_SIZE` to `0x20000000` (512 MiB, ending at
     `0xA0000000`) and add every new slice to `snapshot_excl_windows()`
     (`snapshot.c:270-297`, the treatment `HVSCR_BASE` already gets). **This
     breaks snapshot's core invariant** — it could then no longer mirror the
     full 1 GiB guest, so `snapshot_save()` would have to become partial, and
     `struct snapshot_hdr`'s `dram_size` sanity check (`snapshot.h:293-294`)
     would need re-specifying. Not recommended; that is a redesign of D1, not a
     parameter change.
   * **Option C (the real fix, out of Phase 2 scope):** store snapshots on the
     eMMC instead of in DRAM. Removes the conflict permanently.
2. **The staging landing pad does not scale to four guests** **[code]**.
   `ZSTAGE_LOW_PA = 0x4E000000`, 32 MiB (`zstage.h:45,62`), is a *single*
   landing window inside FreeBSD's own gigabyte, and its copy-in must happen
   before any guest runs — `main_dbg.c:453-467` documents that the position of
   the `zguest_stage_copyin()` call is load-bearing. Three extra guests need
   either three landing windows (there is not room below `0x50000000`), or
   sequential staging (copy guest 2's image out, then reuse the pad for guest 3,
   still all before `smp_init()`), or a landing pad relocated into the free
   `0x80000000`–`0xA0000000` region. **Recommend the last**: it is the only
   option that scales and it removes the "inside FreeBSD's memory" hazard the
   current design has to reason around.
3. **Every guest must be built for its own PA.** All slices are identity-mapped
   (`stage2_zephyr.c:227-236` emits plain identity 2 MiB blocks), so each guest
   image must be *linked* for its slice — exactly what Phase 1 had to do
   (`docs/dual-guest.md:10`: a new `bpi_m64_hv_dual` Zephyr board port with
   `sram0` moved to `0xBE000000`). Four guests = four such ports/relocations.
   **[inferred]** This, not DRAM, is the real per-guest setup cost.
4. **`ZLOAD2_MAX_IMAGE_SIZE` is tied to Zephyr's slice size** (`zload2.h:65`,
   `0x02000000`, *"== ZSTAGE2_DRAM_SIZE"*) **[code]**. A 256 MiB guest needs that
   ceiling to become per-guest rather than a single `#define`.
5. **Breadcrumb lanes**: `hv_addrmap.h:286-355` allocated the dual-guest lanes
   (`0x50070000`–`0x5007A040`) for **one** second guest. Guests 3 and 4 each need
   their own `ZG*`/`ZLD*`/`STGZ*`/`MABS*` lanes plus a vconsole channel lane, and
   the free space between `0x5007A040` and `0x50100000` (the non-cacheable
   EMAC-DMA boundary asserted at `hv_addrmap.h:352-354`) is ample. Extend the
   `_Static_assert` chain; do not hand-pick literals (`hv_addrmap.h:41-43`).
6. **vconsole channels**: `vconsole.c` has exactly two static channel instances,
   `vc_chan0` and `vc_chan1` (`vconsole.c:265`), selected by
   `vconsole_handle_fault(frame, channel)` with a two-way ternary at
   `vconsole.c:551-554` **[code]**. Guests 3 and 4 need channels 2 and 3: turn
   the ternary into an array index, and keep `interactive`/`wdt_note_progress()`
   channel-0-only (`vconsole.c:15-23`, `:482-485`) so no new guest can feed the
   dead-man's switch.

---

## 4. Ordering and risk

Phase 1's own precedent, which this sequence follows: board-free
`*-qemu-ci.sh` proof before any hardware contact; weak/strong linkage plus
**new-target-only object lists** so existing targets are *provably* unchanged
(`smp.c:442-460`, `smp.c:467-482`, `Makefile:270-284`).

**Invariant for every step: `DBG_OBJS` (`Makefile:240-245`) is not edited.** If
`microkernel-dbg.elf`'s object list is byte-identical, the hardware-verified
FreeBSD boot path cannot have regressed by construction.

### Step P0 — Formalise the memory map. No behaviour change.
Add the §3.2 slice constants and their `_Static_assert` non-overlap chain (new
lane in `hv_addrmap.h`, or a new `guests.h`), plus the snapshot-vs-guest
compile-time conflict assert of §3.3-A. Zero functions touched.
**Verify:** `make test` (the 15 hosted tests, `Makefile:121`) and every existing
`*-qemu-ci.sh` still pass; `size` output for `microkernel-dbg.elf` unchanged.
**Risk:** none. This is the step that makes every later step checkable.

### Step P1 — Per-core-instance `gic_timer.c` and `vgic.c` state.
Convert `gt_period_ticks`/`gt_next_deadline`/`gt_ticks`/`gt_mismatches`
(`gic_timer.c:496-499`), `irq_counter[160]` (`:505`), the CNTV trio (`:391-393`),
and `vgic.c`'s `vg_*` + `vg_pendq` + `vgic_gicd_shadow` (`vgic.c:247-285`,
`:606`) into `[SMP_MAX_CPUS]` arrays indexed by `smp_cpu_id()`, cache-line
padded like `struct smp_percpu` (`smp.c:183-188`).
**Verify:** board-free. `vgic-qemu-ci.sh`, `test_vgic_pendq` (extend it to drive
two core indices and assert no cross-talk), `qemu-ci.sh`.
**Why first:** §1.6 — every later step depends on it, and on CPU0 alone the
behaviour is provably identical (index is always 0).
**Risk:** medium. It touches the guest's timer path, which this project has
broken and reverted twice (project memory `vgic-revert-complete`,
`vtimer-masked-vgic-off-deadlock`). Mitigate by keeping the CPU0 code path
literally the same instructions with a constant-foldable index.

### Step P2 — Watchdog: interim (b), then the real (a) arming.
* **P2a:** add `wdt_debug_kick()` to CPU2's loop (`vblk_async.c:40-72`) *in
  addition to* CPU1's, so two independent cores feed the dog. Purely additive;
  the death-detection property weakens only if *both* die, which is the case the
  HW window covers anyway.
* **P2b:** wire the dead `smp_timer_init_secondary()` (`smp.c:363-399`) for a
  chosen core behind a new `-D` gate, plus a bounded "service pass" function
  extracted from `smp.c:545-683`, and `daifclr` on that core. **Do not** point it
  at CPU1 yet — validate it on **CPU2** first, where nothing depends on the core
  today.
**Verify:** a new `smp-tick-qemu-ci.sh` in the shape of `smp-qemu-ci.sh`,
asserting a secondary's own CNTP tick fires N times and its service pass runs,
with CPU0 unaffected. A hosted `test_wdt_policy.c` for the pet/kick/hold state
machine (`wdt.c` has no test today).
**Risk:** **high, and this is the step to be most careful about.** Unmasking IRQ
on a second core is the exact ingredient of the twice-reverted vGIC work.
`smp.c:531-533` chose masked IRQs on CPU1 deliberately *"so nothing preempts the
poll"* — P2b reverses that choice. Bound the service pass; measure its worst case
(**[unknown]** today).

### Step P3 — Third guest on CPU2, **no disk**.
Copy the Phase-1 pattern exactly: `zguest_cpu2.c` (mirror of `zguest_cpu3.c`),
`stage2_cpu2.c` (mirror of `stage2_zephyr.c`, pointed at `0xA0000000`), reuse
`zload2.c`/`mmio_absorb.c` unchanged, a new vconsole channel 2, a `z2boot`
dbgmon verb. New Makefile target `tri` = `DUAL_OBJS` − `vblk_async.o` + the new
objects. `DBG_OBJS`/`DUAL_OBJS` untouched.
**Verify:** `tri-qemu-ci.sh` in the exact shape of `dual-qemu-ci.sh` — sample
**three** independent DRAM counters twice, ~1 s apart, all strictly increasing;
plus each new guest's `AT S12E1W` isolation self-check reporting pass=1.
**Risk:** low. This is the proven mechanism applied a second time. The one new
thing is `vblk_async.o` leaving the link, which changes CPU2's role — assert
`g_vblk_async_ready == 0` in that build.

### Step P4 — First `GICD_ITARGETSR` write, board-free.
Route one synthetic SPI to CPU2, enable its GICC, and prove the vIRQ lands in
CPU2's guest and **not** CPU0's. This is the tree's first ever ITARGETSR write
(§2.1), so it gets its own step and its own proof.
**Verify:** `spi-route-qemu-ci.sh` — inject via `GICD_ISPENDR` (the mechanism
`vblk_emmc.c:630` already uses), assert guest-3 sees it and guest-1's vGIC
counters are unchanged.
**Risk:** medium. QEMU's GICv2 `ITARGETSR` emulation fidelity vs. the real
GIC-400 is **[unknown]**; a QEMU pass does not prove the board. Expect to re-prove
on hardware, and prefer routing a *new* SPI over re-routing one FreeBSD uses.

### Step P5 — Per-guest virtio-blk, hosted-test first.
Refactor `g_blk`/`g_async`/`g_bounce_q`/`g_isr_scan_used_idx` into an
instance struct with per-instance `{mmio_base, intid, lba_base, lba_count,
gpa_lo, gpa_hi}`; instance 0 keeps today's exact constants so guest 1's
behaviour is unchanged. Add LBA-window enforcement in `serve_data()`
(`vblk_emmc.c:810-936`) and per-instance `gpa_in_range()`.
**Verify:** extend `test_vblk_ring.c` (which already covers the
`ack_window_rearm` case, `:568-666`) with: two instances, disjoint LBA windows,
a cross-window access **rejected**, and a lock-contention case asserting neither
instance ever gets `S_IOERR` under fair alternation. Hosted, board-free.
**Risk:** **high.** This is `vblk_emmc.c` (2000+ lines, the site of the project's
worst bug hunts). The lock-fairness question (§2.3) is a design change, not a
refactor: the plain TAS lock must gain a queue or a per-actor budget, or guest 3
will hand `S_IOERR` to guest 1 under load. Do not skip the contention test.

### Step P6 — Fourth guest on CPU1 (last, riskiest).
Only after P2b is proven on CPU2: repoint the service-sliver tick at CPU1, add
guest 4 at `0xB8000000`, keep **all** of §1.1 running in the sliver.
**Verify:** board-free `quad-qemu-ci.sh` (four counters). Then, on hardware and
in this order: (1) FreeBSD boots and `ssh` answers with the sliver active and no
fourth guest started — this is the safety property Phase 1 checked first
(`docs/dual-guest.md:18`); (2) the debug channel and `bc` reads still work; (3)
guest networking still works (§1.3); (4) only then start guest 4.
**Risk:** highest. Failure mode is loss of the debug channel *and* the watchdog
simultaneously, i.e. the one state that needs a physical power-cycle. **Prime
break-glass (`\x00~BZRST\x00` to `/dev/ttyACM0`, project memory
`breakglass-usb-acm-emac-dark-recovery`) before the first CPU1-sliver flash**, and
keep `dbg_core_enable` (`smp.c:72`) as the runtime escape hatch back to the old
behaviour.

### Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Guest networking dies when CPU1's `emac_poll()` duty cycle drops (§1.3) | **Highest** — it is also the recovery channel | Measure `emac_poll()`'s worst-case pass first; size the sliver period from that, not from a guess. Test with FreeBSD only, no 4th guest, before starting one |
| eMMC clock dies if PC5 re-enforcement is dropped or slowed (§1.1 #3) | High — kills the root fs | Establish **first** whether FreeBSD re-clears PC5 after boot (**[unknown]**). If it does, PC5 enforcement sets the sliver's minimum frequency |
| eMMC lock starvation → `S_IOERR` → dead root fs (§2.3) | High | P5's contention test; a fair/queued lock; consider giving guest 1 strict priority |
| Second core in `gic_timer`/`vgic` corrupts CPU0's timer state (§1.6) | High | P1 before anything else; the twice-reverted vtimer history is the precedent |
| QEMU's `ITARGETSR` fidelity ≠ GIC-400 (P4) | Medium | Treat P4's pass as necessary-not-sufficient; re-prove on hardware with a fresh SPI |
| Snapshot silently mis-restores over a guest slice (§3.3) | Medium | Compile-time assert (P0), not a comment |
| A guest on CPU1 makes `sr`/`sr2` confidently wrong (§1.4) | Medium — it is a *diagnosis* corrupter, the worst kind here | Add the core selector in the same change that gives CPU1 a guest, not later |
| Staging pad reuse races a running guest (§3.3-2) | Medium | Keep all copy-in before `smp_init()`, as `main_dbg.c:453-467` already requires; relocate the pad out of FreeBSD's gigabyte |

---

## 5. What I would **not** do

1. **Do not parameterize `stage2.c`.** `stage2_zephyr.h:7-19` records that it is
   frozen and owned by the A1 isolation milestone, which is hardware-proven
   (project memory `a1-isolation-hardware-proven`). Phase 1's answer — a
   separate, disjoint table set exploiting per-PE `VTCR_EL2`/`VTTBR_EL2`
   banking — is both cheaper and safer, and it scales to four guests by
   duplication. Copy it; do not generalise the load-bearing one.
2. **Do not move or shrink FreeBSD's gigabyte.** It would touch
   `STAGE2_DRAM_SIZE`, the guest DTB's `memory` node, `kload`'s placement
   arithmetic, `vblk_emmc.c:202-204`'s duplicated range, `snapshot.h:77-78`'s
   equality invariant, and the A1 self-check — to buy DRAM that §3.2 shows we do
   not need. Pure downside.
3. **Do not build the SGI/IPI path just to make `hbreak` work.**
   `gdbstub_hw.c:44-58` already weighed exactly this and declined: *"a mis-built
   async-IRQ-into-guest path risks hanging or corrupting the guest, which is worse
   than 'hbreak silently doesn't fire'"*. Nothing in Phase 2 changes that
   calculus. (If an SGI path arrives later for another reason, `hbreak` gets
   fixed for free.)
4. **Do not try to keep snapshot and multi-guest simultaneously.** §3.3 — Option
   B breaks D1's own invariant, and the honest fix (eMMC-backed store) is a
   different project. Formalise the exclusion instead.
5. **Do not keep CPU2's async eMMC offload while CPU2 hosts a guest.** §2.4 —
   the single-slot mailbox would make each guest's traps block on the other's
   I/O for up to 6 s, re-opening the worst bug class in the tree's history to buy
   throughput nobody has measured.
6. **Do not implement vCPU time-slicing or VMID multiplexing.** The whole
   architecture is one guest per physical core with per-PE-banked stage-2
   (`stage2_zephyr.h:12-19`). Time-slicing means context-switching
   `VTTBR_EL2`/`GICH_*`/timer state and inventing a VMID allocator — a
   fundamentally different hypervisor. Four cores is the natural ceiling here,
   and that is fine.
7. **Do not make EMAC interrupt-driven** to "free" CPU1. It would mean the first
   SPI the HV ever enables and targets, on the one device whose failure removes
   all remote access, in a tree where the polled path is proven. If any core must
   poll it, keep polling it.
8. **Do not give guests 2-4 any real MMIO.** `stage2_zephyr.h:29-38` +
   `mmio_absorb.c` give them zero passthrough and absorb every device access.
   That is the property that makes them safe to run next to FreeBSD; a real UART
   or timer for a new guest is not worth reopening it.
9. **Do not expose the eMMC 1:1 to a second guest.** §2.3 — without an LBA
   window a bug in guest 3 can destroy FreeBSD's root filesystem. The window
   comes *before* the second guest gets write access, not after.
10. **Do not "just add a second gate" for any of the interlocks.**
    `vblk_emmc.h:361-371`, `vblk_async.h:119-136` and `smp.c:456-460` each warn
    against exactly this, and `hv_addrmap.h:8-20` records what happened the last
    time a fixed address was hand-picked in a `.c` file.

---

## 6. Corrections to `docs/dual-guest.md`

Found while reading the code for this design.

1. **CPU1 is also the FreeBSD guest's network RX engine.**
   `docs/dual-guest.md:44` describes CPU1's role as *"this project's only
   automatic crash-recovery path"*, which is true but incomplete: `emac_poll()`
   runs **only** on CPU1's loop (`main_dbg.c:90` → `dbgmon.c:1167` ← `smp.c:617`)
   and `vnet_emac.c:86-90` states that the guest's virtio-net RX is fed *from
   inside that loop*. Phase 2's CPU1 cost therefore includes guest networking,
   not just the watchdog and the debugger. It also enforces the eMMC PC5 pinmux
   every pass (`smp.c:553-558`). **[code]**
2. **"What's still open" item 1 (no bulk loader) is already stale.** The working
   tree contains `zstage.c`/`zstage.h`/`test_zstage.c` (untracked, mtime
   2026-08-10 14:05), wired into `DUAL_OBJS` (`Makefile:290`), into the hosted
   test list (`Makefile:121`, `:128`, `:164`), and called from `main_dbg.c:468`
   behind the same weak/strong pattern (weak no-op at `main_dbg.c:62-68`). It stages
   from `ZSTAGE_LOW_PA = 0x4E000000` (`zstage.h:62`). **[code]**
3. **"What's still open" item 3 (no `zboot` re-arm path) is already stale.**
   `dbgmon.c` now has a `zunhalt` command (`dbgmon.c:741`, `:1003`) and
   `zguest_cpu3.c`'s uncommitted diff adds a `g_zguest3_rearm_req` flag plus
   sticky `attempts`/`last_fail`/`rearms` breadcrumb words. `git show
   HEAD:dbgmon.c | grep -c zunhalt` returns 0, so this is in-flight, uncommitted
   work. **[code]**
4. **The `sr` limitation will get worse, not stay neutral.**
   `docs/dual-guest.md:35` correctly notes `sr` has always read CPU1's own
   irrelevant EL1 bank. Once CPU1 hosts a guest, `cmd_sr()`'s direct `mrs` reads
   (`dbgmon.c:254-270`) start returning a *real but wrong* guest's state. A
   harmless zero becomes a plausible lie — see §1.4. **[inferred]**
5. **Item 4's scope statement is accurate but under-specifies the blocker.** It
   names CPU1's watchdog. The actual first blocker for *any* extra guest core
   that takes interrupts is that `gic_timer.c` and `vgic.c` hold single-instance
   module state (`gic_timer.c:391-393`, `:496-505`; `vgic.c:247-285`, `:606`),
   which `smp.c:417-419` already alludes to. **[code]**

---

## 7. One-paragraph summary for whoever executes this

Do P0 and P1 first; they are cheap, board-free, and every later step depends on
them. Then take CPU2 for a third guest **without** disk (P3) — that is a pure
repetition of the proven Phase-1 mechanism and gets three concurrent guests on
hardware with no new register ever written. The first genuinely new hardware
mechanism is the `GICD_ITARGETSR` write (P4); give it its own step and its own
proof, and expect QEMU's pass to be insufficient. Per-guest virtio-blk (P5) is
the largest and most dangerous code change, and its real content is lock fairness,
not refactoring. Leave CPU1 for last (P6), keep the whole service sliver on it,
and accept that the fourth guest is cooperative-grade — because the alternative
is trading away the one thing that has repeatedly saved this board without a
human touching it.
