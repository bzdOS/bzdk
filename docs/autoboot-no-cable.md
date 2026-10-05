# Booting the board: no USB cable needed, U-Boot log over the network

**Current state, verified on hardware on 2026-10-02.**

- The board boots U-Boot → hypervisor → FreeBSD guest by itself, with
  nothing plugged into the micro-USB port.
- With the cable plugged in, everything works as before.
- U-Boot's console is copied to the network (netconsole, UDP 6666).

The Ethernet cable and the TFTP server on `fedora` (192.168.88.2) are still
required.

> History: this file used to claim "the USB cable is optional" from
> 2026-08-20. That was true only by accident: the cable was always plugged in.
> On 2026-10-02 the board sat dark for 18.7 h after a reload with no USB
> host. U-Boot was waiting for one forever. The fix is described below.

## Boot path

```
power-on / WDOG reset
  BROM -> SPL (eMMC LBA 16, untouched since July)
       -> FIT at eMMC LBA 0x50  = tftpboot/u-boot-nc.itb  (U-Boot 2026.07-rc5, "Oct 02 2026 - 21:44:45")
       -> env from ESP:uboot.env (vtbd0p2, FAT)
       -> preboot: stdio = serial,nc,usbacm   (waits <= 3 s for a USB host)
       -> bootcmd: up to 10x boothv, then arm the WDOG
            boothv: tftpboot bananapi-min.dtb, kernel, microkernel-dbg.uimg ; bootm 0x48000000
       -> hypervisor (EL2) -> FreeBSD guest
```

The `boothv` stage takes ~10 s from reset to `bootm`. A full guest reload
(`shutdown -r` in the guest) takes ~50 s until ssh answers.

## The U-Boot environment (ESP `uboot.env`)

```
bootdelay=-2
preboot=setenv stdout serial,nc,usbacm ; setenv stderr serial,nc,usbacm ; setenv stdin serial,nc,usbacm
stdin=serial,nc,usbacm   stdout=serial,nc,usbacm   stderr=serial,nc,usbacm
ncip=192.168.88.2        ipaddr=192.168.88.7       serverip=192.168.88.2
autostart=yes
boothv=setenv autostart no; tftpboot 0x4a000000 bananapi-min.dtb && tftpboot 0x44000000 kernel && tftpboot 0x48000000 microkernel-dbg.uimg && setenv autostart yes && bootm 0x48000000
bootcmd=for i in 1 2 3 4 5 6 7 8 9 10; do run boothv; echo [bootcmd] retry $i; sleep 3; done; echo [bootcmd] gave up; mw.l 0x1c20cb4 1; mw.l 0x1c20cb8 0xb1; mw.l 0x1c20cb0 0x14af; ...
```

Read and change it from the host with `microkernel/uboot_env.py`. It mounts
the ESP in the guest over ssh, recomputes the CRC, and verifies the write by
reading it back.

```sh
python3 uboot_env.py get bootdelay
python3 uboot_env.py backup /var/tmp/uboot.env.bak
python3 uboot_env.py set NAME=VALUE ...      # owner-approved changes only
```

A copy of the env from before netconsole is in
`microkernel/uboot.env.bak-2026-10-02-usbacm`.

These settings are load-bearing:

1. **`bootdelay=-2`.** Autoboot cannot be interrupted, so nothing can park U-Boot
   at a prompt. Tools that need the prompt (`uboot_flash_fit.py`,
   `uboot_chainload_test.py`) open a `bootdelay=3` window through
   `uboot_maint.py` and close it again in a `finally`.
2. **The `autostart` split.** Autostart is off for the transfers and on right
   before `bootm`. With `autostart=yes`, `tftpboot` would try to boot every
   file it fetches.
3. **The fallback arms the watchdog, never `reset`.** `reset` at this
   U-Boot's prompt calls `hang()`.
4. **`usbacm` stays in stdio.** It costs nothing without a host, which is the
   point of the patch below, and the U-Boot console over the cable still works.

## Why it no longer needs a USB host

Upstream `acm_stdio_start()` (`drivers/usb/gadget/f_acm.c`) does this:

```c
while (!acm_connected(dev)) { if (ctrlc()) return -ECANCELED; schedule(); }
```

So it waits **forever** for a host to enumerate the gadget, and `schedule()`
keeps the watchdog fed. Any boot with no host on the OTG port therefore never
reached `bootcmd`:
- no TFTP request reached the host;
- EMAC sent no frame at all (the RJ45 LEDs still blink, from RX);
- the WDOG never fired.

This can happen in several ways: the cable is unplugged, it goes to a phone or
a charger, or the host is rebooting.

`microkernel/uboot-patches/0001-f_acm-bound-the-wait-for-a-USB-host.patch`
stops waiting after **3 s** and returns `-ETIMEDOUT`.
- The console mux drops `usbacm` from that assignment.
- The gadget stays registered, so the next assignment (`preboot`) takes it
  back at once, without a second wait.
