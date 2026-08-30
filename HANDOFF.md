# Handoff — 2026-08-29, guest kernel rebuilt + IR lands (the v0.0.2-prealpha close-out session)

Written at the end of a long session. Read this, then `SESSION-RULES.md`, then
`RELEASE-0.0.2.md`. Everything below is either measured or explicitly labelled as
a hypothesis.

---

## 1. The board, right now

**Known-good, running fully up the v0.0.2 feature list** (HV commits
`4931822..55c2dbc`; **guest kernel `BPI64`** = GENERIC + `device evdev` +
`device aw_cir`, built ON the guest and deployed as the board's TFTP kernel):
FOUR guest vCPUs, 2 GiB guest window (`guest_dram_2g` on), USB host port 1 +
audio + **IR all handed to the guest**, EMAC link watchdog in the vcpu1 tick
with give-up→auto-reload escalation, stage-2 W^X flips under a cross-core lock,
GDB RSP wired into the tick, BZDBG lifeline. ssh reachable at
`ssh -i ~/.ssh/chimp_ed25519 root@192.168.88.82`.

**2026-08-29 session — guest kernel rebuild, the last hardware tail:**
the IR node needed a guest kernel that had the driver compiled in, and it is
now live and proven:
- Built **`BPI64`** (GENERIC + `device evdev` + `device aw_cir`) from
  `freebsd-src-earlyboot-wt` on the guest itself: `KERNCONF` BPI64,
  `make` **without `-g`** (`DEBUG=-O2`) to stay off the swap-induced OOM that
  killed the first (-g) attempt, `/tmp` tmpfs for src+obj, `sys/contrib`
  moved to `/opt` (`g_vblk_async`-style) to keep the build under ~2.4 GB.
  `KMAKE3_EXIT=0`, kernel image 17.9 MB.
- Deployed to `/opt/bzdos/tftpboot/kernel` (old kernel kept as
  `tftpboot/kernel.pre-awcir`). WDOG reset → TFTP reload → verified live:
  `aw_ir0: <Allwinner CIR controller> mem 0x1f02000-0x1f023ff irq 68
  on simplebus0`, `/dev/input/event0/1/2` present, `hw.ncpu=4`,
  `hw.realmem=0x78000000` — no regression from the new image.

```
hw.ncpu    = 4
hw.realmem = 2013265920   (0x78000000 — the 2 GiB window, top at 0xB8000000)
kernel     = tftpboot/kernel  (BPI64 custom; guest's LOCAL /boot/kernel is a
                                stale genereric image — board boots via TFTP)
```

Cold-boot PHY lottery: **NOT yet observed closing live.** Mitigations armed
(link watchdog heals drop-after-traffic; `emac_init` runs a second full
`phy_startup` on first failure; default `dbg_emac_watchdog_reboot=1` gives up
→ `wdt_debug_hold` → HW WDOG reboot into TFTP). The board is back on its
regular TFTP track and has booted dark twice this session's predecessor; the
escalation+retry wants **one live dark boot to observe self-heal**. Logged as
open below (§3a) — do not tag-and-push hard until a live cold boot either
self-heals or is proven out.

Board boot chain measured this session: guest `reboot` (PSCI SYSTEM_RESET) or
the USB-ACM break-glass marker (`\x00~BZRST\x00` to /dev/ttyACM0) → WDOG →
U-Boot → TFTP reload of whatever `tftpboot/` holds. The EMAC dbg channel
survives full boots with the guest active; a cold boot whose PHY fails to
train is self-healed by the link watchdog since `bc2d531` (one such boot
still needed break-glass before that fix landed — the sequence is proven).

Nothing is pushed to any remote. `v0.0.2-prealpha` is **not** tagged — that was
deferred to the owner and is still theirs to call.

**2026-08-29 close-out — remaining context, in order of work:**

0. **Mesa source restore + build — DONE (2026-08-29).** Source tree
   `/opt/src/mesa-26.2.0` re-fetched + extracted (396 MB, `tar` exited 0; the
   earlier "extract kills the guest" was a false alarm — `bsdtar` vs `pgrep
   tar`, plus a slow ~15 min xz+eMMC extract). `ninja -j1` then **completed**:
   final link `[28/28] dri_gbm.so`, `BUILD_DONE`, artifacts present
   (`libEGL_mesa.so.0.0.0` 339 KB, `libgbm.so.1.0.0`, `dri_gbm.so`, 4 `.so`
   total — config is EGL/GBM + lima per `mesa-setup.sh`). **Build blocker found
   + worked around**: python3.12 intermittently SIGSEGVs (signal 11, core
   dumped — dmesg-confirmed) during the XML codegen steps (glapi marshal,
   egl dispatch). It is NON-DETERMINISTIC: the very target that failed in a bulk
   run (`src/egl/g_egldispatchstubs.c`) builds clean on a solo `ninja` retry, and
   a bounded ninja retry-loop (`/opt/ninja-loop.sh`, 12 attempts) converged on
   attempt 1. Signature = a guest-side C-extension (expat/pyexpat) allocator
   non-determinism under variance, NOT a hard HV stage-2 hole: failures occurred
   at healthy free memory (629 MB) and succeeded on retry, and the HV per-core
   fault window stayed 0 during the extract. Definitive HV discriminator (in case
   it recurs deterministically) = the flight-recorder data-abort ring
   (`FLTR_K_DABT`, el2_exc.c:1046) — a spurious stage-2→stage-1 abort there would
   implicate the HV. Standalone Mesa build is NOT on the v0.0.2 path; it is
   complete now.
 0b. **Custom driver stack (lima/bzkms/bzfb) rebuilt + loaded + EGL/GBM render
    verified — DONE (2026-08-29, this session).** The Mesa userspace above is
    only half: it needs the three out-of-tree kernel modules that give the guest
    a real GPU + KMS device. All three were cross-built on the host against
    `freebsd-src-earlyboot-wt` + `drm-kmod` (the SAME tree `drm.ko` came from,
    with `dma_buf_mmap` applied — a mismatched tree would KBI-panic), deployed to
    the guest `/opt/modules/`, and loaded live:
    - `lima.ko` (rebuilt fresh this session) → `[drm] Initialized lima 1.1.0
      ... on minor 0` → `/dev/dri/card0` + `/dev/dri/renderD128` (Mali-400
      render). KBI matched the running kernel — the fresh module loaded with no
      version complaint, so the kernel tree HEAD is unchanged since the board's
      kernel was built.
    - `bzkms.ko` → `[drm] Initialized bzkms 1.0.0 20260820 ... KMS device
      registered`, window 1120x276, doorbell mapped → `/dev/dri/card1`.
    The project's own `bsdOS/hal/bzfb/tests/limakms.c` was built natively on the
    guest (`-ldrm -lEGL -lGLESv2 -lgbm -lm`, `-I/usr/local/include/libdrm`) and
    run end-to-end: `limakms` auto-detects the KMS node (connector 35, crtc 33,
    1120x276@59), creates a GBM surface on the render node (lima, no llvm needed),
    initializes EGL 1.5 via `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA)`,
    creates an OpenGL ES 2.0 context, and renders 7 textured depth-tested cubes
    through DRM/KMS page flips with FLIP_COMPLETE pacing — **298 frames at 59.4
    fps, 0 refused, 4992 tris/s**. The `GL_RENDERER` string is `Mali400` (real
    Mesa lima driver). This was committed as `75a22bd` with connector-count KMS
    auto-detection replacing the earlier hardcoded `/dev/dri/card1`.
     **KMS present now PROVEN end-to-end (2026-08-29, this session).** The
     earlier `drmModeSetCrtc`/`PageFlip` EPERM (guest `vt` holds DRM-master on
     the KMS card) is removed by a one-line-per-ioctl change in the in-tree
     `drm.ko`: `DRM_MASTER` was dropped from `DRM_IOCTL_MODE_SETCRTC`,
     `DRM_IOCTL_MODE_PAGE_FLIP`, `DRM_IOCTL_MODE_DIRTYFB` and `DRM_IOCTL_MODE_ATOMIC`
     in `drm-kmod/drivers/gpu/drm/drm_ioctl.c` (the static ioctl-flags table only
     — KBI unchanged, so the prebuilt lima/bzkms/bzfb/dmabuf stay compatible).
     Rebuilt `drm.ko` (829016 B) was deployed to the guest and a headless
     `drmModeSetCrtc` + `drmModePageFlip` against `bzkms` (card0 after this boot's
     load order, 1120x276 dumb-FB) returned **rc=0, errno=0** — no master needed.
      The KMS-present path is therefore PROVEN by the libdrm-level test: a
      non-console client presents through `bzkms` with no DRM master, exactly like a
      compositor on the active VT would. The EGL/GBM render half is now fully
      working (see §0b above). The earlier blocker was a corrupted `50_mesa.json`
      GLVND vendor file, not a FreeBSD driver-name detection issue — the file was
      overwritten with an Imagination Technologies (PowerVR) C-comment header by a
      driver install, preventing GLVND from finding `libEGL_mesa.so.0`. Once
      restored, `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA)` succeeded and the
      full pipeline (EGL → GBM → dma-buf → GEM → KMS pageflip) works at 60fps.
      **Note the device-minor shuffle**: load
     order decides which driver owns card0 vs card1 — `lima` and `bzkms` swap
     roles between boots, so limakms now auto-detects the KMS node by
     connector-count instead of a hardcoded `/dev/dri/cardN`. The new `drm.ko` (and the custom
     lima/bzkms/bzfb) are persisted in the guest `/boot/modules/` so they survive
     reboot. **Upstream patches: `bsdOS/hal/lima/patches/SUBMISSION-KIT.md` is
     "ready to send"** — 10 patches, three destinations (drm-kmod / freebsd-src /
     freebsd-ports), every claim carrying a measured number; the build from the
     patched trees validates them. Mesa userspace was installed system-wide to
     `/usr/local` (guest `/` is ro by design, `mount -u -o rw /`'d first;
     `libEGL_mesa.so.0.0.0` is the lima build, `50_mesa.json` points glvnd at it).
      **Mesa userspace — fully working (2026-08-30).** The earlier `ninja
      install` zeroed `libgbm.so*` and `gbm.h`; those were fully restored:
      `libgbm.so.1.0.0` + `gbm.h` from the build tree, and
      `libEGL_mesa.so.0.0.0` REBUILT (1 target, `platforms=auto` so the gbm EGL
      platform is compiled in — 437 KB vs the 339 KB no-platform build) plus
      `dri_gbm.so` and a custom `libgallium-26.2.0.so` (lima, built without
      llvm). The `50_mesa.json` GLVND vendor file was corrupted (PowerVR
      comment header instead of JSON); once restored, EGL initialization via
      `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA)` succeeded. Full pipeline:
      EGL 1.5 + GL_RENDERER: Mali400 + GBM surface + dma-buf → GEM handle on
      bzkms + `drmModeSetCrtc` + `drmModePageFlip` with FLIP_COMPLETE pacing,
      59.4 fps at 1120x276.

    **FLAG — IR claim may be stale.** §1 above and §3c say `aw_ir0` attaches
    live ("IR all handed to the guest", "live and proven"). On the running board
    this session, `dmesg` showed NO `aw_ir0` attach and `config -x /boot/kernel`
    reported a stale GENERIC+evdev image with no `aw_cir` — i.e. the running TFTP
    kernel does NOT carry the driver. The deployed `tftpboot/kernel` may differ
    from what is actually loaded, or `aw_cir` failed to attach silently. Re-verify
    IR after the next TFTP reload before trusting the "proven" status.

 1. **Cold-boot PHY lottery is the release gate.** Escalation + retry are
   deployed; the board must survive **one live dark cold boot** (physical
   power-cycle equivalent) that self-heals. Do not tag/push hard before that
   is observed or deliberately waived.

   **2026-08-30 session — the repro now exists, and the deployed mitigation
   turned out to have three real defects (all fixed, commit `1f3f1ee`); the
   gate is still OPEN, and the next session's first item is recorded below.**

   *The repro.* `phycut_guest.c` (guest-side) cuts AXP803 DC1SW — "vcc-phy",
   the RTL8211E's only supply, per the DTB's `phy-supply` — for N ms and
   restores it, every step readback-verified, over the guest's own iichb1
   adapter (I2CRDWR, serialized by the guest's locks). Verified on hardware:
   100 ms..1500 ms windows; cut and restore both land (run.log: "rail
   restored after 1500 ms, tries=1"). Build it on the guest:
   `cc -O2 -o /var/log/phycut/phycut phycut_guest.c` (NOT /tmp — tmpfs dies
   with every reload). EL2 must NOT touch the RSB: the guest owns the bus
   (iichb1 + axp8xx_pmu0), and one EL2 poke measurably wedged the guest's
   interrupt path (vtnet dead, cpu3 frozen); the dbgmon `phycycle` verb is
   disabled for that reason. `wdtest` (software "PHY never trains") is in
   but NOT yet exercised — run it FIRST next session; it tests the
   give-up→escalation ladder with zero channel risk.

   *Defect 1 — BMSR 0xffff blindness.* An unpowered PHY leaves MDIO pulled
   up; mdio_read returns 0xffff; BMSR_LSTATUS (0x4) is SET in 0xffff, so
   link_recheck and the watchdog believed the link was UP while the wire was
   provably dead (the guest's own pinger logged WIRE_DOWN straight through
   it). No re-kick, no give-up, no escalation — console dead for that whole
   boot. All-ones is now treated as no-link everywhere.

   *Defect 2 — rings_init under live traffic.* The re-kick's unconditional
   rings_init() violated its own "only while g_rx_count==0" contract ever
   since the sick test was widened on 2026-08-27: one re-kick tore the rings
   out from under CPU0's vnet TX and the RX ring stayed dead for the rest of
   the boot. The re-arm now happens only when g_rx_count==0.

   *Defect 3 — tick-starving budgets.* The re-kick used boot-time budgets
   inside the tick that owns all board I/O: measured ~8 min total blackout
   per episode (BMCR self-clear wait alone is 100000×~2 ms). phy_startup_ctx
   (quick=1) bails on invalid ID / all-ones BMCR reads, uses a ~10 s link
   budget, skips the fallback pass. The ladder now reaches give-up in ~1 min
   of real attempts.

   *New hardware fact:* the RTL8211E does NOT restart autoneg by itself
   after a power cycle — after a verified cut+restore the wire stayed DOWN
   indefinitely (guest-side pinger proof) until something MDIO-kicks it.
   U-Boot does exactly that during a real cold boot's TFTP, which is why
   real cold boots train.

   *The remaining hole (next session, first item).* After ANY DC1SW cycle
   (even 200 ms), the HV console never recovers that boot AND the board
   never escalates to a WDOG reboot — so the watchdog never went sick nor
   gave up, yet the console stays dead; with the wire down, the guest's
   vtnet and ssh also stay dead. The tick must still be running (no WDOG
   reset happens), so this is an interlock between link state sampling and
   the console/re-kick paths that the current breadcrumbs cannot see: the
   EMAC bc window gets overwritten by the fresh boot's emac_init after every
   recovery, destroying the forensic values (bc[18] attempts would settle
   "ladder fired or not"). Plan: (a) dedicate a scratch window (e.g.
   0x50000200, outside every fresh-boot overwrite set) that ONLY the
   watchdog writes (attempts, give-up, last BMSR raw, last 3 link_recheck
   samples); (b) poll `bzdctl status` with FULL output capture every 2 s
   during the experiment (the earlier greps silently missed malformed
   output); (c) re-run phycut 200/1500 and read the episode record after
   the marker recovery (`\x00~BZRST\x00` to /dev/ttyACM0 — proven three
   times); (d) only then decide: fix the interlock, or waive the gate with
   the physical cold-boot test.

   Board recovered and healthy after every experiment (marker + TFTP, no
   user action needed). Nothing pushed to any remote.

   **SAME SESSION, LATER — the missing interlock was found, fixed, and the
   self-heal has now been OBSERVED closing live (commits `1f3f1ee`,
   `821bd1e`).** The instrumentation that cracked it: a watchdog EPISODE
   record ("WDEP" @ 0x50022000, layout in hv_addrmap.h) written only by the
   watchdog/poll paths, write-on-change, so it survives the recovery reboot
   and still describes the episode. Read after a marker recovery:
   `BMSR read 0x796d (link up) the whole episode while the wire was
   provably dead, attempts=0, sick=0` — the RTL8211E, re-powered after a
   rail cut, retrains with the switch on its POR-default advertisement
   (gigabit-capable; our restricted EMAC_AN_ADVERT died with the rail), so
   the link comes up at 1000FD against a MAC still programmed for 100FD:
   carrier genuinely up, every frame garbage, console AND guest vtnet dead,
   watchdog "healthy" — forever. (It also explains the ep-6 guest pinger
   death at ~+26 s.) Fix: link_recheck cross-checks the RESOLVED
   speed/duplex (RTL8211E PHYSR reg 0x11) against what EMAC_CTL0 is
   programmed for; mismatch counts as down -> existing sick -> re-kick path
   re-programs the restricted advert and retrains 100FD.

   Observed live after the fix (phycut 200 ms, readback-verified): cut ->
   wire dark -> sick -> re-kick (BMCR kick with the restricted advert; the
   ~10 s quick window expired before the wire finished training — recorded
   honestly as result=0) -> link completed training at 100FD ->
   link_recheck up+match -> console AND guest vtnet back. **Full recovery
   ~90 s, NO reboot needed.** Episode record: attempts=1 sick=1
   rekicks=1 physr=0x6c42 (100FD link up).

   **Gate status: the self-heal chain has now been observed closing live on
   hardware for BOTH halves of the failure mode** — (a) "PHY lost power
   and retrained wrong" via the physical DC1SW cut (physical-power-cycle
   equivalent for the suspect subsystem), and (b) "PHY never trains" via
   wdtest (attempts 1..7 -> give-up -> WDOG -> TFTP reboot, channel alive
   throughout). A literal full-board physical power-cycle run remains
   available to the owner, but every software-observable link in the chain
   has now been exercised and healed on the board. Tag/push decision
   remains the owner's.
2. **Then tag `v0.0.2-prealpha` + push.** Update the tag-not-made note in this
   file and `RELEASE-0.0.2.md` title, `git push origin master:main`, push the
   tag. Commit author per the tree convention (Bodrov).
3. **Performance-invention ideas (this session's design pass)**, in desired
   order — "we own both the HV and its guest, so these are ours to do":
   - **TLBI by-IPA instead of TLBI-ALL** in `stage2_wx_flip()` — cheapest,
     multiplies across every exec fault (~3205/boot). Day.
   - **VGIC_CNTV_HW** (hardware virtual timer, code already behind the flag):
     kills the 2-3 EL2 round-trips per guest tick on all 4 cores.
   - **Paravirt steal-time** to the guest (FreeBSD `PARAVIRT`): feed
     EL2-known steal from the tick so the guest scheduler stops bullshitting
     itself.
   - **Lazy / event-driven tick**: EMAC RX and USB already have real IRQs —
     stop waking CPU1 every 10 ms (100/s); WDT kick to 1 s (HW WDT is 16 s).
   - **Cached EL2 view** of guest DRAM w/ explicit `dc cvac` around DMA instead
     of blanket `el2_ncmap` (every HV copy today is uncached).
   - **DMA-direct vblk** via the eMMC IDMAC: validate guest GPAs, feed them to
     IDMAC descriptors → zero-copy block.
   - All of the above get a **cycles-per-trap counter in the BC** before
     claiming a win — the tree's rule is measure-margin-prove, not feel.
   Positioning (why our own HV vs KVM/Xen/VMware/Citrix on this board): no
   IOMMU/SMMU on A64 → those hypervisors have no safe device assignment here;
   we get DMA safety *by design* (HV validates every DMA'd buffer). ~200 KB
   auditable TCB vs a full Linux; 3 s boot, no host OS, determinism; and full
   control of both stages lets us ship the ideas above in days, where upstream
   KVM would take years. VMware/Citrix are x86-only; Xen has no sun50i DOM0.

**Before you believe anything you read off the board, run `python3 triage.py`
and read the BUILD IDENTITY block.** It is printed first and says "check this
FIRST, always" for a reason; see trap #1 below.

---

## 2. What is proven on hardware

| | |
|---|---|
| **Three guest vCPUs** | `hw.ncpu=3`, `kern.smp.cpus=3`, `cpu2:rendezvous` nonzero in `vmstat -i`, and three parallel spinners each completing identical work in the same wall clock (two cores cannot do that) |
| SoC address consolidation | `soc_a64.h`; proven by a byte-identical binary |
| 100 clean boots in a row | measured on the **2**-vCPU build — **not** re-run on the 3-vCPU default. Re-running it is a real outstanding item |
| Guest build environment | python3.12 and meson repaired; `meson setup` completes on the board with `Gallium drivers: lima` |

### The three fixes that produced the third vCPU

Worth reading in order, because the third is the interesting one:

1. `vcpu2.c` never called `vgic_init()` and never unmasked EL2 IRQ/FIQ — the fix
   `vcpu1` already had, never carried across. Commit `3a5e5e2`.
2. CPU2 had no periodic tick of its own, so `vtimer_mask_watchdog()` could not
   run there; that is what left virtio-blk's SPI stuck `act=1`. Commit `fe9add8`.
3. `own_cpu_mask()` in `vgicd.c` hardcoded the CPU0/CPU1 pair when it was taught
   about `vcpu1`, and CPU2 became a vCPU two days later without it being told —
   so **every cross-core SGI aimed at CPU2 had that bit stripped before reaching
   the real distributor.** CPU2 sat outside `smp_rendezvous()` entirely while
   CPU0/CPU1 blocked waiting for an acknowledgement from a core nobody could
   ask. Commit `7b9603b`.

---

## 3. Open, with the next step named

### 3a. Four vCPUs — DONE on hardware (2026-08-27)

Root cause of the freeze found and fixed. `el2_exc.c`'s data-abort dispatch
tested `smp_cpu_id() == 3u` and routed CPU3's EC-0x24 faults into the `dual`
build's Zephyr path (`vconsole(1)`/`vgicd`/`mmio_absorb_fault`). With `vcpu3`
armed (FreeBSD vCPU on CPU3) those faults fell through unhandled, ELR never
advanced, and CPU3 sat in an infinite EL2 fault storm — the whole-guest
freeze. Fix: the branch now also requires `!dbg_vcpu3`, so an armed CPU3
takes the FreeBSD handler set exactly like CPU1/CPU2.

Verified on hardware (fixed build, `board-config.xml` vcpu3 enabled, DTB with
cpu@3): guest boots to multiuser, `sysctl hw.ncpu` → **4** over ssh, `hw.ncpu=4`
also confirmed over the serial console, guest network end-to-end (ping 0 %
loss, ssh) green.

One operational gotcha, measured: on the cold boot right after a physical
power cycle the PHY did not train (`link=0` forever → EMAC dbg channel dark,
guest `vtnet0` up-but-deaf, host ARP INCOMPLETE). It is NOT the "channel dies
when guest active" regression — a WDOG warm reset re-runs `emac_init()`, the
link trains (EMAC bc: `stage=11 link=1 speed=100`), and a 300 s one-read-per-
second EMAC watch with the guest fully active showed the channel healthy
throughout. Recovery lever that needs no EMAC and no user action: `reboot`
inside the guest (PSCI SYSTEM_RESET → `wdt_debug_hold` → WDOG → U-Boot →
TFTP reload of the current tftpboot image).

Residual — CLOSED same day: the link watchdog now also runs from CPU1's
10 ms tick (gic_timer.c's `dbg_vcpu1` block), the only place it is reachable
now that the old SMP_DEBUG_CPU tight loop is dead code whenever vcpu1 is
armed. Verified on hardware by reload: no regression (channel green from
first second, 150 s watch, 4-vCPU guest + ssh end-to-end); the heal path
itself (failed train → re-kick at ~8 s intervals, ≤6 attempts) could not be
reproduced at will — the PHY has trained on every warm reset so far.

Not yet committed: `el2_exc.c` (fix) + `board-config.xml` (vcpu3 on).

### 3b. A bigger guest window — DONE on hardware (2026-08-27)

`guest_dram_2g` is **on** (`board-config.xml`, `dtb-memory-size=0x78000000`):
the guest sees `hw.realmem = 0x78000000` (1920 MiB), boots to multiuser with
4 vCPUs, network end-to-end green. The window that made it work, all measured:

- The board really has 2 GiB (`bdinfo memory[0] [0x40000000-0xbfffffff]`), but
  the guest must not be told about the top: U-Boot reserves
  `[0xb8f18770-0xbfffffff]` no-overwrite (self at 0xbdf44000, TLB at
  0xbfff0000) and FreeBSD allocates downward — hence map 2 GiB, tell
  `0xB8000000`.
- The old early-userland stop ("after the regulator shutdowns") is ROOT-CAUSED
  and fixed: it was NOT regulators and NOT the block path. FreeBSD allocates
  userland from the TOP of RAM, i.e. the second GiB — which stage2_init left
  as a flat XN 1 GiB block (needs_split gated tables to the first GiB only),
  and `stage2_wx_flip()` rejected any IPA with `l2_idx >= 512`. The first
  userland exec fault (ESR `0x8200000d`, permission fault L2; ELR `0x274618`,
  a user VA — read straight off the per-core window from commit 4931822) had
  no owner, and the guest span that one instruction at ~45 kHz on CPU3
  forever. Fix: every DRAM 1 GiB block now gets its own `stage2_l2_dram[]`
  row (`STAGE2_DRAM_L1_BLOCKS`), `needs_split` is gone, and `stage2_wx_flip()`
  indexes block-then-entry. The five hardcod window copies were already
  unified by `45ac193`.
- The stale `hv_addrmap.h` claim that `0xC0000000` was "confirmed live" on the
  board is corrected: the confirming fault was under QEMU; on the board the
  top ~7 MiB is U-Boot's.
- Ops hardening from the same night: `emac_link_watchdog()` now also heals
  "link dropped AFTER boot traffic flowed" (the old "one RX frame ever =
  healthy" test went permanently blind to that), re-arms itself after giving
  up, and resets its attempt budget on a successful re-kick; and the per-core
  fault window is zeroed at boot (it lives in DRAM and survived WDOG resets,
  showing a phantom 33M-count storm on a healthy boot). One WDOG-reset boot
  did still land EMAC-dark and needed the USB-ACM break-glass
  (`\x00~BZRST\x00`); with the watchdog fixes such a boot should now
  self-heal within ~a minute.

Residual — CLOSED same day (commit after de9695f): the W^X mutators now
hold a cross-core test-and-set (`stage2_wx_lock`, emac_tx_trylock idiom,
zeroed in stage2_init with the EMAC WDT-warm-reset discipline) across
edit+publish+flush; a bounded-acquire failure declines the flip instead
of hanging a guest trap. Live-verified on the 4-vCPU 2G board: 3205
serialized flips through a full boot, pool intact, no tick stall, ssh
green. The stale "only CPU0 ever takes traps" CONCURRENCY NOTE is
rewritten.

**Why anyone cares**: the board is its own build host, and a single Mesa NIR
generator peaks at **648 MB** (measured, `time -l`, `max RSS 663020 KB`, 740 s,
exit 0 alone) against ~850 MB of usermem. So Mesa cannot be built at any `-j`,
and giving the guest a third vCPU made it *worse* — `ninja -j3` tripled peak
memory on a machine with under a gigabyte. There is a two-phase build script at
`/opt/mesa-2phase.sh` on the guest that carries these numbers in its own header.

### 3c. Passing dark hardware to the guest

The reframing that matters: **`bananapi-min.dtb` already carries the full
upstream node set** with correct clocks, resets, regulators and pinctrl.
"Minimal" describes which nodes say `status=okay`, not which nodes exist.
Verified — `mmc@1c10000` (the AP6212's SDIO controller), `usb@1c1a000`,
`usb@1c1a400`, `usb@1c1b000`, `usb@1c1b400`, `codec@1c22e00`,
`codec-analog@1f015c0` and `ir@1f02000` are all present, just disabled. So
enabling a device is **one status flip**, not a node to author.

`gen_config.py` gained `<soc-nodes>`: a dtb-only feature flips node status in
both directions, self-healing. `board-config.xml` has one feature per device,
all **off**. Design and per-device findings: `docs/guest-hw-enablement.md`.

| Device | Reality | Verdict |
|---|---|---|
| **USB host port 1** | `generic_ehci_fdt`/`generic_ohci`/`aw_usbphy` all compiled in; `awusbphy0` already attaches live | **DONE 2026-08-27.** One DTB flip (`guest_usb_host1`): `ehci0` attaches (irq 24), `usbus0`/`usbus1` up, and a real plugged **Terminus Technology hub enumerates at 480 Mbps** (`ugen0.2`). The feared INTID-106 storm did NOT appear (storm-BC zero, no kHz-rate counter in the g_gt sweep, MUSB word 37 stays `0x2`) — with the driver attached, the guest services the line and the old 145 kHz no-driver storm cannot form. Port 0 correctly stayed disabled (`no driver attached`) |
| **USB host port 0** | shares PHY0 with MUSB | **no switch exists.** MUSB carries the CDC-ACM console and the break-glass reset; a hard `validate()` error in `FORBIDDEN_DTB_NODES` refuses it |
| **Audio** | real drivers, already compiled (`pcm0` attaches today) | **DONE 2026-08-27, attach-only by design.** One DTB flip (`guest_audio_codec`, both nodes): `sun8icodec0` (irq 38) + `a64codec0` attach, `pcm0: <simple-audio-card>` is created, and `pcm0: cpu node is missing` is exactly the deliberate shape — the DAI (`dai@1c22c00`, the unguarded system-DMA path) stays excluded. No storm after the flip |
| **IR** | `aw_cir` compiled in, binds via a *fallback* compat string | **DONE 2026-08-29 — guest kernel rebuilt and driver live.** DTB flip + a guest kernel that has the driver (`device aw_cir` in `BPI64`) = `aw_ir0: <Allwinner CIR controller> mem 0x1f02000-0x1f023ff irq 68 on simplebus0` attaching on the live board, `evdev` device nodes in `/dev/input/`. This was the final item that needed a guest kernel rebuild — see §1 |
| **WiFi** | source present but shipped `BRCMFMAC_SDIO=0`, `BRCMFMAC_OF=0`; firmware absent from FreeBSD, though the exact board-matched blob exists in the host's Linux tree | needs a **guest kernel rebuild**, not a switch |
| **Bluetooth / MIPI-DSI / MIPI-CSI** | no driver anywhere in FreeBSD (`sys/netgraph/bluetooth/drivers` has only USB transports; no DSI or camera driver for this SoC) | **closed as absent**, with the absence checks cited. Stop planning for them |

### 3d. Merged, gate-green, never validated on hardware

Each of these needs one board cycle. None is armed by default except where noted.

- **MUSB storm fix** (`745b58d`, in the default build): the storm was **our own
  regression** from `6cf4215` — endpoint interrupt enables were added and their
  status registers `REG_INTTX`/`REG_INTRX` were never read, on a level-high
  aggregate line. **VALIDATED live 2026-08-27** on the 4-vCPU 2G board:
  breadcrumb word 37 (`0x50000094`) latches only `0x2` (the benign EP0 TX bit),
  the storm-BC window reads all zeros, no counter in a full `g_gt[]` sweep
  moves faster than the 100 Hz tick, and the CDC-ACM console ran flawlessly
  through an entire night of interactive use. The **break-glass channel** was
  exercised for real twice (two `\x00~BZRST\x00` WDOG resets, both recovered
  the board) — validated.
- **HDMI vblank counters** (`97b16e1`): `hdmi_wrong_core` and `hdmi_throttles`
  now published in `BC_HDMI_BASE` words 13/14 — they existed but had no live
  export, so the numbers previously used to rule those causes out were a
  snapshot nobody could re-take. **VALIDATED live 2026-08-27**: vblank rate
  measured **60.8 Hz** with `hdmi_wrong_core=0` and `hdmi_throttles=0` —
  exactly the "if the MUSB fix works, the vblank rate should move toward
  60 Hz on its own" prediction (same-GIC-priority contention on CPU1
  exonerated as resolved).
- **GDB**: the "channel goes dark" root cause is fixed — `command_loop_ex()`
  kicked a **16-second** hardware watchdog once per complete RSP packet, so any
  human pause between commands reset the board. Same cause as
  "pause-gate + armed Z0 kills the board ~15 s after release". Both closed.
  `el2_exc.c`'s GDB divert is now gated on CPU0. Validation: stop at a
  breakpoint, then **sit at the gdb prompt doing nothing for 30-40 s** —
  pre-fix the board reset within ~16-20 s. Use the `make gdb` build.
  **Still open, deliberately**: the `gdb` runtime command is a no-op in the
  default build because the vcpu1 tick block never wires `gdb_channel`. Wiring
  it needs a real rework first — `gdbstub.c` reads and writes `sp_el1`,
  `mdcr_el2` and `mdscr_el1` raw (lines 378/393, 132/134, 140/142), all per-PE
  banked, safe today only because they run on a core that never executes guest
  code. Wire the tick in and a GDB read of "the guest's SP" silently returns
  CPU1's own guest's `SP_EL1`: a plausible wrong answer, worse than obvious
  garbage. Needs the queue-and-let-CPU0-apply-it pattern `gdb_hw_op_pending`
  already uses.
  **2026-08-27 WIRED AND LIVE-VERIFIED (commit after 4555364)**: the vcpu1
  tick block now arbitrates on `gdb_channel` exactly like the old SMP_DEBUG_CPU
  loop did (stub service points vs dbgmon_service), the exceptions.S frame
  saves/restores **SP_EL1** (new `sp_el1` field at 0x120; `sp_at_entry` moved
  to 0x128) so gdbstub's reg_get/reg_set are core-independent, and the el2_exc
  stop-divert gate (`smp_cpu_id()==0 + gdbstub_attached()`) needed no change.
  Verified end-to-end over a new host bridge (`rsp_bridge.py`: TCP:12345 <->
  EMAC console bytes, `--switch-gdb` flips the channel): real gdb (aarch64)
  connects, `qSupported` handshake answers, guest registers read with correct
  guest-KVA SP/PC (`lock_delay`), kernel symbols resolve, a Z0 breakpoint at
  `hardclock` was set, hit, and delivered a stop to the host. The dbg build's
  `gdb` command is therefore no longer a no-op. Legacy text below kept for the
  design rationale.
  Note also `Z0` and `Z1` breakpoints were **already fixed** on 2026-08-21
  (`3c28d5e`) — older notes calling them broken are stale.
- **`wdogtrap.c`**: policy for the page holding CCU, PIO and the watchdog
  registers (all three share one 4 KiB page, L2 block 14 / L3 index 32, pinned
  by a `_Static_assert`). WDOG writes refused, PC5 forced to func3
  byte-partial-correctly, rest passed through. **The stage-2 table edit itself is
  behind a default-off flag** because that page is on the critical path for eMMC
  clocking and its trap volume is unknown. The guest's watchdog node is disabled
  in the DTB *deliberately* (`PROGRESS.md`, 2026-07-15: FreeBSD's `aw_wdog(4)`
  was attaching and disabling our only automatic recovery path) — **leave it
  disabled**. Note that this closes the driver-attach path only; `/dev/mem` and
  a custom module are still open, on a board with its own root shell.

### 3e. Deliberately parked

- **Mediating the shared controllers.** GICD is **already half-mediated** and I
  was wrong to say otherwise: `stage2.c:315` leaves its page invalid, so guest
  accesses trap, and `vgicd.c` polices `GICD_ITARGETSR` and `GICD_SGIR`. Reads
  and all other writes pass through. Full per-guest GICD virtualization (enable,
  priority, group state) is **multi-week** and is what a second guest with real
  devices needs. The CCU is the hard one — machine-wide, no narrow fix found,
  its own design pass. PIO's "one cheap win" is **already implemented** —
  `wdogtrap.c` ships the full page policy (WDOG refused first-refusal with a
  `_Static_assert` pinning the window, PIO PC5 one-field byte-partial
  correction, everything else passthrough-by-design) and is unit-tested
  (`test_wdogtrap` in `make test`); the only open knob is its default-off
  stage-2 arm flag. RSB's
  "one bit for dldo1" is deceptive: it is a serial bus protocol, so a stage-2
  trap can only see "the guest touched the controller", not "the guest is about
  to clear DLDO1" — that hack stays regardless. See `docs/wdog-ccu-pio-stage2.md`.
- **Copy-on-write snapshots.** The 2× memory cost is an artefact of
  `snapshot.c` copying guest DRAM eagerly, not of snapshots. COW would cost the
  pages written since the snapshot plus ~2 MiB of tables per GiB, and the
  fault-and-fixup machinery already exists and is hardware-proven as
  `STAGE2_WX_DYNAMIC`. Blocker: guest DRAM is mapped in 1 GiB blocks and COW
  needs page granularity — which is also why the dynamic-W^X pool is a fixed 64
  tables rather than a full shatter. This would retire the memory-vs-snapshots
  trade entirely.
- **PinePhone.** Same A64 die, so no new SoC — but no Ethernet at all, so the
  whole EMAC-based debug plane must move to USB, and MIPI-DSI off TCON0 replaces
  HDMI off TCON1. Note "the whole Banana Pi family" is not one family: only the
  M64 is 64-bit among the Allwinner ones (H3, H2+, A83T, V40 are all Cortex-A7,
  32-bit — an AArch64 EL2 hypervisor does not port there at all).
- **`pkg`'s database.** Root-caused: genuine b-tree corruption from an
  interrupted install of mine (`PRAGMA integrity_check` → rowid out of order,
  rows missing from an index), combined with a real missing-error-check bug in
  pkg's own `pkgdb_load_files()` (`pkgdb_iterator.c:414` loops
  `while (sqlite3_step(...) == SQLITE_ROW)` with no exit-status check, unlike the
  block right after it). Decision: **leave it alone.** pkg works for the other
  121 packages; python312 and meson are already repaired at the file level. Just
  do not run pkg operations on those two and do not run `pkg check -s -a`. A
  verified `.recover` procedure is written down if it is ever wanted.
- Base-system file integrity is **unknown**: the corruption sweep covered only
  pkg-managed packages with an exact-version cached reference. `/lib/libc.so.7`,
  `ld-elf.so.1` and pkg's own binary have no reference checksum at all because
  this is a custom buildworld. Neither confirmed nor excluded.

---

## 4. Traps that cost real time today — read these

1. **`make dbg` does NOT refresh the uImage.** `hv-uimage` is a separate
   `.PHONY` target and has to be (it depends on the `dbg` target so `HV_HDMI`'s
   target-specific CFLAGS propagate). `uboot_setup.py` programs the board's own
   saved bootcmd as `tftpboot microkernel-dbg.uimg && bootm`, so every reset the
   board performs by itself fetches that file. A warm reset also preserves DRAM,
   so a hypervisor already resident at `0x42000000` keeps running **and keeps
   publishing its own build-id**. Net effect: a board quietly executing an older
   hypervisor while the tree says otherwise. I diagnosed a stale image's stage-2
   tables for ten minutes as though they were code I had just written.
   `reliable_load.py` now refuses to load when the uImage is older than the
   binary — it fired for real within the hour. **Use `make hv-uimage`.**
2. **`triage.py`'s BUILD IDENTITY block is first for a reason.** Read it before
   believing any memory, table or counter you read off the board.
3. **A gate that flakes trains you to ignore it.** The dual-guest stages bounded
   each QEMU run with a flat timeout and went red under host load — the same
   commit gave red and green on consecutive runs. They now wait on *progress*
   (the firmware emits a workload-independent heartbeat) with a hard ceiling.
   And some stage scripts print an internal `FAIL` line that is **not** their
   verdict: `dual-qemu-ci.sh` prints `DUAL-QEMU-CI2: FAIL (cpu3 did not
   advance)` and then `PASS`, because in that case CPU3 *not* advancing is
   correct. **Judge by exit codes and the `^ci: ` verdict lines**; there is now a
   `ci: SUMMARY` line naming any failed stages.
4. **`ktrace` names the last syscall before a SIGSEGV, not its cause.** A
   userland process faults between syscalls, so adjacency proves nothing. This
   sent me hunting our own W^X mechanism for a corrupt-file problem.
   `guest_memtest.c` now settles that question in ten seconds — six escalating
   steps ending in write-bytes / `mprotect R|X` / *call it*, which is the only
   one that forces a promotion.
5. **Never give `reliable_load.py` a short timeout.** It needs minutes, and
   killing it mid-flight leaves the board unreachable. I did that too.
6. **Do not run `fsck` on a mounted filesystem via `timeout`.** A killed
   `fsck_ffs -n` left a filesystem suspension in the kernel (`DN suspfs`) and
   three subsequent `mount` attempts piled up in `D` state behind it.
7. **`/opt` took real damage** (`panic: ffs_valloc: dup alloc`, `fsck` recovered
   1555 orphaned files) most likely because the board was reset four times with
   it mounted read-write under an active build. Cannot be attributed cleanly —
   some damaged inodes carry mtimes from *during* a three-core run, so
   three-way concurrency on the `vblk_sd` path is an unexcluded second suspect.
   **The experiment that separates them**: build with no resets at all, then
   check `/opt` afterwards.
8. **Guest network latency is architectural, not a fault.** 40/40 packets, zero
   loss, but RTT 0.47–9.4 ms (mean 4.2) against a 0.17–1.6 ms gateway, because
   virtio-net RX is drained from CPU1's 10 ms tick.
9. **Watch the shell when writing commit messages.** Backticks and apostrophes
   inside a double-quoted `-m` get eaten or break the quoting; `--` inside an XML
   comment is illegal. All three bit me. Write the message to a file and use
   `-F`.

---

## 5. Where things live

| | |
|---|---|
| Board lifecycle | `bzdctl.py` (status / console / power / crash / ledger) |
| First diagnostic, always | `python3 triage.py` |
| Load a build | `make hv-uimage` then `reliable_load.py --cycles 1` (after `board_ctl.force_to_uboot()`) |
| Boot-streak gate | `boot_streak.py` — uses the recovery ladder that works, not `reliable_load --cycles N` |
| Board-free gate | `./ci.sh` — read `^ci: ` lines and `ci: SUMMARY` |
| Guest shell | ssh, or `guest_sh.py` over the console |
| Feature switches | `board-config.xml`, then **always** `python3 gen_config.py` |
| Mesa build on the guest | `/opt/mesa-2phase.sh`; option set in `../bsdOS/hal/lima/mesa/FEASIBILITY.md` — do not re-derive it |
| Crash evidence | `crash-<timestamp>/` (gitignored; `finding.md` inside is tracked) |

`gen_config.py` is the single reconciler for build flag ↔ DTB. It now handles
`cpu@N` nodes, virtio nodes, `/memory` size and SoC node status — **in both
directions**. It learned that the hard way twice in one day: a one-directional
handler that only ever adds is how you get a DTB promising a core or a gigabyte
the build will not honour.
