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
                  CURRENTLY fetching (read from the HDMI breadcrumb, not
                  assumed) -- so this follows a client's page flips
  --what both     the composition: the HUD with the window pasted at the
                  coordinates the mixer places it, which is what the panel shows

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
                         "HDMI_GUESTWIN_Y"))
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
    # The address the layer is ACTUALLY fetching. A client that page-flips has
    # moved this away from HDMI_GUESTWIN_PA, and capturing the stale constant
    # would silently screenshot the wrong buffer.
    bc = hv.read_words(HDMI_BC, 14) or []
    live = bc[11] if len(bc) > 11 and bc[11] not in (0, 0xFFFFFFFF) else None
    win_pa = live if live else d["HDMI_GUESTWIN_PA"]
    print("guest window: %dx%d stride %d at %#x%s" %
          (gw, gh, gstride, win_pa,
           "  (live, from the HDMI breadcrumb)" if live else
           "  (hdmi.h default -- no flip has happened)"))
    print("HUD         : %dx%d at %#x" % (scr_w, scr_h, d["HDMI_FB_BASE"]))

    step = max(1, a.step)
    out = a.out or ("/tmp/bzdos-%s-%s.png" % (a.what, time.strftime("%H%M%S")))

    if a.what in ("window", "both"):
        wrows, wcols = grab(hv, a.iface, win_pa, gw, gh, gstride, step,
                            "window", a.slow)
        if wrows is None:
            return 1
    if a.what in ("hud", "both"):
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
        # Paste the window layer where the mixer puts it. This is a composition
        # done here because the hardware composes it on the fly and no buffer in
        # memory ever holds the result.
        comp = [bytearray(r) for r in hrows]
        for j, wr in enumerate(wrows):
            dy = (gy // step) + j
            if dy >= len(comp):
                break
            dx = (gx // step) * 3
            comp[dy][dx:dx + len(wr)] = wr
        png(out, hcols, len(comp), [bytes(r) for r in comp])
        w, h = hcols, len(comp)

    nz = 0
    total = 0
    src = wrows if a.what == "window" else (hrows if a.what == "hud" else comp)
    for r in src:
        for i in range(0, len(r), 3):
            total += 1
            if r[i] or r[i + 1] or r[i + 2]:
                nz += 1
    print("wrote %s  %dx%d  (%d of %d sampled pixels non-black, %.1f%%)"
          % (out, w, h, nz, total, 100.0 * nz / max(1, total)))
    if step > 1:
        print("NOTE: --step %d, so this is every %dth row and column -- a "
              "preview, not a faithful capture." % (step, step))
    return 0


if __name__ == "__main__":
    sys.exit(main())
