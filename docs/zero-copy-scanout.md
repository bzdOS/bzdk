# Zero-copy GPU scanout: the hypervisor-side design

Written 2026-08-11, board-free (no `ssh`/`ttyACM0`/`bzdctl.py`/`chimpd.py`/`hvdbg.py`/
`guest_sh.py`/`reliable_load.py`/EMAC tooling was used while researching or writing
this, or at any point in this task — a hardware gate was running throughout).
Everything below is either a citation to a file this repository already has, or a
result of a hosted build/test run on this machine. Where something cannot be
checked without the board, it is named as such rather than assumed.

## 0. Bottom line

**True zero-copy — the Mali PP's write-back unit writing directly into the exact
physical pages HDMI scans out, no CPU copy — is achievable, and most of the
mechanism already exists or is now implemented.** Concretely:

- The DE2 scanout base address (`DE_UI1_CFG0_TOP_LADDR`) **can** be reprogrammed at
  runtime with no display re-initialisation, via the same two-register
  write-then-commit sequence this codebase's own bring-up code already repeats
  three times (§1). Implemented as `hdmi_set_scanout_addr()`.
- Double buffering **fits** the existing 8 MiB `hv-fb` DTB reservation, for the
  geometry `hdmi_init()` is actually run with today — but would **not** fit, and
  would collide with a named neighbour, at 1080p (§2).
- Whether the GPU's own write-back DMA needs the guest's stage-2 tables opened at
  all is answerable precisely: **no, the write itself does not** — the A64 has no
  SMMU, and the Mali-400 GPU is confirmed (§4.1, decompiling the live boot DTB) to
  already be a guest-controlled, stage-2-bypassing DMA bus master, independent of
  anything in this task. What genuinely *is* still open is whether the **guest
  kernel's own import machinery** (building an sg_table, or a future CPU-mmap
  fallback) needs CPU read/write reachability of the pixel range — and there is
  now a real, already-written counterpart document (found during this task, not
  produced by it — see §4.4) that explicitly asks for that reachability, for
  reasons this document evaluates rather than accepts or rejects outright.
- The guest-facing contract (an address, geometry, a flip verb) is implemented as a
  new trapped-MMIO doorbell device, `scanout.c`/`scanout.h`, that needs **zero**
  `stage2.c` changes (§5) — it reuses the 2 MiB block `vblk_emmc.h`/`vnet_emac.h`
  already leave stage-2-invalid. This is a **better** realisation of the flip
  contract than the placeholder the counterpart guest-side document sketched, and
  §4.4 explains precisely why.
- Two pre-existing, real bugs were found and fixed as a direct consequence of the
  reading this task required (§6.3) — both are stale mirrors of `hdmi.c`'s own
  2026-07-25 breadcrumb relocation that nobody had updated.

Everything this document could not verify without hardware is collected in §8.

---

## 1. Where the scanout base is programmed, and can it change at runtime

**Yes, at runtime, without re-initialising anything else.** The DE2 mixer's UI
layer target address is one plain 32-bit register,
`DE_UI1_CFG0_TOP_LADDR = DE_UI1_BASE + 0x10` (`hdmi.c:264`), and the codebase
already reprograms it and "commits" the change via a second register,
`DE_GLB_DBUFF = DE2_MUX1_BASE + 0x0008` (`hdmi.c:234`), **three separate times** in
the existing, hardware-confirmed bring-up path — this is not a new pattern
invented for this task, it is the pattern the working driver already uses:

| Call site | What it does |
|---|---|
| `hdmi.c:578` (`stage_de2()`) | `wr32(DE_UI1_CFG0_TOP_LADDR, (uint32_t)HDMI_FB_BASE);` — the initial, boot-time program |
| `hdmi.c:582` (`stage_de2()`, tail) | `wr32(DE_GLB_DBUFF, 1);` — "apply (double-buffer flip register, sunxi_de2.c:177)" |
| `hdmi.c:920` (`stage_scanout()`, tail) | `wr32(DE_GLB_DBUFF, 1);` again — "re-apply the DE2 mixer double-buffer flip now that the whole downstream pipe is live" |
| `hdmi.c:1029` (`hdmi_relock()`, tail) | `wr32(DE_GLB_DBUFF, 1);` a third time, after a live PHY relock, **while the guest is running**, from CPU1 |

That third call site is the load-bearing evidence for "no re-init needed": per
`hdmi_relock()`'s own comment (`hdmi.h:142-153`), it runs **post-boot, with the
guest already up**, and confirms live on real hardware (2026-07-25) that "every
CCU/DE2/TCON/PHY-config register `hdmi_init()` programmed... still reads back
bit-for-bit identical... only `PHY_STATUS`'s own lock bit has dropped." In other
words: DE2's mode-set registers are **stable** once programmed; nothing in the
existing driver ever needs to re-run clock/DE2/TCON/PHY bring-up to change what
address the mixer reads from. `DE_GLB_DBUFF` being written after every single
config change, not just at the two boot-time call sites, is itself the evidence
that it is a **commit/strobe register**, not a one-time init flag — this is the
mechanism a runtime flip needs, and it was already there.

**Implemented** (`hdmi.c:1043-1054`, declared `hdmi.h`):

```c
void hdmi_set_scanout_addr(uint32_t pa)
{
	wr32(DE_UI1_CFG0_TOP_LADDR, pa);
	wr32(DE_GLB_DBUFF, 1);   /* commit -- same "apply" strobe as every other
	                          * mode-set write in this file */
	g_scanout_addr = pa;
	bc_write(8, pa);
}
```

`hdmi_fb()`/`hdmi_width()`/`hdmi_height()`/`hdmi_stride()` are **unchanged** — they
still always name buffer 0 (`HDMI_FB_BASE`), so `hud.c` and `hdmi_demo()` keep
working exactly as today whether or not anything ever calls
`hdmi_set_scanout_addr()`. A new accessor, `hdmi_scanout_addr()`, returns the
address **currently live** (defaults to `HDMI_FB_BASE`, i.e. correct even if never
called), for anything that needs to know what is actually on screen right now
rather than what buffer 0 always is.

