# EL2 Non-cacheable guest-DRAM remap — findings, design, residual risks

Companion to `docs/aw-mmc-dma-coherency.md` (root-cause analysis: the aw_mmc
IDMAC corruption is caused by EL2-on-CPU0 and the CPU1 dbgmon core being
extra *cacheable* observers of guest DRAM that don't exist bare-metal).
This document covers the fix: `el2_ncmap.h`/`el2_ncmap.c`, hooked from
`main_dbg.c`. Read that doc first for the corruption mechanism; this one is
about the remap that removes it.

## 1. Findings

### 1.1 Current EL2 stage-1 setup (before this change)

**EL2 runs entirely on U-Boot's inherited stage-1 tables.** This is stated
explicitly by the codebase itself, not inferred:

- `start.S`'s header contract: *"Do NOT touch the MMU, caches, or U-Boot's
  stack pointer — we run on whatever SP U-Boot handed us"* (`start.S:9-12`).
- `stage2.h`'s own header: *"We do NOT touch stage-1 anywhere (neither our
  own EL2 stage-1, which stays exactly as U-Boot's flat mapping left it, nor
  a future guest EL1 stage-1)"* (`stage2.h:11-13`).
- `smp.h`'s design comment: *"We do not build our own page tables: we
  CAPTURE the primary's MMU/EL config in smp_boot_config[] (smp_init(), on
  CPU0) and each secondary reloads it verbatim"* (`smp.h:22-24`).

So before this change: MAIR_EL2/TCR_EL2/TTBR0_EL2 are whatever U-Boot's
final, post-relocation state left them — granule, level count, MAIR index
assignment and any holes in the map are all **unknown and unverified** from
this tree's source. The FreeBSD guest's own memory attributes (Normal-WB
data buffer, Normal-NC IDMAC descriptor ring, as established in
`aw-mmc-dma-coherency.md` §1) are a completely separate concern (guest EL1
stage-1 + stage-2) and are untouched by anything in this document.

### 1.2 CPU1 (and CPU2/3) translation regime

**Same regime, same tables, not per-CPU.** `smp_init()` (`smp.c:181-224`)
captures the primary's live `MAIR_EL2`/`TCR_EL2`/`TTBR0_EL2`/`VBAR_EL2`/
`HCR_EL2`/`SCTLR_EL2` into `smp_boot_config[]` (a flat array, cleaned to PoC
by `clean_boot_config()` so a secondary can read it MMU-off). `_start_secondary`
(`start.S:114-166`) reloads those exact captured values, in order
MAIR→TCR→TTBR0→VBAR→HCR, then last enables the MMU by writing the captured
`SCTLR_EL2`. **CPU1 does not get its own copy of the tables — it walks the
literal same physical L1 table CPU0 built**, via the TTBR0 value CPU0 handed
it.

This has one direct consequence exploited by the design below: **whatever
this file switches TTBR0_EL2/MAIR_EL2/TCR_EL2 to on CPU0, BEFORE `smp_init()`
runs, is what CPU1 (and 2/3) will run under too — automatically, with zero
changes to `smp.c`/`start.S`.** If the switch happened *after* `smp_init()`
captured the old values, secondaries would keep using U-Boot's original
(cacheable) map regardless of what CPU0 later did — this is why the call
site in `main_dbg.c` is ordered strictly before `smp_init()`.

### 1.3 HV DRAM-touch inventory

