# Autoboot: the USB cable is no longer needed to boot

**Hardware-verified 2026-08-20, including a genuine cold power-on with the host
not touching the console at all.** The board brings up U-Boot -> hypervisor ->
FreeBSD guest by itself; swapping the hypervisor image is now "copy a file into
the TFTP root and reset over EMAC", with no ymodem and no serial interaction.

## The U-Boot environment that does it

```
bootdelay=3
boothv=setenv autostart no; tftpboot 0x44000000 kernel && tftpboot 0x4a000000 bananapi-min.dtb && tftpboot 0x48000000 microkernel-dbg.elf && setenv autostart yes && bootelf -p 0x48000000
wdreset=mw.l 0x1c20cb4 1; mw.l 0x1c20cb8 0xb1; mw.l 0x1c20cb0 0x14af; echo [wdreset] SoC watchdog armed; sleep 30
bootcmd=for i in 1 2 3 4 5 6 7 8 9 10; do run boothv; echo [bootcmd] retry $i; sleep 3; done; echo [bootcmd] gave up; run wdreset
```

Four things in there are load-bearing:

1. **The `autostart` split.** `autostart=yes` makes `tftpboot` try to BOOT what
   it just fetched, which is why it cannot be left on for the kernel and DTB
   transfers -- but `bootelf` only JUMPS when it is on. So: off for the
   transfers, on immediately before `bootelf`. A `boothv` without this loads the
   ELF and silently returns to the prompt.
2. **`bootdelay=3`, never `-1`.** The delay is the only window in which the
   prompt can be caught once autoboot works, and catching it needs interrupt
   spam (`loady_over_acm.catch_uboot()`), not a bare newline. `bootdelay=-1`
   removes the window entirely and with it the ability to load anything else.
3. **The fallback arms the WATCHDOG, not `reset`.** `reset` at the U-Boot prompt
   on this platform prints "System reset not supported on this platform" and
   calls `hang()`: the CLI dies, the USB gadget stops being serviced, and NO
   remote lever recovers it (not uhubctl -- the root hub has no `ppps`; not the
   port `disable` -- it drops the link without cutting VBUS; not `sunxi-fel` --
   `1f3a:efe8` here is U-Boot's own download gadget, not BROM FEL). That costs a
   physical power-cycle. Verified: `run wdreset` resets and autoboots cleanly.
4. **Quoting.** This U-Boot honours single quotes in `setenv`, so a multi-command
   value keeps its `;` separators. Without quotes the parser splits the line and
   only the first fragment lands in the variable.

## Why the previous attempt was believed impossible

The old note said `bootelf` cannot boot a TFTP'd ELF because of a stale dcache.
That is **wrong**. Two ordinary problems were being read as one exotic one:

- `autostart` was `no`, so `bootelf` never jumped (point 1 above);
- `/opt/bzdos/tftpboot/microkernel-dbg.elf` was a **stale 371856-byte file from
  five weeks earlier**. `cp` is aliased to `cp -i` on this host and had been
  silently declining to overwrite it; the transfer log showing
  `Bytes transferred = 371856` is what gave it away. Use `install -m 0644`.

There is no `dcache` command in this U-Boot build at all, so the flush that the
old theory called for could never have been the fix.

## Operating notes

- Keep the TFTP root current: `install -m 0644 microkernel/microkernel-dbg.elf
  /opt/bzdos/tftpboot/microkernel-dbg.elf` and check the size in U-Boot's
  transfer log.
- To reload: write 1 to `HVMAP_WDT_DEBUG_HOLD` (0x50095000) over EMAC; CPU1 stops
  petting, the watchdog fires within 16 s, and the board comes back on the new
  image. `wdt_arm()` clears the flag on the way up.
- The Ethernet cable IS still required -- the boot fetches over TFTP from
  192.168.88.2. Only the USB cable is now optional.
