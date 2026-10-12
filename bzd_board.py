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
import os


# ── USB identity: U-Boot download gadget vs. the HV's own console gadget ───
# The udev rule maps BOTH to the same stable /dev/ttyCHIMP symlink, so vid:pid
# is the only reliable way to tell which one is actually enumerated right now.
UBOOT_VID, UBOOT_PID = "1f3a", "efe8"        # U-Boot download gadget
HVCON_VID, HVCON_PID = "1d6b", "0010"        # HV's bzdOS USB console gadget
UBOOT_VIDPID = f"{UBOOT_VID}:{UBOOT_PID}"    # "1f3a:efe8" (supervise.py's form)
HVCON_VIDPID = f"{HVCON_VID}:{HVCON_PID}"    # "1d6b:0010"

# The board's OTG port on this host, as a USB sysfs node.
#
# This used to be the whole story, and it was wrong: a hard-coded port means
# that the moment the board is plugged into a different physical socket (or the
# host renumbers its root hubs across a reboot), every caller of
# usb_vidpid_str() reads nothing and concludes the board has VANISHED. Found
# live on 2026-08-10: the board was on 2-4 while this constant said 1-4, and
# soak72.py failed instantly with "board-off-usb — the ONE case with no remote
# recovery path" against a board that was sitting there perfectly healthy. A
# false GONE is worse than no check at all, because it aborts the very harnesses
# that exist to run unattended.
#
# So the constant is now only the FIRST GUESS. usb_find_node() below resolves
# the board by its vid:pid, which is the identity that actually matters.
USB_NODE_DEFAULT = "/sys/bus/usb/devices/1-4"
USB_NODE = USB_NODE_DEFAULT

# Stable udev symlink to the board's enumerated ttyACMn (survives renumbering)
# — this is what U-Boot's CDC-ACM download gadget shows up as.
CHIMP_TTY = "/dev/ttyCHIMP"

# The guest's own USB-ACM console (usbacm.c bridges this to the guest UART RX/
# TX rings) — used both for reading the guest's interactive console (auto
# mountroot answer) and for sending the break-glass reset magic sequence.
GUEST_ACM_TTY = "/dev/ttyACM0"

# ── EMAC raw-Ethernet debug console (dbgmon / repl-cmd protocol) ───────────
# IFACE (the interface every raw-socket tool binds) comes from board.conf
# below: the wire (`iface`, br0) for transport=l2, or the tap that
# bzd_udplink.py bridges to the board's UDP debug port for transport=udp.
ETYPE = 0x88B5
BCAST = b"\xff" * 6

# ── the hypervisor's Ethernet address ──────────────────────────────────────
# The HV no longer has a fixed MAC. emac.c derives it from the A64 SID exactly
# as U-Boot derives eth0's `ethaddr` (02:<sid0 low byte>:<crc32(sid[1..3])>),
# so it is stable per board, and gives the guest a different one (first octet
# | 0x10, U-Boot's eth1 rule) through VIRTIO_NET_F_MAC. This board:
#   HV / U-Boot eth0  02:ba:fb:3b:f5:8c      guest vtnet0  12:ba:fb:3b:f5:8c
# Until 2026-10-12 both were 02:bd:05:00:00:01; an image older than that, or a
# board whose SID reads 0, still uses it (LEGACY_BOARD_MAC).
#
# Tools do not hard-code the new address: hvauth.Channel learns it from the
# board's authenticated handshake answer (the CHAL that carries a valid proof)
# and records it in BOARD_MAC_FILE, so listeners that start before any
# handshake (console followers, coredump capture) know whom to listen to.
# $BZDK_BOARD_MAC overrides everything.
LEGACY_BOARD_MAC = bytes.fromhex("02bd05000001")   # 02:bd:05:00:00:01
BOARD_MAC = LEGACY_BOARD_MAC     # kept for old imports; use board_mac()
BOARD_MAC_FILE = os.path.expanduser("~/.config/bzdk/board.mac")
_board_mac = None


def _parse_mac(txt):
    b = bytes.fromhex(txt.strip().replace(":", "").replace("-", ""))
    if len(b) != 6:
        raise ValueError(f"not a MAC: {txt!r}")
    return b


def board_mac():
    """The HV's MAC as bytes: $BZDK_BOARD_MAC, else the last one a verified
    handshake reported (BOARD_MAC_FILE), else LEGACY_BOARD_MAC. Reads the
    file on the first call only (no import-time side effects)."""
    global _board_mac
    env = os.environ.get("BZDK_BOARD_MAC")
    if env:
        return _parse_mac(env)
    if _board_mac is None:
        try:
            _board_mac = _parse_mac(open(BOARD_MAC_FILE).read())
        except (OSError, ValueError):
            _board_mac = LEGACY_BOARD_MAC
    return _board_mac


def remember_board_mac(mac):
    """Record the HV MAC a verified handshake came from."""
    global _board_mac
    mac = bytes(mac)
    if len(mac) != 6 or mac == board_mac():
        _board_mac = mac if len(mac) == 6 else _board_mac
        return
    _board_mac = mac
    try:
        os.makedirs(os.path.dirname(BOARD_MAC_FILE), exist_ok=True)
        tmp = BOARD_MAC_FILE + ".tmp"
        with open(tmp, "w") as f:
            f.write(mac.hex(":") + "\n")
        os.replace(tmp, BOARD_MAC_FILE)
    except OSError:
        pass


def is_board_mac(src):
    """True if an Ethernet source address is the hypervisor's. The legacy
    address is accepted too, so a rollback to an older image stays
    debuggable; nothing else uses it any more (the guest has its own MAC)."""
    src = bytes(src)
    return src == board_mac() or src == LEGACY_BOARD_MAC