**What is not verified**: whether `DE_GLB_DBUFF`'s commit is *immediate* or
*latched at the next vblank*. The three-call-site pattern above (config write,
then strobe, repeated per batch of changes) is exactly how display-controller
shadow/double-buffer registers are conventionally used, and is the reason this
document assumes a vblank-latched commit rather than a mid-scan splice — but no
comment in this codebase, and nothing in the ported U-Boot source cited at
`hdmi.c:29-31` (`sunxi_de2.c` lines 33-59/61-178), states the exact latch boundary
in so many words, and no register-level Allwinner datasheet was consulted. This
is the one piece of §3's tearing analysis this document cannot close without
either a datasheet or a real-hardware timing capture (an oscilloscope/logic
analyser on the HDMI signal, or a frame-counter readback if the DE2 mixer on this
SoC exposes one — not checked).

---

## 2. Double-buffering arithmetic — computed for the mode `hdmi_init()` actually runs

**The call sites, not an assumption.** Both places that call `fb_init()` —
`hud.c:282` and `hdmi.c:1047` (`hdmi_demo()`) — call it as
`fb_init(hdmi_fb(), hdmi_width(), hdmi_height(), hdmi_stride())`, i.e. through the
accessors, never a literal. Those accessors return `HDMI_MODE_HACTIVE` (1280) /
`HDMI_MODE_VACTIVE` (720) / `HDMI_MODE_HACTIVE` again (`hdmi.c:1029-1031`) — **720p,
not 1080p**. `hdmi.h:70-78` documents why: "the 1080p bump (148.5 MHz...) never
locked the DWC-HDMI PHY on real hardware... 720p is the timing this driver was
actually 'confirmed working to scanout stage 6 on the physical monitor'."

**The arithmetic:**

```
buffer size   = 1280 * 720 * 4 (XRGB8888)      = 3,686,400 bytes  (0x384000)
              = exactly 900 * 4096              -- page-aligned with zero remainder
two buffers   = 7,372,800 bytes                 (0x708000)
hv-fb window  = 0x800000                        = 8,388,608 bytes (8 MiB, confirmed
                                                   against the live boot .dtb below)
slack         = 8,388,608 - 7,372,800 = 1,015,808 bytes ≈ 991.99 KiB
```

**It fits, with about 992 KiB to spare.** The 8 MiB reservation was sized (per
`hdmi.h:53-61`'s original comment) against a *single* 1080p buffer
(1920×1080×4 = 8,294,400 bytes, "~7.91 MiB"); nobody has revisited that sizing note
since the mode moved to 720p, so the document you are reading is the first place
this is written down against the mode that is actually live.

**Confirmed against the live boot artifact, not just the source tree**: decompiling
`/opt/bzdos/tftpboot/bananapi-min.dtb` (read-only; `dtc -I dtb -O dts`, output to
the session scratchpad, nothing written back) shows:

```
hv-fb@4d000000 {
        reg = <0x4d000000 0x800000>;
        no-map;
};
```

— `0x800000` bytes exactly, matching `stage2.c:451`'s `HVFB_SIZE` and this
document's arithmetic above.

**Buffer layout implemented** (`hv_addrmap.h`, new section, mirroring `hdmi.h`'s
geometry the same way this file already mirrors `HDMI_FB_BASE` for the LOW-BLOCK
section):

```c
#define HVMAP_FB_BASE            0x4D000000UL
#define HVMAP_FB_WINDOW_SIZE     0x00800000UL

#define HVMAP_FB_BUF_W           1280UL
#define HVMAP_FB_BUF_H           720UL
#define HVMAP_FB_BUF_BPP         4UL
#define HVMAP_FB_BUF_STRIDE      (HVMAP_FB_BUF_W * HVMAP_FB_BUF_BPP)
#define HVMAP_FB_BUF_SIZE        (HVMAP_FB_BUF_STRIDE * HVMAP_FB_BUF_H)

#define HVMAP_FB_BUF0_BASE       (HVMAP_FB_BASE + 0UL)                  /* == HDMI_FB_BASE */
#define HVMAP_FB_BUF1_BASE       (HVMAP_FB_BASE + HVMAP_FB_BUF_SIZE)

_Static_assert(HVMAP_FB_BUF_SIZE % 0x1000UL == 0UL, "...page-aligned...");
_Static_assert(HVMAP_FB_BUF0_BASE + HVMAP_FB_BUF_SIZE <= HVMAP_FB_BUF1_BASE, "...overlap...");
_Static_assert(HVMAP_FB_BUF1_BASE + HVMAP_FB_BUF_SIZE
               <= HVMAP_FB_BASE + HVMAP_FB_WINDOW_SIZE, "...overflows the 8 MiB reservation...");
```

Buffer 0 is, **by construction**, the same address `HDMI_FB_BASE`/`HVFB_BASE`
already use — this is what keeps every existing single-buffer caller correct with
zero changes (§6.1). Buffer 1 sits immediately after it, zero gap, zero overlap;
these three asserts were confirmed to have real teeth, not just be present: a
scratch copy of the header with `HVMAP_FB_WINDOW_SIZE` shrunk by 1 MiB was compiled
and failed exactly as expected:

```
hv_addrmap_broken_test.h:490:1: error: static assertion failed: "double-buffered
hv-fb layout overflows the 8 MiB hv-fb DTB reservation -- see
docs/zero-copy-scanout.md's fit arithmetic"
```

### If 1080p is ever restored, here is what it collides with

The task asked this explicitly, so: at 1920×1080×4 = 8,294,400 bytes/buffer, two
buffers need 16,588,800 bytes ≈ 15.83 MiB. Rounding to a clean 16 MiB (`0x1000000`)
just barely fits (184 KiB slack) — but **16 MiB starting at `0x4D000000` ends at
exactly `0x4E000000`**, which is `zstage.h:62`'s `ZSTAGE_LOW_PA` — the documented
TFTP landing window for the `dual` (Zephyr-on-CPU3) build's second-guest ELF,
inside the range `zstage.h:45` calls out as "`[0x4E000000, 0x50000000)` — 32 MiB,
verified unclaimed by a full-tree grep." Growing `hv-fb` to 16 MiB would need that
grep re-run and either the zstage window moved or the two documented as adjacent
on purpose — not a silent one-line size bump.

---

## 3. Tearing: the mechanism, and what v1 does not solve

**Mechanism**: exactly the double-buffer flip the task anticipated. Two buffers
(§2); the guest's GPU renders into whichever one is *not* currently the front
buffer; a doorbell write (§5) tells the HV to make the just-rendered buffer the
new front, which calls `hdmi_set_scanout_addr()` (§1) — write `TOP_LADDR`, strobe
`DBUFF`. The flip is triggered from **CPU0, synchronously inside the guest's own
trapped MMIO write** (`scanout_mmio_fault()`, §5) — there is no CPU1 involvement in
the flip path itself (CPU1 is where `hdmi_relock()` and `hud_update()` already
live, and neither touches the flip state — see the concurrency note in §5.3).

**What v1 does NOT solve, stated as plainly as the task asked:**

- **No vsync/vblank interrupt.** `FLIP_COUNT` (§5.2) increments the instant the
  trapped write is serviced — proof "the HV accepted the request", not proof "the
  new frame is on the wire." Whether the mixer has actually latched
  `DE_GLB_DBUFF`'s commit by the time `FLIP_COUNT` is observed to have changed is
  exactly the unverified latch-timing question from §1.
- **No fences.** Nothing here waits for the Mali PP's write-back job targeting the
  back buffer to finish before honouring a flip to it. That ordering is the
  guest driver's job (and per the counterpart document's own §2, this port's
  Mali-side L2-cache-flush is currently a verified no-op — see §4.4 below — so the
  "job actually finished and is visible in DRAM" question has an open gap on the
  *guest* side too, independent of anything the HV can fix).
