# Why this exists, and what came out of it

This is the rationale-and-retrospective document. It answers three questions
that the other docs deliberately do not:

- **why build this at all**, when a hypervisor on an ARM SoC is not a novel idea;
- **what was actually produced**, in numbers rather than adjectives;
- **what the accumulated bugs taught**, which turned out to be the most portable
  thing here.

For the plan and the acceptance numbers see `../ROADMAP.md` (§2 is the v1 gate).
For individual incidents in full detail, with file:line citations, see
`war-stories.md` — this document does not repeat them, it says what they add up
to. For the operating rules see `../SESSION-RULES.md` and `../DEBUG_RULES.md`.

---

## 1. The actual motivation

The interesting claim was never "an EL2 hypervisor exists". It was:

**A substrate on real, unfamiliar silicon where you can see everything.**

Bring-up on this board starts with no UART header, no JTAG, and no reference
hypervisor for the SoC. Under those conditions the binding constraint is not
architecture, it is observability: every hour spent guessing is an hour not
spent fixing. So the project inverted the usual order and built the
instrumentation as a first-class subsystem, on a dedicated core, with the
explicit property that **it outlives the guest**:

- CPU1 runs the debug monitor and the EMAC channel and owns the hardware
  watchdog, so a wedged or panicking guest on CPU0 does not take the channel
  with it (`smp.c`, `dbgmon.c`, `wdt.c`);
- the guest's console is a trapped UART, so its output is captured by EL2 and
  survives into the next boot generation — see `bzdctl.py console --postmortem`;
- state worth keeping is published into fixed breadcrumb windows in hv-scratch
  with a documented address map (`hv_addrmap.h`), so a post-mortem is a read,
  not a re-run;
- the watchdog resetting the board is **by design**, not a failure: an
  unattended board recovers itself instead of waiting for a human
  (`../SESSION-RULES.md` R2).

Two architectural choices follow from the same instinct rather than from
performance:

- **Static partitioning with `HCR_EL2.IMO=0`** — the guest owns its own
  interrupt delivery. This gave up the interesting-sounding vGIC in exchange for
  a system whose failure modes are legible. That trade was re-litigated three
  times and reverted twice; see `war-stories.md` §1 and §9.
- **The hypervisor owns the display; the guest gets a window inside it.** Not a
  compromise — it is what makes an accelerated guest safe. See §4 below.

---

## 2. What was produced

Measured, not estimated. Each line is reproducible from the tree.

**Hypervisor and guest.** FreeBSD 15.1 arm64 boots to userland under EL2 with
virtio-blk on real eMMC and virtio-net bridged over the board's EMAC. A second
guest (Zephyr) runs concurrently on CPU3 in the `dual` build
(`docs/dual-guest.md`).

**v1 gate** (`../ROADMAP.md` §2) — three of four criteria closed:

| criterion | state |
|---|---|
| 100 clean boot cycles in a row | CLOSED 2026-08-04 (streak 100, 193 reloads / 188 clean) |
| 20 break-glass resets in a row | CLOSED 2026-08-11 (20/20, 0 failures) |
| isolation, incl. W^X on guest DRAM | CLOSED 2026-08-20 — see below |
| 72 h soak under continuous disk load | **open** (harness ready; smoke run only) |

The isolation item is worth naming because it closed on a measurement, not an
argument. Dynamic W^X (`stage2.h`, `STAGE2_WX_DYNAMIC`, now default on) flips a
4 KiB page to read-execute on its first instruction fetch and back on the next
write. The open question was never the flip logic — that was proven board-free in
`test_stage2_tables.c` — but whether a bounded L3 pool survives a real guest's
`kldload` fragmentation, which the header itself said could not be answered
without hardware. Under the full workload (boot to multiuser, `kld_list` loading
`drm.ko` + `lima.ko` + `bzfb.ko`, then a 2421-draw GL workload and 12052
presented frames):

    pool_used      = 27 of 64      (2.4x headroom)
    pool_exhausted = 0
    flip_count     = 4045

Exhaustion still fails **open** — one 2 MiB block degrades to W+X rather than
hanging the guest — so a heavier workload loses enforcement on a block instead of
breaking. Full analysis: `wx-enforcement.md`.

**Graphics**, from "nothing renders" to a standard interface:

- the Mali-400 renders: `hal/lima/tests/limabench.c` passes with sampled
  textures, 2421 draw calls, depth testing and alpha blending;
