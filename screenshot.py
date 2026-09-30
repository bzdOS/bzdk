#!/usr/bin/env python3
"""Take a RAW screenshot straight out of the hypervisor, over the debug channel.

WHY THIS EXISTS

Everything about the display has, until now, been verified either by asking the
user to look at the monitor or by sampling a handful of words from EL2 and
reasoning about the numbers. Both are weak: the first does not scale and cannot
be recorded, and the second answers "are there non-background pixels here" but
never "what is actually on screen".

This reads the pixels themselves. Nothing in the guest is involved -- EL2 reads
guest DRAM and the framebuffer directly -- so a screenshot is obtainable while
the guest is wedged, panicked, or lying about what it drew.

WHAT CAN BE CAPTURED

  --what hud      the hypervisor's own framebuffer (the HUD), HDMI_FB_BASE
  --what window   the guest window layer, at whatever address the DE2 layer is
                  CURRENTLY fetching -- read from the mixer's own layer
                  register, NOT from a breadcrumb (see live_layer_addr() for
                  why that distinction cost a wrong screenshot) -- so this
                  follows a client's page flips
  --what both     the composition, which is what the panel shows: every
                  enabled plane (HUD, guest window, both overlays, the video
                  plane) recomposed from the mixer's own registers -- see
                  read_planes()/compose()

The two are separate DE2 layers composited by hardware; there is no single
buffer in memory holding the composed image, which is exactly why `both` has to
paste rather than read.

COST, STATED UP FRONT

By default this uses fbdump_recv.fetch(), which rides the fire-and-forget raw-
Ethernet path (fbdump.c/fbdump.h, ethertype 0x88B9) instead of the debug
channel's hex `r` command -- see fbdump_recv.py's docstring for the framing
and the (expected, not yet board-measured) throughput: on the order of ~1.5s
for a 1.2 MiB guest window, ~9s for a full 8 MiB 1080p HUD frame.

`--slow` switches to the ORIGINAL transport: hvdbg.read_words(), 512 words per
command at roughly one RTT each, so a full 1920x1080 HUD takes minutes. This
is kept as the explicit fallback for a host that cannot open an AF_PACKET raw
socket (e.g. no CAP_NET_RAW) -- the one case where the fast path is simply
unavailable, not just slower. --step N reads every Nth row (and every Nth
column) for a quick look under either transport; the geometry in the PNG
stays correct because the image is written at the reduced size.

All geometry is parsed out of hdmi.h rather than hardcoded here: a second copy
of 1120x276 is a second thing to drift.
"""
import argparse
import os
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hvdbg
import fbdump_recv

HDMI_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hdmi.h")
HDMI_BC = 0x50011800          # HVMAP_LOW_HDMI_BC; word 11 = last layer address


def defines(path, names):
    """Pull integer #defines out of a C header. Returns {name: int}."""
    out = {}
    want = set(names)
    pat = re.compile(r"^#define\s+(\w+)\s+\(?\s*(0[xX][0-9a-fA-F]+|\d+)")
    for line in open(path):
        m = pat.match(line)
        if m and m.group(1) in want:
            out[m.group(1)] = int(m.group(2), 0)
    return out


def png(path, w, h, rows):
    """Write RGB rows (each bytes of length 3*w) as a PNG. zlib is stdlib, so
    this needs nothing installed on the host."""
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", hdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


# Pixel format note shared by both transports below: x8r8g8b8, the 32-bit
# word is 0x00RRGGBB, so red is bits 23:16. Verified against a known clear
# colour (0.04, 0.05, 0.10 read back as 0xff0a0d1a = R 10, G 13, B 26) rather
# than assumed from the name.
def _pixel_bytes(v):
    return bytes(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))


def grab_slow(hv, base, width, height, stride, step, label):
    """--slow transport: hvdbg.read_words(), one command per row -- see
    module docstring for why this is minutes for a full frame."""
    rows = []
    ys = range(0, height, step)
    t0 = time.time()
    for i, y in enumerate(ys):
        words = hv.read_words(base + y * stride, width)
        if not words:
            print("  %s: no reply at row %d -- channel down?" % (label, y))
            return None, 0
        px = bytearray()
        for x in range(0, width, step):
            v = words[x] if x < len(words) else 0
            px += _pixel_bytes(v)
        rows.append(bytes(px))
        if i % 16 == 0:
            done = i + 1
            rate = done / max(0.001, time.time() - t0)
            sys.stdout.write("\r  %s: row %d/%d (%.1f rows/s, ~%ds left)   "
                             % (label, done, len(ys), rate,
                                int((len(ys) - done) / max(0.1, rate))))
            sys.stdout.flush()
    sys.stdout.write("\r" + " " * 72 + "\r")
    return rows, len(range(0, width, step))


