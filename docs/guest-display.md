# Getting the guest's output onto the monitor

Status 2026-08-19: **WORKING.** The guest draws its console into the buffer the
HDMI block scans out. Every link measured; the three obstacles below were real and
are all cleared, so they are kept as the record of what to do again.

Measured end to end, this configuration:

    HV:     BC_HDMI_BASE stage = 6 (HDMI_STAGE_SCANOUT)  -- clocks -> DE2 ->
            TCON -> HDMI ctrl -> PHY -> scanout all up, not a timeout
    stage2: [10] BUF0 PAR.F = 0 shared, [11] BUF1 PAR.F = 1 private, [18] pass = 1
    guest:  VT(simplefb): resolution 1280x720
            kern.console = ttyv0,ttyu0,...   BOTH consoles active
    pixels: non-zero words in the top 8 text rows, columns 0-199 --
              baseline 87  ->  after the guest clears the screen 8
                           ->  after the guest prints 8 rows of '#' 300

The last line is the proof that matters: the count follows what the guest does, so
the guest is driving the scanned-out DRAM and not something else.

Whether the physical monitor lights up is the one thing that cannot be measured
from here -- it needs eyes on the panel.

Build: `make dbg EXTRA_CFLAGS="-DHV_HDMI -DHV_FB_GUEST"`, plus the DTB node
described under obstacle 1/2, plus the kload.c howto fix under obstacle 3.

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

## What is left

The console is on the monitor. Getting **lima's rendering** there is the next step
and needs no new kernel work: `scanout.h`'s doorbell is implemented HV-side, so a
guest client can render into BUF0 directly (zero-copy) or render elsewhere and blit.
BUF1 stays private, so double-buffered flipping through FLIP_REQUEST needs BUF1
shared too — a one-line change to the block classification in stage2.c, and the
isolation selfcheck's expectation for `[11]` would have to move with it. Do not make
that change silently.

A `/dev/fb0` for ordinary framebuffer clients (X11 `scfb`, anything writing pixels
without GL) still needs a small guest driver: simplefb is console-only and
registers no `fbd`.

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