- zero-copy presentation: the guest renders into a `gbm_surface` and the display
  engine fetches *that* buffer — ~1030 fps at 1120x276, and across the session
  **153471 addresses accepted by EL2 with zero rejected**
  (`../../bsdOS/hal/bzfb/tests/README-zerocopy.md`);
- a real DRM/KMS device in the guest since 2026-08-20 (`hal/bzkms`,
  `/dev/dri/card1`): PRIME import plus `drmModePageFlip`, 59.7 fps **paced by
  `DRM_EVENT_FLIP_COMPLETE`**. The drop from 1030 to 59.7 is the point, not a
  regression: frames are now paced by the display instead of fired blind, which
  is what any real client needs and what a private ioctl could not offer.

**Operating model.** The board autoboots the entire stack — U-Boot, hypervisor,
guest — with no host interaction, verified on a genuine cold power-on
(`autoboot-no-cable.md`). Swapping the hypervisor is now "install the ELF into
`/opt/bzdos/tftpboot` and reset over EMAC". That changed day-to-day work more
than any single feature.

---

## 3. What the bugs taught

### The interesting layer works; the plumbing lies

Almost every expensive bug was in a seam, not in the virtualization:

| symptom | actual cause |
|---|---|
| years of guest filesystem corruption | `rd_cntpct()` reading **backwards** → unsigned underflow → instant spurious timeout → a retried `CMD24` mid-data-phase |
| ~17% inbound packet loss no board counter could see | the PHY forced to 100/full with autoneg off, so the switch fell back to **half** duplex |
| an interactive `mountroot>` prompt on every boot | `kload.c` passed `0x1` calling it `RB_SINGLE`; `0x001` is `RB_ASKNAME`. We were requesting the prompt |
| the board resetting every 15–16 s across four different images | `wdt_debug_hold` lives at a fixed address in hv-scratch **DRAM**, so a hold survived the very reset it asked for and stopped CPU1 petting on every later generation |
| the GPU rendering black | LinuxKPI's `dma_alloc_coherent` returned `VM_MEMATTR_DEFAULT` — **write-back** on arm64 — for the one allocator whose contract is "no cache maintenance needed" |
| five separate panics bringing up KMS | a NULL parent; an unnamed `kobj` surfacing as `-ENOMEM`; NULL `drm_encoder_funcs` (`WARN_ON(!funcs->destroy)` **dereferences** it); drm-kmod assuming every DRM device is PCI; LinuxKPI's DMA API dereferencing `dev->dma_priv` with no NULL check |

The pattern is consistent enough to be a working assumption: when something
fails on this board, suspect a constant, a cache attribute, a lifetime, or a
clock **before** suspecting stage-2, the GIC, or the scheduler.

### Instruments lie more often than code

This is the more valuable lesson, and it cost more. A partial list of
confidently wrong conclusions, each caused by measuring the wrong thing:

- **Banked registers.** `HCR_EL2`, `MDSCR_EL1`, `DBGB*`, GIC PPI state read over
  the debug channel are answered by **CPU1** and say nothing about the guest's
  core. This produced at least four wrong diagnoses, including a "dead guest
  timer" that was CPU1's own bank (`../ORIENTATION.md`, rule 4).
- **A stale frame read as live.** `gr` shows the last *saved* guest frame, not
  the current PC. "Moving vs frozen" needs two samples — hence `triage.py`
  existing and being mandatory before `gr`.
- **A dead channel returning zeros.** 60 consecutive monitor polls printed
  `ticks=0 init_done=0 timer=FROZEN` and were read as a nine-minute generation
  that had failed to start its guest. There was no such generation: the board
  had died 16 s in and the zeros were a dead channel. The same build's own
  healthy status line says `timer n/a (this build publishes no tick counter)`.
- **A leak that was a measurement error.** `v_wire_count` sampled around a
  workload gave 4, 228, 1041, 1727 and 60868 pages for identical work — the last
  physically impossible, since only 86 MB was free. Measured around the
  **module** instead (load → run → unload) everything came back: net +4 pages.
  A real defect turned up while chasing it (an unbalanced wire reference), but
  the leak did not exist, and a commit claiming it had to be retracted.
- **A checksum that proves the wrong property.** md5-verifying a pushed file
  proves *transfer* integrity, not *freshness*: a failed build left stale
  binaries matching perfectly on both sides. Separately, a push that verified
  correctly read back later with the right size and a different md5, because the
  guest crashed before the data reached eMMC — and `kldload` then failed with no
  kernel message at all, which looks exactly like a link error.

