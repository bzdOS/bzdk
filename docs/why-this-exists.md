# Why this exists

A from-scratch ARM64 hypervisor on a $40 single-board computer, running FreeBSD
15.1 as a guest — with hardware-accelerated 3D reaching the screen without a
single copy, a board that boots the entire stack unattended and reboots itself
out of trouble, and a debug plane that keeps answering while the guest is
panicking.

No JTAG. No UART header. No reference hypervisor for the SoC. No vendor support.

This document is the *why* and the *so what*. `../ROADMAP.md` has the plan,
`war-stories.md` has the twelve incidents in forensic detail, and
`../DEBUG_RULES.md` has the rules that were paid for in hours.

---

## The bet

Everyone knows how to write "a hypervisor". The bet here was different:

**build the substrate so that nothing can hide from you.**

On this board that is the whole game. Bring-up starts with no wire you can
attach a probe to, so the binding constraint is never architecture — it is that
every hour spent guessing is an hour not spent fixing. So the instrumentation
was built first, given its own CPU core, and designed around one property:
**it outlives the guest.**

- CPU1 runs the debug monitor, the network channel and the hardware watchdog, so
  a guest that wedges or panics on CPU0 does not take the channel with it
  (`smp.c`, `dbgmon.c`, `wdt.c`).
- The guest's console is a trapped UART, captured by EL2 — so it survives into
  the *next* boot generation. `bzdctl.py console --postmortem` reads the log of
  the machine that just died.
- Everything worth knowing is published into fixed breadcrumb windows with a
  documented address map (`hv_addrmap.h`). A post-mortem is a read, not a re-run.
- The watchdog resetting the board is a **feature**. An unattended board digs
  itself out instead of waiting for a human (`../SESSION-RULES.md` R2).

That choice is why a day's work can move this project from "the GPU renders
black" to "accelerated 3D through a standard kernel interface", and it is the
part most worth copying.

---

## What it does today

**It runs a real operating system.** FreeBSD 15.1 arm64 boots to userland under
EL2 with `virtio-blk` on real eMMC and `virtio-net` bridged over the board's
Ethernet. Interrupts are genuinely virtualized — `HCR_EL2.IMO=1`/`FMO=1` with
every physical IRQ forwarded into a guest List Register in hardware mode
(`main_dbg.c`), superseding an earlier static-routing workaround. The memory
posture stays Jailhouse-style static partitioning. A second guest (Zephyr) runs
concurrently on CPU3 in the `dual` build.

**It boots itself.** Power on, and U-Boot → hypervisor → FreeBSD comes up with
zero host interaction — verified on a genuine cold power-on
(`autoboot-no-cable.md`). Shipping a new hypervisor is now: drop the ELF into
the TFTP root, reset over the network. The USB cable is no longer part of the
boot path.

**It recovers itself.** 100 clean boot cycles in a row with no human touching
anything. 20 break-glass resets in a row, every one recovering unaided — 16 of
those needed a filesystem repair and not one needed a second pass.

**It isolates, provably.** The hypervisor is cut out of the guest's stage-2 map,
and the proof is the CPU's own table walker: an `AT S12E1W` self-check that the
guest cannot forge. W^X on guest DRAM is on by default, enforced dynamically —
a page flips to read-execute on its first instruction fetch and back on the next
write. Under the real workload (boot to multiuser, three kernel modules loading,
a full GL workload, 12052 presented frames) it used **27 of 64** page-table
slots, exhausted **zero**, and performed **4045** transitions. Exhaustion is
designed to fail *open* — one 2 MiB block loses enforcement rather than the guest
losing its life. Details: `wx-enforcement.md`.

**It draws.** The Mali-400 renders: sampled textures, 2420 draw calls, depth
testing and alpha blending, zero GPU MMU faults. Getting there meant fixing five
defects nobody had found, four of them in FreeBSD's own compatibility layer.

**It presents without copying.** The guest renders into a `gbm_surface`; the
display engine fetches *that* buffer. ~1030 fps at 1120×276, and across a
session **153471 buffer addresses accepted by the hypervisor with zero
rejected**.

**And it speaks the standard language.** Since 2026-08-20 the guest has a real
DRM/KMS device (`hal/bzkms`, `/dev/dri/card1`): PRIME import, `drmModePageFlip`,
59.7 fps **paced by `DRM_EVENT_FLIP_COMPLETE`**. The drop from 1030 fps is the
achievement, not a regression — frames are now timed by the display instead of
fired blind, which is what every real client needs and what a private ioctl can
never give. X11's `modesetting` driver and any Wayland compositor can talk to
this.

---

## The idea worth stealing

**The hypervisor owns the display; the guest gets a window inside it — and still
gets zero-copy acceleration.**

The guest writes a physical address into a trapped MMIO register. EL2 validates
the *whole extent* against guest DRAM and its own carve-outs, then repoints the
display layer (`scanout.c`, `scanout_addr_allowed()`; design in
`zero-copy-scanout.md`). That check is not ceremony: the display engine is an
IOMMU-less DMA reader, so an unvalidated address would put hypervisor memory on
screen. One range check buys a real security property — and it turned out to be
the *only* KMS primitive actually needed, which is why the KMS driver is a thin
layer over it rather than a rewrite.

---

## We fixed other people's bugs on the way

Eight patches, against two upstream trees, all diagnosed from symptoms on this
board. Three carry full write-ups under
`../../bsdOS/hal/lima/patches/UPSTREAM-*.md`.

**FreeBSD** (branch `linuxkpi-dma-fix` over `releng/15.1`):

