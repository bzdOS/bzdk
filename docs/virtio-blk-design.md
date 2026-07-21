# virtio-blk over virtio-mmio, backed by the real eMMC — design

**Status:** design + compilable skeleton (`vblk_emmc.c` / `vblk_emmc.h`). No
existing `.c`/`.h` was modified. Board verification pending; every unverified
point is marked **TODO(board)** here and in the code.

**Author intent:** give the FreeBSD/arm64 EL1 guest a clean paravirtual disk so
it stops driving the real `aw_mmc` controller (the GEOM taste-storm race). The
hypervisor owns the physical eMMC via the already-hardware-verified block-I/O
primitives in `emmc_bio.c`; the guest only ever sees virtio-mmio.

---

## 0. Naming / relationship to the existing `virtio_*.c`

The tree already contains `virtio.c` / `virtio.h` / `virtio_blk.c` /
`virtio_console.c`. That stack is **not** what we want here:

| | existing `virtio_blk.c` | this design (`vblk_emmc.c`) |
|---|---|---|
| backing store | fixed 16 MiB DRAM RAM disk (`0x7C000000`) | **the real eMMC** via `emmc_bio_read/write` |
| IRQ injection | `vgic_inject()` → GICH list registers (**needs IMO=1**) | write **real `GICD_ISPENDR`** (works under **IMO=0**) |
| DRAM carve-out | requires `/memory` trim or `/reserved-memory` | **none** — no guest RAM is stolen |
| builds in `main_dbg`? | no — `main_dbg.c` removed vgic/gic_timer (IMO=0) | yes — matches the live IMO=0 policy |

The live debug hypervisor (`main_dbg.c`) deliberately runs **HCR_EL2.IMO=0**
(physical IRQs go straight to the guest via the real GICv2) and has removed
`vgic_init()`/`gic_timer_init()` — see its "IRQ POLICY" comment and memory
`ehci-intid106-storm`. So the existing `virtio_blk.c` **cannot inject an
interrupt on this build at all**. Rather than edit it (forbidden by the task's
code-freeze, and it would break the IMO=1 assumption it documents), this is a
**new, self-contained module**. It reuses the *good ideas* of the existing
`virtio.h` (modern virtio-mmio, IPA==PA descriptor mapping, `0x0A000000` window,
HPFAR-based IPA reconstruction) but swaps the backing store and the IRQ path.

If/when the RAM-disk stack is retired, `vblk_emmc.*` can simply take over the
`virtio*.o` slot; until then they must not both be linked (they share the
`0x50005000` breadcrumb window and the `0x0A000000` MMIO base).

---

## 1. Transport: virtio-mmio, **modern (v2 / VIRTIO 1.x)** — chosen, justified

FreeBSD's `virtio_mmio(4)` probes both legacy (`Version==1`) and modern
(`Version==2`). We advertise **`Version==2`** and offer **only
`VIRTIO_F_VERSION_1`** (global feature bit 32). Reasons:

1. **The device learns the exact ring addresses from register writes.** Modern
   split virtqueues give three independent 64-bit base registers —
   `QueueDescLow/High` (0x080/0x084), `QueueDriverLow/High` (avail, 0x090/0x094),
   `QueueDeviceLow/High` (used, 0x0a0/0x0a4) — plus an explicit `QueueReady`
   (0x044) handshake. The hypervisor reads the guest-physical ring addresses
   directly; no legacy `GuestPageSize`/`QueuePFN`/`QueueAlign` arithmetic and no
   assumption that desc/avail/used are one contiguous block.
2. **No legacy config-endianness ambiguity.** Legacy config space is
   guest-native endianness with driver-visible quirks; modern is defined LE,
   which matches the little-endian A64 and our byte-copy helpers exactly.
3. **The one thing legacy gives us — rings implicitly laid out from a single
   PFN — we do not want.** We prefer the explicit addresses.

Modern requires the `VIRTIO_F_VERSION_1` handshake: the driver sets
`FEATURES_OK` in Status after accepting bit 32; we honour that but do not gate
on it in the skeleton (any well-behaved FreeBSD driver sets it).

### Register map we trap-and-emulate

