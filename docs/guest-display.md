# Getting the guest's output onto the monitor

Status 2026-08-19: **WORKING, composited.** The HUD owns the screen; the guest's
console appears in a window inside it. Zero copy, and the guest never touches the
scanout buffers.

## The design

Two DE2 layers in the same UI channel, blended by the mixer:

    UI1 layer 0   full screen 1280x720   HV's HUD          buffer 0x4D000000
    UI1 layer 1   1134x276 at (16,66)    guest's console   buffer 0x4B000000

The guest's buffer is ORDINARY GUEST DRAM, reserved `no-map` in its DTB and
declared as a `simple-framebuffer` node. So:

- **zero copy** -- the mixer's DMA reads the guest's buffer directly, nothing is
  blitted per frame (~1.2 MiB/frame avoided);
- **a window, not the screen** -- which stage-2 sharing could not express anyway:
  pages are 4 KiB and contiguous while a rectangle's scanlines are 5120 B apart,
  so only full-width bands are shareable;
- **isolation untouched** -- the guest writes only its own DRAM, the scanout
  buffers stay private, and `HV_FB_GUEST` is not needed for this path at all.

A64 mixer1 has one UI channel with FOUR layer configs (0x20 stride); only layer 0
was ever programmed. Layer 1 was chosen over the idle VI channel because layers
within one channel composite inside it and feed the blend pipe the HUD already set
up -- no blender changes, no second pipe.

## Measured, both layers independently live

    HUD    0x4D000000  1200 non-zero, UNCHANGED across everything the guest did
    window 0x4B000000  113 baseline -> 8 after the guest clears -> 80 after it
                       prints 10 rows of '#'
    guest  VT(simplefb): resolution 1134x276      kern.console = ttyv0,ttyu0,...
    HV     HDMI stage 6 (SCANOUT), layer PA 0x4b000000, size 1134x276

The HUD count not moving is as important as the guest count moving: it shows the
two buffers are genuinely separate and the guest cannot reach the HV's.

Whether the panel lights up is the one thing that cannot be measured from here.

## Geometry is one unit across three files

`hdmi.h`'s `HDMI_GUESTWIN_*`, `hud.c`'s `CON_*` rectangle, and the DTB node's
width/height/stride must agree. If they disagree the picture is skewed rather than
absent, which is much harder to spot. hud.c's layout is the source of truth.

Build: `make dbg EXTRA_CFLAGS="-DHV_HDMI"` (NOT `-DHV_FB_GUEST` -- unnecessary
here), plus the DTB nodes below, plus the kload.c howto fix under obstacle 3.

DTB, on top of the deployed blob with `fdtput` (see guest-dtb.md -- never `dtc`):

    /reserved-memory/guest-fb@4b000000   reg = <0x4b000000 0x200000>, no-map
    /chosen/framebuffer@4b000000         compatible = "simple-framebuffer"
                                         reg = <0x4b000000 0x132fc0>
                                         width=1134 height=276 stride=4536
                                         format="x8r8g8b8" status="okay"
    /chosen/framebuffer-lcd  compatible -> "allwinner,pipeline"   (see obstacle 2)
    /chosen/framebuffer-hdmi compatible -> "allwinner,pipeline"

0x4B000000 came from a full-tree occupancy sweep, not a guess: 0x4C000000 is the
guest's SP_EL1 and 0x46000000..0x47146000 is its kernel image, so this is the gap --
14 MiB below a stack that grows down.

## What works

`HV_FB_GUEST` (stage2.c) shares the FRONT scanout buffer with the guest at 4 KiB
granularity and keeps everything else private. Verified live:

    [10] BUF0 PAR.F = 0   reachable by the guest, on purpose
    [11] BUF1 PAR.F = 1   still private
    [15] hv-image = 1  [16] hv-scratch = 1  [17] control = 0  [18] pass = 1

So the isolation boundary is exactly where it was designed to be: the guest can
write the buffer being scanned out and nothing else.

## Why the shortest guest-side path does not work

FreeBSD has `dev/vt/hw/simplefb/simplefb.c`, which binds the standard
`simple-framebuffer` DT node, maps it `pmap_mapdev_attr(VM_MEMATTR_WRITE_COMBINING)`
— non-cacheable, so guest writes reach DRAM with no cache maintenance, which is
what the DE2 scanout DMA needs — and draws the vt console into it. It is already
compiled into GENERIC via `arm64/conf/std.dev`. No new driver needed. That is why
it was tried first.

Three things it gets wrong for this board, in the order they were hit:

**1. The node must be a child of `/chosen`, not the root.** `vt_simplefb_node()`
searches `OF_child(OF_finddevice("/chosen"))` only. A `/framebuffer@...` node at
the root is invisible to it.

**2. It matches on `compatible` and IGNORES `status`.** `/chosen` on this board
already has U-Boot's `framebuffer-lcd` and `framebuffer-hdmi`, both
`compatible = "allwinner,simple-framebuffer", "simple-framebuffer"` and both
`status = "disabled"`. The search loop breaks on the FIRST compatible child, so it
picks a disabled node with no `reg`, `width` or `height` — and the symptom is

    VT(simplefb): resolution 0x0