def grab_fast(hv, iface, base, width, height, stride, step, label):
    """Default transport: fbdump_recv.fetch(), one raw-Ethernet bulk pass
    (plus host-side retries for any gaps) instead of one command per row.
    Fetches the whole [base, base+stride*height) region in one call, then
    slices rows/columns out of the returned bytes locally -- no further
    round trips regardless of `step`."""
    total_len = stride * height
    print("  %s: fetching %d bytes via fbdump (0x88B9)..." % (label, total_len))
    t0 = time.time()
    try:
        data, missing = fbdump_recv.fetch(base, total_len, hv=hv, iface=iface)
    except (PermissionError, OSError) as e:
        print("  %s: could not open the raw fbdump socket (%s) -- retry with "
              "--slow" % (label, e))
        return None, 0
    dt = time.time() - t0
    if missing:
        mb = sum(e - s for s, e in missing)
        print("  %s: %d/%d bytes MISSING after retries (%d gaps) -- those "
              "pixels will read as black, not real data" %
              (label, mb, total_len, len(missing)))
    else:
        print("  %s: %d bytes in %.1fs (%.0f KB/s)" %
              (label, total_len, dt, total_len / 1024.0 / max(dt, 0.001)))

    rows = []
    for y in range(0, height, step):
        row_off = y * stride
        px = bytearray()
        for x in range(0, width, step):
            off = row_off + x * 4
            v = struct.unpack_from("<I", data, off)[0] if off + 4 <= len(data) else 0
            px += _pixel_bytes(v)
        rows.append(bytes(px))
    return rows, len(range(0, width, step))


def grab(hv, iface, base, width, height, stride, step, label, use_slow):
    if use_slow:
        return grab_slow(hv, base, width, height, stride, step, label)
    return grab_fast(hv, iface, base, width, height, stride, step, label)


"""The DE2 mixer's own layer registers -- the authoritative answer to "what is
the display engine fetching RIGHT NOW".

Derived from hdmi.c's own #defines: DE2_BASE 0x01000000, DE2_MUX1_BASE = +0x200000,
DE_CHAN_REGS_BASE = +0x2000, DE_CHAN_SZ = 0x1000, UI1 is channel 1, and layer n's
TOP_LADDR sits at +0x20*n + 0x10 (DE_UI1_L_TOP_LADDR).

This replaced trusting the HDMI breadcrumb, which cost a wrong screenshot the
first time this tool ran: hv-scratch DRAM survives a warm reset, so word 11 still
held a page-flip address from the PREVIOUS boot generation while the layer had
been re-pointed at the hypervisor's own buffer. The capture succeeded, reported
"100% non-black" and was of unrelated memory. A breadcrumb says what somebody
wrote once; a device register says what the device is doing.
"""
DE_UI1_LAYER0_LADDR = 0x01203010
DE_LAYER_STRIDE = 0x20
# DE2_MUX1_BASE + 0x0C -- the mixer's global size, ((h-1) << 16) | (w-1).
DE_GLB_SIZE = 0x0120000C


def live_mode(hv, fallback_w, fallback_h):
    """Return (width, height, provenance) for the screen, read from the MIXER.

    Do NOT trust hdmi.h for this. Its HDMI_MODE_HACTIVE/VACTIVE are defined
    TWICE -- once under `#if defined(HDMI_MODE_1080P)` and once in the `#else` --
    so a text scrape cannot tell which branch the running image was built with.
    It silently returned 1280x720 for a 1080p build, which is not a cosmetic
    error: the row stride would be wrong by 2560 bytes and every row after the
    first would be read from the middle of its predecessor.
    """
    v = hv.read_words(DE_GLB_SIZE, 1)
    if v and v[0] not in (0, 0xFFFFFFFF):
        w = (v[0] & 0xFFFF) + 1
        h = ((v[0] >> 16) & 0xFFFF) + 1
        if 320 <= w <= 4096 and 240 <= h <= 2160:
            return (w, h, "live, from DE_GLB_SIZE")
    return (fallback_w, fallback_h, "hdmi.h fallback -- mixer read failed")