One virtio-mmio device (blk) at **`0x0A000000`**, 0x200 bytes. All accesses are
32-bit word loads/stores (FreeBSD reads config as 32-bit halves). Registers
emulated (offsets from base) — full list in `vblk_emmc.h`:

| off | name | R/W | our behavior |
|-----|------|-----|--------------|
| 0x000 | MagicValue | R | `0x74726976` "virt" |
| 0x004 | Version | R | `2` |
| 0x008 | DeviceID | R | `2` (virtio-blk) |
| 0x00c | VendorID | R | `"bzds"` |
| 0x010 | DeviceFeatures | R | word 1 → bit0 (`VERSION_1`); word 0 → 0 |
| 0x014 | DeviceFeaturesSel | W | latch word select |
| 0x020/0x024 | DriverFeatures/Sel | W | record what the driver accepts |
| 0x030 | QueueSel | W | only queue 0 exists |
| 0x034 | QueueNumMax | R | `256` |
| 0x038 | QueueNum | W | negotiated queue size (clamped ≤ 256) |
| 0x044 | QueueReady | RW | latch; on 1, reset `last_avail` |
| 0x050 | QueueNotify | W | **the kick** → drain avail ring |
| 0x060 | InterruptStatus | R | `int_status` (bit0 = used ring advanced) |
| 0x064 | InterruptAck | W | clear acked bits |
| 0x070 | Status | RW | device status; 0 = driver reset |
| 0x080–0x0a4 | Queue{Desc,Driver,Device}{Low,High} | W | latch ring PAs |
| 0x0fc | ConfigGeneration | R | `0` |
| 0x100 | Config | R | virtio-blk config: `capacity` le64 at +0 |

virtio-blk `config.capacity` (sectors) is advertised at Config+0; everything
else in config reads 0 (no `BLK_SIZE`, `GEOMETRY`, `TOPOLOGY`, `MQ`, `RO` — we
offer no optional blk feature bits, keeping the device minimal but fully
functional read/write).

### Fault decode (identical to `vconsole.c`)

The dispatch entry `vblk_mmio_fault(frame)` mirrors `vconsole_handle_fault()`:

- Only `ESR_EL2.EC == 0x24` (lower-EL data abort).
- **Reconstruct the IPA from `HPFAR_EL2`**, not `FAR_EL2`. Once the guest MMU is
  on, `FAR_EL2` holds the guest VA the driver mapped the device at (not
  `0x0A000000`). `IPA = ((HPFAR_EL2 & 0xFFFFFFFFF0) << 8) | (FAR_EL2 & 0xFFF)`.
  virtio registers are touched *late* (driver attach, well after the MMU is on),
  so this is mandatory — it is the single subtlety `vconsole`'s early-boot path
  didn't need.
- Decode `ISS`: `ISV` (syndrome valid), `WnR` (write), `SRT` (transfer
  register; 31 = XZR/discard). `SAS` (size) is decoded but unused — all
  registers are 32-bit.
- Emulate the read/write, then `frame->elr += 4` to step past the faulting
  instruction, and return 1. Return 0 if the IPA is outside our window so the
  generic fault path records it.

---

## 2. Virtqueue handling (split ring, VIRTIO 1.x)

Single request virtqueue (queue 0). Each request is a descriptor chain:

```
desc[0]        read-only  16-byte header { le32 type; le32 reserved; le64 sector }
desc[1..k-2]   data       write-only for T_IN (device writes guest),
                          read-only  for T_OUT (device reads guest)
desc[k-1]      write-only 1-byte status (VIRTIO_BLK_S_OK / _IOERR / _UNSUPP)
```

Ring memory lives in **guest DRAM**, which stage-2 **identity-maps
(IPA==PA)** — confirmed in `stage2.c` (DRAM is a 1 GiB Normal-WB identity
block, self-checked IPA 0x42000000 → PA 0x42000000). Therefore **a guest PA in a
descriptor is directly a valid EL2 pointer**; no per-request address
translation, no page-table walk. This is the property that makes the whole
device cheap.

