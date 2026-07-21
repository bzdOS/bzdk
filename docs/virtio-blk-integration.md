# virtio-blk (eMMC-backed) — integration steps

Exact edits to apply the `vblk_emmc.c` / `vblk_emmc.h` device to the **`dbg`
build** (`main_dbg.c`, the live IMO=0 debug hypervisor). **Nothing here has been
applied** — these are snippets for the parent to apply later. All five edits are
required together; order of application does not matter, but the runtime order
(init before enable, dispatch after vconsole) does.

Files touched: `Makefile`, `main_dbg.c`, `stage2.c`, `el2_exc.c`, and the guest
DTB source. Line numbers are from the versions read on 2026-07-19; match on the
surrounding text, not the numbers.

---

## 1. `Makefile` — add `vblk_emmc.o` to the dbg build

`DBG_OBJS` (line ~99) already links `emmc_bio.o`. Append `vblk_emmc.o`:

```make
 DBG_OBJS := start.o main_dbg.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
             gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
             emac.o dbgmon.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
-            musb.o usbacm.o emmc_bio.o
+            musb.o usbacm.o emmc_bio.o vblk_emmc.o
```

The generic `%.o: %.c` rule builds it. **Do NOT** add the old `virtio*.o`
(RAM-disk stack) to this build — it shares the `0x0A000000` base and the
`0x50005000` breadcrumb window with `vblk_emmc.o` and assumes IMO=1.

Optional: add `vblk_emmc.o` to the `clean` target's `rm -f` list.

---

## 2. `main_dbg.c` — initialize the device before the guest can fault

Add the include near the other device headers (top of file, by `#include
"vconsole.h"`):

```c
#include "vblk_emmc.h"
```

Call `vblk_init()` next to `vconsole_init()` (currently line ~79), i.e. **before
`stage2_init()`/`stage2_enable()`** so the MMIO window is emulatable and the
eMMC is up before the guest touches the window:

```c
 	el2_install();
 	vconsole_init();
+	/* eMMC-backed virtio-blk: bring the eMMC up and register the device at
+	 * 0x0A000000 before stage-2 goes live (the guest can fault on the window
+	 * the moment stage2_enable() runs). Returns <0 if the eMMC didn't come up;
+	 * the device still registers and completes requests as S_IOERR until it does. */
+	vblk_init();
```

`emmc_bio_init()` is already effectively exercised in this build (the PC5 pinmux
fix is applied later in `main_dbg.c`); `vblk_init()` calls it directly. If you
want the PC5 mux forced *before* `emmc_bio_init()`, move the existing PC5 block
(currently ~line 207) up to just before `vblk_init()`, or rely on
`emmc_bio_init()`'s own PC5→func3 write (it does this itself, `emmc_bio.c:238`).

**No `guest_config()` / HCR / IMO change is needed** — the device injects via
the real GICD under the existing IMO=0 policy.

---

## 3. `stage2.c` — trap the `0x0A000000` 2 MiB block

`0x0A000000` is **L2 index 80** (`0x0A000000 >> 21 == 80`) in the low-1 GiB MMIO
level-2 table `stage2_l2_mmio[]`, built in `stage2_build_mmio_tables()` (the L2
loop at ~line 291). Nothing real occupies `0x0A000000–0x0A200000`, so leave the
whole 2 MiB block **invalid** (no L3 split needed, unlike UART0 whose block also
holds GICD/GICC):

```c
 	/* Level-2: identity 2 MiB Device-nGnRE blocks everywhere, except the
 	 * one block containing UART0_BASE, which is a TABLE descriptor down
-	 * to stage2_l3_uart[] instead of a block. */
+	 * to stage2_l3_uart[] instead of a block; and the virtio-mmio block at
+	 * 0x0A000000 (L2 index 80), left INVALID so guest accesses there fault to
+	 * EL2 for vblk_mmio_fault() (docs/virtio-blk-design.md). */
 	for (unsigned i = 0; i < STAGE2_L2_ENTRIES; i++) {
 		if (i == UART_L2_IDX) {
 			uint64_t l3_pa = (uint64_t)(uintptr_t)&stage2_l3_uart[0];
 			stage2_l2_mmio[i] = stage2_table_desc(l3_pa);
 			continue;
 		}
+		if (i == (unsigned)(VBLK_MMIO_BASE >> STAGE2_L2_BLOCK_SHIFT)) {  /* ==80 */
+			stage2_l2_mmio[i] = 0;   /* INVALID: trapped virtio-mmio window */
+			continue;
+		}
 		uint64_t pa = (uint64_t)i * STAGE2_L2_BLOCK_SIZE;
 		stage2_l2_mmio[i] = stage2_l2_block_desc(pa,
 			S2_MEMATTR_DEVICE_nGnRE, S2_SH_OUTER, /*xn=*/1);
 	}
```

Add `#include "vblk_emmc.h"` to `stage2.c` for `VBLK_MMIO_BASE`, **or** to avoid
coupling, hard-code `if (i == 80)` with a comment. `STAGE2_L2_BLOCK_SHIFT` is
already defined in `stage2.c` (== 21). This runs inside `stage2_init()`, before
`stage2_enable()`, so the trap is live before the guest starts.

