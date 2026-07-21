# Live confirmation of the aw_mmc IDMAC DMA corruption mode

Companion to `docs/aw-mmc-dma-coherency.md`. Goal: decide **on the live board**
whether a corrupt guest read is **(b‑i) data‑cache staleness** (DRAM is correct,
the guest read a stale cache line) or **(b‑ii) DRAM clobbered / mis‑targeted
DMA** (DRAM itself is wrong). This needs **no new HV C code** — it is driven from
the host in Python using the existing `dbgmon` `r`/`w` (physical read/write
words) and `hv.call(emmc_bio_read, …)` primitives. An optional one‑line HV tweak
(`dc ivac` in the debug read path) makes the DRAM read PoC‑accurate; without it
the read is still useful as a second lever (see §4).

The mechanism: the guest's SMHC2/eMMC controller programs the IDMAC with the
descriptor‑list base and the descriptors carry the data‑buffer PA + length. The
HV can read those controller registers and walk that ring from the host, exactly
as `emmc_bio` reaches the same hardware — because it is the *same* controller at
the *same* MMIO base.

---

## 1. Register map (guest eMMC = SMHC2 = `aw_mmc1`)

`EMMC_BASE = 0x01c11000` (confirmed: `emmc_bio.c:32`, and FreeBSD binds the eMMC
node `allwinner,sun50i-a64-emmc` at this base). Offsets from `aw_mmc.h`:

| Reg | Offset | Absolute | Meaning |
|---|---|---|---|
| `GCTL`  | 0x00 | **0x01c11000** | global control; bit5 `DMA_ENB`, bit31 `FIFO_AC_MOD` (0 ⇒ DMA mode) |
| `BKSR`  | 0x10 | 0x01c11010 | block size |
| `BYCR`  | 0x14 | **0x01c11014** | byte count of the current transfer |
| `CMDR`  | 0x18 | 0x01c11018 | command reg (opcode in [5:0]; bit9 `DATA_TRANS`, bit10 `DIR_WRITE`) |
| `CAGR`  | 0x1C | **0x01c1101C** | command argument = **LBA** (sector index on this high‑capacity eMMC) |
| `DMAC`  | 0x80 | 0x01c11080 | IDMAC control; bit7 `IDMA_ON` |
| `DLBA`  | 0x84 | **0x01c11084** | **IDMAC descriptor‑list base address** |
| `IDST`  | 0x88 | 0x01c11088 | IDMAC status; bit0 `TX_INT`, bit1 `RX_INT`, bits[16:13] FSM state |
| `IDIE`  | 0x8C | 0x01c1108C | IDMAC interrupt enable |

For the eMMC config (`a64_emmc_conf`, aw_mmc.c:112‑116): `dma_xferlen = 0x2000`,
**`dma_desc_shift = 0`**. `dma_desc_shift = 0` is critical: `DLBA`, and the
descriptor `buf_addr`/`next` fields, hold **unshifted 32‑bit physical
addresses** (no `>> shift` to undo). All DMA PAs are ≥ 0x40000000 (DRAM).

### Descriptor format (`struct aw_mmc_dma_desc`, aw_mmc.h:214) — 16 bytes

| Word | Offset | Field | Notes |
|---|---|---|---|
| 0 | +0x0 | `config` | bit31 `OWN` (1 = owned by IDMAC), bit1 `DIC`, bit2 `LD` (last), bit3 `FD` (first), bit4 `CH` (chain), bit5 `ER` (end‑of‑ring) |
| 1 | +0x4 | `buf_size` | byte length of this segment; **0 ⇒ max = `dma_xferlen` = 0x2000** |
| 2 | +0x8 | `buf_addr` | **data‑buffer PA** (unshifted) |
| 3 | +0xC | `next` | PA of next descriptor (unshifted); chain ends when `LD`/`ER` set |

---

## 2. What to read, and when

The reliable capture point is **right at the guest's DMA completion**: the IDMAC
has written the buffer and raised `RX_INT`. Two ways to land there:

* **Passive/poll (no new C):** from the host, poll `IDST` (0x01c11088). When
  `RX_INT` (bit1) is set and the FSM (bits[16:13]) is idle/close, a read just
  finished; the ring registers still describe it (the guest clears `IDST` in its
  ISR, so poll fast, or repeat across many transfers to catch samples).
* **Deterministic (tiny existing hook):** the HV already traps to EL2 on guest
  faults/SMC and services `dbgmon`. Snapshot `DLBA`/`CAGR`/`BYCR`/first
  descriptor into a breadcrumb window on each EL2 entry while a read is in
  flight; read the breadcrumbs from the host. (Optional; the passive path is
  usually enough because the failure is frequent under userland exec.)