- While disconnected, `putc` only buffers.
- A host that appears later still enumerates the gadget.

Build: the 09-26 config that was flashed before
(`build/patch/u-boot.config.flashed-2026-09-26`) plus `CONFIG_NETCONSOLE=y`;
the full config is in `build/patch/u-boot.config.nc-2026-10-02`. Source tree:
`/opt/bzdos/build/u-boot-nc`. BL31 is `build/atf/build/sun50i_a64/release/bl31.bin`,
and there is no SCP. The FIT is cut from `u-boot-sunxi-with-spl.bin` at
offset 32 KiB.

## Netconsole

Watch every boot from the host:

```sh
tcpdump -l -ni br0 -A 'udp port 6666'
```

Each boot prints `U-Boot 2026.07-rc5-dirty (Oct 02 2026 - 21:44:45 +0300)`,
`In: serial,nc,usbacm`, the TFTP transfers, and `Image Name: bzdk-hv`.
TFTP output is suppressed while a transfer runs, which is normal for
netconsole.

For an interactive U-Boot over the network, `bootdelay` must be ≥ 1, which
is a deliberate env change. Then run `nc -u -l 6666` on the host and type
into it; the replies go to 192.168.88.7:6666. With `bootdelay=-2` there is no
window to type into, and that is intended.

## Testing a U-Boot candidate without touching the media

**Never write an untested loader to the eMMC.** First chain-load it from
the loader that works. The script below needs neither a prompt nor an env
change:

```sh
tools/chainload/uimg_chainload_test.sh <candidate/u-boot.bin> <tag> [nousb]
```

It wraps the candidate in `tools/chainload/chainload_uimg.S` and serves the
result once as `microkernel-dbg.uimg`. The stub copies the candidate to
0x4a000000, cleans caches by set/way, turns the MMU off, **arms the 16 s
WDOG**, and jumps. The script then puts the real HV back right away.

If the candidate hangs, the cost is one watchdog reset into the stock loader,
which then boots the normal HV. With `nousb`, the host stops configuring new
USB devices once the stock loader has reached TFTP
(`echo 0 > /sys/bus/usb/devices/usb2/authorized_default`). That is exactly the
"no USB host" case. The script re-authorizes the port when it exits.

Logs go to `/var/tmp/uimg-chainload-<tag>.{log,nc}`.

## Flashing a FIT and rolling back

The owner runs the flash (media writes are not delegated):

```sh
python3 uboot_flash_fit.py --fit <new>.itb --current <on-card>.itb --expect-version "<build stamp>"
```

What it does:
1. Opens a `bootdelay=3` window and resets the board.
2. Catches the prompt over USB, so the cable must be plugged in for this step.
3. Checks that the card holds `--current`, then writes the FIT at LBA 0x50.
4. Reads it back, compares it with `cmp.b`, and resets into the new FIT.

It never touches the SPL. Afterwards, check the card from the guest
read-only:
`dd if=/dev/vtbd0 bs=512 skip=80 count=1719` and compare with the `.itb`.

The previous FIT is `tftpboot/u-boot-retry.itb` (09-26, no netconsole, waits
forever for USB). To roll back:

```sh
python3 uboot_flash_fit.py --fit u-boot-retry.itb --current u-boot-nc.itb --expect-version "Sep 26 2026 - 13:46"
```

Gotcha, fixed 2026-10-05: the port lock used to be `/tmp/chimp-acm.lock`. A
lock created by user `agent` there could not be opened by root
(`fs.protected_regular`), and the umask made it 0644, so the other user could
not open it either. The flash tool then reported `!!! no tty` while the gadget
was in `dmesg`. The lock is now `/run/chimp/acm.lock` in a setgid
`root:fleet` directory, mode 0660. The directory comes from
`/etc/tmpfiles.d/chimp.conf` (copy `tools/host/chimp-tmpfiles.conf`, then
`systemd-tmpfiles --create chimp.conf`); without it the tools stop with a
message that says so. `python3 test_port_lock.py` (as root) checks the lock in
both directions between root and `agent`. If a flash still fails with
`no tty`, look at who holds the lock: `fuser -v /run/chimp/acm.lock`.

## Board dark after a reload: checklist

1. `ping 192.168.88.82` and `safemode.py show`. If the channel answers but
   ssh does not, look for safe mode / the hold gate first.
2. `journalctl -t in.tftpd --since -10min`. No RRQs from 192.168.88.7 means
   the board is stuck in or before U-Boot. Watch netconsole: no `U-Boot`
   banner means it is before U-Boot (SPL/BROM) or the env failed to load.
3. `tcpdump -eni br0 ether host 02:bd:05:00:00:01` to see whether the HV sends
   anything at all.
4. Since 2026-10-02 a missing USB host is **not** a reason to hang. If the
   board still parks waiting for USB, the card does not hold
   `u-boot-nc.itb`.