| Consumer | Location | Verdict |
|---|---|---|
| EMAC descriptor rings + packet buffers | `emac.c:140-144`: `SCRATCH_BASE=0x50100000`, `TX_DESC_BASE`/`RX_DESC_BASE`/`TX_BUF_BASE`/`RX_BUF_BASE`, spanning `0x50100000..0x50109000` | **NOT** inside the HV image (`0x42000000+1MiB`); **NOT** inside the stated 1 MiB breadcrumb/debug reserved window either (`0x50000000+1MiB` ends exactly at `0x50100000`) — it sits in the 1 MiB **immediately above** that window. Falls into the new **NC region**. Safe: `emac.c:175-188`'s `cache_clean()`/`cache_inval()` already hand-roll the identical `dc civac`/`dc ivac` dance aw_mmc's own driver does for non-coherent descriptor DMA — those calls become harmless no-ops (data already at PoC) once this range is NC. See §4.3 for the DTB-reservation gap this exposes. |
| MUSB gadget buffers (`ep0_out_buf`, `tx_buf`, `rx_ring`, descriptors) | `musb.c` static arrays (confirmed via `nm`: e.g. `ep0_out_buf` @ `0x42036c68`) | Inside HV `.bss`, i.e. inside the HV-image island (`0x42000000..0x42100000`). Stays Normal-WB. MUSB has no bus-master DMA (CPU-mediated FIFO PIO), so cacheability here was never a coherency hazard to begin with. |
| `kload`'s kernel/DTB/modinfo copy targets | `main_dbg.c`: `K_PABASE=0x46000000`, `DTB_DST=0x47200000`, `MODINFO=0x47400000` — all guest DRAM, by design | Ordinary guest DRAM, now NC (outside both islands). `kload.c:229-238` (`kload_cache_clean_inval`, `dc cvac` clean + `dc ivac` invalidate) already explicitly flushes every span it writes (`kload.c:384,486,637`). Under the new mapping these writes bypass the cache entirely (already at PoC), so those existing calls become **redundant, harmless no-ops** — not removed here (`kload.c` is out of scope for this change) but flagged. |
| `gmem`/dbgmon peek-poke (`cmd_read_words`/`cmd_read_bytes`/`cmd_write_word`/`cmd_write_byte`/`cmd_dump`, `dbgmon.c:248-330`) | Arbitrary PA, by design (any guest or host address) | Plain `volatile` loads/stores today, **zero existing cache maintenance**. Once the remap is applied, every one of these against guest DRAM outside the two islands becomes **coherent by construction** (NC memory, no stale-line risk) with no code change to `dbgmon.c` needed — a direct, free correctness improvement to the live debugger itself. |
| Guest kernel/DTB/env execution + data (everything FreeBSD touches at EL1) | All of guest DRAM | Entirely governed by stage-2 + the guest's own EL1 stage-1 — **untouched by this change** (see §1.1). |

## 2. Design

### 2.1 Why a fresh table instead of editing U-Boot's live one

U-Boot's table layout (granule, levels, MAIR assignment, and whether it has
holes anywhere outside the regions this tree is known to touch) is
unverified. Editing an unknown table in place is exactly the class of bug
behind the failed prior attempt (§3). Building a **fresh** table in HV
`.bss`, with MAIR chosen from scratch (no need to match/guess U-Boot's index
assignment) and exactly the coverage this tree is known to need, removes
that unknown entirely — every mapping's correctness is verifiable from this
file alone.

### 2.2 Table topology

4 KiB granule, `T0SZ=25` (39-bit input address space, 3-level walk
starting at level 1 — comfortably covers everything this tree touches, all
below 2 GiB). `PS` comes from `ID_AA64MMFR0_EL1.PARange` read at runtime,
mirroring `stage2.c`'s own `VTCR_EL2.PS` convention (never hard-coded).
`MAIR_EL2` defines 3 indices: `0=Device-nGnRE`, `1=Normal Inner+Outer
Non-cacheable`, `2=Normal Inner+Outer Write-Back RW-Allocate`.