def live_layer_addr(hv, fb_base, guestwin_default, layer):
    """Return (address, provenance, stale_breadcrumb_or_None).

    SELF-CHECKING: layer 0 of the same channel is the HUD, so its TOP_LADDR must
    read back as HDMI_FB_BASE. If it does not, the register map above is wrong
    for this build and every other value read through it is worthless -- so say
    so and fall back rather than screenshot a guess.
    """
    bc = hv.read_words(HDMI_BC, 14) or []
    bc11 = bc[11] if len(bc) > 11 and bc[11] not in (0, 0xFFFFFFFF) else None

    l0 = hv.read_words(DE_UI1_LAYER0_LADDR, 1)
    if not l0 or l0[0] != fb_base:
        got = ("%#x" % l0[0]) if l0 else "no reply"
        print("  WARNING: DE2 layer 0 reads %s, expected the HUD at %#x -- the "
              "register map does not fit this build, not trusting it" %
              (got, fb_base))
        if bc11:
            return (bc11, "breadcrumb fallback, register map rejected", None)
        return (guestwin_default, "hdmi.h default, register map rejected", None)

    lv = hv.read_words(DE_UI1_LAYER0_LADDR + DE_LAYER_STRIDE * layer, 1)
    if not lv or not (0x40000000 <= lv[0] < 0x80000000):
        got = ("%#x" % lv[0]) if lv else "no reply"
        print("  WARNING: DE2 layer %d reads %s, which is not guest DRAM" %
              (layer, got))
        return (guestwin_default, "hdmi.h default, layer register implausible",
                None)

    stale = bc11 if (bc11 is not None and bc11 != lv[0]) else None
    return (lv[0], "live, from the DE2 layer %d register" % layer, stale)


# ---------------------------------------------------------------------------
# `--what both`: the whole screen, composed from the mixer's own registers.
#
# Every plane the hypervisor can put on the panel, read back from DE2 rather
# than from hdmi.h or a breadcrumb: UI channel 1 layers 0..3 (HUD, guest
# window, the two overlays -- hdmi_overlay_set()) and the VI channel (the video
# plane -- hdmi_video_set()). Geometry, pitch, format, alpha, the scaler's
# output size, the blender pipe that places the video, and the CSC matrix are
# all what the hardware holds right now. The hardware composes on the fly and
# no buffer in memory holds the result, so it is recomposed here in the same
# order: layers by index inside the UI channel, then blender pipe 1 (VI) over
# pipe 0 (UI). Not bit-exact in two places, both stated in the output: the
# video plane is scaled nearest-neighbour (the VSU uses an 8/4-tap filter),
# and alpha is blended in straight 8-bit arithmetic.
# ---------------------------------------------------------------------------
DE_CHAN = 0x01202000          # DE_CHAN_REGS_BASE; channel n at +0x1000*n
DE_UI1 = DE_CHAN + 0x1000
DE_VI0 = DE_CHAN + 0x0000
DE_BLD = 0x01201000           # DE_BLD_BASE
DE_VSU = 0x01220000           # DE_VSU_REGS
DE_CCSC0 = 0x012A0000         # DE_CCSC0_BASE (CCSC10)
UI_FMT_ARGB, UI_FMT_XRGB = 0, 4
VI_RGB_MODE = 1 << 15


def _wh(v):
    return (v & 0x1FFF) + 1, ((v >> 16) & 0x1FFF) + 1


