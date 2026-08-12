# W^X on guest DRAM: can it be enforced, and is it? (ROADMAP v1 gate)

This is the last unclosed half of the v1 isolation gate item ("W^X на
гостевых маппингах"). `stage2_wx_scan()`/`stage2_wx_selfcheck()` (commit
`8723328`) already measured, board-free and on hardware, that guest DRAM is
RW+executable everywhere (510 leaves, every boot) and explained why a
**static** boot-time fix cannot close that without either leaving `kldload`'s
pages unenforced or breaking `kldload` outright (a hardware-verified working
feature no board-free CI exercises). This document evaluates the **dynamic**
alternative the ROADMAP task asked for, with evidence, and records the
outcome: **implemented, off by default.** Read `stage2.c`'s own comments
(around `stage2_wx_selfcheck()` and the new `STAGE2_WX_DYNAMIC` block that
follows it) and `stage2.h`'s `STAGE2_WX_DYNAMIC` block before changing
anything here — this file explains *why*, they are the load-bearing *what*.

## Bottom line

**Yes, dynamic W^X enforcement is mechanically sound and is implemented** —
stage-2 permission faults on execute/write are correctly distinguishable and
reachable, the flip logic is genuinely W^X (not "sometimes XN"), and it
reuses only already-hardware-proven primitives (the existing broad TLB
flush, the existing on-demand-table-split pattern from the first-fault
vector probe). It is **shipped disabled** (`STAGE2_WX_DYNAMIC` defaults to
`0` in `stage2.h`) because the mechanism's safety net — a bounded pool of
on-demand L3 tables that fails *open* (degrades to today's behavior) rather
than *closed* (hangs the guest) when exhausted — has a capacity that is a
**judgment call, not a validated number**: whether it is big enough for a
real `kldload` workload's physical-page placement depends on the guest
FreeBSD kernel's own page allocator, which this hypervisor cannot see and
this task could not measure without the board it was expressly forbidden
from touching. That one unmeasurable number, not a flaw in the mechanism, is
what keeps this off. See "What needs hardware" at the end.

## Q1 — Does an instruction abort from a lower EL actually reach `el2_trap()`, distinguishably from a data abort?

**Yes, already, today — confirmed by reading `el2_exc.c`, not assumed.**

`el2_trap()`'s guest-sync-trap block (`(kind>>2)==2u && (kind&3u)==EL2_KIND_SYNC`,
`el2_exc.c` around line 741) decodes `ec = (esr>>26)&0x3f` once and dispatches
on it. Before this change, exactly one consumer looked at EC 0x20/0x21
(instruction abort) at all:

```c
/* First-fault probe: a stage-2 abort (instr EC 0x20/0x21 or data
 * 0x24/0x25) on the guest's unmapped EL1 vector page carries the
 * ORIGINAL, still-unmasked EL1 fault — latch it, remap, resume. */
if ((ec == 0x20u || ec == 0x21u || ec == 0x24u || ec == 0x25u) &&
    firstfault_handle(frame))
    return;
```

`firstfault_handle()` only matches one fixed IPA (`GUEST_VECTOR_IPA`) and
returns 0 for everything else. Past that point, the dispatch chain's *next*
block is gated `ec == 0x24u` only (the "Guest (lower-EL) synchronous DATA
ABORT" section that calls `vconsole_handle_fault`/`vblk_mmio_fault`/
`vnet_mmio_fault`/`scanout_mmio_fault`) — an EC 0x20/0x21 instruction abort
that isn't the vector page falls straight through *all* of that (the `if`
guard excludes it) to the generic "record for post-mortem" path, which never
advances ELR for a guest fault. Net effect, confirmed by reading the code:
**an unhandled real instruction-abort permission fault would have re-faulted
the SAME instruction forever until the watchdog reset the board** — exactly
the "static split breaks kldload" failure the DRAM measurement already
flagged, just triggered dynamically instead of at boot.

