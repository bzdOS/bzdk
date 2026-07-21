# aw_mmc IDMAC DMA corruption under the bzdOS EL2 hypervisor — root-cause analysis

**Status:** analysis + ranked fixes. Confirmation procedure in
`docs/aw-mmc-dma-instrument.md`.

**Symptom (given, proven):** the on-disk eMMC image is byte-perfect (the HV's own
`emmc_bio` CPU-PIO/FIFO reads return the GPT and UFS2 superblock verbatim). The
FreeBSD guest reads the *same* eMMC through its native `aw_mmc` driver (SMHC2
IDMAC = internal DMA) and gets **intermittent** corruption: root mounts, rc.d
runs (hundreds of reads OK), then a userland binary (sysctl, automount) reads
back subtly wrong → SIGSEGV → init dies → A64 watchdog reboot. PIO never
corrupts; IDMAC DMA intermittently does.

---

## 0. TL;DR

* The stage‑1 ⊗ stage‑2 **attribute combining is correct** on this core. The
  descriptor ring stays **Normal Non‑cacheable** and the data buffer stays
  **Normal Write‑Back**, exactly as FreeBSD intends. Stage‑2 does **not**
  silently make the "DMA‑coherent" descriptor memory cacheable. **`FEAT_S2FWB`
  is neither available (A53 is ARMv8.0; S2FWB is v8.4) nor needed** — reject
  fix (a) outright.
* Because the descriptor ring is genuinely Non‑cacheable, **descriptor
  staleness (failure mode a) is ruled out**. The IDMAC always fetches the
  descriptors the guest wrote.
* The corruption is **data‑buffer cache incoherency (failure mode b)**, and it
  is **hypervisor‑specific**: the HV adds **two extra cacheable observers of
  guest DRAM that do not exist on bare metal** — EL2 itself (CPU0) and the
  **CPU1 dbgmon debug core** — both mapping all guest DRAM as Normal‑WB‑Inner‑
  Shareable and both actively reading/writing DRAM while the guest runs
  non‑coherent IDMAC DMA. The guest's per‑transfer `dc ivac` maintenance is
  correct for a *single*-observer system; the extra observers intermittently
  leave a stale/dirty line in the shared A53 L2 that either survives the
  invalidate or writes back over freshly‑DMA'd data. Per‑transfer race ⇒
  "mostly OK, some corrupt". PIO is immune because the CPU *writes* the buffer
  through its own cache and thereby owns the line.
* **Fix on ARMv8.0:** remove the competing cacheable aliases, not S2FWB. In
  priority order: (1) keep all HV‑owned state and the dbgmon read path from
  cacheably touching guest‑usable DRAM (memory‑map hygiene + a `dc ivac` in the
  debug read path), and clean+invalidate guest DRAM to PoC before first guest
  entry; (2) fall back to forcing the guest's `aw_mmc` data buffer
  Non‑cacheable if (1) is impractical.

---

## 1. What FreeBSD actually allocates, and with which cacheability

Source: `/opt/bzdos/build/freebsd-src/sys/arm/allwinner/aw_mmc.c` and the arm64
busdma/pmap layer (`sys/arm64/arm64/busdma_bounce.c`, `pmap.c`, `locore.S`,
`cpufunc_asm.S`, `sys/arm64/include/{vm.h,pte.h,armreg.h}`). All confirmed by
reading the tree, not assumed.

### 1a. The IDMAC descriptor ring — Normal Non‑cacheable

`aw_mmc_setup_dma()` (aw_mmc.c:553‑609):

* `aw_dma_tag` created with **`flags = 0`** (line 567) → a **non‑coherent** tag
  (`BF_COHERENT` is *not* set).
* ring allocated `bus_dmamem_alloc(..., BUS_DMA_COHERENT | BUS_DMA_ZERO, ...)`
  (line 574).