- **No back-pressure.** Flipping twice inside one frame period just rewrites
  `TOP_LADDR` twice; whichever value is live at the next mixer read wins. This can
  skip a frame; it cannot corrupt one, because every value `FLIP_REQUEST` can
  produce is one of exactly two fully-valid, fully-sized buffer addresses
  (`scanout_decide_flip()`, §5.2 — this bounded-index property is deliberate, see
  §7).
- **PITCH/format/size are not part of the flip.** Both buffers share one
  `hdmi_init()`-programmed stride/format; only the base address changes. A future
  variable-geometry scanout would need `DE_UI1_CFG0_PITCH` folded into the flip
  contract too — not attempted here, not needed for two same-shaped buffers.

---

## 4. The stage-2 question

### 4.1 Does the Mali MMU make the guest's stage-2 mapping relevant to the GPU's own DMA? No.

`docs/dma-bypass-stage2.md` already established the general fact: "The A64...has
**no SMMU/IOMMU** in front of its peripheral bus masters" — stage-2
(`VTTBR_EL2`/`VTCR_EL2`) governs only translations the **CPU's own table-walker**
performs; a peripheral's DMA engine issues bus transactions straight to the memory
controller, "never through the CPU's stage-2 (or stage-1) MMU at all"
(`docs/dma-bypass-stage2.md:16-22`).

That document's own device inventory (its table, written 2026-07-24) does **not**
list the Mali GPU — because at the time it was written, the GPU was not yet
enabled in the DTB. Decompiling the **current** live boot `.dtb` for this task
found:

```
gpu@1c40000 {
        status = "okay";
        compatible = "allwinner,sun50i-a64-mali", "arm,mali-400";
        reg = <0x1c40000 0x10000>;
        interrupts = <... 0x61 ... 0x62 ... 0x63 ... 0x64 ... 0x66 ... 0x67 ... 0x65 ...>;
        interrupt-names = "gp", "gpmmu", "pp0", "ppmmu0", "pp1", "ppmmu1", "pmu";
        ...
};
```

`status = "okay"` — the GPU is enabled and, per `stage2_build_mmio_tables()`'s own
L3 split (`stage2.c:284-321`), it sits inside the **same** 2 MiB block as UART0
(`0x1c40000` is within `[0x1c00000, 0x1e00000)`), and every 4 KiB page in that
block is a plain identity `Device-nGnRE` passthrough **except** the UART0 page and
the two GICC→GICV redirect pages. The GPU's own 16 register pages are not among
those exceptions — the guest CPU has **full, real, unmediated, direct MMIO access
to the actual Mali-400 hardware**, matching the task's own framing ("A Mali-400
GPU is now working in the FreeBSD guest").

The `*mmu*` interrupt names (`gpmmu`, `ppmmu0`, `ppmmu1`) are the Mali-400's **own**
per-core on-chip MMU — a GPU-internal address translator whose page tables the
guest's lima driver builds in ordinary, already-guest-accessible DRAM. It is not,
and cannot be, a system-level IOMMU: it translates the GPU's own "virtual" job
addresses to physical bus addresses *before the GPU issues the transaction*, but
nothing external polices what physical address the guest is allowed to put in
those page tables.

**Conclusion, stated plainly**: the guest **already had**, before this task and
independent of it, the raw hardware capability to program the real Mali GPU to
write to *any* physical address in the low 4 GiB — including `hv-image`
(`0x42000000`) and `hv-scratch` (`0x50000000`), the exact windows `stage2.c`'s own
"hard boundary" comment (`stage2.c:391-397`) says are protected — because that
protection, by its own documented caveat (`stage2.c:262-270`), only ever gated the
CPU path. This document extends `docs/dma-bypass-stage2.md`'s inventory with this
one new row (Mali GPU: enabled, real DMA master via its own MMU, guest has full
raw MMIO control) — the general finding is unchanged, this is a new confirmed
instance of it.

### 4.2 So does hv-fb's CPU-side stage-2 exclusion actually matter today? Partially, and conditionally.