- `vq_pop_avail()` reads `avail->idx` (le16 at avail+2) and, if ahead of our
  `last_avail`, returns `ring[last_avail % num]` (le16 at avail+4+2*slot).
- `vq_read_desc()` reads a 16-byte `struct vring_desc` at `desc + 16*idx`.
- `vq_push_used()` writes `{ le32 id=head; le32 len }` at `used+4+8*slot`,
  **`dsb sy`**, then bumps `used->idx` (le16 at used+2), **`dsb sy`**. The
  barrier between the entry write and the idx bump is required so the guest
  never sees an advanced `used->idx` pointing at a half-written entry.

`VRING_DESC_F_INDIRECT` is **not** advertised and treated as malformed if seen
(FreeBSD only emits indirect when the feature is negotiated). Chain length is
bounded by `VBLK_MAX_CHAIN` (32).

---

## 3. Request flow → `emmc_bio_read` / `emmc_bio_write`

virtio-blk sector units are 512 bytes; `emmc_bio` does `SET_BLOCKLEN(512)` and
one CMD17/CMD24 per 512-byte block. **virtio sector N maps 1:1 to eMMC LBA N.**

For each data descriptor of length `len` (a multiple of 512 in the fast path):

- **T_IN (read):** for each 512-byte sector, `emmc_bio_read(lba, guest_pa)`
  writes the block **straight into the guest buffer** (guest PA == EL2 PA,
  512-aligned, Normal-WB). `used_len += len`.
- **T_OUT (write):** for each sector, copy 512 bytes from the guest buffer into
  an **EL2-private 512-byte bounce block**, then `emmc_bio_write(lba,
  BOUNCE_PA)`. (Bouncing keeps the eMMC FIFO source aligned and isolated; the
  direct form `emmc_bio_write(lba, guest_pa)` is valid too and is a noted
  perf TODO.)
- **T_FLUSH:** no-op success — `emmc_bio_write` is fully synchronous (waits for
  `DATA_OVER` + card-not-busy before returning), so nothing is buffered in the
  HV to flush.
- anything else → `VIRTIO_BLK_S_UNSUPP`.

`serve_data()` fully implements the **512-aligned fast path** (the case FreeBSD's
`virtio_blk` actually produces: page-granular, sector-aligned segments). A
descriptor that is *not* a whole number of sectors would need cross-descriptor
partial-sector stitching — left as a **TODO(board/correctness) backstop** that
fails the request cleanly (`S_IOERR`) rather than corrupt data, because in
practice it should never be exercised.

Bounds: each request is checked against advertised `capacity`; an out-of-range
sector returns `S_IOERR`.

### Capacity

`config.capacity` should be the eMMC's real sector count from CSD/EXT_CSD
`SEC_COUNT`. `emmc_bio.c` does not yet expose it, so the skeleton advertises a
fixed **`0x01000000` sectors (8 GiB)** — the board's known eMMC size (memory
`emmc-pc5-pinmux-fix`). **TODO(board):** read the true count during
`emmc_bio_init()` before trusting this; a too-large capacity lets the guest read
past the end (surfaces as `S_IOERR` from an eMMC timeout, not corruption).

---

## 4. IRQ injection under IMO=0 — write the real `GICD_ISPENDR`

This is the crux of running under the live policy. Facts:

- `main_dbg.c` sets **HCR_EL2.IMO=0, FMO=0**: every physical IRQ is delivered
  **directly to the guest EL1** by the real GICv2. EL2 keeps IRQ masked and owns
  no interrupts. There is **no vgic**, no GICH.
- The guest DTB (`bananapi-min.dtb`) exposes the real GIC (`arm,gic-400`,
  GICD `0x01c81000`, GICC `0x01c82000`) identity-passed-through in stage-2, and
  FreeBSD's `gic_v2` driver drives it natively.

So to deliver a completion interrupt we make the guest's SPI **pending in the
real distributor** and let the hardware forward it:

```c
/* GICD_ISPENDR is a bitmap: word = intid/32, bit = intid%32. */
*(uint32_t *)(0x01C81000 + 0x200 + 4*(VBLK_INTID/32)) = 1u << (VBLK_INTID%32);
```