- `dma_alloc_coherent()` returned **cacheable** memory on arm64 — the one
  allocator whose contract is "no cache maintenance required" handed out
  write-back memory and gave its physical address to a device. This was the black
  frame.
- `dma_map_sg()` could not map multi-page lists on non-coherent arm64
  (`nsegments = 1` bounded the per-map sync list, so entry 1 of a 64-entry list
  always failed `EFBIG`).
- `sgl->dma_map` left stale on every map failure path, so unmap dereferenced
  garbage.
- `aw_ccung`: transposed gate/lock arguments on A83T CPUX PLLs.
- `ccu_a64`: every fractional clock missing `AW_CLK_HAS_GATE` — which is why the
  GPU's power gate never opened.

**drm-kmod:**

- `dma_buf_mmap()` simply did not exist, so no imported buffer could be mapped.
- A device-alias lifecycle bug.
- `drm_add_busid_modesetting()` assumes **every** DRM device is PCI —
  `to_pci_dev()` then `pdev->bus->number`, unconditionally. Any platform GPU
  driver walks backwards off a struct that is not a `pci_dev`; ours panicked,
  and the existing lima port survives only because its parent happens to sit in
  readable heap memory and the bus id is silently garbage.

None are submitted yet — that needs the author's own accounts — but each is
diagnosed, patched locally, and written up.

---

## What the bugs taught

This is the most portable thing in the project.

### The interesting layer works; the plumbing lies

| symptom | actual cause |
|---|---|
| years of guest filesystem corruption | a clock counter reading **backwards** → unsigned underflow → instant spurious timeout → a retried write landing mid-data-phase |
| ~17% inbound packet loss no board counter could see | the PHY forced to 100/full with autoneg off, so the switch fell back to **half** duplex |
| an interactive `mountroot>` prompt on every single boot | we passed `0x1` calling it `RB_SINGLE`. `0x001` is `RB_ASKNAME`. We were *asking* for the prompt |
| the board resetting every 15–16 s across four different images | a debug hold flag living at a fixed address in DRAM, surviving the very reset it asked for, and stopping the watchdog pet on every later generation |
| the GPU rendering black | see the cacheable-`dma_alloc_coherent` bug above |
| five panics bringing up KMS | a NULL parent; an unnamed kernel object surfacing as `-ENOMEM`; a NULL function table that `WARN_ON(!funcs->destroy)` *dereferences*; the PCI assumption; a DMA shim with no NULL check |

Working assumption, earned: when something breaks here, suspect **a constant, a
cache attribute, a lifetime, or a clock** before you suspect stage-2, the GIC or
the scheduler.

### Instruments lie more often than code

The expensive mistakes were never bad code. They were confident readings of the
wrong thing:

- **Banked registers.** `HCR_EL2`, `MDSCR_EL1`, `DBGB*`, GIC state read over the
  debug channel are answered by *CPU1* and say nothing about the guest's core.
  Four wrong diagnoses, including a "dead guest timer" that was CPU1's own bank.
- **A stale frame read as live.** `gr` shows the last *saved* guest frame, not
  the live PC. Moving-vs-frozen needs two samples — which is why `triage.py`
  exists and comes first.
- **A dead channel's zeros read as data.** Sixty consecutive polls of
  `ticks=0 init_done=0 timer=FROZEN` were read as a nine-minute generation that
  had failed to start its guest. No such generation existed: the board died 16 s
  in, and that build publishes no tick counter anyway — its own healthy status
  line says so.
- **A leak that was arithmetic.** Wired-page counts sampled around a workload
  gave 4, 228, 1041, 1727 and 60868 pages for identical work — the last
  physically impossible, with only 86 MB free. Measured around the *module*
  instead, everything came back: net +4 pages. A real defect surfaced during the
  hunt, but the leak never existed and a commit claiming it had to be retracted.
- **A checksum proving the wrong property.** md5 on a pushed file proves
  *transfer*, not *freshness* — a failed build left stale binaries matching
  perfectly on both sides. And a push that verified fine read back later with the
  right size and a different hash, because the board crashed before the data
  reached flash. `kldload` then failed with no kernel message at all, which looks
  exactly like a link error.

The one-line rule: **before believing a number, ask what would make this
instrument produce it on a healthy system.**

---

## What's next

- **A real vblank.** EL2 owns the display interrupt, so the KMS driver's vblank
  source is currently a timer at the mode's refresh rate. Events and pacing work;
  the phase is not locked to the panel. The fix is for EL2 to publish a vblank
  counter — the register file has room.
- **A desktop.** X11 and Wayland can talk to the KMS device now; neither is
  installed yet. The catch is packaging, not code: the guest's Mesa is hand-built
  with the Mali driver, and FreeBSD's packaged `mesa-dri` ships no gallium
  drivers at all, so an unguarded `pkg install` would replace a working stack
  with one that cannot drive this GPU.
- **Video decode.** The SoC's VPU is untouched. FreeBSD has neither a driver nor
  the framework Linux's is built on, so it is a project, not a finishing touch.
- **Upstreaming.** Eight patches are written and ready to submit.
- **Extracting the FreeBSD Mali port.** Every reference to this project inside it
  is a *comment*, not code — so it can ship as a standalone deliverable that
  outlives this board. See `../../bsdOS/hal/lima/LOOSE-ENDS.md`.

## Read next

- `war-stories.md` — twelve incidents in full, wrong first hypotheses included,
  which are usually the useful part.
- `autoboot-no-cable.md` — how the cable stopped being part of the boot path, and
  the two ordinary problems that had been mistaken for one exotic one.
- `../DEBUG_RULES.md` — the rules, and what each one cost.
