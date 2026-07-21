# Guest snapshot / restore (checkpoint) — design

**Status:** design + skeleton (`snapshot.c` / `snapshot.h`). CODE-ONLY. No board
edits, no `make`, no changes to existing `.c/.h`. Integration edits are listed
separately in [`snapshot-integration.md`](snapshot-integration.md) as snippets
to apply later.

## 1. Motivation

The FreeBSD/arm64 EL1 guest takes **~300 s** to reach the `mountroot>` prompt /
userland, then reboot-loops (cngrab NULL; PSCI `SYSTEM_RESET` we already block
in `el2_exc.c`). Every debug iteration pays that ~300 s again.

A **snapshot** captures the *complete* EL1/EL0 architectural state plus the
guest's DRAM at a known-good instant (e.g. sitting at `mountroot>` or in a
shell). A later **restore** drops the guest back into that exact state in
**<1 s**, turning "boot for 5 minutes to reproduce" into "restore and go".

This is a VM checkpoint in the classical sense, scoped down to what a
single-guest bare-metal EL2 monitor on the A64 actually needs.

## 2. What defines "the guest state"

Three parts. All three must be captured atomically at a quiesced trap boundary.

### 2.1 GP registers + PC + PSTATE — already captured

On **every** group-2 (lower-EL) trap, `el2_common` (exceptions.S) saves the
full guest context into a `struct el2_frame` (see `exceptions.h`) and, when the
SMP debug core is up, `el2_trap` copies it into the global `g_last_guest_frame`
(`el2_exc.c:37,241`). The frame already holds exactly what the architecture
`eret`s from:

| frame field | offset | meaning |
|-------------|--------|---------|
| `x[0..30]`  | 0x000  | GP regs x0..x30 |
| `kind`      | 0x100  | vector index + group |
| `elr`       | 0x108  | **ELR_EL2 = guest PC** |
| `spsr`      | 0x110  | **SPSR_EL2 = guest PSTATE** (EL1h, DAIF, SS, ...) |
| `esr`       | 0x118  | ESR_EL2 |
| `far`       | 0x120  | FAR_EL2 |
| `sp_at_entry`| 0x128 | EL2 SP at entry (host, not guest) |

So x0..x30, the guest PC and the guest PSTATE cost **nothing extra** — they are
handed to `snapshot_save(frame)` directly.

### 2.2 Banked SP + EL1 system registers — captured via MRS from EL2

At EL2 the EL1 registers are *distinct banked* registers; a plain `MRS` reads
the guest's value (this is exactly what `dbgmon.c`'s `cmd_sr`/`cmd_sr2` already
do to dump live guest state — `dbgmon.c:228,533`). The full set
(`struct snapshot_sysregs` in `snapshot.h`):

- **Stack pointers:** `SP_EL0`, `SP_EL1` (banked — *not* in the eret frame, so
  they must be saved/restored by MSR, see §6.4).