Distinguishing instruction from data abort is a single EC comparison
(0x20/0x21 vs 0x24/0x25 — both already computed). Distinguishing a
*permission* fault (the case this mechanism must handle) from a
*translation* fault (unmapped IPA — the case it must NOT touch, e.g. the A1
hv-image/hv-scratch carve or the vblk/vnet trapped windows) is bits[5:2] of
the ISS, the fault-status-code *class* field, identical position/encoding
for IFSC and DFSC alike:

```c
uint32_t fsc = (uint32_t)(frame->esr & 0x3Fu);   /* IFSC or DFSC alike */
if ((fsc & 0x3Cu) != 0x0Cu)
    return 0;   /* not a permission fault at any level -- not ours */
```

`0x3C` masks bits[5:2]; `0x0C` is the permission-fault class (`0b0011`) at
any level (the level bits[1:0] are deliberately left unmasked/ignored, so
this matches a permission fault reported at level 2 — a still-flat 2 MiB
block — or level 3 — an already-split page — without needing to know which
in advance).

**Not the decisive question.** This was the easiest of the five: reachable
today, cheaply distinguishable, and the insertion point (right after
`firstfault_handle()`, guarded by `#if STAGE2_WX_DYNAMIC`) adds one branch to
an existing chain, mirroring the exact calling convention every sibling
handler (`vblk_mmio_fault(frame)`, `hwbp_handle(frame, esr)`) already uses.
Proven board-free in `test_stage2_tables.c`'s
`test_wx_dynamic_fault_classification` (rejects the wrong EC, rejects a
translation fault, rejects a non-DRAM IPA, rejects both HV windows, accepts
the genuine case).

## Q2 — Granularity cost: does per-page splitting fit?

**The full/unconditional version provably does not. A bounded, on-demand
version comfortably does — measured, not estimated.**