In `bounce_bus_dmamem_alloc()` (busdma_bounce.c:504‑538), the combination
"non‑coherent tag **+** `BUS_DMA_COHERENT` request" hits:

```c
else if ((flags & BUS_DMA_COHERENT) != 0 &&
    (dmat->bounce_flags & BF_COHERENT) == 0)
        attr = VM_MEMATTR_UNCACHEABLE;     /* line 530-536 */
```

`VM_MEMATTR_UNCACHEABLE == 1` (`vm.h`), used as the MAIR index
(`ATTR_S1_IDX(x)=x<<2`, `pte.h`), and MAIR index 1 = `MAIR_NORMAL_NC = 0x44` =
**Normal Inner+Outer Non‑cacheable** (`locore.S`, `armreg.h`). Shareability is
`pmap_sh_attr = ATTR_SH(ATTR_SH_IS)` — Inner‑Shareable bits — but SH is
architecturally **ignored for Normal‑NC** (treated Outer‑Shareable).

The ring's map is then flagged `DMAMAP_COHERENT`, so **`bus_dmamap_sync` on the
descriptor tag is a no‑op** (aw_mmc.c:689, 1003 do nothing for the ring). The
guest simply writes descriptors into Non‑cacheable memory (writes go straight to
the Point of Coherency = DRAM) and the IDMAC reads them from DRAM. No cache
maintenance is required or performed for the ring — **because it is genuinely
Non‑cacheable.**

Ring format (`aw_mmc.h:214`, one entry = 16 bytes):
`{ uint32 config; uint32 buf_size; uint32 buf_addr; uint32 next; }`.
For the eMMC (`a64_emmc_conf`, aw_mmc.c:112) `dma_desc_shift = 0`, so `buf_addr`
and `next` hold **unshifted physical addresses**; `buf_size == 0` means "max" =
`dma_xferlen = 0x2000`.

### 1b. The data buffer — Normal Write‑Back, explicit maintenance

`aw_mmc_prepare_dma()` (aw_mmc.c:662‑721) loads the caller's buffer into
`aw_dma_buf_tag` (created with `flags = BUS_DMA_ALLOCNOW`, **no**
`BUS_DMA_COHERENT`, line 598 → non‑coherent, non‑DMAMAP_COHERENT map). The
buffer is ordinary kernel memory: **Normal‑WB (MAIR 0xff), Inner‑Shareable**.
Coherency with the IDMAC is by explicit `bus_dmamap_sync`:

* PRE, read path (aw_mmc.c:687‑688) → `dma_dcache_sync(..., PREREAD)` →
  `dma_preread_safe()` (busdma_bounce.c:968‑981): clean the two boundary
  cachelines (`dc cvac`) then **invalidate the range (`dc ivac`)**.
* POST, read path (aw_mmc.c:996‑1006) → `dma_dcache_sync(..., POSTREAD)` →
  **invalidate the range (`dc ivac`)** (busdma_bounce.c:1029‑1031).

All ops are **by‑VA to the Point of Coherency**, each followed by `dsb ish`
(`cpufunc_asm.S`). A53 caches are PIPT, so `dc ivac` on the guest VA acts on the
physical line regardless of which alias created it.

**This is standard, correct non‑coherent‑DMA handling and it works on bare metal
FreeBSD/A64.** The driver is not buggy.

---

## 2. Stage‑1 ⊗ stage‑2 attribute combining under THIS HV (no FWB)

HV facts (from `stage2.c`, `main_dbg.c`, `guest.c`, `start.S`):

* Stage‑2 maps **all** guest DRAM (0x40000000‑0x80000000) as a single attribute:
  `S2_MEMATTR_NORMAL_WB = 0xF` (Normal Inner+Outer WB RW‑alloc), `SH = 0b11`
  (Inner‑Shareable), `AF=1`, `S2AP=RW` — one 1 GiB block per GiB
  (`stage2_init()`, stage2.c:584‑597; `stage2_block_desc()`, :136‑148).