- **MMU / translation:** `SCTLR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `TCR_EL1`,
  `MAIR_EL1`, `AMAIR_EL1`, `CONTEXTIDR_EL1`.
- **Exception / vector:** `VBAR_EL1`, `ESR_EL1`, `FAR_EL1`, `ELR_EL1`,
  `SPSR_EL1` (the guest's *own* EL1 exception link/PSTATE — distinct from the
  ELR_EL2/SPSR_EL2 in the frame).
- **Thread pointers:** `TPIDR_EL0`, `TPIDRRO_EL0`, `TPIDR_EL1`.
- **EL0 access / features:** `CPACR_EL1` (FP/SIMD trap), `MDSCR_EL1` (debug/SS —
  must be coherent with any EL2 single-step state), `PMCR_EL0`.
- **Generic timer (skew-sensitive, §7.3):** `CNTKCTL_EL1`, `CNTP_CTL_EL0`,
  `CNTP_CVAL_EL0`, `CNTV_CTL_EL0`, `CNTV_CVAL_EL0`.
- **EL2-side per-guest state:** `VTTBR_EL2` + `VTCR_EL2` (the stage-2 tables the
  guest runs under), `HCR_EL2` (VM/IMO/RW/TSC trap routing), `CNTVOFF_EL2`
  (virtual-counter offset — the timer re-base knob, §7.3).

> **FP/SIMD note.** This tree builds `-mgeneral-regs-only`; the guest (FreeBSD)
> *does* use FP/SIMD. A complete checkpoint of a running FreeBSD userland
> eventually needs the V0..V31 + FPSR/FPCR banked FP state too. It is **omitted
> from the skeleton** (the EL2 monitor never touches FP, and capturing it needs
> a `CPTR_EL2.TFP=0` window + FP save area). Flagged as a v2 item; for snapshots
> taken at `mountroot>` (kernel context, no live FP user) it is not needed.

### 2.3 Guest DRAM — bulk copy

`stage2.c` identity-maps the guest's IPA→PA over **PA `0x40000000`..`0x80000000`
= 1 GiB** (`stage2.h:85` `STAGE2_DRAM_BASE`/`STAGE2_DRAM_SIZE`;
`coredump.c:18-19` treats the same range as guest RAM, `DRAM_HI = 0x80000000`).
That whole 1 GiB is the guest's memory and must be copied verbatim.

The **stage-2 page tables themselves** (`stage2_l1/l2/l3` in `stage2.c`) live in
the *hypervisor's* .bss, not guest DRAM. They are static and identical across
runs, so we do **not** snapshot the table storage — we snapshot only
`VTTBR_EL2`/`VTCR_EL2` (which point at them) and rely on `stage2_init()` having
rebuilt the same tables. (A future dynamic-stage2 lane would need to snapshot
the tables too; called out as a landmine.)

## 3. Save format

A single `struct snapshot_hdr` (see `snapshot.h`) at the base of the store,
followed by the verbatim 1 GiB DRAM copy:

```
+0x00000  struct snapshot_hdr {
            magic "SNP1", version, valid, store_kind,
            dram_base/size/store, taken_cntpct,
            struct el2_frame   frame;      // x0..x30, PC, PSTATE
            struct snapshot_sysregs sysregs; // full EL1 + per-guest EL2 set
            crc32
          }