This hypervisor has a hard, pre-existing, unrelated constraint that turns
out to be exactly the right lens for this question: the whole HV image
(`.text+.rodata+.data+.bss`) must fit in a fixed 2 MiB window at
`0x42000000` (`link.ld`'s `ASSERT(__bss_end - 0x42000000 <= 0x200000, ...)`,
which is also the same window `stage2.c`'s A1 partition excludes from the
guest's stage-2 map — see `HVIMG_L2_IDX`). Every static array this mechanism
needs competes with the rest of the hypervisor for that same 2 MiB, forever.

**Measured baseline** (this tree, before this change, `size
microkernel-dbg.elf`):

```
   text	   data	    bss	    dec	    hex	filename
 149712	    284	 524080	 674076	  a491c	microkernel-dbg.elf
```

674076 bytes used of 2097152 (2 MiB) — **1423076 bytes (≈1.357 MiB) of
slack.**

**Full/unconditional cost** (every 2 MiB block in the 1 GiB DRAM window
individually split to a 4 KiB-granularity L3 table, i.e. what "just split
everything up front" would mean): 512 blocks × one 4 KiB table each = exactly
**2 MiB** — the *entire* budget, before counting a single byte of the rest
of the hypervisor. Proven, not asserted: compiling `stage2.c` with the pool
sized to this exact worst case (`-DSTAGE2_WX_POOL_TABLES=512`) and linking it
against the real `dbg` object set fails **today's pre-existing, unrelated**
`link.ld` safety net immediately:

```
/usr/bin/aarch64-linux-gnu-ld: HV image exceeds the 2 MiB stage-2 protected
window at 0x42000000 -- extend HVIMG protection in stage2.c or shrink the image
```

(verbatim linker output from the experiment; see "Verification" for the
exact commands.) So the full version is not merely expensive — it is
**unbuildable** in this tree's current architecture. This is the fact that
forces the design to be a *bounded pool with a fail-open degrade path*
rather than "just enforce it everywhere": there is no version of "everywhere"
that fits.

**The bounded design's actual cost, measured** (default
`STAGE2_WX_POOL_TABLES=64`, i.e. 64 on-demand 4 KiB L3 tables = 256 KiB
nominal): linking the real `dbg` object set with `stage2.o`/`el2_exc.o`
recompiled `-DSTAGE2_WX_DYNAMIC=1` (default pool size):

```
   text	   data	    bss	    dec	    hex	filename
 149712	    284	 794416	 944412	  e691c	microkernel-wxdyn-experiment.elf
```

944412 / 2097152 = **45.0% of the budget**, i.e. **1152740 bytes (≈1.10 MiB)
of slack still remaining** — comfortably fits, with room for the rest of
this actively-growing tree (dozens of subsystems keep landing in this same
image) to keep growing too. The delta from baseline is +270336 bytes of
`.bss` (64 × 4096 = 262144 nominal; the extra 8192 is linker/alignment
overhead from the `aligned(4096)` array, not a bug).

**Why 64, precisely**: not a validated capacity (see Q3/Q4 and "What needs
hardware") — a deliberately conservative starting point, picked to leave the
large majority of this image's slack untouched for everything else in this
tree. `STAGE2_WX_POOL_TABLES` is one `#define` (`stage2.h`) to retune once
someone has a real number to retune it against.

**Not, by itself, decisive** — a *reasonable* bound fits fine. What Q2
proves is that *some* bound is mandatory (the unconditional version is
provably impossible here), which sets up the real question in Q4.

## Q3 — Fault cost: is it acceptable for a realistic `kldload`?

**Cheap in aggregate time; the interesting number is how many POOL SLOTS a
real boot consumes, not how many faults.** Two components, kept separate on
purpose because they have very different evidence quality:

**1. Per-fault latency: an architectural estimate, not a measurement in this
repo.** This tree has no existing measured EL2-trap round-trip figure for
this exact path (`wcet.c`/`profiler.c` measure the RTOS tick handler's own
latency, not a stage-2 fault round trip). A synchronous exception entry +
frame save + C dispatch + `stage2_wx_flip()`'s few dozen instructions +
`stage2_tlb_flush()` (a `dsb`+`tlbi`+`dsb`+`isb` sequence) + `eret` is, on a
Cortex-A53-class core at this SoC's clock, architecturally on the order of
hundreds to low thousands of cycles — call it roughly a microsecond,
explicitly an estimate this task cannot turn into a measurement without the
board.

**2. How many pool slots a real boot actually needs: bounded by BLOCK count,
not page count, and this part IS board-free-verifiable from this tree's own
placement code.** `kload_place_segments()` (`kload.c`) places every PT_LOAD
segment of the *initial* kernel image at `dest = pa_base + (p_vaddr -
kernbase)` — a fixed linear offset, which means the whole kernel image (every
segment together) lands in **one contiguous physical range**, not scattered
pages. Reading the actual deployed kernel's section sizes (read-only,
`/opt/bzdos/tftpboot/kernel.debug` — never written, per this task's hard
constraints):

```
.text    0xa7d560  (≈10.47 MiB)
.rodata  0x1525cc  (≈1.39 MiB)
.data + .data.* + .bss  ≈ 4.2 MiB combined
```

Total kernel image footprint ≈ 16 MiB, contiguous ⇒ **≈8 distinct 2 MiB
blocks** — about 12% of the default 64-slot pool, for the *entire* kernel's
own code and data, before a single `kldload`. This directly contradicts my
own first-pass (wrong) estimate that a 1 MiB module could need up to 256
slots by assuming worst-case per-page fragmentation: this tree's own
placement code proves the *initial* image is not fragmented at all.

**What this does NOT cover, and cannot cover board-free**: `kldload`
allocates *module* pages from the running guest kernel's own free-page list,
entirely inside FreeBSD, a stock/vendor kernel this project does not control
the source of. Whether a `kldload`'d module's pages land in one contiguous
run (likely, for a freshly-booted, lightly-fragmented system — the common
case for FreeBSD's `link_elf_obj` module loader, which requests one sized
allocation) or scattered across many 2 MiB regions (possible after enough
`kldload`/`kldunload` churn fragments the free list — exactly the shape of a
long build session, which is what is running on the board right now, which
this task was forbidden from inspecting) is genuinely unknown from here.
**This is the one number in this whole analysis that only the board can
supply.**

**Not, by itself, decisive** — the per-fault cost is negligible either way;
the pool-slot count depends on the same unmeasurable quantity Q4 does.

## Q4 — The dangerous case: ping-pong, and how it's bounded

**Bounded by construction for the ping-pong RATE (cheap); bounded by policy,
not by proof, for the pool-EXHAUSTION risk (the actual open question).**

*Ping-pong rate.* Once a 2 MiB block has been split (once, ever, the first
time any page in it executes), every later flip — either direction — rewrites
one already-allocated L3 entry. It never allocates a new table again for
that block. So the *rate* of W↔X transitions on one page is bounded by how
often the **guest** alternates writing and executing that specific address,
not by anything this mechanism does — and FreeBSD's own `kld` loader follows
a write-phase-then-execute-phase-then-(maybe, on unload)-write-phase pattern
per module, not an instruction-by-instruction interleave. A page recycled
from code back to data (the task's own example: a module unloads, its
physical page is reused) costs exactly one write-side flip; a page recycled
back to code again costs one more execute-side flip. Proven in
`test_stage2_tables.c`: `test_wx_dynamic_write_fault_flips_back_to_writable`
drives a page through code→data and asserts it lands on genuine RW+XN (not
RW+X — the negative control in that same test hand-poisons the leaf to RW+X
and confirms `stage2_leaf_is_wx()`, the existing checker, still correctly
flags it as a violation, so the positive assertions are not tautological).

*Pool exhaustion — the real risk, and how it's bounded.* A **new**,
never-before-split 2 MiB block's first execute fault, after the pool is
already full, cannot be given a table. Two options exist architecturally:

- **Fail closed** (deny the fetch): a guest stage-2 fault never advances
  ELR, so a denied fetch re-faults the identical instruction forever —
  the guest hangs until the hardware watchdog resets the board. This is
  categorically the failure this whole feature exists to avoid (it is the
  *dynamic* version of "static split breaks kldload").
- **Fail open** (what this implements): revert the *whole* 2 MiB block to a
  plain RW+X descriptor — today's unconditional, hardware-verified behavior
  for that one region — and count it. The guest is never denied. The region
  is honestly reported as a W^X violation by `stage2_wx_scan()` (it is not
  hidden, and is not enforced there) — that is the truth, and is the
  documented, bounded cost of a fixed-size pool, not a bug.

Proven in `test_stage2_tables.c`'s `test_wx_dynamic_pool_exhaustion_fails_open`:
fills the pool exactly to `STAGE2_WX_POOL_TABLES`, then forces one more
distinct block through the fault path and asserts (a) it is still handled
(`handled == 1` — the guest is not denied), (b) the pool did not grow past
its configured size, (c) the exhaustion counter incremented, (d) the
overflowed block is now a plain, non-split, RW+X 2 MiB leaf — and that every
block filled *before* exhaustion is completely unaffected (bounded means
bounded, not "everything degrades together"). Verified load-bearing by
neutering: forcing the exhaustion path to return 0 (fail closed) instead of
1 makes this exact test fail immediately (`Assertion 'handled == 1' failed`)
— see "Verification".

**What is NOT proven, and is exactly Q2's open number restated**: whether
*this* boot's actual demand for distinct code-bearing 2 MiB regions — kernel
image (≈8, proven above) plus every `kldload`'d module, compounded over
however many hours a real session runs — stays under `STAGE2_WX_POOL_TABLES`
(64, comfortably, by this analysis's best-effort estimate) or exceeds it
(in which case coverage silently narrows via the fail-open path, which is
safe but is a *quieter* kind of not-fully-enforced than the current
unconditional state, since it only shows up in the exhaustion counter, not
in an outright regression).

**This, together with Q2, is the decisive pair.** Q2 proves a bound is
mandatory; Q4 supplies the only safe way to live with one (fail open); but
whether the *specific* bound chosen is enough for the *specific* workload
this board is actually running is the one judgment call this document
cannot turn into a proof.

## Q5 — Correctness hazard: self-modifying code / icache / TLB

**No new fundamental hazard found; one real implementation risk, deliberately
avoided by reusing an already-proven primitive instead of a new one.**

*Self-modifying / JIT-like code.* FreeBSD's kernel module loader already
must perform instruction-cache maintenance (`ic`/`dc` maintenance ops) after
writing new code and before it is ever executed — that is an architectural
requirement independent of this hypervisor; ARMv8 I-caches are not
coherent with the D-cache by construction. This mechanism's default state
(RW+XN) matches exactly the phase during which the guest writes/relocates a
module (permitted, no fault); the *first* fetch — which, correctly, only
happens after the guest's own write-then-maintenance sequence completes —
is what triggers the flip. This mechanism does not need to teach the guest
anything new about cache maintenance; it only needs to not fetch-permit a
page before the guest is done writing it, which the default state already
guarantees. Stage-2 permission bits do not gate `dc`/`ic` maintenance
instructions (they are not instruction fetches), so there is no interaction
there either.

*TLB staleness — the one implementation choice this section is really about.*
A stage-2 permission *tightening* (RW+XN → RO+X taking away write, or the
reverse taking away execute) is exactly the class of change where a STALE,
too-permissive cached translation is a real security hazard, not just a
performance one: if the old (more permissive) entry is still cached when
the new descriptor is installed, the CPU could keep honoring the OLD
permission until something invalidates it. This tree has never, until this
change, needed to invalidate the stage-2 TLB for anything other than a
whole-table rebuild or the two existing table-splits
(`stage2_unmap_guest_vector()`/`stage2_map_guest_vector()`), both of which
use the same broad primitive: `stage2_tlb_flush()`
(`dsb ish; tlbi vmalls12e1; dsb ish; isb`) — invalidate *everything*, stage-1
and stage-2, for the current VMID, unconditionally.

A narrower, IPA-scoped invalidate (`tlbi ipas2e1is` + the required follow-up
stage-1 invalidate for the same IPA range) would be measurably cheaper on a
mechanism that may run many times per boot instead of once — but this tree
has **zero** history of using it, and this is the single most
safety-critical file in the tree, being changed with **zero** hardware
verification available this pass. This implementation deliberately reuses
the existing, broad, already-hardware-proven `stage2_tlb_flush()` rather
than invent and hope a narrower invalidate is correctly scoped and ordered —
accepting a real but bounded performance cost (one full stage-1+2 TLB
invalidate per flip, not per instruction) in exchange for not adding a
second, independently-unverified risk on top of the first. This is the same
trade this project's own precedent (`docs/dma-bypass-stage2.md`) already
made explicitly: "erring toward a solid design... is better than a big
risky change."

*Concurrency.* Only CPU0 ever runs the FreeBSD guest and takes its traps
(the `dual` target's second guest on CPU3 has its own, wholly disjoint
stage-2 tables in `stage2_zephyr.c` and never reaches this code at all). A
core cannot take a second synchronous exception while still inside this
handler for the first, so there is no concurrent mutation of
`stage2_l2_dram[]`/the pool to guard against on any target this tree builds
today. Documented explicitly in `stage2.c` so it is a stated scope
assumption, not a silent one, if this is ever extended.

**Not decisive** — the correctness path is sound given the conservative TLB
choice; the risk this section identifies (a narrower invalidate) was
avoided rather than taken.

## What was implemented

Additive, and disabled by default:

- **`stage2.h`**: `STAGE2_WX_DYNAMIC` (defaults to `0` via `#ifndef`,
  overridable with `-DSTAGE2_WX_DYNAMIC=1`), `STAGE2_WX_POOL_TABLES`
  (defaults to `64`), and the `stage2_wx_fault()` prototype — the prototype
  and its one dependency (`exceptions.h`, for `struct el2_frame`) are
  themselves inside the `#if STAGE2_WX_DYNAMIC` block, so an off build does
  not even declare the function.
- **`stage2.c`**: `STAGE2_DRAM_XN_DEFAULT` (`= (unsigned)STAGE2_WX_DYNAMIC`)
  replaces the four call sites that used to hard-code `xn=0` for a DRAM leaf
  (the flat 1 GiB block in `stage2_init()`, the 2 MiB blocks in
  `stage2_build_dram_table()`, the 4 KiB pages in `stage2_unmap_guest_
  vector()`/`stage2_map_guest_vector()`) — with the flag at its default 0,
  this expands to the literal constant every one of those call sites
  already had. The mechanism itself (`S2AP_RO`, `stage2_wx_page_desc()`,
  the pool, `stage2_wx_flip()`, `stage2_wx_fault()`) is entirely inside one
  new `#if STAGE2_WX_DYNAMIC` block, placed after `stage2_wx_selfcheck()`.
- **`el2_exc.c`**: one new dispatch line, `#if STAGE2_WX_DYNAMIC`-guarded,
  inserted right after the existing `firstfault_handle()` check (same ECs,
  same calling convention as every sibling handler).
- **`hv_addrmap.h`**: a new diagnostic breadcrumb lane (`HVMAP_WXDYN_BC`,
  `0x50093000`, "WXD1") with the same `_Static_assert` non-overlap chain
  every other lane in this file gets — confirmed clear by grepping the whole
  tree for `0x5009[0-9a-f]{4}`/`0x500a[0-9a-f]{4}` before picking it (only
  the immediately-preceding `HVMAP_SCANOUT_BC` matched). Diagnostic only:
  pool used/exhausted/flip counters, never read back by any code path,
  never gates behavior.
- **`test_stage2_tables.c`**: a hand-transcribed mirror of the pure
  table-mutation logic (same discipline as every other function in this
  file — see its own header comment), plus four new tests:
  `wx_dynamic_execute_fault_flips_to_read_execute` (the positive proof — a
  RW+XN leaf becomes genuinely RO+X, not "RW and X", cross-checked against
  the existing `stage2_leaf_is_wx()`), `wx_dynamic_write_fault_flips_back_
  to_writable` (the symmetric proof, plus a hand-poisoned negative control),
  `wx_dynamic_pool_exhaustion_fails_open` (the bounded-coverage proof),
  `wx_dynamic_fault_classification` (the Q1 dispatch-correctness proof).

**Nothing about the default, hardware-verified boot path changes.** No
Makefile target was added or edited — `STAGE2_WX_DYNAMIC`'s header-level
default is what every existing target (`dbg`/`dual`/`gdb`/`fbsd`/`zephyr`/
`qemu`/...) already gets, with zero edits to any of them.

## Verification (verbatim)

**`make test`** — full hosted suite, exit code 0. The stage-2 section:

```
./test_stage2_tables
...
[ RUN ] wx_dynamic_execute_fault_flips_to_read_execute
[ OK  ] wx_dynamic_execute_fault_flips_to_read_execute
[ RUN ] wx_dynamic_write_fault_flips_back_to_writable
[ OK  ] wx_dynamic_write_fault_flips_back_to_writable
[ RUN ] wx_dynamic_pool_exhaustion_fails_open
[ OK  ] wx_dynamic_pool_exhaustion_fails_open
[ RUN ] wx_dynamic_fault_classification
[ OK  ] wx_dynamic_fault_classification
---- stage2 table tests: 17/17 passed ----
```

Every other suite in `make test` (virtqueue, kload, vconsole, gdbstub, vgic,
sd_bio, snapshot, bmc, coredump, scanout, plus every Python self-test and
dry-run) passed unchanged.

**`make dbg`** — `make clean && make dbg`, clean cross-build:

```
   text	   data	    bss	    dec	    hex	filename
 149712	    284	 524080	 674076	  a491c	microkernel-dbg.elf
```

**Byte-for-byte identical** to the measured pre-change baseline — the
off-by-default flag genuinely changes nothing in the shipped image.

**`./qemu-ci.sh`**:

```
qemu-ci: PASS
HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)
HV: first tick — EL2 preempted the EL1 guest
...
QEMU-CI: PASS (stage-2 + EL1 guest + GICv2 timer preemption confirmed)
```

**`hv_addrmap.h` `_Static_assert` chain covering the new window** (already
compiled and checked by `make dbg`/`make test` today — `bmc.c`, `emmc_bio.c`,
`sd_bio.c`, `smp.c`, and `vconsole.c` all already include this header
unconditionally in the default `dbg` build, independent of this feature):

```c
#define HVMAP_WXDYN_BC        0x50093000UL
#define HVMAP_WXDYN_BC_SIZE   0x20UL
#define HVMAP_WXDYN_MAGIC     0x57584431UL   /* "WXD1" */

_Static_assert(HVMAP_WXDYN_BC >= HVMAP_SCANOUT_BC + HVMAP_SCANOUT_BC_SIZE,
               "dynamic W^X breadcrumb lane overlaps the scanout breadcrumb lane");
_Static_assert(HVMAP_WXDYN_BC + HVMAP_WXDYN_BC_SIZE <= 0x50100000UL,
               "dynamic W^X breadcrumb lane runs into el2_ncmap.c's "
               "non-cacheable DMA scratch window (SCRATCH_BASE 0x50100000)");
```

**Supplementary evidence (not required by the gate, done to ground Q2 with
real numbers instead of arithmetic)**: `stage2.c`/`el2_exc.c` recompiled with
`-DSTAGE2_WX_DYNAMIC=1` and linked against the real `dbg` object set in a
throwaway build (never touching the tree's own `microkernel-dbg.elf`/`.bin`,
never touching `/opt/bzdos/tftpboot/`, never touching the board) — clean
compile, zero warnings under `-Wall -Wextra`, sizes as quoted in Q2 above.
The same experiment with the pool forced to the full theoretical worst case
(`-DSTAGE2_WX_POOL_TABLES=512`) reproduced the `link.ld` failure quoted in
Q2, confirming the existing safety net catches an oversized pool
automatically. Two deliberate-bug ("neutering") checks confirmed the new
tests are load-bearing rather than tautological: forcing the RO+X grant to
RW+X instead makes `wx_dynamic_execute_fault_flips_to_read_execute` fail
(`Assertion 'l.s2ap == S2AP_RO' failed`); forcing the exhaustion path to fail
closed instead of open makes `wx_dynamic_pool_exhaustion_fails_open` fail
(`Assertion 'handled == 1' failed`) — both exactly as expected, both
reverted (only the tree's real, unmodified files are part of this change).

## What needs hardware to confirm

1. **The decisive one.** Whether `STAGE2_WX_POOL_TABLES=64` covers a real
   session's actual demand for distinct code-bearing 2 MiB regions — the
   initial kernel image's cost is proven here (≈8 blocks, from
   `kload_place_segments()`'s contiguous placement and the real kernel's
   measured section sizes), but every `kldload`'d module's physical
   contiguity is a property of the guest FreeBSD kernel's own allocator,
   invisible to this hypervisor and unmeasured by anything board-free
   available to this task. A single board experiment — build with
   `STAGE2_WX_DYNAMIC=1`, boot, watch the `WXD1` breadcrumb
   (`pool_used`/`pool_exhausted`/`flip_count` at `0x50093000`) through a
   real boot and a real `kldload` session — would answer this directly.
2. Whether the broad `stage2_tlb_flush()` call on every flip is fast enough
   in practice under a real, busy fault sequence — `qemu-ci.sh` never
   exercises a real FreeBSD guest touching this path, and the hosted tests
   check logical correctness, not timing.
3. Whether this specific deployed kernel build ever writes to its own
   `.text`/`.rodata` after the very first instruction fetch (e.g. an early
   self-relocation or errata-workaround patch) — this was considered and
   is architecturally unlikely for a non-PIE, non-KASLR arm64 FreeBSD
   kernel, but "unlikely" is not "confirmed", and this task had no way to
   inspect the actual kernel source/config that produced the deployed
   image.
4. General hardware validation that this mechanism, once turned on, does not
   regress any of the existing hardware-proven gates (100-boot streak,
   break-glass streak, A1 isolation self-check) — none of which have ever
   run with `STAGE2_WX_DYNAMIC=1`.

Per this task's own instruction: enabling this by default, or claiming the
ROADMAP gate item closed, on the strength of the board-free evidence above
alone would be exactly the "half-working enforcement in the most
safety-critical file in the tree" outcome this task asked not to produce.
What is delivered instead is a mechanism that is *ready* for the one
hardware experiment that would close it for real, with every board-free
question this task posed answered with evidence rather than left open.