The rules that came out of this are in `../DEBUG_RULES.md` and
`../SESSION-RULES.md`. The short form: **before believing a number, ask what
would make this instrument produce it if the system were fine.**

---

## 4. What is reusable beyond this board

Three things here are not board-specific and are the most likely to outlive it.

**A hypervisor-owned display with a validated flip doorbell.** The guest gets
zero-copy accelerated output *without owning the display*: it writes a physical
address to a trapped MMIO register, and EL2 validates the **whole extent**
against guest DRAM and its own carve-outs before repointing the DE2 layer
(`scanout.c`, `scanout_addr_allowed()`; design in `zero-copy-scanout.md`). This
matters because the display engine is an IOMMU-less DMA reader — an unchecked
address would put hypervisor memory on screen. The security property costs one
range check, and the same doorbell turned out to be the only KMS primitive
actually needed, so `hal/bzkms` is a thin driver over it rather than a rewrite.

**A FreeBSD port of the lima DRM stack.** Independently valuable: it includes a
shmem GEM helper that drm-kmod does not ship at all, and a newbus↔LinuxKPI
`platform_device` bridge that FreeBSD lacks entirely (its own header is a stub
whose `platform_driver_register()` returns `-ENXIO`, making every DT driver's
probe unreachable dead code). Every reference to bzdOS inside that port is in a
**comment**, not in code, so it is extractable as a standalone deliverable —
see `../../bsdOS/hal/lima/LOOSE-ENDS.md`.

**Defects found in other people's trees.** Written up under
`../../bsdOS/hal/lima/patches/UPSTREAM-*.md`: `dma_alloc_coherent` handing out
cacheable memory on arm64; `nsegments = 1` making multi-page `dma_map_sg`
impossible; `sgl->dma_map` left stale on every map failure path; a missing gate
flag on every `ccu_a64` fractional clock (`PLL_GPU`); transposed gate/lock
arguments in `ccu_a83t`; and drm-kmod's `drm_add_busid_modesetting()` assuming
every DRM device is PCI. None are submitted — that needs the author's own
accounts — but they are diagnosed and patched locally.

---

## 5. Honest state, and the risks

- **The last v1 number is calendar, not engineering.** 72 h of soak, on final
  code, uninterrupted.
- **No real vblank in the guest.** EL2 owns the interrupt, so `hal/bzkms`'s
  vblank source is a callout at the mode's refresh rate: flip events and pacing
  work, but their phase is not locked to the panel. The fix is for EL2 to publish
  a vblank counter; the scanout register file has room.
- **No X11 or Wayland in the guest yet**, and installing them is not free: the
  guest's Mesa is hand-built with the lima driver into `/usr/local`, while
  FreeBSD's packaged `mesa-dri` ships no `*_dri.so` at all — so any `pkg install`
  pulling `mesa-libs` would replace a working stack with one that cannot drive
  this GPU. It needs `pkg lock` or a separate prefix.
- **The VPU (Cedrus) is untouched.** There is no FreeBSD driver and no V4L2
  stateless M2M framework to build one on; that is a from-scratch project, not a
  finishing touch.
- **One board, one port.** Velocity depends on a single cable and a single
  `flock`; two processes on that port make a healthy board look dead. Several
  hours have been lost to exactly that.
- **Validation leans on live runs.** The board-free suites are good for the
  *harnesses* (`soak72` dry-run 36/36, `breakglass` dry-run 13/13) and for table
  logic (`test_stage2_tables.c`), but the hypervisor itself is mostly proven by
  hardware sessions. A regression is caught by a person, not by CI.
- **Tree entropy.** Duplicated patch directories, stale intermediate docs, and
  documents that contradict the current state in places. It is the predictable
  cost of moving this fast, and it is worth paying down deliberately.

---

## 6. If you read one more thing

- `../ROADMAP.md` §2 — the gate, with the numbers and what is deliberately out
  of scope.
- `war-stories.md` — twelve incidents in full, including the wrong first
  hypotheses, which are usually the useful part.
- `../DEBUG_RULES.md` — the rules that earned their place the expensive way.
- `autoboot-no-cable.md` — why the board no longer needs a USB cable to boot,
  and the two ordinary problems that were mistaken for one exotic one.
