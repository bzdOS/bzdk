# virtio-net-over-EMAC — guest DTB node (parent must apply on the board host)

Mirrors `docs/virtio-blk-dtb.md` exactly, for the SECOND virtio-mmio device
this tree now builds: `vnet_emac.c` (virtio-net, DeviceID 1) at `0x0A001000`,
inside the SAME already-stage-2-trapped 2 MiB block vblk's device lives in
(`0x0A000000..0x0A200000`, see `vnet_emac.h`'s "MMIO window" comment) — no
`stage2.c` change needed for the trap itself, ONLY the guest DTB needs a new
node so FreeBSD's `virtio_mmio(4)` bus walker finds and probes it.

## Why this is a doc, not an applied edit

Same reasoning `docs/virtio-blk-dtb.md` gives, still true today:

- The boot artifact TFTP'd to the board is `/opt/bzdos/tftpboot/bananapi-min.dtb`.
- That path is **shared, live boot infrastructure outside this git worktree**
  (no version control, potentially read by an independent, concurrently
  running board/TFTP session). This task explicitly has no board access from
  this worktree — editing that shared file blind, with no way to coordinate
  with whatever may be using it right now, is exactly the kind of
  hardware-adjacent side effect out of scope here.
- `/opt/bzdos/tftpboot/bananapi-min.dts` is (still, as of this writing) stale
  relative to the `.dtb` — no build step recompiles it — so it must NOT be
  hand-edited as if it were the source of truth either. Confirmed live: a
  `dtc -I dtb -O dts` decompile of the CURRENT `bananapi-min.dtb` shows the
  vblk node (`virtio_mmio@a000000`) already present with `#address-cells` /
  `#size-cells` of **1** (single-cell `reg`/`interrupts`, NOT the two-cell
  form the original vblk doc's now-superseded warning described) — i.e.
  someone DID apply `docs/virtio-blk-dtb.md`'s recipe to the real blob at
  some point after that doc was written, confirming the doc-then-apply
  workflow is the correct one here too.

## The node actually in the live blob today (verified, for exact mirroring)

```sh
dtc -I dtb -O dts /opt/bzdos/tftpboot/bananapi-min.dtb 2>/dev/null | sed -n '1,10p;/virtio_mmio/,/};/p'
```
yields (root `#address-cells = <0x01>`, `#size-cells = <0x01>`; `/soc` same):
```dts
virtio_mmio@a000000 {
    status = "okay";
    interrupt-parent = <0x01>;
    interrupts = <0x00 0x69 0x01>;
    reg = <0xa000000 0x200>;
    compatible = "virtio,mmio";
};
```

## Node to add under `/soc { ... }` for THIS device (vnet_emac.c)

Single-cell `reg`/`interrupts`, exactly mirroring the block above, with the
new base address and `VNET_SPI` (`vnet_emac.h`, `106` / `0x6a`):

```dts
virtio_mmio@a001000 {
    compatible = "virtio,mmio";
    reg = <0xa001000 0x200>;
    interrupts = <0x00 0x6a 0x01>;      /* GIC_SPI 106 (0x6a), IRQ_TYPE_EDGE_RISING */
    interrupt-parent = <0x01>;
    status = "okay";
};
```

Keep this `#define`/DTB-cell pair in lockstep exactly like vblk's: if
`VNET_SPI` in `vnet_emac.h` ever changes, this node's middle `interrupts`
cell must change with it (and re-check `docs/virtio-blk-dtb.md`'s SPI-
collision table — `0x68..0x73` is the free contiguous block; vblk already
holds `0x69`, this device now claims the next slot, `0x6a`).

## How to apply (same two options `docs/virtio-blk-dtb.md` gives)

### Option A — decompile, add both nodes, recompile
```sh
cd /opt/bzdos/tftpboot
cp bananapi-min.dtb bananapi-min.dtb.pre-vnet     # rollback point
dtc -I dtb -O dts -o /tmp/vnet.dts bananapi-min.dtb
# ...add the virtio_mmio@a001000 node under /soc, alongside the existing
#    virtio_mmio@a000000 node (see block above)...
dtc -I dts -O dtb -o bananapi-min.dtb /tmp/vnet.dts
```

### Option B — `fdtput` in place (no decompile round-trip)
```sh
cd /opt/bzdos/tftpboot
cp bananapi-min.dtb bananapi-min.dtb.pre-vnet
D=bananapi-min.dtb; N=/soc/virtio_mmio@a001000
fdtput -t s  $D $N compatible "virtio,mmio"
fdtput -t x  $D $N reg 0xa001000 0x200
fdtput -t x  $D $N interrupts 0x0 0x6a 0x1
fdtput -t x  $D $N interrupt-parent 0x1
fdtput -t s  $D $N status "okay"
```

Verify afterward with:
```sh
dtc -I dtb -O dts /opt/bzdos/tftpboot/bananapi-min.dtb 2>/dev/null | grep -A6 'virtio_mmio@a001000'
```

## Edge vs level — must be EDGE (trailing `1`), same rationale as vblk

`vnet_inject_irq()` (`vnet_emac.c`) uses the identical software
`GICD_ISPENDR` set-pending injection as `vblk_inject_irq()`, under the same
`HCR_EL2.IMO=0` real-GICD-passthrough policy. That only delivers reliably for
an **edge**-triggered SPI (`IRQ_TYPE_EDGE_RISING` = `1`); a level SPI would
have the distributor re-derive pending from a nonexistent physical wire and
may clear the injected bit before delivery. See `docs/virtio-blk-design.md
§4` for the full argument — it applies unchanged here.

## NOT YET DONE / unknowns for whoever applies this

- **Not applied to the live `.dtb`** — this doc describes the exact change;
  it has not been run against `/opt/bzdos/tftpboot/bananapi-min.dtb`.
- **Not verified against a live boot.** Whether FreeBSD's `virtio_mmio(4)` +
  `if_vtnet(4)` actually probe and attach this second node correctly (two
  sibling `virtio_mmio` nodes under the same `/soc` — should be fine per the
  virtio-mmio binding, but unconfirmed on THIS kernel/DTB combination) is
  unverified without a real boot.
- **SPI 106 collision check**: re-run the same grep `docs/virtio-blk-dtb.md`
  recommends (`dtc -I dtb -O dts ... | grep -oE '0x00 0x[0-9a-f]+ 0x0[0-9a-f]'`)
  against the CURRENT blob right before applying, in case anything changed
  the SPI map since this doc was written.