> Alternative (finer-grained): mirror the UART0 L2→L3 split for index 80 and
> invalidate only the single 4 KiB page at `0x0A000000`, leaving the rest of the
> 2 MiB block identity-mapped. Unnecessary here (the block is empty) but harmless.

---

## 4. `el2_exc.c` — dispatch the abort to `vblk_mmio_fault()`

In `el2_trap()`, the guest data-abort (EC==0x24) block currently tries
`vconsole_handle_fault()` (lines ~356-363). Add the `vblk` attempt immediately
after vconsole (same pattern, same "handled → return without recording" rule):

```c
 	if ((kind >> 2) == 2u && (kind & 3u) == EL2_KIND_SYNC &&
 	    (((uint32_t)(frame->esr >> 26)) & 0x3fu) == 0x24u) {
 		if (vconsole_handle_fault(frame)) {
 			if (!dbg_core_active)
 				dbgmon_service(frame);
 			return;
 		}
+		if (vblk_mmio_fault(frame)) {
+			if (!dbg_core_active)
+				dbgmon_service(frame);
+			return;
+		}
 	}
```

Add the include at the top of `el2_exc.c` (by `#include "vconsole.h"`):

```c
#include "vblk_emmc.h"
```

`vblk_mmio_fault()` returns 0 for any abort outside `0x0A000000..+0x200`, so it
is safe to call unconditionally on every guest data abort; it only claims its
own window. Ordering after vconsole is arbitrary (disjoint address windows) but
keeps the console path first.

---

## 5. Guest DTB — add the `virtio_mmio` node

Guest DTB source is what compiles to **`/opt/bzdos/tftpboot/bananapi-min.dtb`**
(TFTP'd to `0x4a000000` by `fbsd-boot.py`). Decompile, add the node, recompile:

```sh
dtc -I dtb -O dts -o bananapi-min.dts /opt/bzdos/tftpboot/bananapi-min.dtb
# add the node below under /soc, then:
dtc -I dts -O dtb -o /opt/bzdos/tftpboot/bananapi-min.dtb bananapi-min.dts
```

Node to add under `/soc` (`#interrupt-cells = 3`, `interrupt-parent = <0x01>`
is the GIC phandle, already the global default):

```dts
virtio_mmio@a000000 {
    compatible = "virtio,mmio";
    reg = <0x0a000000 0x200>;
    interrupts = <0x00 0x32 0x01>;   /* GIC_SPI 50, IRQ_TYPE_EDGE_RISING */
    interrupt-parent = <0x01>;
    status = "okay";
};
```

- `0x00` = `GIC_SPI`; `0x32` = **50** (SPI number → INTID 82); `0x01` =
  **`IRQ_TYPE_EDGE_RISING`**. The trailing `1` (edge) is deliberate and differs
  from the EMAC node's `<0 0x52 4>` (level-high) — see
  `docs/virtio-blk-design.md §4` for why software `GICD_ISPENDR` injection needs
  edge. Keep `VBLK_SPI` in `vblk_emmc.h` (currently 50) and this `interrupts`
  cell in lockstep.
- **VERIFY SPI 50 is unused** by a real A64 peripheral before flashing (IMO=0
  shares the real GICD). `grep -o 'interrupts = <[^>]*>' bananapi-min.dts` to
  list what's taken (EMAC SPI 82, MUSB SPI 71, pinctrl SPI 11 seen in use). If
  50 collides, pick a free SPI and update both places.

FreeBSD's `virtio_mmio(4)` + `virtio_blk(4)` will then probe `virtio-blk`
DeviceID 2 at this node and attach a `vtbdN` disk. No `/memory` or
`/reserved-memory` change is required (the backing store is the eMMC, not guest
DRAM).

---

## 6. Post-integration verification (on board)

1. **Device probes:** guest dmesg shows `virtio_mmio0` + `vtbd0` with the
   advertised capacity. Breadcrumb `bc 0x50005000`: `[0]`=`"VBK1"`,
   `[11]`=`emmc_ready`==1.
2. **Register chatter works:** breadcrumb `[3]/[4]/[5]` (desc/avail/used PA lows)
   become nonzero, `[2]` (QueueNum) sane, `[1]` (Status) reaches `0xf`
   (ACK|DRIVER|FEATURES_OK|DRIVER_OK).
3. **Reads:** `dd if=/dev/vtbd0 ...` in the guest → `[6]` (reads) increments,
   `[10]` (IRQ injections) increments, data matches the eMMC (compare against
   an `emmc_bio_read` over the debug channel).
4. **IRQ delivery:** if `[6]` climbs but the guest hangs, the SPI isn't reaching
   the guest — check the SPI is free (§5), that it's edge (§5/§4), and that the
   guest enabled it (`GICD_ISENABLER` bit for INTID 82).
5. **Writes** (only after read-back-verified, `emmc_bio_write` is unverified):
   `[7]` increments; read back and compare. Mount read-only first.
6. **eMMC contention:** ensure the CPU1 debug-core `emmc_bio`/`gpart` path is
   quiescent (or locked) while the guest does virtio-blk I/O — see
   `docs/virtio-blk-design.md §8.1`.
```
