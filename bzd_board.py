#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""
bzd_board.py — single source of truth for the host-side Python tooling's
board constants: USB VID:PIDs, the USB sysfs port node, the EMAC console
iface/MAC, guest TTY paths, and the fixed "hv-scratch" breadcrumb addresses.

WHY THIS FILE EXISTS
--------------------
reliable_load.py, hvdbg.py, chimpd.py, supervise.py, soak.py and
loady_over_acm.py each re-declared the same board literals (and, in two
places, re-implemented the same little "read a USB vid/pid off sysfs"
helper). That is the exact class of bug that hit the C side on 2026-07-24
(see hv_addrmap.h's header comment: a stale/duplicated literal address
silently aliased two unrelated breadcrumb words). This module gives the
Python tooling the same fix the C side got: one declaration per constant,
imported everywhere it's needed.

This module has ZERO import-time side effects (no sockets, no file opens,
no board access) — every tool here must stay independently runnable, and
importing this module must never itself talk to hardware.

ADDRESS MAP
-----------
The fixed DRAM "hv-scratch" window (DTB-reserved hv-scratch@0x50000000) is
authoritatively documented on the C side in hv_addrmap.h (that file owns the
compile-time non-overlap proof for the densest sub-block, 0x50020000..). The
constants below MIRROR the values relevant to host tooling; they are not
generated from hv_addrmap.h (no build step wires C headers into Python here)
so if a C-side address in that file ever moves, these must be updated by
hand to match. Only the 0x50020000 I/O-storage block is __Static_assert__
non-overlap-checked on the C side today; the rest of the map (including
everything below) is comment-documented only, on both sides.

  0x50000000..0x50000eff  early boot / GIC / misc breadcrumbs (GICT @0x800)
  0x50000f00..0x50000f1f  vconsole ring HEADER only (magic/total/faults/ver/
                           base/size) — the 64 KiB data buffer used to follow
                           it here and ran to 0x50010f0f, burying the vgic,
                           gtrace, first-fault, single-step and HDMI windows;
                           moved to 0x50040000 on 2026-08-01
  0x50012000..            flightrec "FLTR" ring
  0x50040000..0x5004ffff  vconsole 64 KiB capture buffer (v2)
  0x50020000..0x50020fff  virtio-blk / eMMC / SD I/O storage (HVMAP_VBLK_BC
                           in hv_addrmap.h == VBLK_BC_PA below == the "VBK1"
                           breadcrumb reliable_load.py's --expect-vbk checks)

A DISCREPANCY FOUND WHILE CENTRALIZING (see the giant comment at BC_GICT_*
below, not silently reconciled — read it before trusting either value):
chimpd.py's own module docstring says its GICT breadcrumb lives at
0x50000800 (the same address hvdbg.py's HV.gict() reads), but the constant
chimpd.py actually POLLS with is 0x00018200 — a different address in the
low-SRAM breadcrumb block (0x00018xxx), not the DRAM hv-scratch block. Both
values are preserved below, unrenamed in meaning, exactly as each tool used
them — this module does not silently pick one.
"""

# ── USB identity: U-Boot download gadget vs. the HV's own console gadget ───
# The udev rule maps BOTH to the same stable /dev/ttyCHIMP symlink, so vid:pid
# is the only reliable way to tell which one is actually enumerated right now.
UBOOT_VID, UBOOT_PID = "1f3a", "efe8"        # U-Boot download gadget
HVCON_VID, HVCON_PID = "1d6b", "0010"        # HV's bzdOS USB console gadget
UBOOT_VIDPID = f"{UBOOT_VID}:{UBOOT_PID}"    # "1f3a:efe8" (supervise.py's form)
HVCON_VIDPID = f"{HVCON_VID}:{HVCON_PID}"    # "1d6b:0010"

# The board's OTG port on this host, as a USB sysfs node.
USB_NODE = "/sys/bus/usb/devices/1-4"

# Stable udev symlink to the board's enumerated ttyACMn (survives renumbering)
# — this is what U-Boot's CDC-ACM download gadget shows up as.
CHIMP_TTY = "/dev/ttyCHIMP"

# The guest's own USB-ACM console (usbacm.c bridges this to the guest UART RX/
# TX rings) — used both for reading the guest's interactive console (auto
# mountroot answer) and for sending the break-glass reset magic sequence.
GUEST_ACM_TTY = "/dev/ttyACM0"

# ── EMAC raw-Ethernet debug console (dbgmon / repl-cmd protocol) ───────────
IFACE = "br0"
ETYPE = 0x88B5
BOARD_MAC = bytes.fromhex("02bd05000001")   # 02:bd:05:00:00:01
BCAST = b"\xff" * 6

# ── host <-> board TFTP addressing (U-Boot's serverip/ipaddr) ──────────────
BOARD_IP = "192.168.88.7"
SRV_IP = "192.168.88.2"

# ── guest root filesystem (kload.c's vfs.root.mountfrom kenv) ─────────────
ROOT_MOUNTFROM = "ufs:/dev/vtbd0p3"

# ── A64 watchdog MMIO (see reboot.c) ────────────────────────────────────────
WDOG_CFG_PA = 0x01c20cb4
WDOG_MODE_PA = 0x01c20cb8
WDOG_CTRL_PA = 0x01c20cb0
# The exact (addr, value) triple every tool writes, in order, to arm a
# whole-system reset. Reused as-is (list of tuples) by reliable_load.py,
# hvdbg.py and chimpd.py's bare-WDOG fallback paths.
WDOG_ARM_SEQUENCE = ((WDOG_CFG_PA, 1), (WDOG_MODE_PA, 0x21), (WDOG_CTRL_PA, 0x14af))

# MUSB (USB-OTG) controller base — used to drop the D+/D- pull-up for a clean
# USB disconnect before arming the WDOG (see reboot_clean()/wdt_reset()'s
# "why" comments in hvdbg.py / reliable_load.py: skipping this step can wedge
# the U-Boot gadget with no remote recovery — project memory
# wdt-reset-needs-usb-disconnect.md).
MUSB_BASE = 0x01c19000

# ── hv-scratch breadcrumb addresses (DRAM, board-physical) ──────────────────
VCONSOLE_HDR_PA = 0x50000f00     # vconsole ring header: magic/total/faults/ver/base/size
# Fallbacks ONLY. The header is self-describing from layout v2 on (word[3]=ver,
# word[4]=buffer base, word[5]=size) — read it, do not assume. These constants
# said 4 KiB while the firmware wrote 64 KiB at 0x50000f10, so the host never
# read far enough to see that the ring had swallowed five other subsystems'
# breadcrumb windows, and its wrap arithmetic (total_bytes % 4 KiB over a 64 KiB
# ring) produced self-contradictory reconstructions. Buffer relocated to
# 0x50040000 on 2026-08-01.
VCONSOLE_RING_PA = 0x50040000    # vconsole ring data start (v2)
VCONSOLE_RING_SZ = 0x10000       # 64 KiB

GICT_BC_DRAM_PA = 0x50000800     # GICT breadcrumb per hvdbg.py's HV.gict() and
                                  # per chimpd.py's OWN module docstring (see the
                                  # discrepancy note in this file's header comment)

FLTR_BASE = 0x50012000           # flight-recorder "FLTR" ring (see flightrec.h)

VBLK_BC_PA = 0x50020000          # virtio-blk breadcrumbs (== hv_addrmap.h's
                                  # HVMAP_VBLK_BC); word[0] == "VBK1" magic
                                  # (0x56424b31) once the virtio-blk build ran
VBLK_MAGIC = 0x56424b31           # "VBK1" little-endian

# ── low-SRAM breadcrumb block (0x00018xxx) — chimpd.py-only today ─────────
# Distinct from the DRAM hv-scratch block above. See the discrepancy note at
# the top of this file: chimpd.py actually polls GICT_BC_SRAM_PA (not
# GICT_BC_DRAM_PA) despite its own docstring saying otherwise.
BC_EXC_SRAM_PA = 0x00018100      # exception record
GICT_BC_SRAM_PA = 0x00018200     # gic_timer (tick + IAR/EOI diagnostic)
VGIC_BC_SRAM_PA = 0x00018000     # VGIC breadcrumb window (see el2_exc.c)


# ── shared USB vid:pid sysfs reader ─────────────────────────────────────────
def read_sysfs(path):
    """Read and strip a single-line sysfs attribute file. Returns None on any
    error (missing node, permission, transient unplug, decode error, ...) —
    deliberately broad, mirroring reliable_load.py's original `_rd()`."""
    try:
        return open(path).read().strip()
    except Exception:
        return None


def usb_vidpid(node=USB_NODE):
    """Return (vid, pid) strings (each independently None on read failure).
    Matches reliable_load.py's original usb_vidpid()."""
    return read_sysfs(f"{node}/idVendor"), read_sysfs(f"{node}/idProduct")


def usb_vidpid_str(node=USB_NODE):
    """Return "vid:pid", or None if either half couldn't be read. Matches
    supervise.py's original vidpid() in outcome (None whenever either sysfs
    read fails) — built on the shared per-field reader above.

    NOTE (discrepancy found while centralizing, worth flagging): the original
    supervise.py caught only OSError around both reads together, so a
    non-OSError exception (never observed in practice against a sysfs int
    attribute, but not impossible — e.g. a decode error) would have propagated
    out of vidpid() and crashed supervise.py's poll loop. This shared
    implementation catches broadly per field (like reliable_load.py's helper
    always did) and returns None in that case instead. This is a strictly
    more defensive behavior change for an edge case that has never been
    observed live; flagged here rather than silently carried forward."""
    v, p = usb_vidpid(node)
    if v is None or p is None:
        return None
    return f"{v}:{p}"
