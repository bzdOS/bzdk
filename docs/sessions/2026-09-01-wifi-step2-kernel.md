# internal-note (WiFi chain step 2) — kernel built, deployed, verified — 2026-09-01

Outcome: the guest now runs the **BPI64WIFI** kernel (MMCCAM + evdev + aw_cir),
the flipped DTB reaches it, and the WiFi SDIO host controller probes. The
brcmfmac module itself is **not buildable in releng/15.1** — that is now the
chain's critical path (task internal-note).

## What is running

- `uname`: FreeBSD 15.1-RC3 `#0 allwinner-ccu-a64-pll-gpu-f38f90d1efce-dirty:
  Tue Sep 1 17:12:33 MSK 2026 root@workstation:/usr/obj/.../sys/BPI64WIFI arm64`
- dmesg evidence: `aw_mmc0 mem 0x1c10000` (SMHC1, the AP6212 SDIO side — was
  absent before the flip), `aw_ir0` (CIR; the evdev+aw_cir rebuild closes the
  "driver absent" gap from docs/guest-hw-enablement.md), hw.ncpu=4.
- chimpd runs persistently on workstation; its TFTP kernel slot
  (`/opt/bzdos/tftpboot/kernel`) is now BPI64WIFI (old GENERIC backed up as
  `kernel.GENERIC-0829`). eMMC-autoboot default is still GENERIC on the loader
  path — a deliberate safety fallback; the guest also carries
  `/boot/BPI64WIFI/kernel` (md5-identical) for the loader/nextboot route.

## How it was built (recipe, all on workstation)

```
cd /opt/bzdos/freebsd-src-earlyboot-wt          # dirty lineage of the running kernel
env MAKEOBJDIRPREFIX=/opt/bzdos/fbsd-obj \
  bmake TARGET=arm64 TARGET_ARCH=aarch64 -j8 KERNCONF=BPI64WIFI buildkernel
# kernel → /opt/bzdos/fbsd-obj/opt/bzdos/freebsd-src-earlyboot-wt/arm64.aarch64/sys/BPI64WIFI/kernel (812 s)
```
KERNCONF (`sys/arm64/conf/BPI64WIFI`): `include GENERIC-MMCCAM` + ident +
`options MMCCAM` + `device evdev` + `device aw_cir` (hv's 23:59 draft had
`include GENERIC`, missing GENERIC-MMCCAM's nodevice semantics; module Makefile
gates SDIO on `KERN_OPTS:MMMCCAM`).

Module attempt (fails, see internal-note): cross kmod build needs
`KERN_OPTS="MMCCAM DEV_PCI"` in env, `-m <src>/share/mk`, and
`CC="$W/usr/bin/cc --target=aarch64-unknown-freebsd15.1 --sysroot=$W -B$W/usr/bin"`
with `$W=/opt/bzdos/fbsd-obj/.../arm64.aarch64/tmp`. sdio.c then dies on
implicit declarations: 15.1's linuxkpi has no `linux/mmc/` headers (the include
resolves to the empty dummy), so `sdio_readb/writeb/retune_*` do not exist.

## Findings worth remembering

1. **Guest cannot absorb big pushes** (internal-note): a 1.4G tar-over-ssh to /opt
   wedged all guest disk I/O at ~733M (vblk frozen, sshd kex hangs, RTT
   15-145ms; no console messages; killing the push unwedged it). Root-fs
   (vtbd0, shared with swap) dies even on a 19M scp. The kernel push that
   WORKED: 4M chunks × dd-seek with 3s gaps + md5 verify (workstation
   `/tmp/kernel-push.sh`).
2. `/tmp` tmpfs staging + `make -j4` on the 2G guest = OOM in 57s. Stage to
   /opt, or better: don't stage at all — cross-build on workstation.
3. **build-host↔workstation rides one reverse tunnel** (workstation dials build-host:<port> from
   94.25.170.179). It died ~17:20-17:33 (self-healed). build-host has no tailscale
   and no routed path into 192.168.88.0/24 — when the tunnel is down, the
   board is unreachable from build-host.
4. DTB provenance: guest loader.conf's `fdt_name` points to a nonexistent
   file, so the guest kernel uses the HV-passed DTB (tftpboot
   `bananapi-min.dtb`, wifi flip verified via fdtget: `/soc/mmc@1c10000
   status = okay`). One DTB to rule them all.
5. chimpd's `timer=FROZEN` monitor lines are the "this build publishes no tick
   counter" quirk; vblk r/w/kicks advancing is the real liveness signal it
   acts on.

## Chain state

internal-note kernel side done. Next: internal-note (linuxkpi-mmc port) → internal-note
(firmware blob) → internal-note (wlan0 gate). hv restart remains fleet's business.