```
el2_l1[512]        1 GiB/entry.  idx0 -> el2_l2_low, idx1 -> el2_l2_dram.
                   Everything else: fault (nothing above 2 GiB is ever touched).

el2_l2_low[512]    2 MiB/entry, VA 0..1G. Only idx 8..15 (0x01000000..
                   0x02000000, the MMIO this HV uses: GIC/CCU/PIO/MUSB/EMAC/
                   eMMC controller) populated, Device-nGnRE. Rest UNMAPPED
                   (see §4.1 — the SRAM breadcrumb window used by
                   gic_timer.c/vgic.c/el2_exc.c is dead code in this build).

el2_l2_dram[512]   2 MiB/entry, VA 0x40000000..0x80000000 = guest DRAM.
                   Default: Normal-NC. idx16 (0x42000000..0x42200000) and
                   idx128 (0x50000000..0x50200000) instead point to
                   el2_l3_img[]/el2_l3_bcw[] (each WB island is exactly half
                   of one of these 2 MiB blocks). Whichever blocks bracket
                   the CURRENT call stack (captured at apply() entry, padded)
                   are ALSO left Normal-WB (§4.2).

el2_l3_img[512]    4 KiB/entry, VA 0x42000000..0x42200000.
                   idx0..255   (0x42000000..0x42100000): Normal-WB, XN=0 —
                     the HV image; EL2 executes code from here.
                   idx256..511 (0x42100000..0x42200000): Normal-NC, XN=1 —
                     ordinary guest DRAM just above the image.

el2_l3_bcw[512]    4 KiB/entry, VA 0x50000000..0x50200000.
                   idx0..255   (0x50000000..0x50100000): Normal-WB, XN=1 —
                     breadcrumb/debug DRAM window (vconsole ring, eMMC lock
                     word at 0x50020100, etc.).
                   idx256..511 (0x50100000..0x50200000): Normal-NC, XN=1 —
                     includes emac.c's own DMA scratch (§1.3, §4.3).
```

Total new `.bss`: 5 tables × 4 KiB = 20 KiB (24 KiB with alignment padding,
confirmed by a scratch build) — negligible against the 1 MiB HV-image
budget (current image ≈224 KiB per `nm`; 248 KiB after this change, checked
live against `__bss_end` before ever switching — see §2.4 gate 1).

Every block/page is marked `XN=1` (non-executable) except the HV image's own
WB half. This only affects EL2's OWN accesses (see §1.1's last row) — it has
no effect on what the guest can execute, since guest execution is governed
by stage-2 + the guest's own EL1 stage-1, not EL2 stage-1.

### 2.3 Ordering: why the call site is where it is

`el2_ncmap_apply()` is called from `main_dbg.c`, immediately after
`wdt_arm()` and before `kload_parse_elf()`:

1. **After `el2_install()`** (already several lines earlier): a stray fault
   during table build/switch lands in *our* vector table, not U-Boot's.
2. **After `wdt_arm()`**: the ~16 s HW dead-man's-switch is already armed, so
   the transition-maintenance sweep (§3, potentially ~1 GiB of `dc civac`)
   can safely call `wdt_pet()` periodically without relying on undefined
   pre-arm behaviour. `wdt_arm()` stamps "last progress" as fresh at the
   moment it runs (`wdt.c:70`), so a `wdt_pet()` moments later is guaranteed
   to actually re-arm the hardware timer, not just no-op.
3. **Before `kload_parse_elf`/`kload_place_segments`/`kload_build_modinfo`**:
   the ~30 MB kernel + DTB + modinfo copy goes through the *new* NC mapping
   directly, so `kload.c`'s own explicit clean+invalidate calls become
   redundant (harmless) rather than needed.
4. **Before `smp_init()`**: per §1.2, this is the load-bearing constraint —
   CPU1 must capture the *post-switch* MAIR/TCR/TTBR0.

### 2.4 Fail-safe by construction

This cannot be tested on the real board as part of this change (hard rule:
no board access). Every assumption is therefore checked at runtime, and a
failed check **aborts the remap and returns**, leaving TTBR0_EL2 untouched
— the HV keeps running on U-Boot's original (today's, imperfect but known)
tables rather than risking a table that might immediately fault:

1. **Gate 1 (`el2_ncmap.c`, top of `el2_ncmap_apply`)**: `__bss_end` must
   still be below `0x42100000` (the HV-image island boundary) — else this
   file's own newly-added state could straddle the WB/NC boundary it is
   about to create.
2. **Gate 2**: the captured stack pointer must land inside guest DRAM
   (`0x40000000..0x80000000`) — expected (§4.2) but verified, never assumed.