def _sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def fetch_raw(hv, iface, base, length, slow, label):
    """Raw bytes of [base, base+length), by either transport."""
    if slow:
        words = hv.read_words(base, (length + 3) // 4)
        if not words:
            print("  %s: no reply -- channel down?" % label)
            return None
        return struct.pack("<%dI" % len(words), *words)[:length]
    try:
        data, missing = fbdump_recv.fetch(base, length, hv=hv, iface=iface)
    except (PermissionError, OSError) as e:
        print("  %s: raw fbdump socket unavailable (%s) -- retry with --slow"
              % (label, e))
        return None
    if missing:
        print("  %s: %d bytes MISSING after retries -- they read as black"
              % (label, sum(e - s for s, e in missing)))
    return bytes(data)


def read_planes(hv):
    """Every enabled plane as a dict, from the DE2 registers. None if the
    register map does not fit (layer 0 must be enabled and in DRAM)."""
    planes = []
    ui = hv.read_words(DE_UI1, 0x20)            # 4 layers x 8 words
    if not ui or len(ui) < 0x20:
        return None
    for n in range(4):
        attr, size, coord, pitch, addr = ui[8 * n:8 * n + 5]
        if not attr & 1:
            continue
        w, h = _wh(size)
        fmt = (attr >> 8) & 0xF
        amode = (attr >> 1) & 3
        galpha = (attr >> 24) & 0xFF
        planes.append(dict(kind="ui", layer=n, addr=addr, pitch=pitch, w=w, h=h,
                           x=coord & 0xFFFF, y=coord >> 16, fmt=fmt,
                           amode=amode, galpha=galpha, dst_w=w, dst_h=h))
    if not planes or planes[0]["layer"] != 0:
        return None
    vi = hv.read_words(DE_VI0, 8)
    bld = hv.read_words(DE_BLD, 4 * 4)
    if vi and len(vi) == 8 and vi[0] & 1 and bld and len(bld) == 16 and \
            bld[0] & (1 << 9):
        attr = vi[0]
        w, h = _wh(vi[1])
        vsu = hv.read_words(DE_VSU, 1)
        if vsu and vsu[0] & 1:
            ow = hv.read_words(DE_VSU + 0x40, 1)
            dst_w, dst_h = _wh(ow[0]) if ow else (w, h)
        else:
            dst_w, dst_h = w, h
        coord = bld[1 * 4 + 3]                  # ATTR_COORD(1)
        csc = None
        cc = hv.read_words(DE_CCSC0, 1)
        if cc and cc[0] & 1:
            m = hv.read_words(DE_CCSC0 + 0x10, 12)
            if m and len(m) == 12:
                # read back as 13-bit coefficients and 20-bit offsets
                # (0x1e6f is -401), whatever width hdmi.c wrote
                csc = [_sext(v, 20 if i % 4 == 3 else 13)
                       for i, v in enumerate(m)]
        planes.append(dict(kind="vi", addr=vi[6], addr1=vi[7], pitch=vi[3],
                           pitch1=vi[4], w=w, h=h, x=coord & 0xFFFF,
                           y=coord >> 16, fmt=(attr >> 8) & 0x1F,
                           rgb=bool(attr & VI_RGB_MODE), dst_w=dst_w,
                           dst_h=dst_h, csc=csc))
    return planes


def _describe(p):
    if p["kind"] == "ui":
        f = {UI_FMT_ARGB: "ARGB", UI_FMT_XRGB: "XRGB"}.get(p["fmt"],
                                                         "fmt%d" % p["fmt"])
        a = ["pixel", "global %d" % p["galpha"], "pixel*global %d" %
             p["galpha"], "?"][p["amode"]]
        name = {0: "HUD", 1: "guest window"}.get(p["layer"],
                                                "overlay %d" % (p["layer"] - 2))
        return ("UI1 layer %d (%s): %dx%d %s at (%d,%d), pitch %d, %#x, alpha %s"
                % (p["layer"], name, p["w"], p["h"], f, p["x"], p["y"],
                   p["pitch"], p["addr"], a))
    f = "XRGB" if p["rgb"] else {8: "NV12", 9: "NV21", 0: "YUYV"}.get(
        p["fmt"], "fmt%d" % p["fmt"])
    return ("VI0 (video): %dx%d %s -> %dx%d at (%d,%d), %#x/%#x, CSC %s" %
            (p["w"], p["h"], f, p["dst_w"], p["dst_h"], p["x"], p["y"],
             p["addr"], p["addr1"], "from hardware" if p["csc"] else "off"))


def _yuv_rgb(csc, y, u, v):
    """The CCSC as the hardware holds it: rows [Y, Cb-slot, Cr-slot, off],
    Q10. Byte 0 of a chroma pair goes to the Cb slot for NV12 and NV21
    alike, the model hdmi.c's ccsc_load() programs to (it swaps the
    matrix columns for NV21)."""
    out = []
    for r in range(3):
        c = csc[4 * r:4 * r + 4]
        val = (c[0] * y + c[1] * u + c[2] * v + c[3] + 512) >> 10
        out.append(0 if val < 0 else 255 if val > 255 else val)
    return out


def _vi_sampler(p, data, data1):
    """(sx, sy) in source pixels -> (r, g, b)."""
    fmt, pitch = p["fmt"], p["pitch"]
    csc = p["csc"] or [1024, 0, 0, 0] * 3       # CSC off: luma as grey
    if p["rgb"]:
        def f(sx, sy):
            o = sy * pitch + sx * 4
            v = struct.unpack_from("<I", data, o)[0] if o + 4 <= len(data) else 0
            return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
    elif fmt in (8, 9):
        p1 = p["pitch1"]

        def f(sx, sy):
            o = sy * pitch + sx
            y = data[o] if o < len(data) else 16
            oc = (sy // 2) * p1 + (sx // 2) * 2
            u, v = (data1[oc], data1[oc + 1]) if oc + 1 < len(data1) else (128, 128)
            return _yuv_rgb(csc, y, u, v)
    else:                                       # YUYV: Y0 U Y1 V
        def f(sx, sy):
            o = sy * pitch + (sx // 2) * 4
            if o + 3 >= len(data):
                return (0, 0, 0)
            y = data[o + (2 if sx & 1 else 0)]
            return _yuv_rgb(csc, y, data[o + 1], data[o + 3])
    return f


def compose(hv, iface, planes, scr_w, scr_h, step, slow):
    """Recompose the screen at 1/step resolution. Returns RGB rows."""
    ow, oh = len(range(0, scr_w, step)), len(range(0, scr_h, step))
    img = [bytearray(ow * 3) for _ in range(oh)]
    for p in planes:
        label = "UI1.%d" % p["layer"] if p["kind"] == "ui" else "VI0"
        data = fetch_raw(hv, iface, p["addr"], p["pitch"] * p["h"], slow, label)
        if data is None:
            return None
        data1 = b""
        if p["kind"] == "vi" and not p["rgb"] and p["fmt"] in (8, 9):
            data1 = fetch_raw(hv, iface, p["addr1"],
                              p["pitch1"] * ((p["h"] + 1) // 2), slow, "VI0 uv")
            if data1 is None:
                return None
        vi = _vi_sampler(p, data, data1) if p["kind"] == "vi" else None
        opaque = p["kind"] == "vi" or (
            p["amode"] == 0 and p["fmt"] == UI_FMT_XRGB) or (
            p["amode"] == 1 and p["galpha"] == 0xFF)
        # output pixels whose screen coordinate falls inside the plane
        xs = [i for i, x in enumerate(range(0, scr_w, step))
              if p["x"] <= x < p["x"] + p["dst_w"]]
        ys = [j for j, y in enumerate(range(0, scr_h, step))
              if p["y"] <= y < p["y"] + p["dst_h"]]
        for j in ys:
            sy = (j * step - p["y"]) * p["h"] // p["dst_h"]
            row = img[j]
            for i in xs:
                sx = (i * step - p["x"]) * p["w"] // p["dst_w"]
                if vi is not None:
                    r, g, b = vi(sx, sy)
                    a = 255
                else:
                    o = sy * p["pitch"] + sx * 4
                    v = struct.unpack_from("<I", data, o)[0] \
                        if o + 4 <= len(data) else 0
                    r, g, b = (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
                    if opaque:
                        a = 255
                    else:
                        pa = (v >> 24) if p["fmt"] == UI_FMT_ARGB else 255
                        a = [pa, p["galpha"], pa * p["galpha"] // 255, 255][
                            p["amode"]]
                k = i * 3
                if a == 255:
                    row[k], row[k + 1], row[k + 2] = r, g, b
                elif a:
                    row[k] = (r * a + row[k] * (255 - a)) // 255
                    row[k + 1] = (g * a + row[k + 1] * (255 - a)) // 255
                    row[k + 2] = (b * a + row[k + 2] * (255 - a)) // 255
    return [bytes(r) for r in img]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--what", default="window",
                    choices=("hud", "window", "both"))
    ap.add_argument("--step", type=int, default=1,
                    help="read every Nth row and column (1 = full resolution)")
    ap.add_argument("--out", default=None, help="output PNG path")
    ap.add_argument("--slow", action="store_true",
                    help="use hvdbg.read_words() instead of the raw fbdump "
                         "path -- minutes instead of seconds, but the only "
                         "option if AF_PACKET raw sockets aren't available")
    ap.add_argument("--iface", default=fbdump_recv.IFACE,
                    help="bridge interface for the raw fbdump path")
    a = ap.parse_args()

    d = defines(HDMI_H, ("HDMI_FB_BASE", "HDMI_MODE_HACTIVE", "HDMI_MODE_VACTIVE",
                         "HDMI_GUESTWIN_PA", "HDMI_GUESTWIN_W", "HDMI_GUESTWIN_H",
                         "HDMI_GUESTWIN_STRIDE", "HDMI_GUESTWIN_X",
                         "HDMI_GUESTWIN_Y", "HDMI_GUESTWIN_LAYER"))
    missing = [k for k in ("HDMI_FB_BASE", "HDMI_GUESTWIN_W") if k not in d]
    if missing:
        print("could not parse %s from hdmi.h" % ", ".join(missing))
        return 1
    scr_w = d.get("HDMI_MODE_HACTIVE", 1920)
    scr_h = d.get("HDMI_MODE_VACTIVE", 1080)
    gw, gh = d["HDMI_GUESTWIN_W"], d["HDMI_GUESTWIN_H"]
    gstride = d.get("HDMI_GUESTWIN_STRIDE", gw * 4)
    gx, gy = d.get("HDMI_GUESTWIN_X", 0), d.get("HDMI_GUESTWIN_Y", 0)

    hv = hvdbg.HV()
    scr_w, scr_h, mode_src = live_mode(hv, scr_w, scr_h)
    win_pa, src, bc_stale = live_layer_addr(hv, d["HDMI_FB_BASE"],
                                            d["HDMI_GUESTWIN_PA"],
                                            d.get("HDMI_GUESTWIN_LAYER", 1))
    # Mark which target is actually being captured. Printing both lines
    # unconditionally made a `--what window` run announce "HUD : 1920x1080"
    # and then write a 1120x276 guest-window PNG -- the banner described a
    # capture that never happened.
    def _tag(which):
        return "  <== CAPTURING" if a.what in (which, "both") else ""

    print("guest window: %dx%d stride %d at %#x  (%s)%s" %
          (gw, gh, gstride, win_pa, src, _tag("window")))
    if bc_stale is not None:
        print("  NOTE: the HDMI breadcrumb still says %#x -- STALE, left in "
              "hv-scratch DRAM by a previous boot generation (that memory "
              "survives a warm reset). Trusting the DE2 register instead."
              % bc_stale)
    print("HUD         : %dx%d at %#x  (%s)%s" %
          (scr_w, scr_h, d["HDMI_FB_BASE"], mode_src, _tag("hud")))

    step = max(1, a.step)
    out = a.out or ("/tmp/bzdos-%s-%s.png" % (a.what, time.strftime("%H%M%S")))

    if a.what == "both":
        planes = read_planes(hv)
        if planes is None:
            print("  WARNING: the DE2 layer registers do not fit this build "
                  "(layer 0 not enabled) -- no composition")
            return 1
        for p in planes:
            print("  " + _describe(p))
        t0 = time.time()
        comp = compose(hv, a.iface, planes, scr_w, scr_h, max(1, a.step),
                       a.slow)
        if comp is None:
            return 1
        print("  composed %d plane(s) in %.1fs%s" %
              (len(planes), time.time() - t0,
               "; video plane scaled nearest-neighbour, not the VSU filter"
               if any(p["kind"] == "vi" and (p["w"], p["h"]) !=
                      (p["dst_w"], p["dst_h"]) for p in planes) else ""))
    if a.what == "window":
        wrows, wcols = grab(hv, a.iface, win_pa, gw, gh, gstride, step,
                            "window", a.slow)
        if wrows is None:
            return 1
    if a.what == "hud":
        hrows, hcols = grab(hv, a.iface, d["HDMI_FB_BASE"], scr_w, scr_h,
                            scr_w * 4, step, "HUD", a.slow)
        if hrows is None:
            return 1

    if a.what == "window":
        png(out, wcols, len(wrows), wrows)
        w, h = wcols, len(wrows)
    elif a.what == "hud":
        png(out, hcols, len(hrows), hrows)
        w, h = hcols, len(hrows)
    else:
        png(out, len(comp[0]) // 3, len(comp), comp)
        w, h = len(comp[0]) // 3, len(comp)

    nz = 0
    total = 0
    src = wrows if a.what == "window" else (hrows if a.what == "hud" else comp)
    for r in src:
        for i in range(0, len(r), 3):
            total += 1
            if r[i] or r[i + 1] or r[i + 2]:
                nz += 1
    print("wrote %s  [%s]  %dx%d  (%d of %d sampled pixels non-black, %.1f%%)"
          % (out, a.what, w, h, nz, total, 100.0 * nz / max(1, total)))
    if step > 1:
        print("NOTE: --step %d, so this is every %dth row and column -- a "
              "preview, not a faithful capture." % (step, step))
    return 0


if __name__ == "__main__":
    sys.exit(main())