# ── board network config: ~/.config/bzdk/board.conf ──────────────────────
# The ONE host-side place for the board's addresses and the debug transport.
# Plain `key = value` lines, `#` comments; missing keys fall back to the
# defaults below (the 192.168.88.0/24 setup); $BZDK_BOARD_CONF names another
# file, $BZDK_TRANSPORT overrides `transport`. The board's side of the same
# facts lives in U-Boot's env (ipaddr/netmask/gatewayip/serverip, which the
# HV also reads at boot -- dbgip.h) and the guest's /etc/rc.conf;
# tools/subnet/cutover.py changes all three together.
#
#   transport  l2 | udp     raw Ethernet on `iface`, or UDP via `tap`
#   iface      br0          host interface on the board's segment (l2)
#   tap        bzd0         tap served by bzd_udplink.py (udp)
#   board_ip   HV + U-Boot address          dbg_port  HV UDP debug port
#   guest_ip   FreeBSD guest address        server_ip this host (TFTP, udplink)
#   gateway / netmask / dns                 the board's subnet
#
# Values are read lazily on first use (module __getattr__), so importing
# this module still opens nothing.
BOARD_CONF_FILE = os.path.expanduser("~/.config/bzdk/board.conf")
BOARD_CONF_DEFAULTS = {
    "transport": "l2",
    "iface": "br0",
    "tap": "bzd0",
    "board_ip": "192.168.88.7",
    "dbg_port": "34997",
    "guest_ip": "192.168.88.82",
    "server_ip": "192.168.88.2",
    "gateway": "192.168.88.1",
    "netmask": "255.255.255.0",
    "dns": "192.168.88.1",
}
_conf = None


def parse_conf(text):
    d = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        k, _, v = line.partition("=")
        d[k.strip()] = v.strip()
    return d


def conf_path():
    return os.environ.get("BZDK_BOARD_CONF") or BOARD_CONF_FILE


def conf():
    """board.conf merged over the defaults (dict of strings)."""
    global _conf
    if _conf is None:
        c = dict(BOARD_CONF_DEFAULTS)
        try:
            c.update(parse_conf(open(conf_path()).read()))
        except OSError:
            pass
        if os.environ.get("BZDK_TRANSPORT"):
            c["transport"] = os.environ["BZDK_TRANSPORT"]
        if c["transport"] not in ("l2", "udp"):
            raise ValueError(f"{conf_path()}: transport must be l2 or udp")
        _conf = c
    return _conf


def debug_iface():
    """Interface the raw-socket debug tools bind for the configured transport."""
    c = conf()
    return c["tap"] if c["transport"] == "udp" else c["iface"]


def guest_ssh_target():
    return "root@" + conf()["guest_ip"]


GUEST_SSH_KEY = "/root/.ssh/chimp_ed25519"


def guest_ssh_argv(connect_timeout=10):
    """ssh argv prefix for the guest (append the remote command)."""
    return ["ssh", "-o", f"ConnectTimeout={connect_timeout}", "-o",
            "BatchMode=yes", "-i", GUEST_SSH_KEY, guest_ssh_target()]


_LAZY = {
    "IFACE": debug_iface,
    "WIRE_IFACE": lambda: conf()["iface"],
    "TRANSPORT": lambda: conf()["transport"],
    "BOARD_IP": lambda: conf()["board_ip"],      # U-Boot ipaddr == HV address
    "SRV_IP": lambda: conf()["server_ip"],       # U-Boot serverip (this host)
    "GUEST_IP": lambda: conf()["guest_ip"],
    "GUEST": guest_ssh_target,
    "DBG_PORT": lambda: int(conf()["dbg_port"]),
    "GATEWAY": lambda: conf()["gateway"],
    "NETMASK": lambda: conf()["netmask"],
}


def __getattr__(name):
    if name in _LAZY:
        return _LAZY[name]()
    raise AttributeError(f"module 'bzd_board' has no attribute {name!r}")

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

WDT_DEBUG_HOLD_PA = 0x50095000   # wdt.c's debug-core reset gate (== hv_addrmap.h's
                                  # HVMAP_WDT_DEBUG_HOLD). A single `w` to this
                                  # address stops CPU1's unconditional WDOG pet
                                  # (wdt_debug_kick()/wdt_pet(), see wdt.c) so the
                                  # HW WDOG fires within its <=16s window --
                                  # fixed address on purpose, so hvdbg.py's
                                  # wdt_reset() never needs an nm-resolved symbol
                                  # (see that function's docstring).

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


def usb_find_node():
    """Locate the board's USB sysfs node by identity rather than by position.

    Scans /sys/bus/usb/devices for a device whose idVendor:idProduct is either
    the U-Boot download gadget or the HV's own console gadget (the two identities
    the board can present). Returns the node path, or None if neither is
    enumerated — which is the only reading that should ever be taken as "the
    board is off the bus".

    Prefers USB_NODE (last known / configured position) when it still matches,
    so a stable setup keeps its stable answer and does no scanning.
    """
    import glob

    want = {UBOOT_VIDPID, HVCON_VIDPID}

    if usb_vidpid_str(USB_NODE) in want:
        return USB_NODE

    for node in sorted(glob.glob("/sys/bus/usb/devices/*-*")):
        if ":" in node.rsplit("/", 1)[-1]:
            continue            # interface, not a device
        if usb_vidpid_str(node) in want:
            return node
    return None


def usb_board_vidpid():
    """The board's current vid:pid wherever it happens to be plugged in, or
    None if it is genuinely not enumerated. This is what callers asking "is the
    board on the bus, and in which identity?" should use -- NOT
    usb_vidpid_str(USB_NODE), which answers a question about a socket."""
    node = usb_find_node()
    return usb_vidpid_str(node) if node else None


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