`stage2.c`'s `HVFB_BASE`/`HVFB_SIZE` carve-out (`stage2.c:441-453`, applied at
`stage2.c:469-471`) is real and, when active, does exactly what the task's framing
assumes: it makes the guest **CPU** take a stage-2 fault on any load/store to
`hv-fb`, same mechanism as `hv-image`/`hv-scratch`. But it is entirely
`#ifdef HV_HDMI`-gated — and **`HV_HDMI` is not defined by any target in this
Makefile today**:

```
$ grep -n "HV_HDMI" Makefile
(no output)
```

This is intentional and documented elsewhere in the tree (not a bug): `ROADMAP.md`
calls it "opt-in `-DHV_HDMI`"; `snapshot.c:276` states outright "neither `dbg` nor
`snapshot-qemu` currently defines HV_HDMI, grep the Makefile"; `test_stage2_tables.c:334`
calls it "`#ifdef`'d out of every target except `dbg -DHV_HDMI`." The supported way
to build with it is `make dbg EXTRA_CFLAGS=-DHV_HDMI` (`Makefile:36`,
`CFLAGS := ... $(EXTRA_CFLAGS)`) — confirmed this pass by a from-clean rebuild with
that flag (§6.2).

**What this means concretely**: whether `hv-fb` is CPU-reachable from the guest
*on the board right now* depends entirely on whether whatever image is currently
deployed there was built with `-DHV_HDMI`. This document could not determine that
without querying the live board (forbidden by this task's hard constraints); the
one local, read-only artifact checked (`/opt/bzdos/tftpboot/microkernel-dbg.elf`,
dated 2026-07-16) predates `hdmi.o`/`hud.o`/`fb.o` even existing in `DBG_OBJS`
(`nm` on it finds none of their symbols), so it is stale and proves nothing about
the currently-running image. **A human with board access should check this** —
e.g. via the existing `stage2_isolation_selfcheck()`'s `hvfb_f` breadcrumb
(`STG2` window, index 10 — `stage2.c:966`) once a build with `-DHV_HDMI` is
confirmed live, or simply by confirming which flags produced whatever is
deployed.

### 4.3 What does the guest actually need, then?

Two genuinely separate things, with two different answers:

1. **The GPU's own write-back DMA reaching `hv-fb`.** Needs nothing from stage-2 —
   established in §4.1, unconditionally true regardless of `HV_HDMI`/the exclusion
   above. If the guest's Mali MMU page tables (built in ordinary guest DRAM, via
   ordinary already-permitted CPU stores) contain `hv-fb`'s physical address as a
   target, the GPU will write there. This is the actual "zero-copy" data path and
   it needs **no HV-side stage-2 change at all**.
2. **A way to learn the address and request a flip.** This is a CPU-side
   operation (the guest kernel driver does need to *read* an address and *write* a
   flip request), and it is fully solved **without touching `hv-fb`'s own
   exclusion at all** — see §5. The doorbell device lives in an entirely different,
   already-harmless-to-trap physical range.

**What is genuinely still open** is a *third* thing this document did not invent
and cannot close alone: whether the guest kernel's own dma-buf/sg_table plumbing
needs `hv-fb`'s pixel-data range to be CPU-reachable for reasons internal to that
machinery (not for the GPU's DMA, which doesn't need it). §4.4 covers this.

### 4.4 A real counterpart document already exists — reconciling with it

While researching this task (read-only; nothing under `hal/lima/*` was edited),
`/opt/bzdos/bsdOS/hal/lima/SCANOUT-IMPORT.md` was found: a parallel, already-written
design for the **guest** side of this exact feature, dated the same day, also
produced board-free. It is real prior art, not a hypothetical, and this section
reconciles this document with it rather than silently duplicating or ignoring it.

**Where the two documents agree, independently:**
- The physical range (`0x4D000000`, `0x800000`) and the live geometry
  (1280×720, XRGB8888, stride 5120) — their `lima_hvfb.c:114-138` hardcodes the
  identical numbers this document derives in §2, citing the same `hdmi.h`/`hdmi.c`
  lines.
- That `hv-fb`'s stage-2 exclusion is the real blocker for anything beyond the
  GPU's own DMA (their §4.2 states this "is the fact that most changes the shape
  of 'what the HV needs to do'" — matching §4.2 above, reached independently).
- That format/stride/pitch line up with no forcing (their §3.2's Mali PP
  write-back register analysis — `pitch` must be a multiple of 8 bytes; 5120÷8=640
  exactly — cross-checked against the same `hdmi.c:569` `DE_UI1_CFG0_PITCH`
  program this document also cites in §1).

**Where this document's design does more than their placeholder, and why:**