The guest then takes a normal physical IRQ, `GICC_IAR` returns `VBLK_INTID`,
its virtio-mmio ISR reads `InterruptStatus` (trapped → we return `VRING`),
writes `InterruptAck`, drains the used ring, and `GICC_EOIR`s. **The HV never
touches GICC** — ack/EOI/deactivate are 100% native guest behavior. No IMO=1,
no list registers, no maintenance interrupt. This is strictly simpler than the
existing RAM-disk path's vgic injection and is the reason for the new module.

### Which SPI / INTID

Skeleton uses **`VBLK_SPI = 50` → `VBLK_INTID = 82`**, declared in both
`vblk_emmc.h` and the DTB node. **TODO(board): confirm SPI 50 is unused by any
real A64 peripheral.** Because IMO=0 means the guest shares the *real* GICD with
real device SPIs, the chosen SPI must not alias a physical peripheral's line
(else a real device IRQ and our injected IRQ collide). Verify against the A64
SPI map and `grep interrupts bananapi-min.dts` (seen in use: EMAC SPI 82→INTID
114, MUSB SPI 71→INTID 103, pinctrl SPI 11). Pick from a documented gap; adjust
the one `#define` and the DTB `interrupts` cell together.

### Edge vs level — **use EDGE-triggered** (DTB flag `1`)

`GICD_ISPENDR` software set-pending is **reliable only for an edge-configured
SPI.** For a *level* SPI the GICv2 distributor re-derives the pending state from
the (physical) input line; with no real wire the line reads deasserted, so a
software-set pending bit **may be cleared before it is ever delivered** — the
classic "you can't software-trigger a level SPI" hazard. Edge semantics make our
set-pending a clean one-shot that latches until the guest's `IAR` read.

Therefore the DTB node must declare **`interrupts = <0 50 1>`**
(`GIC_SPI 50 IRQ_TYPE_EDGE_RISING`), *not* the level-high (`4`) form the EMAC
node uses. FreeBSD's GIC driver programs `ICFGR` from this flag and services
edge SPIs normally; virtio's `InterruptStatus`/`Ack` handshake is unaffected
(the ISR drains the whole used ring, so one edge covers multiple completions).
The level alternative is documented but disrecommended for exactly the
delivery-reliability reason above — **TODO(board): confirm one delivery per
`ISPENDR` write on this silicon.**

### Completion timing

Everything is synchronous on CPU0 inside the `QueueNotify` trap: by the time we
`eret`, the used ring is updated, `InterruptStatus.VRING` is set, and the SPI is
pending. The guest takes the IRQ as soon as it unmasks — often immediately,
which is fine (better latency than a real async disk).

---

## 5. Stage-2: trap the `0x0A000000` window

`0x0A000000` currently sits at **L2 index 80** of `stage2_l2_mmio[]` as an
identity Device-nGnRE 2 MiB block (`stage2.c` builds it in the L2 loop). Nothing
real lives in `0x0A000000–0x0A200000` (A64 peripherals end at `0x02000000`), so
the cleanest trap is to **leave that whole 2 MiB L2 block invalid** (descriptor
= 0). Any guest access there then takes a stage-2 data abort to EL2, exactly
like the UART0 page — but at 2 MiB granularity with no L3 split needed (unlike
UART0, whose 2 MiB block also holds GICD/GICC that must stay mapped).

Exact edit is in `docs/virtio-blk-integration.md`. `stage2_init()` runs before
`stage2_enable()`, and `vblk_init()` runs before `stage2_enable()` too, so the
window is trapped and the device is ready before the guest can fault on it.

**No `/memory` change is required** — unlike the RAM-disk design, we steal no
guest DRAM.

---

## 6. Coherency & barriers

- **Vring / descriptor buffers:** processed on **CPU0**, the same core the guest
  runs on, through the same Normal-WB cacheable identity mapping. Same-core
  accesses are cache-coherent with the guest with **no explicit maintenance**.
  We still issue `dsb sy` before reading guest-produced data (avail ring,
  headers, T_OUT data) and after writing device-produced data (T_IN data, used
  ring, status byte) to **order** our accesses against the guest.