Worked around by rewriting those two nodes' `compatible` to `allwinner,pipeline`
alone. They are NOT removable: both carry a `phandle` with live referrers
elsewhere in the blob.

**3. vt becomes the system console and there is no keyboard — SOLVED, and the fix
was a bug in this tree.**
With simplefb present, vt is the console; the guest then blocks in
`vtterm_cngetc()` (`vt_core.c:2022`) waiting for a keypress that cannot come, and
the serial console goes silent (`console bytes=4` for 21 minutes, versus ~27000 in
a normal boot). Observed live, and the board had to be recovered.

The fix is `RB_MULTIPLE` (multiple consoles: vt draws on the monitor, the serial
keeps input). Two of the three ways to set it are closed here, which is worth
knowing before trying them:

- `boot_multicons` as a kenv goes through `boot_env_to_howto()`, which on FreeBSD
  is called from `x86/xen/pv.c` ONLY — never on arm64.
- `/chosen bootargs` goes through `parse_fdt_bootargs()`, which runs only
  `if (loader_envp == NULL)`. This hypervisor loads the kernel directly and passes
  `MODINFOMD_ENVP` (kload.c), so `loader_envp` is non-NULL and the DTB's bootargs
  are never parsed.

The third works: **`MODINFOMD_HOWTO`**, fetched by `machdep_boot.c:210` in the very
function that also fetches the `MODINFOMD_ENVP` this loader already depends on.
kload.c was already emitting that record.

And emitting it wrongly, which turned out to matter far beyond the display.
`howto_val` was `0x800u | 0x1u` with a comment calling `0x1` "RB_SINGLE".
**`RB_SINGLE` is `0x002`; `0x001` is `RB_ASKNAME` — "force prompt of device of root
filesystem".** So this hypervisor had been asking the guest for the interactive
root prompt on every boot (`vfs_mountroot.c:899`). That is where every
`mountroot>` in every boot log came from — not a mount failure, not a GEOM race.
Dropping that bit removed the prompt entirely (0 occurrences) and `/etc/rc` now
completes on its own, network and sshd included, with no manual intervention.

Current value: `RB_VERBOSE | RB_MULTIPLE`.

## GPU rendering into the window: working, and the bottleneck is measured

`bsdOS/hal/bzfb/` — a small guest driver handing userspace a **write-combining**
mapping of the window buffer (`/dev/bzfb0`), plus `tests/limashow.c`, a GL demo that
renders an animated textured scene and presents into it.

The driver exists for a non-obvious reason. `/dev/mem` *can* reach the address —
arm64's `memrw()` `CDEV_MINOR_MEM` path validates nothing — but **mmap of /dev/mem
is CACHEABLE**: `memmmap()` declares `vm_memattr_t *memattr __unused` and never sets
it. Writes sit in CPU cache, the mixer's DMA reads DRAM, and nothing appears, in a
way that looks like a broken display rather than a wrong mapping. (Confirmed: a `dd`
to /dev/mem landed nowhere EL2 could see.) simplefb(4) works over the same buffer
precisely because it maps it `VM_MEMATTR_WRITE_COMBINING`.

Measured on hardware:

    render only                        476.1 frames/s   (9523 frames / 20.0 s)
    render + readback + present         13.7 frames/s   (274 frames / 20.1 s)

The GPU draws a frame in **~2.1 ms**. `glReadPixels` costs **~71 ms of a 73 ms
frame — 97%**, a 35x factor. On a tile-based renderer that is a resolve, not a copy.

So the path works end to end (verified from EL2: every sampled word in the window
buffer non-zero and **all** of them changing between reads 2 s apart), and the
number to quote for the platform's rendering is 476 fps, not 13.7 — 13.7 is the
speed of *this present path*.

**True zero-copy render-to-window** is therefore worth doing and its payoff is
known: lima must render directly into the window buffer instead of into a BO that
gets read back. That needs a GEM import of the reserved physical range (or the
`scanout.h` doorbell extended to take a guest-supplied buffer address), and it would
move the ceiling from 13.7 toward 476.

Also still missing: a `/dev/fb0` for non-GL clients (X11 `scfb`). bzfb registers no
`fb_info`/`fbd`, deliberately — that is a separate step.

## Do not repeat

- The DTB node alone is not enough, and its absence is not the reason a picture
  does not appear. Check `VT(simplefb): resolution NxN` in dmesg first — `0x0`
  means it bound the wrong node.
- Enabling `HV_FB_GUEST` without suppressing the HUD produces a blank-looking
  screen that is actually a fight: CPU1 redraws the HUD every frame over whatever
  the guest wrote. main_dbg.c handles this; keep it that way.
- `reboot_clean` failed again during this work (`U-Boot gadget never appeared`).
  `board_ctl.force_to_uboot()` — the ladder that ends in the break-glass ACM
  marker — recovered it every time. Use that, not repeated reload attempts.
