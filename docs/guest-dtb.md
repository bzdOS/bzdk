# The guest's DTB: what is authoritative, and how to edit it

The FreeBSD guest boots with `/opt/bzdos/tftpboot/bananapi-min.dtb`, TFTP'd by
`chimpd.py` to `0x4a000000`. Everything about that file is easy to get wrong, and
two separate incidents this project has paid for came from getting it wrong, so
this is the single place that says how it works.

## The `.dtb` is authoritative. The `.dts` is not a build source.

`/opt/bzdos/tftpboot/bananapi-min.dts` exists, is **badly stale**, and **nothing
in the tree ever recompiles it**. The boot path only ever TFTPs the prebuilt
`.dtb`. Rebuilding the `.dtb` from that `.dts` would silently undo months of
accumulated surgical edits. Measured 2026-08-11:

| | deployed `.dtb` | the stale `.dts` |
|---|---|---|
| CPU nodes under `/cpus` | `cpu@0` only | four (`cpu@0..3`) |
| `/pmu` `interrupt-affinity` | one entry | four entries |
| `/pmu` `status` | `disabled` | absent |

The CPU trimming is the one that would hurt most: exposing `cpu@1..3` to the
guest would hand it cores that belong to the hypervisor (CPU1 is the EMAC/debug
core and owns the watchdog, CPU2 does async eMMC, CPU3 can run a second guest).
The guest is *supposed* to see exactly one CPU, and `hw.ncpu` returning 1 is the
correct, deliberate state.

The `.dtb.pre-*` / `.dtb.bak-*` siblings in that directory (there are 15) are the
rollback trail for those edits. Keeping one before every change is the
convention; follow it.

## Do NOT use dtc to round-trip this file

`docs/virtio-blk-dtb.md` and `docs/virtio-net-dtb.md` both recommend
decompile → edit → recompile with `dtc`. **That does not work on this blob**, and
it is not a permissions or warnings problem — dtc genuinely refuses to rebuild
it. Verified 2026-08-11:

```
$ dtc -I dtb -O dts bananapi-min.dtb > cur.dts     # fine
$ dtc -I dts -O dtb -o new.dtb cur.dts             # FAILS, no output produced
```

The blob contains a U-Boot `binman` subtree whose child is named `@fdt-SEQ` — a
template name, not a real unit address — and dtc will not accept it on the way
back in. Those two older docs are not wrong about anything else; just ignore
their recompile step and use the flow below.

## The flow that works: surgical edits with `fdtput`

```sh
cd /opt/bzdos/tftpboot
install -m644 bananapi-min.dtb bananapi-min.dtb.pre-<reason>   # rollback first
install -m644 bananapi-min.dtb /tmp/work.dtb                   # edit a copy
fdtput -t s /tmp/work.dtb /pmu status disabled                 # the change
# verify BEFORE deploying:
fdtget -p /tmp/work.dtb /pmu                                   # property present?
fdtget -l /tmp/work.dtb /                                      # node count unchanged?
fdtget -l /tmp/work.dtb /soc
install -m644 /tmp/work.dtb bananapi-min.dtb                   # deploy
sha256sum bananapi-min.dtb bananapi-min.dtb.pre-<reason>       # record both
```

`fdtput` handles the structure-block resize properly, which hand-editing the
binary does not. Node counts are a cheap structural check: 21 under `/`, 62 under
`/soc` as of 2026-08-11.

## Edits currently applied to the deployed blob

Reconstructed from the rollback trail and from what the running guest reports.
This list is best-effort for anything predating 2026-08-11 — the earlier edits
were applied without a record of this kind, which is part of why this document
now exists.

| Edit | Why | Backup |
|---|---|---|
| `/cpus` trimmed to `cpu@0` | CPU1-3 belong to the HV | `.pre-upcpu` |
| `/pmu` `status = "disabled"` | see below — this one bit hard | `.pre-pmu-disable` |
| `ethernet@1c30000` disabled | the HV owns the EMAC | `.pre-emac-disable` |
| USB nodes disabled | HV owns the OTG debug gadget | `.pre-usb-disable` |
| `reserved-memory`: hv-image / hv-scratch / hv-fb | guest must not allocate over them | `.pre-resmem`, `.pre-fb`, `.pre-hvimage-2mb` |
| virtio-mmio nodes (blk, net) | `vblk_emmc.c` / `vnet_emac.c` | `.pre-virtio`, `.pre-vnet` |

## Why `/pmu` is disabled — the expensive one

The A64 wires the PMU as four SPIs (116-119, `0x74..0x77`), one per core, so
`/pmu` lists four `interrupts`. When `/cpus` was trimmed to one node, the
`interrupt-affinity` list was trimmed with it — down to a single phandle — and
the `interrupts` list was **not**. FreeBSD's `pmu_attach()` then reads affinity
per interrupt, fails to find the second, prints *"Missing value in interrupt
affinity property"* and returns ENXIO — **after** having already allocated and
activated IRQ resource 0, which it never undoes. `device_attach()` parks the
device in `DS_NOTPRESENT` explicitly so it can be retried when new drivers load.

Result: every `kldload` of any driver triggered that retry and panicked the guest
with *"Attempt to double activation of resource id: 0"*. It looked like a hang
for four investigation passes. Full account:
`/opt/bzdos/bsdOS/hal/probe_test/ROOT-CAUSE-NOTES.md`.

Disabling the node fixes it completely, and costs nothing: the guest does not use
PMU counters (`hwpmc` is not loaded).

**The alternative, if guest PMU is ever wanted**: trim `interrupts` to the single
CPU0 SPI so it matches the single affinity entry —
`fdtput -t x <dtb> /pmu interrupts 0 0x74 4` — and remove `status`. That is
expected to attach cleanly and has **not been tried**; it adds interrupt
behaviour to a hardware-verified boot path, which is why it was not the change
made under time pressure.

## The general lesson

A trimmed DTB must stay internally consistent. Removing `/cpus` entries silently
invalidated a per-CPU list in a completely different node, and the failure did
not surface at boot as anything worse than one line of dmesg — it surfaced weeks
later as a panic in an unrelated operation. When trimming, grep the whole blob
for anything that is per-CPU-indexed, not just the node being changed.