- **`emmc_bio` DMA/FIFO path:** `emmc_bio` uses PIO (CPU drains/fills the
  controller FIFO), not bus-master DMA, so there is no device-DMA coherency
  problem — the CPU itself moves every byte, and `emmc_bio_read` ends with
  `dsb sy`. Reading straight into a guest Normal-WB buffer is therefore
  coherent on this core.
- **`GICD_ISPENDR` write:** `dsb sy` before and after so the pending bit is
  globally visible before we `eret` into the guest.

---

## 7. DTB node to add

Add under `/soc` in `bananapi-min.dts` (then recompile to `.dtb`). `#interrupt-cells
= 3`, global `interrupt-parent = <0x01>` (the GIC phandle):

```dts
virtio_mmio@a000000 {
    compatible = "virtio,mmio";
    reg = <0x0a000000 0x200>;
    interrupts = <0x00 0x32 0x01>;   /* GIC_SPI 50, IRQ_TYPE_EDGE_RISING */
    interrupt-parent = <0x01>;
    status = "okay";
};
```

`0x32 = 50` (SPI number; INTID 82). Note the trailing **`1`** (edge) per §4, in
contrast to the EMAC node's `<0 0x52 4>` (level). Full integration steps and the
exact `stage2.c` / `el2_exc.c` / `main_dbg.c` / `Makefile` edits are in
`docs/virtio-blk-integration.md`.

---

## 8. Open risks (call-outs)

1. **BIGGEST RISK — synchronous, slow, single-block eMMC I/O inside the vCPU
   trap, plus controller-ownership contention.** A `QueueNotify` blocks CPU0
   (the guest's core) while `emmc_bio` does **one 512-byte CMD17/CMD24 per
   sector at the 400 kHz init clock (~1–10 ms each)**. A multi-sector GEOM read
   (e.g. 128 sectors) stalls the guest for >100 ms with **no async completion**;
   `el2_trap` pets the WDT only on trap *entry*, so a long in-trap loop must stay
   well under the ~16 s window. Worse, the **CPU1 debug core also drives the same
   eMMC controller** (the `emmc_bio`/`gpart` path, memory `smp-debug-core-working`
   / `emmc-pc5-pinmux-fix`): two cores poking `0x01c11000` concurrently will
   corrupt transfers. **Mitigations (must implement before board use):** a
   spinlock around all `emmc_bio` calls shared with the CPU1 bio path (or disable
   the CPU1 bio while virtio-blk is live); raise the eMMC clock past 400 kHz to
   HS mode; cap sectors-per-trap and re-pet the WDT inside long loops; longer
   term, make completion async (kick a worker, complete later) so the vCPU isn't
   held. This single issue is the main thing standing between "compiles" and
   "works without regressing the board."
2. **`emmc_bio_write` is NOT hardware-verified** (its own header says so). The
   whole T_OUT path inherits that risk; validate with read-back compares before
   trusting guest writes. Consider mounting read-only first.
3. **SPI number aliasing** (§4): under IMO=0 the guest shares the real GICD, so a
   wrongly-chosen SPI collides with a real peripheral. Must verify 50 is free.
4. **Edge-vs-level `ISPENDR` delivery** (§4): confirm one delivery per write on
   this GIC-400; fall back to clearing pending after a delay if level is forced.
5. **MMIO-trap performance for register chatter.** Device *setup* is a burst of
   trapped 32-bit accesses (feature negotiation, ring programming) — cheap and
   one-time. The hot path is one trap per `QueueNotify` (batches all queued
   requests) plus one per ISR (`InterruptStatus` read + `InterruptAck` write),
   which is fine. No per-descriptor trapping.
6. **Capacity honesty** (§3): advertise the real EXT_CSD sector count, not the
   hard-coded 8 GiB guess.
7. **Ring index wrap / robustness:** the skeleton trusts `avail->idx`
   monotonicity and `num` power-of-two; a malicious/buggy driver could feed bad
   indices. Bounded by `VBLK_MAX_CHAIN` and capacity checks, but harden
   (validate desc `addr`/`len` inside guest DRAM) before treating the guest as
   untrusted.
