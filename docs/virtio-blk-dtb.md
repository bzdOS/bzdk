# virtio-blk — guest DTB node (parent must apply on the board host)

> **2026-08-11 — the recompile step below does NOT work on this blob.** `dtc`
> refuses to rebuild `bananapi-min.dtb` from a decompile (it contains a U-Boot
> `binman` child named `@fdt-SEQ`, a template name dtc will not accept back in).
> Use `fdtput` for surgical edits instead. Everything else here still stands.
> See `docs/guest-dtb.md` for the working flow and for the full list of edits
> currently applied to the deployed blob.


The hypervisor side of virtio-blk (`vblk_emmc.c`, MMIO trap @ `0x0A000000`,
INTID injection into the real GICD) is built and integrated into the `dbg`
target. For the FreeBSD guest to *probe* the device, its DTB must advertise a
`virtio,mmio` node. **This step is NOT applied — the parent owns the board and
the boot artifacts; apply it there.** The `.dtb` was deliberately left
untouched (do-not-hand-edit-the-binary rule).

## Why this is a doc, not an applied edit

- Boot artifact: `/opt/bzdos/tftpboot/bananapi-min.dtb` (TFTP'd to `0x4a000000`).
- A `.dts` exists (`/opt/bzdos/tftpboot/bananapi-min.dts`) **but it is stale**:
  the `.dtb` (mtime Jul 18 23:24) is newer than the `.dts` (Jul 15 21:32), and
  **no build step in the tree recompiles `.dts` -> `.dtb`** (grep found none;
  the boot scripts only TFTP the prebuilt `.dtb`). So the `.dts` is NOT a
  reliable source for the running blob. Treat the **`.dtb` as authoritative**
  and regenerate it from a fresh decompile, not from the stale `.dts`.

## Recommended flow — decompile the REAL blob, add the node, recompile

```sh
cd /opt/bzdos/tftpboot
cp bananapi-min.dtb bananapi-min.dtb.bak          # keep a rollback
dtc -I dtb -O dts -o /tmp/vblk.dts bananapi-min.dtb
# ...add the node under /soc (see below)...
dtc -I dts -O dtb -o bananapi-min.dtb /tmp/vblk.dts
```

### Node to add under `/soc { ... }`

```dts
virtio_mmio@a000000 {
    compatible = "virtio,mmio";
    reg = <0x00 0x0a000000 0x00 0x00000200>;   /* #address-cells/#size-cells==2 in this blob */
    interrupts = <0x00 0x69 0x01>;             /* GIC_SPI 105, IRQ_TYPE_EDGE_RISING */
    interrupt-parent = <0x01>;
    status = "okay";
};
```

> NOTE on `reg` cell count: the decompiled `/soc` uses `#address-cells = <2>`
> and `#size-cells = <2>` in this blob, so `reg` is FOUR cells
> `<hi_addr lo_addr hi_size lo_size>` = `<0x00 0x0a000000 0x00 0x200>`. (The
> two-cell form `<0x0a000000 0x200>` in the design doc is the QEMU-virt style;
> match whatever `#address-cells`/`#size-cells` the decompiled `/soc` actually
> shows — check the top of the `soc { }` node.)

### Alternative — `fdtput` in place (no decompile round-trip)

```sh
cd /opt/bzdos/tftpboot
cp bananapi-min.dtb bananapi-min.dtb.bak
D=bananapi-min.dtb; N=/soc/virtio_mmio@a000000
fdtput -t s  $D $N compatible "virtio,mmio"
fdtput -t x  $D $N reg 0x0 0xa000000 0x0 0x200
fdtput -t x  $D $N interrupts 0x0 0x69 0x1
fdtput -t x  $D $N interrupt-parent 0x1
fdtput -t s  $D $N status "okay"
```

## CRITICAL: interrupt number (SPI) — changed from the skeleton's 50

The skeleton advertised **SPI 50 (INTID 82)**. That number is **already taken**
in the real blob:

```
dma-controller@1c02000 { ... interrupts = <0x00 0x32 0x04>; ... }   /* SPI 50 */
```

Under **HCR_EL2.IMO=0** the guest shares the *real* GICD with real peripherals,
so injecting INTID 82 would **collide with the A64 DMA controller**. The device
and DTB now use **SPI 105 (0x69) -> INTID 137**, which:

- is in the contiguous block `0x68..0x73` that **no node in the real DTB uses**;
- is above the highest real-peripheral SPI seen in the blob (`0x77`=119; EMAC is
  `0x52`=82, MUSB `0x47`=71).

`VBLK_SPI` in `vblk_emmc.h` is set to `105` to match. **Keep the `#define` and
the DTB `interrupts` cell in lockstep** — if you change one, change both.

**TODO(board) — still verify:** the A64 scatters real SPIs up to ~150, some of
which a minimal DTB may omit. Confirm SPI 105 is not wired to a real peripheral
before trusting delivery. To list what the blob already claims:

```sh
dtc -I dtb -O dts bananapi-min.dtb | grep -oE '0x00 0x[0-9a-f]+ 0x0[0-9a-f]' \
  | awk '{print $2}' | sort -u        # middle cell = SPI number (hex)
```

## Edge vs level — must be EDGE (trailing `1`)

The `interrupts` flag is **`1` = `IRQ_TYPE_EDGE_RISING`**, deliberately different
from the level-high (`4`) form every A64 peripheral node uses. Software
`GICD_ISPENDR` set-pending (our injection path, `vblk_inject_irq()`) is reliable
only for an **edge** SPI: for a *level* SPI the distributor re-derives pending
from the (nonexistent) physical wire and may clear our bit before delivery. See
`docs/virtio-blk-design.md §4`.

## After flashing — expected result

FreeBSD's `virtio_mmio(4)` + `virtio_blk(4)` probe the node, read DeviceID 2,
and attach a `vtbdN` disk of the advertised capacity (currently a fixed 8 GiB /
`0x01000000` sectors — see the capacity TODO in `vblk_emmc.c`). Verify with the
breadcrumb window (`bc 0x50005000`) and the checklist in
`docs/virtio-blk-integration.md §6`. Ensure the guest has enabled INTID 137 in
`GICD_ISENABLER` (part of `bus_setup_intr` on the node) — otherwise completions
set-pend but never deliver.

## SPI allocation in the free gap (as of 2026-09-30)

`board-config.xml`'s `<devices>` is the source of truth, and `gen_config.py`
refuses a duplicate or an SPI outside `0x68..0x73`. Current use:

| SPI | INTID | device | base |
|---|---|---|---|
| 105 (0x69) | 137 | vblk_emmc (`vtbd0`) | 0x0A000000 |
| 106 (0x6A) | 138 | vnet_emac | 0x0A001000 |
| — | — | scanout (no IRQ, no DTB node) | 0x0A002000 |
| 107 (0x6B) | 139 | vinput | 0x0A003000 |
| 108 (0x6C) | 140 | vblk_sd (`vtbd1`) | 0x0A004000 |
| 109 (0x6D) | 141 | vblk_zram (`vtbd2`, feature `vzram`) | 0x0A005000 |

`vtbdN` follows DTB child order, which `gen_config.py` keeps sorted by
address (see `reorder_virtio_nodes()`), so a new disk at a higher address
never renumbers the existing ones.
