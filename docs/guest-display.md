# Getting the guest's output onto the monitor

Status 2026-08-19: **HV side done and hardware-verified. Guest side blocked.**
The blocker is specific and in FreeBSD, not in this tree, so it is written down
here rather than rediscovered.

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

**3. THE ACTUAL BLOCKER — vt becomes the system console and there is no keyboard.**
With simplefb present, vt is the console; the guest then blocks in
`vtterm_cngetc()` (`vt_core.c:2022`) waiting for a keypress that cannot come, and
the serial console goes silent (`console bytes=4` for 21 minutes, versus ~27000 in
a normal boot). Observed live, and the board had to be recovered.

The fix would be `RB_MULTIPLE` (multiple consoles: vt draws on the monitor, the
serial keeps input). **There is no way to set it from here.** Both routes are
closed on this configuration:

- `boot_multicons` as a kenv goes through `boot_env_to_howto()`, which on FreeBSD
  is called from `x86/xen/pv.c` ONLY — never on arm64.
- `/chosen bootargs` goes through `parse_fdt_bootargs()`, which runs only
  `if (loader_envp == NULL)`. This hypervisor loads the kernel directly and passes
  `MODINFOMD_ENVP` (kload.c), so `loader_envp` is non-NULL and the DTB's bootargs
  are never parsed.

## What would actually work, in increasing order of effort

1. **Pass `MODINFOMD_HOWTO` from kload.c** with `RB_MULTIPLE` set, if arm64's
   metadata path honours it — this is the cheapest option and was NOT yet checked.
   Start here.
2. **Attach a USB keyboard.** Sidesteps the whole problem; vt gets input and the
   console-takeover stops mattering. Cheapest physically, useless for headless.
3. **A small guest KMS-less framebuffer driver** that claims the scanout doorbell
   (`scanout.h`, already implemented HV-side) and exposes `/dev/fb0` WITHOUT
   registering as a console. Then vt never takes over, X11/`scfb` or a direct
   framebuffer client can draw, and lima's output can be blitted or flipped in
   zero-copy via FLIP_REQUEST.
4. **Port sun4i-drm** (DE2 + TCON + DWC-HDMI) as a real DRM/KMS driver. Months,
   and it requires taking HDMI away from the HV — the opposite of this
   architecture.

Option 3 is the one that fits: it uses the doorbell that already exists, keeps the
HV owning the display hardware, and does not fight FreeBSD's console layer.

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
