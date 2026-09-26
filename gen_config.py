#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""gen_config.py — reads board-config.xml, the single source of truth for
"which optional features are armed" and "what the guest's DTB must advertise
for them to do anything", and emits/applies both from one place:

  1. config.mk — a small Makefile fragment (VCPU2=0/1, VCPU1=0/1, HV_HDMI=..,
     HDMI_MODE_1080P=..) that the Makefile includes BEFORE its own `?=`
     defaults, so this file's values win without editing the Makefile.
  2. DTB edits (fdtput, idempotent) for every cpu@N node and virtio_mmio@...
     node board-config.xml says should exist given the current feature
     flags — the same manual recipe used all session for vinput/vcpu2, now
     driven from one file instead of five ad-hoc `fdtput` invocations typed
     by hand and easy to forget half of.

WHY: this session hit the exact "code says on, DTB doesn't advertise it"
mismatch twice (vcpu2's cpu@2 lived only in a separate, never-merged
bananapi-2cpu.dtb; vcpu1 shipped with no cpu@1 node at all). Both looked like
"armed and ready" from the C/Makefile side while being silently inert on
real hardware. This script makes that mismatch structurally impossible for
anything it manages: one file, one command, both artifacts updated together.

USAGE:
    python3 gen_config.py                 # apply to the live tftpboot DTB
    python3 gen_config.py --dtb PATH      # apply to a specific dtb instead
    python3 gen_config.py --dry-run       # print what would change, do nothing
    python3 gen_config.py --no-backup     # skip the automatic .pre-genconfig copy