* **`HCR_EL2.FWB` is not set** (no `FEAT_S2FWB`; A53 = ARMv8.0 does not implement
  it). Only `HCR_EL2.VM` (bit0), `RW` (bit31) and `TSC` (bit19) are set;
  `IMO/FMO` are cleared (main_dbg.c:170‑185, guest.c:191‑194).
* VTCR: `IRGN0=ORGN0=WB`, `SH0=IS`, `T0SZ=24` (40‑bit IPA), `SL0=1`
  (stage2.c:460‑611). Cacheable, Inner‑Shareable stage‑2 walks.

Without `FEAT_S2FWB`, ARM's stage‑1⊗stage‑2 **combining rule** (ARM DDI 0487,
"Combining stage 1 and stage 2 memory type and cacheability attributes") is
*"the more restrictive attribute wins"* — evaluated independently for the type
and, for Normal memory, for inner and outer cacheability:

* type: `Device` if either stage is Device, else Normal;
* Normal cacheability: **Non‑cacheable if either stage is Non‑cacheable**, else
  Write‑Through if either is WT, else Write‑Back.

Apply it:

| Region | Guest stage‑1 (EL1) | Stage‑2 (this HV) | **Combined effective** |
|---|---|---|---|
| **Descriptor ring** | Normal‑**NC** (MAIR 0x44) | Normal‑WB‑IS | **Normal‑NC** (NC wins) |
| **Data buffer** | Normal‑**WB**‑IS (MAIR 0xff) | Normal‑WB‑IS | **Normal‑WB‑IS** |

**Both regions get exactly the memory type FreeBSD designed for.** Stage‑2
cannot *upgrade* the Non‑cacheable ring to cacheable — that would require
`FEAT_S2FWB` with `MemAttr[3:2]=0b11` forcing WB, which this core does not have
and this HV does not (and could not) set. The "stage‑2 makes the coherent DMA
memory cacheable so the IDMAC reads stale descriptors" hypothesis is
**false here**. Descriptor staleness (mode a) is off the table.

> Shareability combines to Inner‑Shareable for the data buffer (both stages IS),
> so the guest's `dc ivac … dsb ish` is broadcast to the whole Inner‑Shareable
> domain — which on this A53 cluster includes **CPU1**. Keep this in mind for
> §3: the broadcast is what *should* have saved us, and the failure is about the
> cases it does not cover.

---

## 3. Where the corruption actually comes from (mode b, HV‑specific)

The guest driver is correct and the attributes are correct, yet DMA reads
corrupt only under the HV. The only things the HV changes versus bare metal:

1. **EL2 runs on CPU0 with MMU+caches ON, using U‑Boot's tables**, which map all
   DRAM as **Normal‑WB cacheable** (`start.S` contract lines 5‑12: "Do NOT touch
   the MMU/caches — run on whatever U‑Boot handed us"; U‑Boot enters at EL2 with
   the MMU already on). EL2 reads/writes guest DRAM cacheably on every trap.