3. **Gate 3**: a software self-walk of the just-built tables (`self_check()`,
   mirrors `stage2.c`'s own `STAGE2_SELFTEST_IPA` self-test pattern) must
   resolve 6 representative addresses (HV image, both halves of both
   islands, DRAM base, DRAM top) to the intended PA and NC/WB attribute.

Only if all three pass does the code ever write `MAIR_EL2`/`TCR_EL2`/
`TTBR0_EL2`. A distinct abort status code is left in the breadcrumb window
at `0x50011000` ("NCM1") for each gate, plus the diagnostic value that
tripped it (see `el2_ncmap.c`'s `NCM_ST_*` codes).

## 3. Transition maintenance — sequence and why it cannot repeat the old wedge

### 3.1 The old failed attempt (for context — dead `if (0)` code, `main_dbg.c`)

A blanket `dc civac` over the *entire* `0x40000000..0x80000000` range, no
island exclusion, placed at the very top of `main()` — **before**
`el2_install()` even runs. It wedged EMAC (dbgmon dead after load). Since
this ran before `emac_init()`/`smp_init()`, CPU1 did not exist yet, so the
"raced a live DMA descriptor hand-off on another core" mechanism (a real,
separate hazard — see §3.3) cannot be the explanation for *that specific*
placement. The most defensible candidates, none disprovable without board
access, are:

- It invalidated the HV's **own currently-executing image** (`.text`/`.data`/
  `.bss`, all within the swept range) — including this exact loop's own
  code, mid-execution.
- It invalidated the region backing **U-Boot's own live stack and possibly
  its page tables** (nothing in this tree records where U-Boot's
  post-relocation SP/tables physically sit — see §4.2) while the HV was
  still running *on* that same U-Boot stack per `start.S`'s explicit
  contract.
- It ran **before `el2_install()`**, so any translation fault from a hole in
  U-Boot's (unverified, possibly incomplete) table over that 1 GiB would
  have been delivered to *U-Boot's own* exception vector, not ours — an
  unrecoverable, undiagnosable path from this tree's perspective.

### 3.2 This design's sequence, and why each of the above is structurally excluded

1. **Switch first, sweep second** (not the reverse). This is the textbook
   ARM-recommended order for changing a region's memory-type attribute:
   `DC *VAC`-family instructions act on the cache by **physical** address,
   independent of the *currently active* stage-1 attribute for that VA — so
   running the sweep with the new (fully, provably mapped — Gate 3) tables
   already installed also catches any last-instant speculative cache fill
   that occurred in the brief window between old and new mappings.
2. **The sweep only ever touches ranges the software already classified NC**
   (`sweep_nc_region()` walks the exact same per-block classification used
   to build `el2_l2_dram`) — by construction it:
   - **never touches the HV image** (`0x42000000..0x42100000` — excluded by
     the `idx==HVIMG_L2_IDX` branch, which sweeps only the NC *half*,
     `0x42100000..0x42200000`);
   - **never touches the live call stack** (`block_in_stack_margin()`
     ranges are `continue`d, never swept — see §4.2 for the margin's limits);
   - **never touches anything CPU1 might be using, because CPU1 does not
     exist yet** — `smp_init()` runs strictly after `el2_ncmap_apply()`
     returns (§2.3 point 4), so there is no live descriptor hand-off on
     another core to race.
3. **Runs after `el2_install()` and `wdt_arm()`** (§2.3), unlike the old
   attempt — any residual fault is diagnosable, and the ~16 s watchdog is
   fed throughout via periodic `wdt_pet()` calls in the sweep's inner loop
   (every 65536 lines, i.e. every ~4 MiB), removing sheer-duration-trips-a-
   watchdog as a possible failure mode regardless of whether that was ever
   the old mechanism.

### 3.3 The *other* known-real hazard (not this code's failure mode, but worth naming)

A **separate**, previously-tried placement — sweeping right before
`kload_enter()`, i.e. *after* `smp_init()` — is documented as having wedged
EMAC by invalidating live descriptor memory CPU1's dbgmon loop was actively
using. The likely mechanism: forcing an out-of-band cache write-back of a
multi-word descriptor **mid-update** (a CPU1 store sequence doing
`field-writes → dsb → set-owned-by-DMA bit`) can expose a **torn**
descriptor to the actual EMAC hardware DMA engine earlier than the writer
intended. This design avoids that class of hazard entirely by never
sweeping after CPU1 exists (§3.2 point 2, third bullet) — not by reasoning
about safe timing around it.

## 4. Residual risks

### 4.1 SRAM breadcrumb window is deliberately left unmapped

`gic_timer.c`/`vgic.c`/`el2_exc.c` reference an SRAM breadcrumb range
(`0x00018000..0x00018400`, "GICT"/"VGIC"/EL2-exceptions windows). Confirmed
via `grep` that `gic_timer_init()`/`vgic_init()` — the only writers — are
**called only from `repl.c`** (a different build/entry point), never from
`main_dbg.c`'s `main()`. This SRAM range is therefore dead code in the live
`dbg` image and is **not mapped** in the new tables (mapping it would need
either a 4th static assumption about the region's contents or a 5th table
tier for 4 KiB granularity there too, for no live benefit). **If
`gic_timer_init()`/`vgic_init()` are ever wired into `main_dbg.c`'s path in
the future, this SRAM range must be added to `el2_ncmap`'s tables first**,
or those breadcrumb writes will take a translation fault under the new
mapping.

### 4.2 The stack-safety island is a heuristic, not a guarantee

`start.S`'s contract is explicit: the HV never switches away from U-Boot's
inherited stack pointer, for the entire life of the session (`main()`, every
EL2 trap handler, every `dbgmon_service()` call). `el2_ncmap_apply()` reads
the *current* SP once, at the moment it runs (early — right after
`wdt_arm()`), and marks a generously padded (16 MiB below, 2 MiB above,
2 MiB-block-aligned) island around it Normal-WB. Two things follow:

- **This is sized from a single early snapshot.** If the call stack ever
  grows deeper later in the (potentially long-lived, interactive debug)
  session than this margin accounts for, further frames land in NC memory —
  functionally correct (ordinary loads/stores work fine on NC memory) but
  slower. No code here performs an exclusive load/store on a plain
  stack-resident location (§4.4), so this is a **performance-only** risk,
  not a correctness one, short of an actual stack overflow (a pre-existing
  risk, independent of this change).
- **This island is NOT reflected in the guest's DTB `/reserved-memory`**
  the way the HV-image and breadcrumb-window islands are (per this task's
  own stated background, only those two are carved out). Wherever U-Boot's
  stack physically sits is, from the guest's point of view, ordinary
  available RAM — FreeBSD could in principle place a DMA buffer there,
  which would reintroduce the exact cache-coherency hazard this whole
  project addresses, for just that narrow, unreserved slice. This can only
  be fully closed by a DTB edit (out of scope: hard rule forbids touching
  the DTB in this change) reserving that range too. Recommended follow-up:
  determine U-Boot's actual post-relocation SP/heap footprint for this
  board port and add it to the DTB reservation alongside the existing two.

### 4.3 EMAC's own DMA scratch region sits just outside the stated breadcrumb reservation

Per §1.3: `emac.c`'s descriptor ring + packet buffers occupy
`0x50100000..0x50109000`, immediately **above** the stated 1 MiB
breadcrumb/debug reserved window (`0x50000000..0x50100000`), not inside it.
This pre-exists this change (the DTB reservation gap, if it is one, is not
introduced here) and is **not itself worsened** by the NC remap — if
anything, making this range NC is strictly safer for EMAC's own DMA than
leaving it cacheable (matching the same fix rationale as the guest's own
aw_mmc problem, applied to a HV-owned device this time). Flagged here purely
because it was noticed during the DRAM inventory (task deliverable #1), not
because this change makes it worse. Same DTB caveat as §4.2: this range is
not part of the guest's `/reserved-memory` per the task's stated carve-out,
so a future guest kernel could in principle place a DMA target there and
collide with EMAC's live descriptors — independent of caching attributes
entirely.

### 4.4 Exclusive-load/store (`ldxr`/`ldaxr`/`stxr`/`stlxr`) audit

A53 is ARMv8.0 (no LSE atomics) — every exclusive-monitor pair in this tree
was located and checked against the new map. **All resolve to a WB
location** (either the HV image or the breadcrumb/debug DRAM window), never
the new NC region:

| Site | Target | Location | Verdict |
|---|---|---|---|
| `vblk_emmc.c:145-147` | eMMC controller lock word | `VBLK_EMMC_LOCK_PA = 0x50020100` (`vblk_emmc.h:206`) | `0x50020100 < 0x50100000`, inside the breadcrumb-window island (`0x50000000..0x50100000`). **WB, safe.** |
| `smp.c:342-346` | `g_online` (CPU-online bitmap) | plain `static volatile uint32_t` in `smp.c`'s `.bss` | Inside HV-image island. **WB, safe.** |
| `trace.c:55-58` | event-trace ring header word | `TRACE_BASE = 0x50004000` (`trace.h:14`) | Inside breadcrumb-window island. **WB, safe.** |
| `profiler.c:56-58,72-77` | profiler histogram header/bucket word | `PROF_BASE = 0x50006800` (`profiler.h:15`) | Inside breadcrumb-window island. **WB, safe.** |
| `smp.h:129-141` generic `spinlock_t` (`spin_lock`/`spin_unlock`) | every instance: `ktimer.c:52` (`g_kt_lock`), `sched.c:106` (`g_sched_lock`), `wcet.c:32` (`g_w_lock`), plus embedded fields in `ksync.h` objects | All plain `static` file-scope globals | Inside HV-image island (`.bss`). **WB, safe.** |

No exclusive-monitor site needs to move or be converted to a non-exclusive
protocol as a result of this change — a fully clean audit.

### 4.5 Sweep duration

Sweeping ≈1 GiB minus islands at 64-byte granularity is ≈16.6M `dc civac`
operations. Not measurable on real hardware as part of this change; rough
order-of-magnitude estimate (tens of cycles per op on an idle/cold line) puts
this at low hundreds of milliseconds to a few seconds — well inside the
16 s HW watchdog window, which is additionally fed throughout (§3.2 point 3).
If live testing shows this is materially slower than expected, the sweep can
be parallelized across cores after `smp_init()` in a future iteration — not
done here to keep the change minimal and to preserve the "CPU1 doesn't exist
yet" safety property (§3.2).

## 5. Live-test plan (5 lines)

1. Flash/boot as usual; watch for the `NCM1` breadcrumb at `0x50011000`:
   word[1] should read `0x00000007` (`NCM_ST_SWEPT_DONE`) — any other
   terminal value means an abort gate tripped (see the `NCM_ST_*` codes) and
   the HV fell back to U-Boot's original tables (no worse than today).
2. Confirm EMAC/dbgmon/USB console still come up normally (this change must
   be a no-op for everything *except* cache attributes) — same boot
   milestones as before this change.
3. Boot the FreeBSD guest with the direct `aw_mmc` path (as currently
   configured, `vblk_init()` disabled) and confirm `mmcsd0` **enumerates**
   (EXT_CSD read via DMA no longer intermittently corrupt) — the original,
   currently-failing symptom this whole effort targets.
4. Let the guest run through `rc.d` into userland and confirm no SIGSEGV /
   GPT-taste storm under sustained disk activity (the original intermittent
   corruption symptom).
5. Over the network debugger, `bc 0x50011000` mid-session and confirm
   words `[3]`/`[4]` (stack-margin bounds) bracket a plausible U-Boot SP
   value, and spot-check a `dbgmon` `r`/`d` of a few guest-DRAM addresses
   outside the islands against a fresh `emmc_bio` PIO read of the same data
   (per `docs/aw-mmc-dma-instrument.md`'s method) to directly confirm the
   "coherent by construction" claim in §1.3.