Their §4.3 item 3 sketches a flip mechanism as "a new, dedicated 32-bit word...
immediately adjacent to the existing `BC_HDMI_BASE` breadcrumb," explicitly framed
as a placeholder ("This document does not pick the literal address: that is the
HV side's own address-map bookkeeping to make"). Their guest-side module
(`lima_hvfb.c:158`, `LIMA_HVFB_FLIP_BREADCRUMB_PA 0x50011820`) picked a concrete
stand-in address for that placeholder — which lands **inside `hv-scratch`**
(`0x50000000`–`0x501FFFFF`), a window `stage2.c`'s `HVSCR_L2_IDX` exclusion denies
the guest **unconditionally**, not gated behind `HV_HDMI` at all. Their own
function comment acknowledges this plainly: *"THIS FAULTS ON REAL HARDWARE TODAY,
BY DESIGN... there is no guest-side workaround; stage-2 is authoritative"*
(`lima_hvfb.c:453-461`).

Making that specific placeholder work would need **carving one more page out of
`hv-scratch`'s exclusion** — mechanically similar to the UART0/DMA-controller L3
splits, but a *new*, separate stage-2 change, on top of whatever `hv-fb`
reachability item 1 above already needs, and inside a window whose own
neighbouring breadcrumbs already have a documented history of undocumented
aliasing (`hv_addrmap.h:100-112`'s own audit).

**This document's `scanout.c` doorbell (§5) needs none of that.** It lives at
`0x0A002000`, inside the 2 MiB block `vblk_emmc.h`/`vnet_emac.h` already leave
**entirely stage-2-invalid** — confirmed by this task's own build (§6, zero
`stage2.c` changes were made). A guest write there already faults into EL2 *today*,
in every build, `HV_HDMI` or not, with no new exclusion required; `el2_trap`'s
existing data-abort dispatch chain just gained one more link
(`el2_exc.c`, mirroring the exact insertion `vnet_mmio_fault()` made alongside
`vblk_mmio_fault()`). **Recommendation: the guest-side flip notify should target
this MMIO doorbell instead of a bare DRAM word inside `hv-scratch`** — same
`pmap_mapdev_attr()`/cache-maintenance KPI shape their `lima_hvfb_flip_notify()`
already uses, just pointed at `SCANOUT_MMIO_BASE + SCANOUT_R_FLIP_REQUEST`
(`0x0A002024`) instead of `0x50011820`, and it needs no isolation-relevant stage-2
change to start working.

It also gives them something their v1 `LIMA_HVFB_IOC_GET_BUFFER` does not yet
have a way to ask for: their own §4.1 flags "a documented, not-yet-implemented"
runtime cross-check of the hardcoded PA constants against the DTB's `reserved-memory`
node, blocked on not having verified FreeBSD 15.1's linuxkpi can resolve that node
generically. `scanout.c`'s read-only registers (`SCANOUT_R_BUF0_ADDR` etc., §5.2)
are a strictly simpler way to get the same cross-check — one MMIO read each, no
DTB/`of_reserved_mem` plumbing at all — available whenever they want it, without
solving that open question first.

**Where this document leaves their open question open, on purpose:** their §4.3
item 1 asks the HV side to map the pixel-data range itself
(`[0x4D000000, 0x4D000000+size)`) into the guest's stage-2 tables as ordinary
guest-RW DRAM, so their kernel module's `sg_table`/dma-buf export machinery has
something valid to describe. This document's own reasoning (§4.3, item 1) is that
the GPU's DMA does not need this — but whether FreeBSD's dma-buf/PRIME plumbing
*itself* needs a CPU-valid mapping for reasons internal to that machinery (a
generic `begin_cpu_access`/cache-sync call that touches the range even when no
CPU access is semantically happening, or a `struct page`-array sanity check
somewhere in the VM subsystem) is a **guest-kernel-internal question this document
cannot answer from the HV side** — their own §1.4 is explicit that no PRIME import
of this kind has ever actually executed on this port. **This document does not
implement that stage-2 relaxation** (§7 states why), and treats it as the single
most important open item for whoever tries this on hardware next: try the import
with `hv-fb` still CPU-unreachable first; only widen the exclusion if it turns out
to be genuinely needed, and if so, prefer the narrowest sufficient range (their
sg_table today only covers one frame's worth — `LIMA_HVFB_FRAME_BYTES`,
3,686,400 bytes — not the full 8 MiB) over reopening the whole reservation.

---

## 5. What was implemented, and the exact contract for the guest side

### 5.1 Summary of files touched (all under `/opt/bzdos/microkernel`)

| File | Change |
|---|---|
| `scanout.c` / `scanout.h` (new) | The doorbell device: register file, flip decision logic, MMIO fault handler, breadcrumb |
| `hdmi.c` / `hdmi.h` | `hdmi_set_scanout_addr()` / `hdmi_scanout_addr()`; one new breadcrumb word |
| `hv_addrmap.h` | `HVMAP_FB_*` double-buffer geometry + asserts; `HVMAP_SCANOUT_BC` breadcrumb lane + asserts; fixed a stale mirror (§6.3) |
| `el2_exc.c` | One `#ifdef HV_HDMI`-guarded dispatch line for `scanout_mmio_fault()`, mirroring `vnet_mmio_fault()`'s own insertion |
| `main_dbg.c` | `#include "scanout.h"` + `scanout_init()` call, inside the existing `#ifdef HV_HDMI` block |
| `hud.c` | Fixed a stale breadcrumb-address mirror (§6.3) — found while reading this file as "an existing consumer of the framebuffer," unrelated to the new feature but in the same subsystem |
| `Makefile` | `scanout.o` added to `DBG_OBJS`; `test_scanout_regs` wired into `make test` |
| `test_scanout_regs.c` (new) | Hosted unit tests for the doorbell's pure decision logic |

**`hud.c`/`hdmi_demo()`'s existing single-buffer behaviour is unchanged by
default** — `hdmi_fb()` still always returns buffer 0, and nothing calls
`hdmi_set_scanout_addr()`/`scanout_flip()` unless the new doorbell is written to or
a human explicitly invokes it. `scanout_init()` only populates a register file and
writes a breadcrumb; it never touches `DE_UI1_CFG0_TOP_LADDR`.

### 5.2 The register contract (`scanout.h`)

A tiny, custom (not virtio-spec) memory-mapped device at
`SCANOUT_MMIO_BASE = 0x0A002000`, one 4 KiB page inside the 2 MiB block
`vblk_emmc.h`'s own comment already calls "spare for future virtio-mmio devices"
(`vblk_emmc.h:70-71`) — clear of `vblk_emmc`'s `0x0A000000..0x0A000200` and
`vnet_emac`'s `0x0A001000..0x0A001200`:

| Offset | R/W | Meaning |
|---|---|---|
| `0x00` | R | `MAGIC` = `0x53434e41` ("SCAN") |
| `0x04` | R | `VERSION` = 1 |
| `0x08` | R | `BUF0_ADDR` — physical base of buffer 0 (== `HDMI_FB_BASE`) |
| `0x0C` | R | `BUF1_ADDR` — physical base of buffer 1 |
| `0x10` | R | `WIDTH` (pixels) |
| `0x14` | R | `HEIGHT` (pixels) |
| `0x18` | R | `STRIDE` (bytes/scanline) |
| `0x1C` | R | `FORMAT` (0 = XRGB8888, the only value that exists) |
| `0x20` | R | `FRONT_INDEX` — which buffer (0/1) is currently scanned out |
| `0x24` | W | `FLIP_REQUEST` — write 0 or 1: "make that buffer the front" |
| `0x28` | R | `FLIP_COUNT` — completed-flip counter, monotonic |
| `0x2C` | R | `REJECT_COUNT` — out-of-range `FLIP_REQUEST` writes, diagnostic |

Writing the already-front index is a silent no-op (not an error, not counted
either way — a third, distinct outcome from both accept and reject). Writing
anything other than 0 or 1 is **rejected**: `REJECT_COUNT` increments, and —
this is the load-bearing security property, see §7 — the real hardware
(`hdmi_set_scanout_addr()`) is **never called** with a rejected value. Every
register except `FLIP_REQUEST` is read-only; a write there is a silent no-op,
matching `vblk_emmc.c`/`vnet_emac.c`'s own "unknown/read-only register write is a
no-op, not a fault" convention.

### 5.3 Why this needed no `stage2.c` change, and the concurrency note

`vnet_emac.h`'s own comment about *itself* states the mechanism this device reuses
verbatim: "the L2 entry at index 80 is already unmapped/invalid for vblk's sake, so
a guest access anywhere in that 2 MiB range... already takes the same stage-2 data
abort into EL2." `scanout.c`'s device sits at `+0x2000` inside that same,
already-invalid block — no new stage-2 exclusion, no DTB change, nothing for
`stage2_isolation_selfcheck()` to newly cover. This was confirmed empirically, not
just argued: `stage2.c` was not edited at all in this task, and both
`make dbg` and `make dbg EXTRA_CFLAGS=-DHV_HDMI` link clean (§6.2).

**Concurrency**: `scanout_mmio_fault()` runs on CPU0, inside the guest's own trap,
the same core/context every other virtio-mmio device's fault handler already runs
on. `hdmi_relock()` (CPU1) and `hud_update()` (CPU1) run concurrently but touch
neither `scanout.c`'s device state nor, in `hud_update()`'s case, any DE2 register
at all (`hud_update()` only draws pixels via `fb.c`, never pokes DE2 hardware).
`hdmi_relock()`'s own `DE_GLB_DBUFF` write races harmlessly with a concurrent flip's
write of the same register: both are plain 32-bit aligned stores (atomic on this
architecture, no torn-write risk), and `DBUFF` is a commit strobe whose effect is
idempotent — whichever of the two fires "second" just re-commits whatever
`TOP_LADDR` currently holds, which is always a fully valid address either way.

### 5.4 A board-testable entry point independent of the guest driver

`scanout_flip(uint32_t index)` performs exactly what a `FLIP_REQUEST` MMIO write
does, callable directly (no guest, no trap) — e.g. from dbgmon/`hvdbg.py`'s
existing `call` command: `hv.call(<&scanout_flip>, 1)`. This lets a human with
board access exercise the **real** DE2 flip mechanism, on the real monitor,
**before** any guest-side driver work lands — the two halves of this feature are
independently testable.

### 5.5 What the guest side needs from this, restated as a checklist

1. **Reach `0x0A002000`.** Either a proper DTB node (a `simple-bus`/`syscon`-style
   `reg = <0x0A002000 0x100>;`, `status = "okay"` — not added by this task, DTB
   changes are out of scope and `tftpboot` must not be written to) or, matching how
   this exact codebase already treats every other fixed HV physical address
   (`HDMI_FB_BASE`, `HVSCR_BASE`, and the counterpart's own `LIMA_HVFB_PA_BASE`), a
   hardcoded constant the guest driver maps directly
   (`pmap_mapdev_attr()`/`bus_space_map()`) — no DTB node is required for the
   access itself to work, since the stage-2 trap fires regardless of how the guest
   found the address.
2. **Treat it as a plain 32-bit MMIO register file** (§5.2) — ordinary
   `bus_space_read_4()`/`_write_4()`, the same idiom every other simple FreeBSD
   MMIO driver already uses, no virtio negotiation.
3. **Alignment/stride**: both buffers are page-aligned (4096 B) by construction and
   share one stride (5120 B, `1280*4`, a multiple of 8 — satisfies the Mali PP
   write-back `pitch` field's own constraint per the counterpart document's §3.1).
   No stricter base-address alignment is documented in the ported U-Boot source
   this driver is built from; none was assumed beyond page granularity.
4. **Fencing is the guest's job.** Ensure the PP job targeting the back buffer has
   completed (and, per the counterpart's §2, is actually *visible* in DRAM — a
   currently-unresolved gap on that side, not this one) before writing
   `FLIP_REQUEST` for it.
5. **The pixel-data range's CPU-reachability is still an open decision** (§4.4) —
   build and test against `hv-fb` CPU-unreachable first; only ask for the stage-2
   exception if the import genuinely faults without it.

---

## 6. Verification

### 6.1 Hosted tests — `make test`

```
$ make test
...
[ OK  ] MAGIC reads back the fixed constant
[ OK  ] VERSION reads back the fixed constant
[ OK  ] BUF0_ADDR reads back the buffer 0 physical address
[ OK  ] BUF1_ADDR reads back the buffer 1 physical address
...
[ OK  ] flip 0->1 is accepted
[ OK  ] front_index becomes 1
[ OK  ] flip_count increments exactly once
...
[ OK  ] requesting buffer index 2 is rejected
[ OK  ] requesting buffer index 0xFFFFFFFF is rejected
[ OK  ] each rejected request increments reject_count exactly once
...
[ OK  ] an out-of-range FLIP_REQUEST write is rejected
[ OK  ] ...and does NOT touch the real hardware -- a bad write from a
        buggy/malicious guest can never reprogram the scanout address
[ OK  ] writing any register except FLIP_REQUEST changes NOTHING in the
        device state, for every defined offset plus one unknown one
[ OK  ] ...and never touches the real hardware either
---- scanout register tests: passed ----
...
$ echo $?
0
```

Full suite (every pre-existing hosted C test plus every Python selftest) exits 0;
zero `[FAIL]` lines anywhere in the run. `test_scanout_regs.c` hand-transcribes
`scanout.c`'s pure decision logic (`struct scanout_dev`, `scanout_reg_read()`,
`scanout_decide_flip()`, `scanout_reg_write()` — with exact line citations in the
file's own header) with a scripted hardware recorder standing in for
`hdmi_set_scanout_addr()`, following the identical convention `test_sd_bio_addr.c`
documents for the same reason (the real file has AArch64 inline asm a hosted
x86_64 compile cannot touch).

### 6.2 Cross-build — `make dbg`

Plain (default, matches production — `HV_HDMI` **not** defined, exactly as every
target ships today):

```
$ make dbg
...
   text	   data	    bss	    dec	    hex	filename
 149712	    284	 524080	 674076	  a491c	microkernel-dbg.elf
$ echo $?
0
```

Clean — no warnings or errors from any file this task touched (the pre-existing
`smp.c`/`emmc_bio.c` warnings and the linker's RWX-segment warning are unrelated
and present before this task's changes too).

**Also verified** (beyond the task's literal checklist, for real confidence that
the new `#ifdef HV_HDMI` code paths actually work): a from-clean rebuild with
`make clean && make dbg EXTRA_CFLAGS=-DHV_HDMI` also links clean, and disassembly
of the result (`aarch64-linux-gnu-objdump -d`) confirms the new code is genuinely
reachable, not just present:

```
    42001758:	94007472 	bl	4201e920 <scanout_mmio_fault>   # el2_exc.c's new dispatch line
    42020cc0:	97ffe860 	bl	4201ae40 <hdmi_init>
    42020cd8:	97fff692 	bl	4201e720 <scanout_init>          # main_dbg.c's new call
    42020dbc:	97ffedd1 	bl	4201c500 <hud_init>
    42020dc8:	97ffee68 	bl	4201c768 <hud_update>
```

(The plain and `-DHV_HDMI` builds happen to report byte-identical `size` totals —
checked, not hand-waved: `main_dbg.o`/`el2_exc.o` were confirmed compiled with
different command lines between the two runs, and the disassembly above proves
real new call instructions exist in the `-DHV_HDMI` binary; the size tool's
reported totals evidently round/pad past a delta this small, which is a
measurement-granularity coincidence, not evidence the flag had no effect.)

The tree was left in the plain (no `EXTRA_CFLAGS`) state after verification,
matching what a default `make dbg` produces.

### 6.3 Two pre-existing bugs found and fixed while doing the required reading

Reading `hud.c` (as "an existing consumer of the framebuffer," per this task's own
instructions) surfaced that `hud.c:214`'s local `HDMI_BC_BASE` constant still
pointed at `0x50003000` — the breadcrumb address `hdmi.c` used **before**
2026-07-25, when `hdmi.h`'s own comment records it was relocated to `0x50011800`
("that address sits inside the vconsole 64 KiB capture ring... and was being
clobbered"). `hv_addrmap.h`'s own `HVMAP_LOW_HDMI_BC` mirror had the identical,
independent staleness. Both had been silently reading/fencing a dead address —
in `hud.c`'s case, the HUD's on-screen "stage" readout in the memory/breadcrumb
panel — in every `HV_HDMI` build since the relocation. Both are one-line fixes
(update the literal, per this file's own "a comment can be wrong and nobody
notices; a `_Static_assert` cannot" philosophy) and are included in this change,
separately called out here because they are unrelated to zero-copy scanout itself
— found only because this task's own reading requirements pointed at the same
files.

### 6.4 QEMU CI — `./qemu-ci.sh`

```
$ ./qemu-ci.sh
qemu-ci: building microkernel-qemu.elf ...
qemu-ci: running under qemu-system-aarch64 -M virt (timeout 30s) ...
qemu-ci: PASS
HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)
HV: first tick — EL2 preempted the EL1 guest
HV: tick 5 (preempted guest)
HV: tick 10 (preempted guest)
HV: tick 15 (preempted guest)
HV: tick 20 (preempted guest)
QEMU-CI: PASS (stage-2 + EL1 guest + GICv2 timer preemption confirmed)
$ echo $?
0
```

`qemu-ci.sh`'s target (`microkernel-qemu.elf`, QEMU's generic `virt` machine) does
not link `hdmi.c`/`fb.c`/`hud.c`/`scanout.c` at all (`QEMU_OBJS` has no DE2/HDMI
hardware to test against on a generic virt board) — this gate exercises stage-2 /
EL1-entry / GICv2 timer preemption, which this task's changes do not touch, and it
still passes, confirming no regression there.

---

## 7. The security consequence, stated plainly

**What changed, and what did not.** The guest (or a compromised guest kernel)
already had, before and independent of this task, the raw hardware capability to
point the real Mali GPU's DMA at any physical address in the low 4 GiB, including
`hv-image` and `hv-scratch` (§4.1) — that capability is inherent to a GPU with a
guest-controlled on-chip MMU on an SoC with no system IOMMU, and this task neither
created nor could have closed it. What this task's own code changes actually add:

- **A narrow, validated control surface** (`scanout.c`'s doorbell) whose only
  guest-triggerable hardware effect is calling `hdmi_set_scanout_addr()` with one
  of **exactly two** addresses fixed at `scanout_init()` time
  (`g_scan.buf_pa[0]`/`[1]`) — never a guest-supplied address. `scanout_decide_flip()`
  rejects anything else before it ever reaches the hardware call (§5.2,
  test-proven: "an out-of-range `FLIP_REQUEST` write... does NOT touch the real
  hardware"). A fully malicious guest driving this doorbell adversarially can
  make the display flicker between the two legitimate buffers and nothing else —
  it cannot redirect scanout to read from `hv-image`/`hv-scratch`, and it cannot
  reach any address this device does not already know about.
- **Zero new CPU-side stage-2 reachability.** `hv-fb`'s existing exclusion (when
  `HV_HDMI` is built in) is untouched by this change — the doorbell's own MMIO
  window needed no new carve-out (§5.3), and this task deliberately did not
  implement the broader pixel-range CPU-reachability the counterpart guest-side
  document asks for (§4.4), leaving that as an explicit, reviewable, not-yet-taken
  step rather than a silent default.
- **Formalising, via readable MMIO registers, information that was already latent**
  (the buffer addresses/geometry) — the guest kernel already parses its own DTB at
  boot, which already contains `hv-fb@4d000000`'s `reg` property in plaintext, so
  "the guest doesn't know the address" was never a real barrier this change
  removes; it publishes the same numbers through a cleaner, purpose-built channel
  instead of leaving them to be reverse-derived.

**What it can affect**: the on-screen contents of the HDMI framebuffer — a visual
nuisance if abused, not a control-flow or memory-safety issue, **provided** the
existing `hv-fb` CPU exclusion (when active) and the pre-existing GPU-DMA reality
(§4.1, true regardless of this feature) are both understood as the actual
boundaries, which is what §4 exists to make explicit rather than assumed.
**What it cannot affect**: anything outside the two fixed buffer addresses — no
path in the new code ever accepts, forwards, or derives a guest-supplied physical
address.

---

## 8. Everything this document could not verify without hardware

Collected in one place, per the task's own "do not pad" instruction:

1. **`DE_GLB_DBUFF`'s exact commit-latch boundary** (§1) — inferred from the
   existing write-then-strobe usage pattern, not confirmed against a datasheet or
   a real timing capture.
2. **Whether `HV_HDMI` was defined for whatever image is currently deployed on the
   board** (§4.2) — the one local artifact checked (`microkernel-dbg.elf` under
   `tftpboot`, dated 2026-07-16) predates `hdmi.o` even being in `DBG_OBJS` and
   proves nothing about the live image; querying the actual board is forbidden by
   this task's constraints.
3. **Whether the guest kernel's dma-buf/PRIME import machinery genuinely needs
   `hv-fb`'s pixel range to be CPU-reachable** (§4.4) — the counterpart document
   asks for it defensively; this document's own reasoning is that the GPU's DMA
   does not need it, but neither document has run the actual import on hardware.
4. **Whether Mali PP `pixel_format=3,flags=0` really produces the same in-memory
   byte order as DE2's `XRGB8888`** — the counterpart document's own open item
   (§3.2 of `SCANOUT-IMPORT.md`), cited here because a mismatch would produce
   wrong colours silently, the exact kind of bug neither side's hosted checks can
   catch.
5. **Everything about actual pixels on an actual monitor** — `hdmi_set_scanout_addr()`
   and `scanout_flip()` are built and unit-tested for their *decision* logic and
   linked into a build that boots the real hardware pipeline, but no code in this
   task has ever executed on the board. `scanout_flip()` (§5.4) is written
   specifically to make that the very next, minimal, board-only step.

No code path in this feature has ever executed on real hardware. Every claim
above that sounds load-bearing is a citation to a specific file and line this task
read or a command this task actually ran, not a hardware measurement.

## 9. Scanout v2 — guest overlay planes (2026-09-30, `ac87eea`)

This document predates the guest window (UI1 layer 1, `hdmi_guestwin_*`) and
the real vblank mirror; both have long been live. Version 2 adds planes.

**Why these layers.** HDMI on the A64 goes through mixer1 (mux1), which has
one VI channel (YUV, scaler) and one UI channel with four layers. Layer 0 is
the HUD, layer 1 the guest window; 2 and 3 were idle. Overlay plane `p`
(0..1) is UI1 layer `2 + p`. Same channel as the guest window, so they
composite inside it by layer index (3 over 2 over 1 over 0) and need no
blender or route change.

**Registers** (`scanout.h`, `SCANOUT_VERSION` = 2):

| offset | name | |
|---|---|---|
| 0x50 | `OVL_NUM` | R: planes (2) |
| 0x54 | `OVL_COUNT` | R: accepted applies |
| 0x58 | `OVL_REJECT` | R: rejected applies |
| 0x60 + 0x20·p + 0x00 | `ADDR` | RW, staged: physical base |
| … + 0x04 | `PITCH` | RW, staged: bytes per line |
| … + 0x08 | `SIZE` | RW, staged: `w \| h << 16` |
| … + 0x0C | `COORD` | RW, staged: `x \| y << 16`, output pixels |
| … + 0x10 | `CTRL` | RW: **the write applies** the staged state |

`CTRL` bits (`hdmi.h`): `HDMI_OVL_EN` (bit 0; CTRL without it turns the plane
off), `HDMI_OVL_ARGB` (bit 1: ARGB8888 with per-pixel alpha, else XRGB8888),
`HDMI_OVL_GALPHA_ON` (bit 2: apply bits 31:24 as a global alpha, combined
with per-pixel alpha when both are set). An apply is one `DE_GLB_DBUFF`
strobe, i.e. it lands at the next vblank.

**Refused** (`OVL_REJECT`, plane left as it was): zero size, a rectangle
past the output, pitch not a multiple of 4 or below `w*4`, and any address
the guest window itself would be refused (`scanout_addr_allowed()`: outside
guest DRAM, HV image, hv-scratch, hv-fb) — which now also excludes the
vzram slice and U-Boot's top, both mapped by stage-2 but not guest memory.

**Verified** from the guest (root, `/dev/mem` mmap of `0x0A002000`): layer 2
read back exactly as requested (attr `c0000403` = enable, global alpha
0xC0, XRGB; 560x200 at 1300,820 showing the guest window's buffer); an
address in the vzram slice and an off-screen rectangle were both refused;
layer 3 untouched. **Not verified: the panel.** Nobody has looked at the
monitor yet, and `screenshot.py` composes only the HUD and the guest window
from register state.

**Left for the guest:** planes in bzkms and the HWC-style negotiation
(which surface gets a plane, which falls back to GPU composition). Left for
the hypervisor if video should reach the panel without a copy: the VI
channel (YUV formats, the VSU scaler, CSC) as a third kind of plane.