### Host sequence (per captured transfer)

```
1. cagr   = r 0x01c1101C 1      # LBA of the transfer
   bycr   = r 0x01c11014 1      # total byte count
   dlba   = r 0x01c11084 1      # descriptor-ring PA (unshifted)
   idst   = r 0x01c11088 1      # sanity: RX_INT/FSM
2. walk the ring starting at dlba:
     desc = r <p> 4             # config,buf_size,buf_addr,next
     seg_pa   = desc[2]
     seg_len  = desc[1] ? desc[1] : 0x2000
     ... record (seg_pa, seg_len); if config & (LD|ER) or next==0: stop
        else p = desc[3]; repeat   (cap the walk, e.g. <= 512 descs)
3. for each (seg_pa, seg_len) and the matching LBA slice:
     ram  = r  seg_pa  (seg_len/4)          # DRAM as the guest sees it via HV
     disk = hv.call(emmc_bio_read, lba, seg_len)   # PIO ground truth
     compare(ram, disk)
```

`lba` for segment *k* advances by `seg_len / 512` sectors from `CAGR`
(multi‑block CMD18 reads are sequential). `emmc_bio_read` is the existing HV PIO
reader whose output is proven byte‑perfect (the diagnosis baseline).

---

## 3. Decision logic

For each captured, *corruption‑suspected* transfer (ideally correlate with the
guest actually faulting — e.g. capture continuously and keep the last N before a
SIGSEGV/reset):

| `ram` vs `disk` (DRAM vs PIO) | Guest outcome | Conclusion |
|---|---|---|
| **equal** | guest saw garbage / SIGSEGV'd | **(b‑i) data‑cache staleness** — the IDMAC put correct bytes in DRAM but the guest CPU read a stale cache line (its POSTREAD `dc ivac` did not take, or a stale line re‑filled/was never invalidated on CPU0/CPU1/L2). Confirms the extra‑cacheable‑observer / broadcast hypothesis. |
| **differ** | — | **(b‑ii) DRAM itself is wrong.** Sub‑discriminate: if `buf_addr` (`seg_pa`) is a sane DRAM buffer address, DRAM was **clobbered after the DMA** (dirty write‑back from an EL2/CPU1 cacheable line). If `seg_pa`/`seg_len` look wrong (out of range, absurd length, OWN still set), the **DMA was mis‑targeted** (descriptor problem) — which §2 of the coherency doc argues against, so this being clean is itself a useful negative result. |

Expected result, per the coherency analysis: **equal ⇒ (b‑i)**. That is the
prediction to falsify.

---

## 4. The EL2‑cache lever (strongest single test)

`dbgmon`'s `cmd_read_words` reads DRAM through **EL2's own Normal‑WB cacheable
mapping** (dbgmon.c:246 "flat EL2 map"). So step‑3 `ram` is "what EL2's cache /
PoC sees", not guaranteed true DRAM. Exploit this:

* Read `seg_pa` **twice**: once as now (cacheable), once after invalidating that
  line to PoC. The minimal HV change is a `dc ivac` (+`dsb ish`) per line at the
  top of `cmd_read_words` before the load — a NEW one‑line instrumentation edit,
  or expose a second command `ri` (read‑invalidated). Then:
  * **cacheable‑read == invalidated‑read** ⇒ EL2 holds no stale line for that PA;
    the staleness (if any) is on the guest/CPU1 side.
  * **cacheable‑read != invalidated‑read** ⇒ **EL2 itself is caching a stale
    copy of a guest‑DMA page** — direct, unambiguous proof of the multi‑observer
    hazard from the coherency doc, and independently a bug in the debug read
    path (fix #1 in §4(d) of that doc).

This lever needs at most a one‑line addition and confirms the root cause without
relying on catching a guest fault in the act.

---

## 5. Optional self‑contained probe

`emmc_dma_probe.c` (NEW file in this tree) packages steps 1‑2 as a single
host‑callable function `emmc_dma_probe()` the parent can wire into the `dbgmon`
`call` table if the per‑register host reads are too slow to race the guest ISR.
It only **reads** the SMHC registers and walks the ring into a breadcrumb window
(0x50000d00) — no writes to the controller, no new stage‑2 changes — and
optionally `dc ivac`s each segment before copying so the captured bytes are
PoC‑accurate. Wiring it in requires one line in the existing `dbgmon` dispatch
(parent‑owned; not done here per the read‑only constraint on existing files).