Requires `dtc`/`fdtput` on PATH (already required by this project's own
existing DTB-editing docs) and Python's stdlib only.
"""
import argparse
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_XML = HERE / "board-config.xml"
DEFAULT_DTB = Path("/opt/bzdos/tftpboot/bananapi-min.dtb")
# The /memory size the board ships with when no feature widens it: the low
# GiB, matching stage2.h's STAGE2_DRAM_SIZE default.
DEFAULT_MEMORY_SIZE = "0x40000000"
CONFIG_MK = HERE / "config.mk"

# SPI numbers must stay inside the documented free gap (docs/virtio-blk-dtb.md).
SPI_GAP = range(0x68, 0x74)

# Existing-hardware nodes no <feature dtb-nodes="..."> may ever list, checked
# structurally (not just by review) for the same reason wdogtrap.c's WDOG
# check is a hard first-return rather than a case a future edit could shuffle
# past. Each one is a real hazard already root-caused elsewhere in this tree:
#   - usb@1c19000 (MUSB/OTG) and usb@1c1a000/usb@1c1a400 (EHCI0/OHCI0) share
#     the OTG PHY (phys = <&usbphy 0>, same "usb0" lane as MUSB) with the
#     hypervisor's OWN debug/break-glass console (usbacm.c) -- see usbacm.h's
#     "KNOWN OPEN RISK". EHCI1/OHCI1 use the independent "usb1" lane instead
#     (phys = <&usbphy 1>) and are the pair guest_usb_host1 in
#     board-config.xml actually enables.
#   - watchdog@1c20ca0 is disabled BY DESIGN (PROGRESS.md 2026-07-15) -- the
#     board's only unattended recovery path. See ORIENTATION.md.
#   - dma-controller@1c02000 is the SoC's general-purpose memory-to-memory
#     DMA engine: no SMMU gates it, and docs/dma-bypass-stage2.md documents
#     it as "provably unused" today specifically because every consumer that
#     could arm it is also disabled. dai@1c22c00 (the codec-i2s DAI, the
#     ONLY thing in the audio chain with a `dmas` property) is its one path
#     back to armed; leaving both off keeps that invariant true.
#
# The PHY-sharing entries (usb@1c1a000, usb@1c1a400) are ALSO derivable from
# the <silicon-sharing> section below — derive_forbidden_from_sharing() checks
# that they appear here, not duplicated logic. The remaining entries
# (watchdog, dma-controller, dai) are architectural invariants with no sharing
# relationship and stay hardcoded.
FORBIDDEN_DTB_NODES = {
    "/soc/usb@1c19000", "/soc/usb@1c1a000", "/soc/usb@1c1a400",
    "/soc/watchdog@1c20ca0",
    "/soc/dma-controller@1c02000", "/soc/dai@1c22c00",
}

# Nodes that the `dev_relax_dma_isolation` feature un-forbids, and only those.
#
# Why this exists: the audio chain is unusable without dai@1c22c00. The `sound`
# node's simple-audio-card cpu link points straight at it, so with the DAI
# disabled the driver still binds (pcm0 appears in devinfo) and can never
# create a stream -- /dev/sndstat stays empty. There is no way to get audio
# without this node.
#
# What it costs: enabling it arms the SoC's general-purpose DMA engine, which
# no SMMU gates. That is the whole reason it is forbidden, and it is a real
# hole rather than a formality -- a guest able to program that engine can read
# and write hypervisor memory regardless of stage-2.
#
# Therefore: this is a DEVELOPMENT switch. While it is on, the stage-2
# isolation result recorded for this board does NOT hold, and an isolation
# test run in this configuration measures nothing. Do not ship a build with it
# enabled.
#
# The guard stays hardcoded on purpose. You cannot get past it by editing
# FORBIDDEN_DTB_NODES; only by naming this feature in board-config.xml, which
# leaves a visible and reviewable statement of intent.
DMA_ISOLATION_RELAXABLE = {
    "/soc/dai@1c22c00",
}


def effective_forbidden(features):
    """FORBIDDEN_DTB_NODES, minus whatever an explicit dev feature un-forbids.

    Returns (forbidden_set, relaxed_bool).
    """
    feat = features.get("dev_relax_dma_isolation")
    relax = bool(feat and feat.get("enabled"))
    if not relax:
        return set(FORBIDDEN_DTB_NODES), False
    return set(FORBIDDEN_DTB_NODES) - DMA_ISOLATION_RELAXABLE, True


def load_silicon_sharing(root):
    """Parse the <silicon-sharing> section (SPEC_chimp_hal §3.3, часть 1).

    Returns a list of groups, each with:
        name, description, and a list of blocks:
            {path, owner, note?, feature?}
    A group is a set of SoC blocks that share a physical resource (PHY, clock,
    bus). The ownership rule: if ANY block in a group is owner="hv" or "hv-rt",
    then ALL other blocks in that group must be the same owner — a guest-owned
    block that shares silicon with an hv-owned one is a conflict, because the
    guest could corrupt the hv's hardware through shared state.
    """
    groups = []
    ss_el = root.find("silicon-sharing")
    if ss_el is None:
        return groups
    for g in ss_el.findall("group"):
        blocks = []
        for b in g.findall("block"):
            blocks.append({
                "path": b.get("path"),
                "owner": b.get("owner"),
                "note": b.get("note"),
                "feature": b.get("feature"),
            })
        groups.append({
            "name": g.get("name"),
            "description": g.get("description"),
            "blocks": blocks,
        })
    return groups


def derive_forbidden_from_sharing(silicon_groups):
    """Derive FORBIDDEN_DTB_NODES entries from <silicon-sharing> groups.

    In any group where an hv-owned block shares silicon with a non-hv block,
    that non-hv block is forbidden from the guest DTB — the guest could corrupt
    the shared silicon state. Returns a set of paths.
    """
    derived = set()
    for grp in silicon_groups:
        hv_owners = {b["owner"] for b in grp["blocks"] if b["owner"] in ("hv", "hv-rt")}
        if hv_owners:
            # Any non-hv block in this group shares silicon with an hv owner
            for b in grp["blocks"]:
                if b["owner"] not in ("hv", "hv-rt"):
                    if b.get("path"):
                        derived.add(b["path"])
    return derived


def validate_silicon_sharing(silicon_groups):
    """Check structural integrity of <silicon-sharing> declarations.

    Validates per SPEC_chimp_hal §3.3:
    1. Every group must have a name= attribute.
    2. Every block must have path= and owner= attributes.
    3. owner values must be in VALID_OWNERS.
    4. No two blocks in the same group may have conflicting ownership if one
       is hv/hv-rt and the other is guest — this is caught structurally here
       (the FORBIDDEN_DTB_NODES cross-check in validate() handles the DTB side).
    """
    errors = []
    for grp in silicon_groups:
        gname = grp.get("name")
        if not gname:
            errors.append(f"silicon-sharing group: missing name= attribute")
            continue
        seen_owners = set()
        for b in grp.get("blocks", []):
            p = b.get("path")
            o = b.get("owner")
            if not p:
                errors.append(f"silicon-sharing group '{gname}': block missing path= attribute")
                continue
            if not p.startswith("/"):
                errors.append(f"silicon-sharing group '{gname}': path={p!r} must be absolute DTB path")
            if o is None:
                errors.append(f"silicon-sharing group '{gname}': block path={p!r} missing owner= attribute")
            elif o not in VALID_OWNERS:
                errors.append(f"silicon-sharing group '{gname}': block path={p!r} owner={o!r} is not valid")
            if o:
                seen_owners.add(o)
        # hv + guest in same group = ownership conflict
        if "hv" in seen_owners and "guest" in seen_owners:
            errors.append(f"silicon-sharing group '{gname}': hv-owned and guest-owned "
                          f"blocks share silicon — guest could corrupt hv hardware "
                          f"through shared state. This conflict should already be "
                          f"prevented by FORBIDDEN_DTB_NODES")
    return errors


def load_config(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    features = {}
    for f in root.find("features"):
        features[f.get("name")] = {
            "mkvar": f.get("mkvar"),
            "enabled": f.get("enabled", "false") == "true",
            "needs_dtb_cpu": f.get("needs-dtb-cpu"),
            "dtb_memory_size": f.get("dtb-memory-size"),
            # dtb-only: no C code anywhere is gated by this feature, only
            # what the guest's DTB advertises -- see board-config.xml's
            # guest_usb_host1-and-siblings comment. Skips the mkvar
            # requirement below and is never written to config.mk.
            "dtb_only": f.get("dtb-only", "false") == "true",
        }

    devices = []
    for d in root.find("devices"):
        devices.append({
            "name": d.get("name"),
            "base": d.get("base"),
            "spi": d.get("spi"),
            "device_id": d.get("device-id"),
            "always": d.get("always", "false") == "true",
            "enabled_by": d.get("enabled-by"),
            "no_dtb_node": d.get("no-dtb-node", "false") == "true",
            "owner": d.get("owner"),
            "shares": d.get("shares"),
        })

    soc_nodes = []
    soc_nodes_el = root.find("soc-nodes")
    if soc_nodes_el is not None:
        for n in soc_nodes_el:
            soc_nodes.append({
                "feature": n.get("feature"),
                "path": n.get("path"),
                "owner": n.get("owner"),
                "shares": n.get("shares"),
            })

    silicon_sharing = load_silicon_sharing(root)

    return features, devices, soc_nodes, silicon_sharing


VALID_OWNERS = {"hv", "guest", "none", "hv-rt"}


def validate(features, devices, soc_nodes, silicon_sharing):
    """Cheap, load-bearing sanity checks before touching anything real."""
    errors = []

    # --- owner attribute checks (SPEC_chimp_hal §2 ownership classes) ---
    for d in devices:
        o = d.get("owner")
        if o is None:
            errors.append(f"device {d['name']}: missing owner= attribute "
                          f"(must be one of {sorted(VALID_OWNERS)})")
        elif o not in VALID_OWNERS:
            errors.append(f"device {d['name']}: owner={o!r} is not a valid "
                          f"ownership class (must be one of {sorted(VALID_OWNERS)})")

    for n in soc_nodes:
        o = n.get("owner")
        if o is None:
            errors.append(f"soc-node path={n['path']}: missing owner= attribute")
        elif o not in VALID_OWNERS:
            errors.append(f"soc-node path={n['path']}: owner={o!r} is not valid")

    # --- silicon-sharing conflict checks (SPEC_chimp_hal §3.3 part 1) ---
    # A guest-owned block that shares silicon (PHY, clock, bus) with an
    # hv/hv-rt-owned block is a conflict: the guest could corrupt the hv's
    # hardware through shared state. derive_forbidden_from_sharing() computes
    # which nodes this applies to; validate_silicon_sharing() checks the
    # structural integrity of the <silicon-sharing> declaration itself.
    errors.extend(validate_silicon_sharing(silicon_sharing))

    derived_forbidden = derive_forbidden_from_sharing(silicon_sharing)
    for path in sorted(derived_forbidden):
        if path not in FORBIDDEN_DTB_NODES:
            errors.append(f"silicon-sharing group: path={path!r} shares silicon "
                          f"with an hv-owned block but is NOT in FORBIDDEN_DTB_NODES "
                          f"— add it there or fix the sharing declaration")

    # --- SPI uniqueness and gap checks ---

    seen_spi = {}
    for d in devices:
        if d["no_dtb_node"] or d["spi"] in (None, "none"):
            continue
        spi = int(d["spi"], 0)
        if spi not in SPI_GAP:
            errors.append(f"device {d['name']}: SPI {spi} (0x{spi:x}) is "
                          f"outside the documented free gap 0x68..0x73 — "
                          f"check docs/virtio-blk-dtb.md before using it")
        if spi in seen_spi:
            errors.append(f"device {d['name']}: SPI {spi} (0x{spi:x}) "
                           f"already used by device {seen_spi[spi]}")
        seen_spi[spi] = d["name"]

    for name, f in features.items():
        if not f["mkvar"] and not f["dtb_only"]:
            errors.append(f"feature {name}: no mkvar= attribute (and not "
                           f"dtb-only=\"true\") — gen_config.py doesn't know "
                           f"which Makefile variable this controls")

    for n in soc_nodes:
        if not n["path"] or not n["path"].startswith("/"):
            errors.append(f"soc-node feature={n['feature']}: path "
                           f"{n['path']!r} must be an absolute DTB path")
        if n["feature"] not in features:
            errors.append(f"soc-node path={n['path']}: feature "
                           f"{n['feature']!r} has no matching <feature> entry")
        forbidden, _relaxed = effective_forbidden(features)
        if n["path"] in forbidden:
            errors.append(f"soc-node path={n['path']} (feature "
                           f"{n['feature']!r}): this node is in "
                           f"FORBIDDEN_DTB_NODES — see that constant's "
                           f"comment for which hazard it reopens, and do not "
                           f"remove the node from the set to get past this")

    if errors:
        for e in errors:
            print(f"[gen_config] ERROR: {e}", file=sys.stderr)
        sys.exit(1)


def cross_check_board_constants(devices, soc_nodes, features=None):
    """Verify ownership classes are consistent with board_bpi_m64.h / soc_a64.h.

    Reads #define constants from both headers and checks:
    1. Every device with owner="guest" must NOT have a base address matching an
       SoC address documented as EL2-critical (eMMC, EMAC, UART0, RSB, etc.)
       in soc_a64.h — a guest device backed by EL2-owned silicon is a
       ownership-class violation per SPEC_chimp_hal §2.
    2. Every device with owner="hv" that has a base address must have that
       address appear in board_bpi_m64.h or soc_a64.h — a hypervisor device
       whose address isn't in any header is an undocumented constant.
    3. Soc-nodes with owner="guest" must not reference paths that correspond
       to EL2-essential peripherals (dma-controller, dai, watchdog, MUSB).
    """
    board_header = HERE / "board_bpi_m64.h"
    soc_header = HERE / "soc_a64.h"
    errors = []

    # Parse #define NAME value from a header file
    def parse_defines(path):
        defines = {}
        if not path.exists():
            return defines
        for line in path.read_text().splitlines():
            m = re.match(r'^\s*#\s*define\s+(\S+)\s+(.+)', line)
            if m:
                name, val = m.group(1), m.group(2).strip()
                # Strip trailing C comment
                val = re.sub(r'/\*.*?\*/\s*$', '', val).strip()
                # Extract numeric value (hex or decimal)
                vm = re.match(r'^(0x[0-9a-fA-F]+|\d+)', val)
                if vm:
                    defines[name] = vm.group(1)
        return defines

    board_defs = parse_defines(board_header)
    soc_defs = parse_defines(soc_header)

    # EL2-critical addresses from soc_a64.h — a guest device must not back
    # onto these (ownership conflict per SPEC §2).
    EL2_CRITICAL_NAMES = {
        "SOC_A64_GICD_BASE", "SOC_A64_GICC_BASE", "SOC_A64_GICH_BASE",
        "SOC_A64_GICV_BASE", "SOC_A64_CCU_BASE", "SOC_A64_PIO_BASE",
        "SOC_A64_WDOG_CTRL", "SOC_A64_WDOG_CFG", "SOC_A64_WDOG_MODE",
        "SOC_A64_SRAMC_BASE", "SOC_A64_SYSCON_EMAC",
        "SOC_A64_UART0_BASE", "SOC_A64_EMAC_BASE",
        "SOC_A64_MUSB_BASE", "SOC_A64_USBPHY_CTRL_BASE",
        "SOC_A64_THS_BASE",
    }
    el2_critical_addrs = set()
    for name in EL2_CRITICAL_NAMES:
        if name in soc_defs:
            try:
                el2_critical_addrs.add(int(soc_defs[name], 0))
            except ValueError:
                pass

    # RSB/PMIC bases from board_bpi_m64.h — also EL2-owned per SPEC §2.
    for name, val in board_defs.items():
        if "RSB" in name or "PRCM" in name or "AXP803" in name:
            try:
                el2_critical_addrs.add(int(val, 0))
            except ValueError:
                pass

    # Check devices
    for d in devices:
        o = d.get("owner")
        if not o:
            continue
        if d.get("base"):
            try:
                base = int(d["base"], 0)
            except ValueError:
                continue
            if o == "guest" and base in el2_critical_addrs:
                errors.append(
                    f"device {d['name']}: owner=guest but base={d['base']} "
                    f"matches an EL2-critical address in soc_a64.h / "
                    f"board_bpi_m64.h — ownership conflict per SPEC §2")
            if o == "hv":
                # Check if base falls within any known board-level range
                # (virtio-mmio trapped block, etc.) or matches a known address.
                known_ranges = []
                virtio_base = board_defs.get("BOARD_BPI_M64_VIRTIO_MMIO_BASE")
                virtio_size = board_defs.get("BOARD_BPI_M64_VIRTIO_MMIO_SIZE")
                if virtio_base and virtio_size:
                    try:
                        vb = int(virtio_base, 0)
                        vs = int(virtio_size, 0)
                        known_ranges.append((vb, vb + vs))
                    except ValueError:
                        pass
                in_range = any(lo <= base < hi for lo, hi in known_ranges)
                all_known = set()
                for v in board_defs.values():
                    try:
                        all_known.add(int(v, 0))
                    except ValueError:
                        pass
                for v in soc_defs.values():
                    try:
                        all_known.add(int(v, 0))
                    except ValueError:
                        pass
                if not in_range and base not in all_known and base not in el2_critical_addrs:
                    errors.append(
                        f"device {d['name']}: owner=hv but base={d['base']} "
                        f"not found in board_bpi_m64.h or soc_a64.h — "
                        f"undocumented constant")

    # Check soc-nodes: guest-owned must not be EL2-essential peripherals
    EL2_ESSENTIAL_PATHS = {
        "/soc/dma-controller@1c02000",
        "/soc/dai@1c22c00",
        "/soc/watchdog@1c20ca0",
        "/soc/usb@1c19000",      # MUSB
    }
    # dev_relax_dma_isolation waives this for exactly the nodes named in
    # DMA_ISOLATION_RELAXABLE, and for nothing else. The waiver is deliberately
    # spelled out in both guards rather than centralised: each one states a
    # different invariant (this one is ownership, the other is the DMA hazard),
    # and a single shared bypass would be far easier to widen by accident.
    _relaxable = set()
    if features is not None and effective_forbidden(features)[1]:
        _relaxable = DMA_ISOLATION_RELAXABLE
    for n in soc_nodes:
        o = n.get("owner")
        p = n.get("path", "")
        if o == "guest" and p in EL2_ESSENTIAL_PATHS and p not in _relaxable:
            errors.append(
                f"soc-node path={p}: owner=guest but this is an EL2-essential "
                f"peripheral — ownership conflict per SPEC §2")

    if errors:
        for e in errors:
            print(f"[gen_config] OWNER CHECK: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"[gen_config] owner cross-check: {len(devices)} devices, "
          f"{len(soc_nodes)} soc-nodes, {len(EL2_ESSENTIAL_PATHS)} EL2-essential "
          f"paths — OK")


def emit_makefile_fragment(features, dry_run):
    lines = [
        "# GENERATED by gen_config.py from board-config.xml. Do not hand-edit —",
        "# your changes will be silently overwritten the next time someone runs",
        "# `python3 gen_config.py`. Edit board-config.xml instead.",
        "",
    ]
    for name, f in sorted(features.items()):
        if f["dtb_only"]:
            continue
        lines.append(f"{f['mkvar']} = {1 if f['enabled'] else 0}")
    content = "\n".join(lines) + "\n"

    if dry_run:
        print(f"[gen_config] would write {CONFIG_MK}:")
        print(content)
        return
    CONFIG_MK.write_text(content)
    print(f"[gen_config] wrote {CONFIG_MK}")


def dtb_to_dts_text(dtb_path):
    r = subprocess.run(["dtc", "-I", "dtb", "-O", "dts", str(dtb_path)],
                        capture_output=True, text=True)
    return r.stdout


def node_exists(dts_text, node_name):
    return re.search(rf"\b{re.escape(node_name)}\s*{{", dts_text) is not None


def parse_node_cells(dts_text, node_name, prop):
    """Pull a <...> cell-list property's values out of one node's block, as
    ints. Returns None if the node or property isn't found. Used to check an
    EXISTING node actually has the values it should, not just that it
    exists — see the fdtput() hex-vs-decimal bug this caught."""
    m = re.search(rf"\b{re.escape(node_name)}\s*{{(.*?)\n\t*}};", dts_text, re.S)
    if not m:
        return None
    pm = re.search(rf"\b{re.escape(prop)}\s*=\s*<([^>]*)>", m.group(1))
    if not pm:
        return None
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", pm.group(1))]


def fdtput(dtb_path, node, prop_type, prop, *values, dry_run=False):
    # fdtput's "-t x" parses each argument string AS HEX DIGITS, not as a
    # decimal string of a value already computed in Python. Passing str(v)
    # for an int v (e.g. str(0x0A004000) == "167780352") makes fdtput read
    # THAT decimal-looking string as hex, silently corrupting reg/interrupts
    # for any multi-digit value — bit vblk_sd's base/SPI on first use (cpu_id
    # values 1/2/3 happened to survive since a single digit is the same in
    # both bases). Hex-format ints explicitly for "x" so the two never mix.
    if prop_type == "x":
        vals = [v if isinstance(v, str) else format(v, "x") for v in values]
    else:
        vals = [str(v) for v in values]
    cmd = ["fdtput", "-p", "-t", prop_type, str(dtb_path), node, prop, *vals]
    if dry_run:
        print("[gen_config] would run:", " ".join(cmd))
        return
    subprocess.run(cmd, check=True)


def _phandle_of(dts_text, header_re, inner_re=None):
    """Phandle of the first node whose header matches header_re (and, if
    given, whose body matches inner_re), or None."""
    for m in re.finditer(header_re + r"\s*\{", dts_text):
        depth, i = 1, m.end()
        while depth and i < len(dts_text):
            depth += {"{": 1, "}": -1}.get(dts_text[i], 0)
            i += 1
        body = dts_text[m.end():i]
        own = re.sub(r"\{[^{}]*\}", "", body)        # drop nested nodes
        if inner_re and not re.search(inner_re, body):
            continue
        ph = re.search(r"phandle = <(0x[0-9a-f]+)>;", own)
        if ph:
            return int(ph.group(1), 16)
    return None


# CPU DVFS: what every /cpus/cpu@N needs for cpufreq_dt to attach. The OPP
# table, vdd-cpux (dcdc2) and the CCU's CPUX clock (CLK_CPUX = 21) are all in
# the DTB already; only the references were missing from cpu@1..3 (added by
# ensure_cpu_node) and had been stripped from cpu@0 in 2026-07, when guest
# DVFS wedged the SoC. That was with the PMIC's RSB bus shared unarbitrated
# with EL2; since rsbtrap.c (2026-09-26) all frequency steps 648-1152 MHz
# were verified on hardware with the voltages logged.
CPU_DVFS_PROPS = ("clocks", "operating-points-v2", "cpu-supply", "#cooling-cells")


def ensure_cpu_dvfs(dtb_path, dts_text, enabled, dry_run):
    cpus = sorted(set(re.findall(r"\bcpu@(\d+) \{", dts_text)))
    if not enabled:
        # Only what 2026-07 stripped: cpu@0 keeps its clocks/#cooling-cells
        # (thermal cooling-maps reference it).
        for c in cpus:
            for prop in ("operating-points-v2", "cpu-supply"):
                if not dry_run:
                    subprocess.run(["fdtput", "-d", str(dtb_path), f"/cpus/cpu@{c}", prop],
                                   stderr=subprocess.DEVNULL)
        print("[gen_config] cpu DVFS: off (references removed)")
        return
    opp = _phandle_of(dts_text, r"opp-table-cpu")
    vdd = _phandle_of(dts_text, r"dcdc2", r'regulator-name = "vdd-cpux"')
    ccu = _phandle_of(dts_text, r"(?:clock|clock-controller)@1c20000")
    if None in (opp, vdd, ccu):
        sys.exit(f"[gen_config] cpu DVFS: cannot resolve opp={opp} vdd-cpux={vdd} ccu={ccu}")
    for c in cpus:
        path = f"/cpus/cpu@{c}"
        fdtput(dtb_path, path, "x", "clocks", ccu, 0x15, dry_run=dry_run)
        fdtput(dtb_path, path, "x", "operating-points-v2", opp, dry_run=dry_run)
        fdtput(dtb_path, path, "x", "cpu-supply", vdd, dry_run=dry_run)
        fdtput(dtb_path, path, "x", "#cooling-cells", 2, dry_run=dry_run)
    print(f"[gen_config] cpu DVFS: cpu@{',cpu@'.join(cpus)} -> opp 0x{opp:x}, "
          f"vdd-cpux 0x{vdd:x}, ccu 0x{ccu:x}")


def _cpu_phandles(dtb_path, dts_text, dry_run):
    """{cpu_id: phandle} for every /cpus/cpu@N, giving the ones
    ensure_cpu_node() created (which have none) a fresh phandle."""
    used = {int(x, 16) for x in re.findall(r"phandle = <(0x[0-9a-f]+)>;", dts_text)}
    nxt = max(used or {0}) + 1
    out = {}
    for c in sorted(set(int(x) for x in re.findall(r"\bcpu@(\d+) \{", dts_text))):
        ph = _phandle_of(dts_text, rf"cpu@{c}")
        if ph is None:
            ph, nxt = nxt, nxt + 1
            fdtput(dtb_path, f"/cpus/cpu@{c}", "x", "phandle", ph, dry_run=dry_run)
        out[c] = ph
    return out


# The Cortex-A53 PMU: SPIs 116-119, one per core, which the DT pairs with the
# cores through interrupt-affinity. Disabled 2026-08-10 because the DTB then
# exposed one CPU but kept four interrupts: pmu_attach failed with ENXIO after
# activating IRQ 0, and the next kldload's retry panicked ("double activation
# of resource"). With every CPU in the DT, affinity lists all of them.
def ensure_pmu(dtb_path, dts_text, enabled, dry_run):
    if not enabled:
        ensure_node_status(dtb_path, "/pmu", False, dry_run)
        return
    phs = _cpu_phandles(dtb_path, dts_text, dry_run)
    m = re.search(r"pmu \{[^}]*interrupts = <([^>]*)>;", dts_text)
    nirq = len(m.group(1).split()) // 3 if m else 0
    cpus = sorted(phs)[:nirq]
    if len(cpus) != nirq:
        sys.exit(f"[gen_config] pmu: {nirq} interrupts but cpus {sorted(phs)}")
    fdtput(dtb_path, "/pmu", "x", "interrupt-affinity", *[phs[c] for c in cpus],
           dry_run=dry_run)
    ensure_node_status(dtb_path, "/pmu", True, dry_run)
    print(f"[gen_config] pmu: {nirq} interrupts -> cpu@{',cpu@'.join(map(str, cpus))}")


def ensure_cpu_node(dtb_path, dts_text, cpu_id, dry_run):
    node = f"cpu@{cpu_id}"
    path = f"/cpus/{node}"
    if node_exists(dts_text, node):
        print(f"[gen_config] {node}: already present, leaving as-is")
        return
    print(f"[gen_config] {node}: adding (minimal, mirrors cpu@0's essential fields)")
    fdtput(dtb_path, path, "s", "compatible", "arm,cortex-a53", dry_run=dry_run)
    fdtput(dtb_path, path, "s", "device_type", "cpu", dry_run=dry_run)
    fdtput(dtb_path, path, "x", "reg", cpu_id, dry_run=dry_run)
    fdtput(dtb_path, path, "s", "enable-method", "psci", dry_run=dry_run)


def ensure_virtio_node(dtb_path, dts_text, dev, dry_run):
    addr_hex = int(dev["base"], 0)
    node_name = f"virtio_mmio@{addr_hex:x}"
    path = f"/soc/{node_name}"
    spi = int(dev["spi"], 0)
    expected_reg = [addr_hex, 0x200]
    expected_irq = [0x0, spi, 0x1]
    if node_exists(dts_text, node_name):
        reg = parse_node_cells(dts_text, node_name, "reg")
        irq = parse_node_cells(dts_text, node_name, "interrupts")
        if reg == expected_reg and irq == expected_irq:
            print(f"[gen_config] {node_name} ({dev['name']}): already present and correct, leaving as-is")
            return
        print(f"[gen_config] {node_name} ({dev['name']}): present but WRONG "
              f"(reg={reg}, interrupts={irq}, expected reg={expected_reg} "
              f"interrupts={expected_irq}) — correcting")
    else:
        print(f"[gen_config] {node_name} ({dev['name']}): adding")
    fdtput(dtb_path, path, "s", "compatible", "virtio,mmio", dry_run=dry_run)
    fdtput(dtb_path, path, "x", "reg", addr_hex, 0x200, dry_run=dry_run)
    fdtput(dtb_path, path, "x", "interrupts", 0x0, spi, 0x1, dry_run=dry_run)
    fdtput(dtb_path, path, "x", "interrupt-parent", 0x1, dry_run=dry_run)
    fdtput(dtb_path, path, "s", "status", "okay", dry_run=dry_run)


def reorder_virtio_nodes(dtb_path, dry_run):
    """FreeBSD assigns vtbdN/vtnetN unit numbers in DTB CHILD-LIST order,
    NOT by each node's 'reg' address — confirmed live 2026-08-24: adding
    vblk_sd via fdtput -p put its virtio_mmio@a004000 node BEFORE the three
    pre-existing ones (fdtput -p inserts a new node at the head of its
    parent's child list), which silently swapped vtbd0 (root, hardcoded as
    'ufs:/dev/vtbd0p3' in kload.c's kenv and in bzd_board.py's
    ROOT_MOUNTFROM) onto the SD card instead of the eMMC, and the guest sat
    at the mountroot prompt until fixed by hand.
    Leaving node order as "whatever fdtput happened to insert" is a landmine
    for every future device added through board-config.xml, so this makes
    ascending-by-address order a structural invariant re-enforced on every
    run, not a one-off manual fix. Requires the virtio_mmio@ nodes to be
    CONTIGUOUS siblings (true today: they're the only things gen_config.py
    ever inserts into /soc, and fdtput -p always inserts at the same spot
    right after /soc's own properties) — if a future change breaks that
    assumption, this only warns and leaves the DTB untouched rather than
    risk reassembling the tree wrong."""
    dts_text = dtb_to_dts_text(dtb_path)
    blocks = list(re.finditer(r"[ \t]*virtio_mmio@([0-9a-fA-F]+)\s*\{.*?\n[ \t]*\};\n?",
                               dts_text, re.S))
    if len(blocks) < 2:
        return
    for a, b in zip(blocks, blocks[1:]):
        if dts_text[a.end():b.start()].strip():
            print("[gen_config] NOTE: virtio_mmio nodes are no longer "
                  "contiguous siblings — skipping the address-order check "
                  "(reorder_virtio_nodes()'s contiguity assumption no "
                  "longer holds, fix it before trusting vtbdN==this-device "
                  "assumptions anywhere)")
            return

    addrs = [int(b.group(1), 16) for b in blocks]
    if addrs == sorted(addrs):
        print("[gen_config] virtio_mmio nodes already in address order, leaving as-is")
        return

    print(f"[gen_config] virtio_mmio nodes out of order ({[hex(a) for a in addrs]}) "
          f"— this reshuffles vtbdN/vtnetN unit numbers on the guest — resorting")
    if dry_run:
        print("[gen_config] would resort and recompile the DTB")
        return

    ordered = sorted(blocks, key=lambda b: int(b.group(1), 16))
    start, end = blocks[0].start(), blocks[-1].end()
    new_text = dts_text[:start] + "".join(b.group(0) for b in ordered) + dts_text[end:]

    tmp_dts = dtb_path.with_suffix(".gen_config_reorder.dts")
    tmp_dts.write_text(new_text)
    try:
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb_path), str(tmp_dts)],
                        check=True, capture_output=True, text=True)
    finally:
        tmp_dts.unlink()
    print("[gen_config] DTB recompiled with virtio_mmio nodes in address order")


def ensure_memory_size(dtb_path, dts_text, size_hex, dry_run):
    """Keep the DTB's /memory node in step with STAGE2_DRAM_SIZE.

    The guest learns how much RAM it has from this node and nothing rewrites it
    at load time -- kload.c builds a kenv string but never touches /memory. So a
    build with GUEST_DRAM_2G and a 1 GiB /memory node gives the guest a 2 GiB
    stage-2 window it will never allocate out of, and the reverse gives it a
    node promising memory stage-2 will fault on. Both halves are set from one
    switch here for exactly the reason the cpu@N handling above exists."""
    want = int(size_hex, 16)
    cur = fdtget_ints(dtb_path, "/memory", "reg")
    if cur is not None and len(cur) >= 2 and cur[1] == want:
        print(f"[gen_config] /memory: size already {want:#x}, leaving as-is")
        return
    have = f"{cur[1]:#x}" if cur and len(cur) >= 2 else "unreadable"
    # MiB, not GiB: a non-power-of-two size like 0x78000000 (1920 MiB) printed
    # as "want >> 30 GiB" reads as "1 GiB", which is exactly the sort of quietly
    # wrong log line that costs someone an hour later.
    print(f"[gen_config] /memory: size {have} -> {want:#x} "
          f"({want >> 20} MiB), base left at 0x40000000")
    fdtput(dtb_path, "/memory", "x", "reg", 0x40000000, want, dry_run=dry_run)


def fdtget_ints(dtb_path, node, prop):
    r = subprocess.run(["fdtget", str(dtb_path), node, prop],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    try:
        return [int(x) for x in r.stdout.split()]
    except ValueError:
        return None


def fdtget_str(dtb_path, node, prop):
    r = subprocess.run(["fdtget", str(dtb_path), node, prop],
                        capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return r.stdout.strip()


def ensure_node_status(dtb_path, node_path, want_enabled, dry_run):
    """Flip ONE existing silicon node's `status` between "okay"/"disabled" —
    see board-config.xml's <soc-nodes>. Never adds or removes a node, only
    the property that gates whether FreeBSD's OFW device-enumeration probes
    it at all. A DTB node with no `status` property is implicitly "okay" per
    the devicetree spec, so an unreadable/missing property is treated as
    "okay", not as "needs no action" — this is the same both-directions
    self-healing reconcile_drift() does for cpu@N nodes, applied here to a
    property instead of a whole node."""
    want = "okay" if want_enabled else "disabled"
    cur = fdtget_str(dtb_path, node_path, "status")
    cur_effective = cur if cur is not None else "okay"
    if cur_effective == want:
        print(f"[gen_config] {node_path}: status already {want}, leaving as-is")
        return
    print(f"[gen_config] {node_path}: status {cur_effective} -> {want}")
    fdtput(dtb_path, node_path, "s", "status", want, dry_run=dry_run)


def ensure_rpio_fix(dtb_path, dry_run):
    """Board-free fix for gpio1 ENOMEM (internal-note/193).

    Root cause: pinctrl@1f02c00's `interrupts` is 3 cells <0 0x2d 0x04> but its
    parent r_intc declared #interrupt-cells =2. dtc warns
    "size is (12), expected multiple of 8", and FreeBSD's OFW decode of `interrupts` via the parent's #interrupt-cells fails, so aw_gpio's
    bus_alloc_resources(IRQ) returns ENXIO -> "cannot allocate device
    resources".

    FreeBSD's aw_r_intc.c hardcodes ncells==3 (see
    aw_r_intc_gicp_convert_map_data: daf->ncells !=3 -> NULL), so the
    correct spec-compliant fix is to widen r_intc to 3, not to shrink the
    child to 2. The child already carries the correct 3-cell GIC_SPI
    encoding (type, spi, flags) that the driver expects. Also ensures
    pinctrl@1f02c00 has its interrupt-parent pointing at r_intc.
    """
    cur = fdtget_ints(dtb_path, "/soc/interrupt-controller@1f00c00", "#interrupt-cells")
    if cur != [3]:
        have = cur[0] if cur else "missing"
        print(f"[gen_config] /soc/interrupt-controller@1f00c00: #interrupt-cells {have} -> 3 (r_intc driver expects 3 cells, DT had 2)")
        fdtput(dtb_path, "/soc/interrupt-controller@1f00c00", "x", "#interrupt-cells", 0x3, dry_run=dry_run)
    else:
        print(f"[gen_config] /soc/interrupt-controller@1f00c00: #interrupt-cells already 3, leaving as-is")
    cur_parent = fdtget_ints(dtb_path, "/soc/pinctrl@1f02c00", "interrupt-parent")
    want_parent = fdtget_ints(dtb_path, "/soc/interrupt-controller@1f00c00", "phandle")
    want_val = want_parent[0] if want_parent else 0x4e
    if cur_parent != [want_val]:
        have = f"{cur_parent}" if cur_parent else "missing"
        print(f"[gen_config] /soc/pinctrl@1f02c00: interrupt-parent {have} -> <{want_val:#x}> (r_intc)")
        fdtput(dtb_path, "/soc/pinctrl@1f02c00", "x", "interrupt-parent", want_val, dry_run=dry_run)
    else:
        print(f"[gen_config] /soc/pinctrl@1f02c00: interrupt-parent already <{want_val:#x}>, leaving as-is")
    cur_irq = fdtget_ints(dtb_path, "/soc/pinctrl@1f02c00", "interrupts")
    want_irq = [0x0, 0x2d, 0x4]
    if cur_irq != want_irq:
        print(f"[gen_config] /soc/pinctrl@1f02c00: interrupts {cur_irq} -> {want_irq} (SPI45 level-high, 3 cells for r_intc)")
        fdtput(dtb_path, "/soc/pinctrl@1f02c00", "x", "interrupts", *want_irq, dry_run=dry_run)
    else:
        print(f"[gen_config] /soc/pinctrl@1f02c00: interrupts already {want_irq}, leaving as-is")
    # Also fix pmic@3a3 which shares the same r_intc parent: its interrupts was
    # 2 cells <0 0x08> but r_intc now expects 3 cells <0 0x08 0x04>. Without this
    # axp8xx fails "cannot allocate resources" and vcc-wifi never comes up.
    cur_pmic_irq = fdtget_ints(dtb_path, "/soc/rsb@1f03400/pmic@3a3", "interrupts")
    want_pmic_irq = [0x0, 0x08, 0x4]
    if cur_pmic_irq != want_pmic_irq:
        print(f"[gen_config] /soc/rsb@1f03400/pmic@3a3: interrupts {cur_pmic_irq} -> {want_pmic_irq} (r_intc 3 cells)")
        fdtput(dtb_path, "/soc/rsb@1f03400/pmic@3a3", "x", "interrupts", *want_pmic_irq, dry_run=dry_run)
    else:
        print(f"[gen_config] /soc/rsb@1f03400/pmic@3a3: interrupts already {want_pmic_irq}, leaving as-is")


def ensure_wifi_mmc1_clock_fix(dtb_path, dry_run):
    """/soc/mmc@1c10000 (mmc1, the AP6212/BCM43430's SDIO host) ships with
    `max-frequency = 150000000` in our base DTB. Upstream's real
    sun50i-a64-bananapi-m64.dts does not set max-frequency on this node at
    all -- ours almost certainly inherited it from whichever "aggressive"
    variant bananapi-min.dtb was forked from, not a deliberate choice for
    this controller.

    150MHz is a very fast clock to be asking a cheap embedded SDIO WiFi
    module to run CMD52 (command-only, no data phase) at, right after
    enumeration -- and this project already has a proven precedent for
    exactly this class of problem: mmc0 (the boot SD card) was unreliable
    at an aggressive clock and was fixed by reclocking down to 25MHz (see
    memory: sd-write-fixed-by-25mhz-reclock). Clamp mmc1 the same way
    rather than leaving it unconstrained like upstream -- 25MHz is SDIO's
    universal default-speed ceiling, every card supports it, and it's a
    known-good operating point on this exact SoC family.
    """
    want_freq = 25000000
    cur_freq = fdtget_ints(dtb_path, "/soc/mmc@1c10000", "max-frequency")
    if cur_freq != [want_freq]:
        print(f"[gen_config] /soc/mmc@1c10000: max-frequency {cur_freq} -> [{want_freq}] "
              f"(150MHz inherited from an aggressive base DTB, not deliberate for WiFi SDIO)")
        fdtput(dtb_path, "/soc/mmc@1c10000", "x", "max-frequency", want_freq, dry_run=dry_run)
    else:
        print(f"[gen_config] /soc/mmc@1c10000: max-frequency already {want_freq}, leaving as-is")


AUDIO_CPU_PHANDLE = 0xb0   # free: the DTB's highest in use is 0xae


def ensure_audio_simple_card_fix(dtb_path, dry_run):
    """Restate /sound in the single-link form FreeBSD's audio_soc.c can read.

    The DTB inherits upstream Linux's MULTI-link simple-audio-card layout:

        sound {
            simple-audio-card,dai-link@0 {
                format = "i2s"; frame-master; bitclock-master; mclk-fs;
                cpu   { sound-dai = <&dai>;   };
                codec { sound-dai = <&codec 0>; };
            };
        };

    sys/dev/sound/fdt/audio_soc.c does not implement that binding -- it says
    so in as many words (`/* TODO: handle multi-link nodes */`) and then looks
    for children named literally "simple-audio-card,cpu" and
    "simple-audio-card,codec" directly under /sound, with the format,
    mclk-fs and the two master phandles as properties of /sound itself.
    Finding neither, it gives up with "pcm0: cpu node is missing" -- which is
    all you get: pcm0 still attaches, so devinfo looks healthy while
    /dev/sndstat stays empty.

    So write the single-link form alongside the existing one. Both bindings
    are legitimate DT; the multi-link subnode is left untouched so that a
    kernel which later grows multi-link support still sees what it expects.

    Note the codec's sound-dai is TWO cells upstream (<&codec 0>) because that
    codec node advertises #sound-dai-cells = 1. audio_soc.c reads a single
    cell with OF_getencprop(..., sizeof(xref)), so the phandle alone is what
    goes here.

    The real fix belongs in audio_soc.c -- teaching it the multi-link binding
    would serve every board whose DTS uses it, which is most of them. This is
    the DTB-side workaround that gets sound out of the jack today.
    """
    dts_text = dtb_to_dts_text(dtb_path)
    if 'simple-audio-card,cpu {' in dts_text:
        print("[gen_config] /sound: single-link form already present, "
              "leaving as-is")
        return

    cpu_dai = parse_node_cells(dts_text, "cpu", "sound-dai")
    codec_dai = parse_node_cells(dts_text, "codec", "sound-dai")
    if not cpu_dai or not codec_dai:
        print("[gen_config] /sound: could not read the dai-link phandles, "
              "skipping the audio fix", file=sys.stderr)
        return

    print(f"[gen_config] /sound: adding single-link simple-audio-card form "
          f"(cpu dai={hex(cpu_dai[0])}, codec dai={hex(codec_dai[0])})")

    fdtput(dtb_path, "/sound/simple-audio-card,cpu", "x", "sound-dai",
           hex(cpu_dai[0]), dry_run=dry_run)
    fdtput(dtb_path, "/sound/simple-audio-card,cpu", "x", "phandle",
           hex(AUDIO_CPU_PHANDLE), dry_run=dry_run)
    fdtput(dtb_path, "/sound/simple-audio-card,codec", "x", "sound-dai",
           hex(codec_dai[0]), dry_run=dry_run)

    fdtput(dtb_path, "/sound", "s", "simple-audio-card,format", "i2s",
           dry_run=dry_run)
    fdtput(dtb_path, "/sound", "x", "simple-audio-card,mclk-fs", "0x80",
           dry_run=dry_run)
    fdtput(dtb_path, "/sound", "x", "simple-audio-card,frame-master",
           hex(AUDIO_CPU_PHANDLE), dry_run=dry_run)
    fdtput(dtb_path, "/sound", "x", "simple-audio-card,bitclock-master",
           hex(AUDIO_CPU_PHANDLE), dry_run=dry_run)


def ensure_wifi_pwrseq_fix(dtb_path, dry_run):
    """Board-free fix for the WiFi SDIO chip never answering CMD5.

    /pwrseq (mmc-pwrseq-simple) drives WL_REG_ON on PL2, but the AP6212's
    BCM43430 also needs its 32.768 kHz LPO, which on this board is the SoC's
    X32KFOUT pad -- rtc@1f00000's CLK_OSC32K_FANOUT (clock index 1, the
    "osc32k-out" name in its clock-output-names). Upstream's
    sun50i-a64-bananapi-m64.dts wires it via
    `clocks = <&rtc CLK_OSC32K_FANOUT>; clock-names = "ext_clock"`, and the
    flattened DTB we ship dropped both. Without the clock the chip never
    leaves its internal reset, so the probe gets CMD0 out and then stops.

    `post-power-on-delay-ms` is not in upstream's DTS either, but FreeBSD's
    mmc_pwrseq.c only delays between deasserting reset and the first command
    if the property is present, and the BCM43430 needs ~150 ms there.

    BLOCKED, MEASURED ON HARDWARE 2026-09-19/20: the `clocks`/`clock-names`
    half is NOT applied yet, deliberately. Turning the fanout gate on (the
    bit at 0x1f00000+0x60) can make the guest panic ~5 s into boot with
    "Misaligned access from kernel space" in _thread_lock(), on a random
    CPU -- and because that register is in the always-on RTC domain the bit
    SURVIVES a watchdog reset, so every following boot panics too until it
    is cleared from U-Boot with `mw.l 0x1f00060 0`. That is what makes it
    dangerous to wire up speculatively: the failure is not "WiFi does not
    come up", it is "the board stops booting and the reason is invisible in
    the DTB".

    Isolated 2026-09-20 by toggling the two r_pio consumers one at a time,
    with the gate on across a reset each time:
      gate on, wifi@1 off, bluetooth off  -> boots clean
      gate on, wifi@1 ON,  bluetooth off  -> boots clean, reaches login
      gate on, bluetooth ON               -> panics
    So the gate alone is harmless; the panic belongs to the `bluetooth`
    node under serial@1c28400 (its LPO clock is the same fanout, and it
    holds PL4/PL5/PL6). Whoever enables the clocks half MUST disable
    bluetooth in the same change.

    Two more things have to line up before the clocks half can be enabled:
    the aw_rtc fanout-clknode port must be in the guest kernel (upstream
    FreeBSD registers only ids 0 and 2, never id 1, so "ext_clock" cannot
    resolve), and mmc_pwrseq_attach() returns ENXIO when a `clocks`
    property is present but unresolvable -- which takes the whole pwrseq
    node down and leaves WiFi worse off than with no clock at all.

    `post-power-on-delay-ms` is applied: it is harmless on its own and is
    needed regardless once the chip can answer.
    """
    # The clocks half stays OFF until the fanout-gate panic is root-caused
    # -- see this function's docstring. Both halves are asserted absent
    # rather than merely "not added", so a hand-edited DTB carrying them
    # gets healed back instead of quietly bricking the next boot.
    for prop in ("clocks", "clock-names"):
        if fdtget_str(dtb_path, "/pwrseq", prop) is not None or \
           fdtget_ints(dtb_path, "/pwrseq", prop) is not None:
            print(f"[gen_config] /pwrseq: REMOVING {prop} -- the rtc "
                  f"CLK_OSC32K_FANOUT gate panics the guest ~5s into boot "
                  f"and the bit survives reset (see ensure_wifi_pwrseq_fix)")
            if not dry_run:
                subprocess.run(["fdtput", "-d", str(dtb_path), "/pwrseq", prop],
                               check=False)

    # 200ms was the documented ~150ms-plus-margin figure, but live testing
    # 2026-09-20 showed the CAM mmc_xpt probe (CMD0/ACMD41) intermittently
    # gets zero response and silently gives up with no scbus/pass/sdiob
    # device at all -- on some boots, not others. That is the classic sign
    # of a marginal power/clock settle time, not a logic bug: 200ms is
    # sometimes not enough. Bumped to 500ms for real margin; still trivial
    # against boot time.
    WIFI_POWER_ON_DELAY_MS = 500
    cur_delay = fdtget_ints(dtb_path, "/pwrseq", "post-power-on-delay-ms")
    if cur_delay != [WIFI_POWER_ON_DELAY_MS]:
        print(f"[gen_config] /pwrseq: post-power-on-delay-ms {cur_delay} -> [{WIFI_POWER_ON_DELAY_MS}] "
              f"(200ms proved marginal -- intermittent silent CAM probe failures)")
        fdtput(dtb_path, "/pwrseq", "x", "post-power-on-delay-ms", WIFI_POWER_ON_DELAY_MS, dry_run=dry_run)
    else:
        print(f"[gen_config] /pwrseq: post-power-on-delay-ms already {WIFI_POWER_ON_DELAY_MS}, leaving as-is")


def remove_cpu_node(dtb_path, cpu_id, dry_run):
    node = f"cpu@{cpu_id}"
    path = f"/cpus/{node}"
    print(f"[gen_config] {node}: REMOVING (its feature is disabled — a node "
          f"without the build flag is the same mismatch as a flag without "
          f"the node)")
    if dry_run:
        return
    subprocess.run(["fdtput", "-r", str(dtb_path), path], check=True)


def reconcile_drift(features, dts_text, dtb_path, dry_run):
    """Both directions of the mismatch this script exists to prevent.

    An earlier version of this function only PRINTED a note here, on the
    reasoning that a leftover node might be "deliberate for a follow-up boot".
    That reasoning cost real board time on 2026-08-27: vcpu2 was armed, the
    guest stalled, and disarming it again left cpu@2 sitting in the DTB with
    VCPU2=0 — so FreeBSD would enumerate a third core, issue PSCI CPU_ON for
    affinity 2, and EL2 would refuse it. That is precisely the half-configured
    state the header of this file and board-config.xml both warn about, in the
    one direction the script had declined to actually fix. A note you have to
    notice is not a safeguard.

    So: remove it. The DTB is a generated artifact here, a backup is written
    above, and "I want that node" is spelled by enabling the feature."""
    for name, f in features.items():
        if f["needs_dtb_cpu"] and not f["enabled"]:
            node = f"cpu@{f['needs_dtb_cpu']}"
            if node_exists(dts_text, node):
                remove_cpu_node(dtb_path, f["needs_dtb_cpu"], dry_run)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xml", default=str(DEFAULT_XML))
    ap.add_argument("--dtb", default=str(DEFAULT_DTB))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--no-backup", action="store_true")
    args = ap.parse_args()

    features, devices, soc_nodes, silicon_sharing = load_config(args.xml)
    validate(features, devices, soc_nodes, silicon_sharing)
    cross_check_board_constants(devices, soc_nodes, features)
    derived_forbidden = derive_forbidden_from_sharing(silicon_sharing)
    print(f"[gen_config] silicon-sharing: {len(silicon_sharing)} groups, "
          f"{len(derived_forbidden)} derived-forbidden nodes cross-checked "
          f"against FORBIDDEN_DTB_NODES — OK")

    emit_makefile_fragment(features, args.dry_run)

    dtb_path = Path(args.dtb)
    if not dtb_path.exists():
        print(f"[gen_config] {dtb_path} not found — skipping DTB step "
              f"(config.mk was still written)", file=sys.stderr)
        return

    if not args.dry_run and not args.no_backup:
        backup = dtb_path.with_suffix(dtb_path.suffix + ".pre-genconfig")
        if not backup.exists():
            backup.write_bytes(dtb_path.read_bytes())
            print(f"[gen_config] backup: {backup}")

    dts_text = dtb_to_dts_text(dtb_path)
    reconcile_drift(features, dts_text, dtb_path, args.dry_run)
    if not args.dry_run:
        dts_text = dtb_to_dts_text(dtb_path)   # a removal invalidates it

    for name, f in features.items():
        if f["enabled"] and f["needs_dtb_cpu"]:
            ensure_cpu_node(dtb_path, dts_text, f["needs_dtb_cpu"], args.dry_run)

    if not args.dry_run:
        dts_text = dtb_to_dts_text(dtb_path)   # sees cpu nodes just added
    ensure_cpu_dvfs(dtb_path, dts_text,
                    features.get("guest_dvfs", {}).get("enabled", False),
                    args.dry_run)

    if not args.dry_run:
        dts_text = dtb_to_dts_text(dtb_path)
    ensure_pmu(dtb_path, dts_text,
               features.get("guest_pmu", {}).get("enabled", False), args.dry_run)

    # /memory tracks whichever feature declares a dtb-memory-size -- and, just as
    # importantly, gets put BACK when none is enabled. Leaving it wide after the
    # feature is switched off is the same one-directional bug this script had for
    # a stale cpu@N node, and it is worse here: a /memory node promising a GiB
    # that stage-2 no longer maps means the guest faults the moment it allocates
    # up there, which is exactly how it presented (translation fault, level 1,
    # IPA 0xBFFF0000) when the two halves were briefly out of step.
    want = None
    for name, f in features.items():
        if f["enabled"] and f["dtb_memory_size"]:
            want = f["dtb_memory_size"]
    ensure_memory_size(dtb_path, dts_text, want or DEFAULT_MEMORY_SIZE,
                       args.dry_run)

    # NOTE on ORDER, observed the same day: ensure_cpu_node() adds nodes with
    # `fdtput -p`, which PREPENDS. Arming vcpu2 therefore produced /cpus in the
    # order `cpu@2 cpu@1 cpu@0`, and FreeBSD duly enumerated "CPU 1 ...
    # affinity: 2" and "CPU 2 ... affinity: 1" -- its logical numbering is DTB
    # order, not MPIDR. Harmless for bring-up but genuinely confusing while
    # reading a console log, and the exact same prepend footgun that once
    # silently swapped vtbd0/vtbd1 (see the virtio ordering self-heal below,
    # which exists for that reason). Not self-healed for cpu nodes yet.

    for d in devices:
        if d["no_dtb_node"]:
            continue
        wanted = d["always"] or (d["enabled_by"] and features.get(d["enabled_by"], {}).get("enabled"))
        if wanted:
            ensure_virtio_node(dtb_path, dts_text, d, args.dry_run)

    reorder_virtio_nodes(dtb_path, args.dry_run)

    # Real silicon nodes gated on a dtb-only feature (board-config.xml's
    # <soc-nodes>) — always reconciled, both directions, same as everything
    # above: a node left "okay" after its feature is switched off again is
    # the same drift this whole script exists to prevent.
    for n in soc_nodes:
        f = features.get(n["feature"], {})
        ensure_node_status(dtb_path, n["path"], f.get("enabled", False), args.dry_run)

    ensure_rpio_fix(dtb_path, args.dry_run)
    ensure_wifi_mmc1_clock_fix(dtb_path, args.dry_run)
    ensure_wifi_pwrseq_fix(dtb_path, args.dry_run)
    ensure_audio_simple_card_fix(dtb_path, args.dry_run)

    print("[gen_config] done")


if __name__ == "__main__":
    main()