2. **CPU1 (the dbgmon debug core) is brought up cache‑coherent with CPU0**
   (`_start_secondary`, start.S:114‑182: reloads the primary's MAIR/TCR/TTBR0,
   enables MMU+caches, relies on the A53 SCU/SMPEN for coherency). CPU1 maps the
   **same** cacheable DRAM and runs continuously: heartbeat/SMP breadcrumbs
   (`smp.c`), `usbacm_poll` mutating gadget state, and **`dbgmon` physical‑memory
   reads** (`cmd_read_words`/`cmd_write_word`, dbgmon.c:246‑318, "read/write
   PHYSICAL memory — flat EL2 map").
3. The **entire HV image and its mutable state live inside guest DRAM**: U‑Boot
   `go 0x42000000` loads the HV at 0x42000000 (in 0x40000000‑0x80000000), and the
   breadcrumb windows (0x50000000‑0x5000ffff) are also in guest DRAM. EL2 and
   CPU1 write these cacheably while the guest believes it owns that RAM.

So while the guest performs a non‑coherent IDMAC read, **two additional agents
(EL2 on CPU0, dbgmon on CPU1) hold cacheable lines for guest‑DRAM physical
pages in the shared A53 L2.** The IDMAC is *outside* this coherency domain. Two
concrete corruption paths, both intermittent, both DMA‑only:

* **(b‑i) Stale survivor.** A cacheable observer (EL2 during a trap, or CPU1)
  pulls a line of the data‑buffer page into the shared L2 (explicit dbgmon read,
  or hardware prefetch on Normal‑WB memory) around the DMA window. If the A53
  SCU/`dc ivac`‑`ish` broadcast to CPU1 is not fully effective for that line
  (e.g. SMPEN/coherency edge, or the line re‑fills after the guest's POSTREAD
  `dc ivac` but before the guest's load), the guest CPU0 reads a **stale** copy
  instead of the IDMAC's fresh DRAM data.
* **(b‑ii) Dirty write‑back clobber.** A cacheable observer holds a **dirty**
  line for a physical page that FreeBSD has repurposed as a DMA buffer, and a
  natural L2 eviction writes that dirty line back to DRAM **after** the IDMAC
  filled it → DRAM now holds stale bytes → the guest reads garbage. (EL2/CPU1
  create dirty lines whenever their own state or a breadcrumb happens to share a
  physical page the guest later hands to the IDMAC.)

**Why intermittent / mostly‑OK:** each transfer independently wins or loses the
cache race depending on whether an extra observer happened to be caching that
page and on L2 eviction timing. Early boot (rc.d) touches relatively little
memory; heavy userland exec (sysctl, automount) reads many pages and eventually
loses one → single corrupt page → SIGSEGV. This is the signature of a
*data‑side* race (mode b), **not** a descriptor problem (mode a), which would
mis‑target the DMA deterministically and crash hard/early.

**Why PIO (`emmc_bio`) never corrupts:** PIO drains the FIFO with CPU stores
into a Normal‑WB buffer — the CPU **write‑allocates and owns** the line with
correct data, so any pre‑existing/foreign line for that PA is simply
overwritten, and the subsequent CPU read hits the CPU's own fresh line. The
non‑coherent IDMAC, by contrast, writes DRAM *behind* the caches and depends on
every observer's copy being invalidated — which the extra HV observers break.

---

## 4. Ranked candidate fixes (A53 = ARMv8.0, no S2FWB)

### ✗ (a) `HCR_EL2.FWB` + `FEAT_S2FWB` stage‑2 MemAttr — **impossible and unnecessary**
`FEAT_S2FWB` is ARMv8.4; the Cortex‑A53 is ARMv8.0 and does not implement it —
setting `HCR_EL2.FWB` is `RES0`/ignored. And §2 shows the combining already
yields the intended attributes, so there is nothing for S2FWB to fix. **Reject.**

### ✓ (d)+hygiene — **HV‑side: remove the competing cacheable aliases + clean baseline (recommended)**
The problem is *extra cacheable observers of guest DRAM*, so eliminate them.
Locations:

1. **Give the debug read path a coherent (PoC‑accurate) view.** In
   `dbgmon.c:cmd_read_words()` (dbgmon.c:246‑264), issue `dc ivac` on each line
   *before* the load (or map the debug window Non‑cacheable), so debug reads
   neither return stale EL2‑cached data nor leave clean stale lines behind. This
   also makes the confirmation in `docs/aw-mmc-dma-instrument.md` trustworthy.
   *(This is the single smallest change and doubles as the diagnostic lever.)*
2. **Stop EL2/CPU1 from cacheably writing guest‑usable DRAM.** Move HV‑owned
   state out of the guest's usable RAM, or mark it reserved in the guest DTB so
   FreeBSD never allocates those pages as DMA buffers:
   * relocate the breadcrumb windows (0x50000000‑0x5000ffff) and, ideally, the
     HV load/stack region out of guest RAM (SRAM, or a DTB‑`reserved‑memory`
     carve‑out);
   * ensure `smp.c`/`usbacm`/EMAC/vconsole buffers touched by CPU1 live in the
     reserved region too.
   The existing breadcrumb helpers already `dc civac` after each store; extend
   that discipline (or non‑cacheable mappings) to *all* HV DRAM writes so no
   dirty HV line can linger to write back over DMA'd data.
3. **Clean+invalidate guest DRAM to PoC before first guest entry.** `kload`
   cleans only the kernel/DTB/modinfo spans (`kload_cache_clean_inval`,
   kload.c:384/486/637). Add a full clean+invalidate of the guest DRAM range to
   PoC (by‑VA over the range, or set/way over L1+L2) at the end of `main()`
   *before* `kload_enter` (main_dbg.c:250), so no pre‑guest dirty line survives.
   Combined with (2), CPU1 must also clean+invalidate its L1 before/while the
   guest runs.

This is entirely ARMv8.0‑expressible (VA/set‑way cache maintenance + memory‑map
changes) and addresses both (b‑i) and (b‑ii) at the source.

### ✓ (c) — **Guest‑visible: force the `aw_mmc` data buffer Non‑cacheable (fallback)**
If HV memory‑map surgery is too invasive short‑term, make the guest allocate the
`aw_mmc` **data** buffer as coherent/uncached too (e.g. mark the SMHC node
`dma-coherent`‑equivalent for the data path, or patch `aw_dma_buf_tag` to
`BUS_DMA_COHERENT` so its buffer becomes Normal‑NC like the ring). A
Non‑cacheable data buffer removes the cacheable alias entirely — no observer can
hold a stale/dirty line — at a throughput cost. This is a guest change, so it is
a fallback, not the preferred fix. (Note: FreeBSD arm64 does **not** parse
`dma-coherent` from the FDT — grep of the tree shows zero hits — so this must be
done via a driver/tag change or a `bus_dma` coherency shim, not a pure DTB edit.)

### ✗ (b) — **HV traps IDMAC and does maintenance for the guest — impractical**
The HV cannot see the buffer PA without trapping the guest's SMHC MMIO writes
(`DLBA`/`CMDR`). It *could* (SMHC2 @ 0x01c11000 is trappable via a stage‑2 hole
like the existing UART/virtio traps), then walk the ring and `dc ivac` the
buffer around each transfer — but this re‑introduces per‑transfer EL2 trapping
on the hot storage path (the very thing the IMO=0 policy avoided for the EHCI
storm) and duplicates work the guest already does correctly. Use only as a
targeted experiment, not a fix. *(The read‑only version of this walk is exactly
the confirmation harness — see the instrument doc.)*

---

## 5. Confirmation

Which of (b‑i)/(b‑ii) is live is directly observable without new HV C code (or
with a one‑line `dc ivac` in the debug read path). See
`docs/aw-mmc-dma-instrument.md`: read the guest's SMHC2 `DLBA` (0x01c11084),
walk the ring to the data‑buffer PA, and compare **DRAM at that PA** against an
`emmc_bio` PIO read of the same LBA:

* DRAM == PIO but the guest saw garbage ⇒ **(b‑i) stale read** (guest/observer
  cache not invalidated) — confirms the extra‑observer / broadcast hypothesis.
* DRAM != PIO ⇒ **(b‑ii) DRAM clobbered** (dirty write‑back, or a mis‑targeted
  DMA if `buf_addr` is wrong) — confirms the write‑back hypothesis.
* Bonus lever: if a *non‑invalidating* dbgmon read and a *`dc ivac`‑then‑read*
  of the same PA disagree, EL2 itself is holding a stale cacheable line — direct
  proof of the multi‑observer hazard.