+0x10000  verbatim copy of guest DRAM (SNAP_DRAM_SIZE bytes)
```

- All header words written with the tree-standard **`dc civac` + `dsb sy`**
  coherent store (so a host `bc`/`d` dump reads a valid record and it survives a
  warm WDT reset — same discipline as every breadcrumb window).
- `valid` is written **0 first**, flipped to **1 last** with a barrier, so a
  reader (or a crash mid-save) never sees `valid=1` over a torn body.
- `version` gates layout changes; `crc32` (TODO) covers the DRAM copy.

## 4. Where to store it — three back-ends compared

| Back-end | Where | Speed (1 GiB) | Survives power loss? | Complexity | Notes |
|----------|-------|---------------|----------------------|------------|-------|
| **Reserved DRAM (chosen default)** | High GiB `0x80000000`..`0xC0000000`, **guest-invisible** (stage-2 only maps low GiB) | fastest — DRAM→DRAM `ldp/stp`, sub-second | **No** (lost on cold reset; **survives warm WDT reset** — DRAM retained) | low | needs the **2 GiB** board variant |
| **eMMC** via `emmc_bio_write` | `emmc_bio.c` CMD24, one 512 B block per call | slow — 2,097,152 blocks; write path **not yet HW-verified** (`emmc_bio.h:47`) | **Yes** | medium | best for a persistent "golden" checkpoint |
| **Stream over EMAC** | dbgmon channel to a host file | network-bound; needs CPU1 debug core owning EMAC (`dbg_core_active`) | Yes (host disk) | medium | host keeps the image; good for archiving |

**Chosen default: reserved DRAM.** Rationale:

- The guest only sees the **low** 1 GiB (`0x40000000`..`0x80000000`). On the
  2 GiB M64 the **high** 1 GiB (`0x80000000`..`0xC0000000`) is unused by both
  guest and hypervisor — a full-DRAM checkpoint fits there **exactly once**, and
  the guest physically cannot read or clobber it (no stage-2 mapping).
- DRAM→DRAM copy is the only option that hits the **<1 s restore** goal.
- "DRAM survives warm reset" is already exploited across this tree (breadcrumbs,
  `el2_exc.c:54`), so a WDT-reset debug cycle keeps the checkpoint.

**Fallbacks.** On a 1 GiB board there is no high DRAM → use eMMC (persistent,
slow) or EMAC-stream (host-held). `store_kind` in the header records which was
used. The skeleton fully implements the DRAM path and documents the hooks for
the other two (`snapshot.c` `snapshot_save`, ALTERNATE STORE BACK-ENDS comment).

### 4.1 Store-region placement vs. breadcrumb windows

The low breadcrumb region `0x50000000`..`0x50008000` is densely used (MUSB,
EMAC, EXC, GUEST, STAGE2, vconsole's **64 KiB** buffer to `0x50010f10`, gtrace,
FFL1, SST1, HDMI, TX-tee `0x50004000`, trace to `0x50006040`, profiler
`0x50006800`, onebp `0x50007000`). The snapshot store deliberately lives up at
`0x80000000`, clear of all of it; only its **header** is a struct (no fixed
low-DRAM window is consumed).

## 5. Save sequence

1. **Quiesce** (caller/precondition, §7.1): reach a guest trap boundary with
   secondaries parked and no device DMA in flight.
2. Mark header `valid=0`, stamp magic/version/store_kind.
3. Record geometry + **`CNTPCT_EL0`** (`taken_cntpct`, for timer re-base).
4. `frame` verbatim into `hdr.frame`; `sysregs_capture()` MRS sweep into
   `hdr.sysregs`; `dsb sy`.
5. `dram_copy(store, 0x40000000, 1 GiB)` — per-line `dc cvac` clean.
6. `dsb sy`; flip `valid=1` (coherent). Done — **non-destructive**, guest can
   resume immediately (checkpoint, not teardown).

## 6. Restore sequence

Runs at EL2 on the tick/trap path with the live guest `frame`:

1. **Reload DRAM** `dram_copy(0x40000000, store, 1 GiB)` *before* sysregs, so
   the memory the MMU will translate already holds snapshot contents.
2. **Timer re-base** (§7.3): `delta = CNTPCT_now − taken_cntpct`; push
   `CNTVOFF_EL2 −= delta` so the guest's virtual counter reads its snapshot-time
   value instead of jumping forward by real elapsed minutes.
3. **`sysregs_reload()`** — order: stage-2/HCR/CNTVOFF first → EL1 MMU group
   (MAIR/AMAIR/TCR/TTBR0/TTBR1/CONTEXTIDR, then SCTLR_EL1 last) → vector/exception
   → thread pointers → EL0 access → timer last. `ISB` after each regime change.
4. **Cache/TLB maintenance** (§6.3).
5. **Rewrite `*frame = hdr.frame`** in place. When `el2_trap` returns,
   `el2_common` restores x0..x30 and `eret`s using `frame->elr`→ELR_EL2 and
   `frame->spsr`→SPSR_EL2 — landing the CPU in the restored guest at the
   snapshot PC/PSTATE with the snapshot registers. This *is* the "jump into the
   guest"; no separate `eret` is issued from `snapshot.c`.

### 6.3 Cache / TLB invalidation on restore (required)

We changed **both** DRAM contents **and** the translation regime, so stale
state must be dropped (`restore_maintenance()`):

- `dsb ish`
- `tlbi vmalle1is` — guest EL1&0 **stage-1** TLB, inner-shareable.
- `tlbi alle1is` — EL1 incl. **stage-2** (all VMIDs; safe hammer since VTTBR
  base may differ pre/post).
- `dsb ish`
- `ic ialluis` — **I-cache**: we rewrote guest `.text` pages in DRAM; without
  this the core can execute pre-restore instructions.
- `dsb ish; isb`.

The DRAM copy loop itself cleans **D-cache** lines to PoC as it writes, so the
new memory is visible to the guest whether it runs caches-off (early) or on
(FreeBSD).

### 6.4 SP_EL0 / SP_EL1 subtlety

The banked stack pointers are **not** part of the eret-restored frame — they are
restored by `MSR sp_el0` / `MSR sp_el1` inside `sysregs_reload()`. `guest.c:122`
already documents that `msr sp_el1` from EL2 targets the banked (not live)
register, which is exactly what we want. Do **not** try to route SP through
`frame`.

## 7. Correctness constraints & risks

### 7.1 Consistency — must be quiesced at a trap boundary

The whole scheme is only sound if the snapshot is a *consistent cut*: the guest
must be stopped at an EL2 trap boundary (so `frame` is coherent and no guest
instruction is mid-flight) with **no outstanding device transactions**. The
natural trigger is the dbgmon tick path (`dbgmon_service(frame)`), where the
guest is already parked in EL2. This is a **wiring contract**, not something
`snapshot.c` can assert.

### 7.2 SMP considerations

CPU1 is the persistent debug core (`smp.c`, MEMORY: smp-debug-core-working) and
runs `dbgmon`/EMAC independently of the guest. The guest itself may have brought
up secondaries via PSCI `CPU_ON` (forwarded in `el2_exc.c:305`). For a
consistent snapshot:

- **Save:** take it while only CPU0 is in the guest, or ensure guest secondaries
  are at a trap boundary too. A guest running on multiple cores mid-copy yields a
  torn memory image. Simplest: snapshot at `mountroot>` (single-threaded kernel
  context) — the intended use.
- **Restore:** guest secondaries must be **parked** before `snapshot_restore`;
  restoring CPU0's frame while a guest secondary still runs stale code corrupts
  the restored guest. CPU1 (our debug core) stays out of the guest and is fine.
- We restore only CPU0's architectural frame; per-core guest state on secondaries
  is out of scope for v1 (the target checkpoints are single-core).

### 7.3 Timer skew (called-out risk)

`CNTPCT_EL0` free-runs across the (arbitrarily long) gap between save and
restore. The saved `CNTx_CVAL` are **absolute** counts; naively restored they
are now far in the past → the guest either takes an immediate storm of expired
timer IRQs or believes minutes passed instantly. Mitigation: re-base
`CNTVOFF_EL2 −= (CNTPCT_now − taken_cntpct)` so the guest's virtual counter
resumes at its snapshot value. **TODO(board):** confirm FreeBSD/arm64 here uses
the *virtual* timer (CNTVOFF-based); if a build uses the physical timer, the
delta must instead be *added* to `CNTP_CVAL_EL0`.

### 7.4 BIGGEST RISK — device / GIC state is NOT in the snapshot

The checkpoint captures **CPU + DRAM only**. It does **not** capture, and
restore does **not** roll back:

- **MMIO device registers:** eMMC/SMHC2 (`emmc_bio.c`), EMAC, MUSB/USB-OTG,
  UART, HDMI, PMIC, GPIO/pinmux (incl. the PC5 MMC-clock fix). After restore the
  physical devices are in their **current** state, not their snapshot state.
- **GIC** distributor/redistributor/CPU-interface state and any pending/active
  IRQs; the **vGIC** list registers if `vgic.o` is linked.
- **In-flight DMA:** an eMMC/EMAC descriptor the guest programmed can complete
  into DRAM *after* the copy, or reference a buffer whose contents we rolled
  back — silent corruption.

Consequence: a guest that was mid-way through a device transaction at snapshot
time will, on restore, resume with driver software state (in DRAM) that
disagrees with the hardware — e.g. FreeBSD's `aw_mmc` thinks a command is
outstanding that the controller has long since forgotten. **This is the single
biggest correctness risk.** It is *tolerable for the intended use* — snapshot at
`mountroot>` (quiescent I/O, no live device transaction) — but makes arbitrary
mid-I/O snapshots unsound without device-model save/restore (a much larger
effort: paravirtualized or fully-emulated devices with checkpointable state).

## 8. Performance

DRAM→DRAM, 1 GiB, 64-bit `ldr/str` with per-line `dc cvac`: memory-bandwidth
bound. LPDDR3 on the A64 gives multiple GB/s, so a 1 GiB copy is well under a
second — the <1 s restore goal is met by the DRAM back-end. eMMC/EMAC back-ends
are seconds-to-minutes and are for persistence/archival, not the fast loop.

Optimization headroom (v2): snapshot only *dirty* guest pages by write-protecting
DRAM in stage-2 and tracking faults, shrinking both save time and store size for
incremental checkpoints.

## 9. Files

- `snapshot.h` / `snapshot.c` — this module (skeleton).
- `docs/snapshot-integration.md` — exact edits to wire it in (apply later).
