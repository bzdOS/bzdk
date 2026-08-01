# War stories: bring-up on real, unfamiliar silicon

This is not a design document. It is a collection of bugs that were actually
hit, live, on a Banana Pi M64 with no UART, no JTAG, and no reference
hypervisor to copy from — plus the reasoning that got from symptom to fix.
Every story below is reconstructed from this tree's own source comments,
which this project treats as its primary incident record (there is no
separate bug tracker). File:line citations point at the exact comment or
code the claim is drawn from, so you can go verify it yourself.

Two honesty notes up front, because they matter for how much to trust each
story:

- Some of these are **closed**: root-caused, fixed, and the fix is live in
  the default build. Some are **still open** — a rigorous analysis with a
  designed fix that hasn't been confirmed on hardware yet. Each section says
  which kind it is.
- Where the tree itself records a *wrong* first hypothesis, that's kept in,
  not smoothed over — the wrong turns are usually the more useful part for a
  reader debugging their own board.

If you're doing bring-up on unfamiliar SoC silicon with a debug channel this
thin, the shape of these bugs will probably look familiar.

---

## 1. The interrupt-routing odyssey: a 145 kHz storm, and a fix that was blamed on the wrong thing

**Subsystem:** GICv2 virtualization (`vgic.c`, `gic_timer.c`, `stage2.c`, `main_dbg.c`). Tracked throughout as `internal task`.

This is the longest-running saga in the tree, and it runs across three
files and at least three distinct "fixes," one of which was later found to
have fixed nothing.

**Act 1 — the timer tick that never arrived.** Early on, the hypervisor
wanted its own preemptive EL2 tick, driven off the physical CNTP timer. The
first attempt programmed the GIC distributor and CPU interface, armed the
comparator — and nothing happened. Rather than guess, the code adds a bounded
diagnostic busy-wait that samples `CNTP_CTL.ISTATUS` and `GICD_ISPENDR0`
after the deadline should have passed, and documents a decision table for
what each combination means (`gic_timer.c:485–507`). The result: **both bits
were set** — the timer had genuinely fired and the GIC had genuinely latched
it pending — but the CPU never took the exception. That combination is the
fingerprint of a *routing* problem, not a GIC problem: with `HCR_EL2.IMO`
clear (the reset default), a physical IRQ routes to EL1, and an asynchronous
exception targeting a lower EL than the current one is never taken — it just
sits pending forever while the hypervisor runs at EL2. The comment is candid
about correcting itself here: an earlier claim that IMO didn't matter
"because nothing runs below EL2" was backwards — it matters *because*
everything here runs at EL2 (`gic_timer.c:435–460`). Setting `HCR_EL2.IMO=1`
fixed the tick.

**Act 2 — building the virtual GIC, and an interrupt storm at 145 kHz.**
Once physical IRQs routed to EL2, the natural next step was a real vGIC:
forward device interrupts into the guest via GICH list registers, and
redirect the guest's GICC MMIO window to the physical GICV page so its
`IAR`/`EOIR` accesses land on hardware built for exactly this
(`stage2.c:186–199`, `vgic.c:1–49`). That's the standard GICv2 KVM trick, and
`vgic.c`'s header is explicit about the half that was actually finished: the
CNTV virtual timer path (inject on tick, guest EOIs through GICV) worked.
Forwarding for arbitrary *device* IRQs did not get built out to the same
level. The consequence: with `IMO=1` now catching every physical Group-1
IRQ at EL2, a level-triggered device interrupt — the EHCI controller at
`0x01c1b000`, SPI 74 / INTID 106 — arrived at EL2, got EOI'd there, and was
never forwarded to the guest and never cleared at the device. Level-triggered
+ un-cleared means it re-fires immediately. The result was an IRQ storm at
roughly 145 kHz that starved the guest and wedged it inside `ehci_reset()`'s
own `DELAY()` loop (`main_dbg.c:214–224`, referenced as "EHCI INTID 106
storm").

**Act 3 — a clean architectural reversal, and a misdiagnosis caught after
the fact.** The fix was not "finish the vGIC forwarding." It was to step back
and ask why EL2 needed to own physical device IRQs at all: this is a
single-guest debug hypervisor, and FreeBSD's own native drivers already know
how to service and deactivate their interrupts correctly. `main_dbg.c` flips
`HCR_EL2.IMO`/`FMO` back to 0 — routing *every* physical IRQ straight to the
guest's EL1 — and drops the EL2 preemptive tick entirely in favor of
polling `dbgmon_service()` from inside the existing UART-trap path
(`main_dbg.c:214–230`). That single policy change fixed the EHCI storm.

Here's the part worth dwelling on: the GICC→GICV stage-2 redirect from Act 2
was pulled out as part of the same cleanup, and for a while an apparent
regression was blamed on removing it. It wasn't. The `stage2.c` comment
documents the correction in full: under `IMO=0`, the guest must be able to
acknowledge and EOI interrupts through the *real* physical GICC — redirecting
that IPA to the (now-inert, since nothing drives GICH anymore) GICV left the
guest unable to service any interrupt at all, hanging at the MMC/root-mount
phase. Removing the redirect was the right call. But the boards that seemed
to regress *after* an earlier attempt to remove it were actually failing for
an unrelated reason — a `chimpd` `autostart=no` bug that kept `bootelf` from
jumping to the loaded image at all. Once that infrastructure bug was fixed,
the "regression" vanished on its own (`stage2.c:279–286`).

**Lesson:** two independent bugs wearing the same symptom (guest doesn't
boot after a change) will absolutely get blamed on each other if you don't
have an independent way to tell "the change is wrong" apart from "something
else broke." Here that independent signal was the GIC's own pending/latched
state, sampled with bounded polling before anything auto-recovers it — worth
building before you need it.

---

## 2. eMMC bring-up: the clock pin FreeBSD keeps stealing back

**Subsystem:** `emmc_bio.c` (the hypervisor's own PIO eMMC driver), `smp.c`, `main_dbg.c`.

**Symptom:** `CMD1 SEND_OP_COND` came back with `OCR=0` — the card
essentially reporting "not present" — even though the eMMC was known-good
hardware sitting right there on the bus.

**Root-causing:** the A64's eMMC clock line shares a pinmux nibble (PC5,
`PIO PC_CFG0` bit-field `[23:20]`) with a GPIO function, and it turned out
FreeBSD's own A64 pinctrl driver was the one un-muxing it: on this board's
DTS, FreeBSD correctly muxes every *other* MMC2 pin to function 3 but leaves
PC5 at `gpio_in` (function 0) — so the clock pad the eMMC controller needs
is simply dead from the controller's point of view (`smp.c:449–453`). This
was confirmed directly rather than inferred: forcing PC5 to function 3 and
re-issuing the identification sequence over the debug channel got `CMD0` to
`CMD_DONE` and `CMD1` to return `OCR=0xc0ff8080` (ready) — proof the eMMC was
present and answering the whole time; only the clock was missing
(`main_dbg.c:278–286`).

**Fix, and why it needs to run continuously, not once:** `emmc_bio_init()`
forces the mux at bring-up (`emmc_bio.c:237–239`), but because FreeBSD's own
pinctrl driver is the thing un-muxing it, a one-time fix loses the race the
moment the guest kernel re-touches that register. The dedicated CPU1 debug
core therefore re-checks and re-enforces PC5=func3 on every iteration of its
poll loop — reading before writing so it doesn't fight the other bits
FreeBSD legitimately owns on that same register (`smp.c:449–459`).

**A second, adjacent trap in the same file:** getting the controller to
*respond* is only half the story — getting its **new-mode timing
registers** (`NTSR` "mode select new" and `SAMP_DL` "calibration delay") to
actually take effect is a separate, non-obvious precondition. The first
attempt wrote `NTSR` with the card clock already running, and it silently
read back as `0x0` — not an error, just a write that went nowhere. The fix,
found by working out from U-Boot's own `mmc_config_clock()` what it actually
does (not what a generic sunxi-mmc datasheet suggests), is that both
registers only **latch** while `CKCR.CLK_ENABLE` is off: clock off → issue a
clock-update command → program the CCU divider, then `NTSR`, then `SAMP_DL`
→ clock back on → clock-update again (`emmc_bio.c:63–82`, implemented at
`emmc_bio.c:261–283`). Skip that ordering and reads still work at the
crawling 400 kHz init clock (read timing tolerates the missing
calibration) — but `CMD24` **writes** hang forever with `RINT` stuck at
`CMD_DONE|TX_DATA_REQ` and `DATA_OVER` never asserting, because the card's
write CRC/status-token phase needs the new-mode sample-clock alignment to be
recognized at all. A bug that only manifests on the write path, while every
read-path test keeps passing, is exactly the kind of thing that survives
into an integration milestone before it's caught.

---

## 3. The race that only showed up back-to-back

**Subsystem:** `emmc_bio.c`'s read path, exposed by `vblk_emmc.c` (virtio-blk).
**Status:** closed — root-caused and fixed live, 2026-07-20.

**Symptom:** once virtio-blk started driving `emmc_bio_read()` in a tight
loop (one call per sector, back-to-back, with no delay between them — unlike
the earlier `dbgmon`/CPU1 callers, which are naturally paced ~80 ms apart by
an EMAC round-trip), reads started failing spuriously — not deterministically
wrong, just intermittently timing out with `nwords=0`.

**Initial shape of the read path:** drain 128 words from the FIFO, see
`FIFO_EMPTY` together with `DATA_OVER` in `RINT`, declare the block done, and
return (`emmc_bio.c:364–382`). That looks complete — the data is drained, the
transfer-done bit is set — but it's the wrong stopping point.

**Root-causing:** the breadcrumb window this file already keeps for read
failures (`RINT`, `STAR`, words-drained, `GCTL`, at
`emmc_bio.c:333–345`) caught the actual failure signature directly:
`RINT = 0x8` — `DATA_OVER` set — **without** `CMD_DONE`, and `nwords=0`. That
combination only makes sense if a *new* command had already been issued and
its `RINT` cleared before the *previous* transfer's `DATA_OVER` had actually
landed. Draining all 128 FIFO words is not the end of the transfer: the
controller keeps running a CRC/end-of-transfer phase afterward and only sets
`DATA_OVER` slightly *later*. A caller that returns the moment the FIFO
empties and immediately issues the next command resets the FIFO and writes a
new `CMDR` while the *previous* block is still retiring — so the previous
block's late `DATA_OVER` arrives after the new command has already cleared
`RINT`, and the new command's own drain loop sees `FIFO_EMPTY + DATA_OVER`
immediately and bails out with zero words read. Every back-to-back read hit
this; paced callers essentially never did, which is exactly why it surfaced
only once virtio-blk started hammering the controller (`emmc_bio.c:384–396`).

**Fix:** wait for `DATA_OVER` and card-idle *after* draining the FIFO, before
returning — mirroring what the write path (`emmc_bio_write()`) already did
correctly (`emmc_bio.c:397–412`).

**Lesson:** "the data is all there" and "the transaction is over" are two
different facts about a hardware FIFO, and a caller that only checks the
first one will pass every test that isn't back-to-back — which usually means
it passes in isolation and fails only once it's wired into the thing that
actually stresses it.

---

## 4. Ghost sectors: when "the transfer succeeded" doesn't mean the data is there

**Subsystem:** `vblk_emmc.c`, cache maintenance on the read completion path.
**Status:** closed — root-caused and fixed live, 2026-07-22.

**Symptom:** virtio-blk reads completed cleanly — 336 sectors in, no I/O
error, IRQ delivered, `S_OK` — and yet GEOM couldn't find a GPT on the disk,
and the guest panicked with mount error 19. The hypervisor's own read path
(the exact same `emmc_bio_read()` from story 3, now working) had already been
independently proven byte-correct.

**Root-causing:** the FreeBSD `virtio_blk` driver treats its backing device
as a normal non-coherent DMA device: on completion of a read, it performs a
POSTREAD cache **invalidate** (`dc ivac`) on its buffer, on the reasonable
assumption that a real hardware device just wrote fresh bytes to main memory
behind the CPU's cache and the stale cache line needs to be dropped so the
next load refetches from DRAM. But `emmc_bio_read()` fills that buffer with
ordinary **cached EL2 stores** (`SCTLR_EL2.C=1`) and only issues a `dsb` —
it never cleans those lines to the point of coherency. So the sequence is:
EL2 writes the sector into its own dirty cache line; the guest's POSTREAD
`dc ivac` discards that dirty line outright (invalidate, not
clean-then-invalidate); the guest's subsequent load misses the cache and
fetches whatever stale bytes were actually sitting in DRAM. Every sector —
including the GPT header and superblock — could come back wrong even though
the transfer itself reported success, because the "transfer" was never the
problem; the *publication* of its result to a real device's coherency
contract was (`vblk_emmc.c:380–393`).

**Fix:** clean+invalidate the destination range to the point of coherency
after every successful `emmc_bio_read()`, exactly mirroring the cache
maintenance the write path (`gmem_write()`) already performed in the other
direction (`vblk_emmc.c:393`, `vblk_emmc.c:117–141` for `gmem_cmo()`).

**A related, still-open investigation worth knowing about:** this is not the
only DMA-coherency bug this tree has chased. `docs/aw-mmc-dma-coherency.md`
is a much longer, still-unconfirmed analysis of a *different* code path —
FreeBSD's *native* `aw_mmc` driver doing IDMAC (controller-driven) DMA
directly, rather than going through the hypervisor's PIO `emmc_bio` shim.
Its hypothesis is structurally similar in spirit (an extra cacheable
observer of guest DRAM — here, EL2 itself and the CPU1 debug core, both
running cache-coherently on the same DRAM the guest treats as
non-coherent-DMA target) but the proposed fix (`el2_ncmap.c`, making EL2's
own stage-1 mapping of guest DRAM non-cacheable) is implemented and
self-checking but explicitly gated off (`DBG_NCMAP_ENABLE 0`,
`main_dbg.c:185`) pending a live hardware confirmation that hasn't happened
yet as of this writing. Worth reading if you hit DMA corruption that looks
similar but isn't on the virtio-blk path — but don't mistake it for a closed
story.

---

## 5. The USB-OTG lane: from a silent enumeration failure to a break-glass reset

**Subsystem:** `musb.c` (MUSB CDC-ACM gadget), `usbacm.c` (poll/console bridge).

**Act 1 — enumeration that never finished.** The gadget descriptors were
ported from a reference driver, and enumeration simply looped forever
(`-71`, `ENODEV`/protocol error, from the host's point of view) without any
obvious single cause. Live tracing over the REPL turned up **two
independent** bugs stacked on top of each other, both in static descriptor
tables:

  - The 18-byte USB device descriptor the reference carried was actually
    only 17 bytes — `bcdDevice` had been squeezed to a single byte while
    `bLength` still claimed 18 — so every `GET_DESCRIPTOR(device)` handed
    back a short packet, which the host rejected outright, looping
    enumeration from the top. Confirmed directly: a breadcrumb word reading
    back the descriptor length showed 17, not 18 (`musb.c:689–696`).
  - Independently, the configuration descriptor's `wTotalLength` field said
    67 while the descriptor set had actually grown to 75 bytes when an
    Interface Association Descriptor (IAD) was added for the CDC-ACM
    composite layout — the IAD was appended to the byte array without
    updating the length field next to it. A host reads `wTotalLength` first,
    then requests exactly that many bytes; getting 67 truncated a real
    75-byte set by 8 bytes — precisely the final endpoint descriptor — so
    every enumeration attempt received a config that looked internally
    inconsistent and started over (`musb.c:708–715`).

Neither bug alone would have been the whole story; both had to be found and
fixed before enumeration completed, which is a good reminder that "the host
keeps re-enumerating" is a symptom with more than one simultaneous cause
available, not a single bug to stop looking after finding the first
plausible one.

**Act 2 — turning the same channel into a recovery mechanism.** Once the
USB-ACM console worked, a real incident exposed a gap: EMAC/`dbgmon` went
dark, the guest was hung, and CPU1 kept dutifully petting the hardware
watchdog forever — the one combination with **no** remote recovery path at
all. It cost a physical power-cycle (`usbacm.c:30–34`, dated 2026-07-21).
The fix is a "break-glass" sequence: because the USB-ACM RX path is serviced
independently by CPU1 regardless of what EMAC or the guest are doing, typing
a specific, deliberately console-traffic-unlikely byte sequence
(`0x00 '~' 'B' 'Z' 'R' 'S' 'T' 0x00`) into `/dev/ttyACM0` sets a flag that
stops both watchdog-pet paths, letting the real hardware watchdog fire within
~16 seconds and reboot to U-Boot, where a persistent `chimpd` reloads the
image automatically (`usbacm.c:36–57`). The interesting design point isn't
the mechanism itself — it's recognizing that the *last independently-alive
channel* is exactly the one that needs a way to force a reset, and building
that in before the next incident rather than after.

---

## 6. The register write that hangs a whole CPU core, silently, forever

**Subsystem:** `start.S` (`_start_secondary`), `smp.c`/`smp.h`.

**Symptom:** bringing up secondary cores via PSCI `CPU_ON` (the standard way
to start additional cores on this platform) simply never completed for
CPU1 — no crash, no fault, just a core that never reported itself online.

**Root-causing:** the per-core progress breadcrumbs this path writes at each
stage (`STAGE 0x00` through `STAGE 0x04`, `start.S:98–182`) are written
MMU-off, straight to DRAM, specifically so they survive a hang and are
readable via `md` after a reset. They showed the secondary core reaching
`STAGE 0x00` — "just entered `_start_secondary`" — and never reaching
`STAGE 0x01`, confirmed live on 2026-07-18. The very first substantive thing
the old code did after that breadcrumb was set `CPUECTLR_EL1.SMPEN=1` — a
real requirement on this core family, since the A53's D-cache does not join
the inner-shareable coherency domain until that bit is set, and a secondary
that enabled its MMU/cache without it would silently corrupt every shared
structure it touched. The step is architecturally necessary; the problem is
*how* it was being done. `CPUECTLR_EL1` (`S3_1_C15_C2_1`) is
implementation-defined, and on this SoC, accessing it from EL2 **traps to
EL3**, where ARM Trusted Firmware has no handler installed for it — the trap
has nowhere to go, and the core simply stops there, forever, with no
exception visible from EL2's side at all (`start.S:128–135`).

**Fix:** don't write it. ATF/BL31 already sets `CPUECTLR_EL1.SMPEN` as part
of its own per-core power-on sequence before handing control to the
hypervisor at all — which is also, on reflection, why the *primary* core
(which boots via U-Boot's `go`, not PSCI) never needed to touch this
register either and was cache-coherent from its very first instruction. The
fix is simply to remove the write and mark the stage as already passed
(`start.S:128–135`).

**Lesson:** on a platform with a firmware layer between you and the
hardware, a register access that is architecturally correct can still be
fatal if the *privilege level* it traps to doesn't expect it — and the only
visible symptom from your own level is "this core just... stopped," with no
exception frame to read, which is exactly the situation a stage that's
recorded *before* attempting the risky operation (not after) is built to
distinguish from every other kind of hang.

---

## 7. Catching a guest's first breath before it erases the evidence

**Subsystem:** `firstfault.c`, `stage2.c`/`stage2.h` (`stage2_unmap_guest_vector()`).

This one isn't a bug in the hypervisor — it's a debugging *technique*, built
because the two obvious tools both failed against this specific guest
failure mode, and it's a good illustration of what "no JTAG" actually forces
you into.

**The problem it solves:** the FreeBSD guest, early in its boot, was dying in
a recursive-exception storm fast enough that the original fault's own
records — `ELR_EL1`/`ESR_EL1`/`FAR_EL1` — were gone before anything could
read them: each re-entry into the guest's own EL1 vector table overwrites
those banked registers with a new fault, and by the time the hypervisor's
generic trap handler gets a look, it's looking at the *Nth* fault, not the
first one. Worse, FreeBSD's `locore` runs this early stretch with
`PSTATE.D=1` — the debug-exception mask bit set — which means hardware
breakpoints, watchpoints, and software single-stepping are all masked and
simply cannot fire here, closing off the normal answer to "which instruction
faulted first" (`stage2.h:104–120`).

**The technique:** rather than trying to observe the guest's EL1 state from
outside it, make the guest's very first vector *fetch* itself visible to
EL2. `stage2_unmap_guest_vector()` leaves the guest's EL1 vector table page
(the physical page backing its `VBAR_EL1`, resolved once from the known
kernel load address) deliberately **invalid** at stage-2
(`stage2.c:319–342`, `stage2.h:122–132`). The guest's hardware fault
mechanism doesn't care that PSTATE.D is set — it still sets
`ELR_EL1`/`ESR_EL1`/`FAR_EL1`/`SPSR_EL1` to describe the *original* fault and
branches to `VBAR_EL1`, exactly per the architecture. But because that page
is unmapped at stage-2, the vector *fetch itself* takes a stage-2
instruction abort straight to EL2 — before the guest's own vector code ever
executes and before the storm can overwrite anything.
`firstfault_handle()` catches that specific abort, confirms the faulting IPA
is the vector page (via `HPFAR_EL2`), and reads the guest's still-pristine
EL1 fault state directly with `mrs` — an EL2 read of EL1-banked registers
takes no trap at all — plus all 31 GPRs from the saved trap frame
(`firstfault.c:68–95`). It records all of that into a breadcrumb, then
re-maps the vector page and returns control to the guest, which proceeds
into its own (storm-continuing) handler having lost nothing — the original
fault is already safely captured.

**Why this is worth knowing even outside this project:** it's a general
answer to "how do I catch the very first exception of something that
immediately masks or destroys the evidence of its own first exception,"
using only a stage-2 translation fault as the tripwire — no hardware debug
facilities required, which matters a great deal when those facilities are
exactly the ones the guest has masked.

---

## 8. Two ways to get the FreeBSD boot contract wrong

**Subsystem:** `kload.c`/`kload.h` — this hypervisor loads the FreeBSD kernel
directly, emulating just enough of the loader's ELF-note/modinfo protocol
for the kernel to think a normal loader handed it off.

**Bug 1 — an off-by-one in a tag enum, caught by tracing, not by inspection.**
The machine-independent `MODINFOMD_*` tag numbers the loader protocol uses
(`sys/sys/linker.h`) were originally transcribed as `ENVP=7 / HOWTO=8 /
KERNEND=9`. The real header defines them as `ENVP=6 / HOWTO=7 / KERNEND=8`.
Because FreeBSD's metadata-fetch matches records purely by
`MODINFO_METADATA | tag`, this off-by-one didn't produce a missing field or
an obvious parse failure — it made the kernel's boot-parameter code read the
hypervisor's *ENVP* record while thinking it was reading *HOWTO*, and its
*HOWTO* record while thinking it was reading *KERNEND*. Every record was
present, well-formed, and simply aliased to the wrong meaning one slot down.
The symptom this produced — a traced register holding what looked like the
kernel's `lastaddr` value where the `HOWTO` boot flags should have been —
was real and reproducible, but compatible with several different theories
(a compiler quirk, a calling-convention mismatch) before the tags themselves
were checked line-for-line against the exact FreeBSD source commit the
on-board binary was actually built from (`releng/15.1`, commit `9263fb9`)
and the off-by-one fell out directly (`kload.h:164–176`).

**Bug 2 — a device string that was never valid syntax, hiding behind a much
scarier-looking failure.** For a long stretch, the guest panicked at
mountroot with `error 19` trying to mount
`/dev/vtbd0p3;ufs:/dev/gpt/rootfs` — a device name that obviously can't
exist — and the investigation reasonably suspected the disk itself: a stale
GPT backup header from an earlier, smaller disk image, a GEOM taste race, a
virtio-blk transport bug. All of those were individually and carefully ruled
out on real hardware (independently reading both the primary and backup GPT
headers via `emmc_bio` and confirming both matched the current full-disk
image, cross-referenced against the completed cache-coherency fix from story
4). The actual bug was much smaller and had been sitting in plain sight the
whole time: the kernel environment string this hypervisor injects had set
`vfs.root.mountfrom=ufs:/dev/vtbd0p3;ufs:/dev/gpt/rootfs`, intending a
primary device with a fallback. But FreeBSD's kenv parser
(`sys/kern/vfs_mountroot.c`, `parse_token()`) splits **only on whitespace** —
a semicolon is not a delimiter anywhere in that path — so the entire string
was read as one token and handed whole to `parse_mount()`, which took
everything up to the next whitespace as a single (non-existent) device name.
That is exactly the string the panic was printing, the whole time
(`kload.c:519–537`). The fix was simply to stop using `;` as a separator —
multiple whitespace-**separated** `fstype:device` tokens are the actual
FreeBSD-supported way to list fallbacks, but a single clean device sufficed
here.

**Lesson shared by both:** when the "protocol" you're implementing is
someone else's undocumented internal contract (a linker-note tag enum, a
kenv parser's tokenization rule), the only trustworthy source is the exact
matching source tree the on-board binary was built from — not a
recollection of "how loaders usually work," and not the first plausible
theory that fits the symptom. Both bugs here produced confusing,
theory-compatible symptoms right up until someone read the real parser.

---

## 9. The virtual interrupt that only exists as Group 0

**Subsystem:** `vgic.c`/`gic_timer.c` — GICv2 (GIC-400) virtualization, the
piece that lets an EL1 guest receive *virtual* interrupts through the
hypervisor's List Registers instead of the physical CPU interface.

**Status: closed** (FreeBSD boots to root mount + init under full IMO=1 vGIC,
~11 min uptime), though the whole feature is deliberately *outside* the v1 gate
(v1 ships the IMO=0 static-partitioning model; this was pursued because it was
on the milestone list).

Three earlier attempts at "route every physical IRQ to EL2 and forward it to
the guest as a virtual one" had all regressed — a NULL panic, a 145 kHz
EHCI/INTID-106 storm (story 1), a guest wedged at `reads=1`. The instrumented
fourth attempt bisected it on hardware and found the guest booting to **GEOM
disk-tasting** and then freezing: the flight recorder (story 7's descendant,
now a generalized `(kind,a0,a1)` ring) showed the guest **polling** the virtio
ISR-status register 1000+ times — i.e. *not receiving interrupts at all* — and
the injected List Register for the timer sitting un-EOIed. Both the timer PPI
and the device SPI were injected as **Group 1**, and *neither* reached the
guest.

The root cause is a property of the silicon that no amount of List-Register
juggling changes: the GICv2 **virtual** CPU interface (GICV) has **no security
banking**. A guest that believes it is non-secure and enables "Group 1" by
writing `GICC_CTLR` bit 0 actually sets `GICV_CTLR.EnableGrp0` — so a Group-1
virtual interrupt is never presented. KVM injects everything as **Group 0**
for exactly this reason, and it is nowhere in the GIC-400 TRM's happy path.
Flipping injection to Group 0 (`VGIC_GROUP0`, LR.Grp1=0 + `VMCR` VENG0|VENG1)
was the entire fix: device SPIs *and* the CNTV timer immediately started being
delivered, `vtblk0`+`vtnet0` enumerated on real interrupts, and the guest ran.

**Lesson:** the earlier three attempts weren't "the timer is hard" — they were
all the same silent Group-1 delivery failure wearing different symptoms. A
status bit that reads "injected OK" (the LR accepted the write) says nothing
about whether the guest's virtual interface will ever *present* it. When a
whole class of interrupts vanishes, suspect the one global gate they share
(here, the group enable) before instrumenting each interrupt individually.

## 10. You cannot debug a display you cannot see

**Subsystem:** `hdmi.c`/`fb.c`/`hud.c` — the DE2→TCON1→DWC-HDMI→PHY scanout
pipeline, integrated into the running hypervisor so the HV drives a physical
monitor with a live HUD (`-DHV_HDMI`, opt-in).

**Status: CLOSED** (root-caused live and fixed; the signal now survives guest
boot and the HUD is on the monitor). It reads as an "open" story below because
that is how it *felt* for most of its life — the resolution is at the end.

`hdmi_init()` runs to completion — breadcrumb stage 6 (SCANOUT), no stall, and
`PHY_STATUS` bit 7 (analog PHY lock) set (`0x00086ef4`). The framebuffer at
`0x4D000000` holds real HUD pixels. But a live read a second after the guest
boots shows `PHY_STATUS = 0x00020600`, bit 7 **dropped, stable** — while
`DE_GLB_CTL`, `TCON1_CTRL`, and the CCU `PLL_VIDEO0` (the *pixel*-clock PLL) all
still read exactly what `hdmi_init()` programmed. So the digital pipeline is
untouched; only the analog PHY's own PLL loses lock.

Every attempted fix chased the wrong layer in turn: it is **not** the 1080p vs
720p mode (both drop bit 7 identically); it is **not** FreeBSD gating the
display clocks (a full register-by-register read post-boot showed every CCU/
DE2/TCON bit `hdmi_init()` wrote is bit-for-bit unchanged); a CPU1 re-lock that
re-runs `PLL_VIDEO0` + `phy_init()` fires (`hdmi_relock()`, gated behind
`dbg_hdmi_relock`, default off) but does **not** restore bit 7. The remaining
suspect is the one domain the digital reads can't see: analog supply. FreeBSD's
AXP803 PMIC driver reconfigures rails on boot — including `dldo1`
(`vcc-hdmi-dsi`), the HDMI PHY's supply — but confirming it live was blocked
because once the guest boots it **owns the RSB bus** (0x01F03400) and the HV's
`rsb_read()` returns −1.

A follow-up experiment pinned `dldo1` (`vcc-hdmi-dsi`, the DTB `hvcc-supply`
of the HDMI PHY) as the leading suspect and tried the obvious fix — marking it
`regulator-always-on` + `regulator-boot-on` in the guest DTB so FreeBSD's
axp8xx driver can't turn it off as "unused" (its only consumers, the `hdmi`
nodes, are `status="disabled"`). It did **not** help, and made the register
picture *worse*: post-boot `PHY_STATUS`, `TCON1_CTRL`, and `DE_GLB_CTL` all
now read `0x0` and `PLL_VIDEO0`'s enable bit dropped — i.e. the whole display
clock domain went down, not just the PHY lock. So the naive supply-pin fix is
refuted. What it *does* prove: changing the guest DTB changes the display
outcome, so the controlling lever really is FreeBSD's clock/regulator handling
of that domain — just not `dldo1` always-on specifically. Reverted.

**The resolution.** The block came from a wrong assumption that had gone
unchallenged: "the guest owns the RSB bus, so the HV can't read the AXP live."
It was refuted by one question — *why can't it?* The HV and the guest drive the
**same** RSB controller; there is no second bus. The hypervisor can reclaim it:
`rsb_init()` re-runs the whole controller bring-up, and FreeBSD's rsb driver
re-inits its side on its next transaction. Reclaiming it and reading the AXP803
live gave the answer instantly — REG 0x12 = `0x80`: DLDO1 (bit 3) **cleared**.
FreeBSD had turned the PHY's supply *off*. Not a clock, not the PHY config, not
the mode — a power pin. The fix writes DLDO1 back on over that reclaimed RSB
bus and re-runs the PHY bring-up (`hdmi_relock`, now CPU1-default-on); the PHY
re-locks and holds, and a human confirmed the HUD on the monitor.

**Lesson (three now):** first, `PHY_STATUS` bit 7 was treated as "is there a
signal?" for a long time before it was pinned down as *the analog PLL lock,
distinct from the still-locked pixel PLL* — a status bit is not a feature until
you know which of several PLLs it reports. Second, the ground-truth oracle
("is there a picture?") really is a human at a monitor — but it only had to be
consulted **once, at the end**, to confirm the fix; the diagnosis itself was
done headless over RSB. Third and most useful: the thing that unblocked this
was deleting a "can't" that was never true. "The guest owns the bus" sounded
like a hardware fact; it was an unexamined assumption, and one *why?* dissolved
it. When you catch yourself narrating why something is impossible, check
whether it's physics or just habit.

## 11. The host driver that ate `/dev/ttyACM0`

**Subsystem:** none on the board — this one is entirely host-side, and is here
because it cost real time being mistaken for a board/HDMI regression.

**Status: closed** (host `modprobe.d` blacklist).

After the **host** dev machine rebooted mid-session, `reliable_load
--boot-to-shell` / `auto_mount_root` began reporting "/dev/ttyACM0 never
appeared", so the guest could not be driven past `mountroot>`. The board was
healthy and its USB console gadget *did* enumerate (`lsusb` showed
`1d6b:0010 USB Console`). The first hypothesis blamed the freshly-integrated
HDMI HUD refresh starving CPU1 — a plausible, entirely wrong theory that
survived one code change (delaying the HUD) that did nothing.

The gadget presents as CDC-ACM, but its "USB Debug" product id **1d6b:0010** is
claimed by the host's in-tree **`usb_debug`** driver *before* `cdc_acm` can
bind it — so no `/dev/ttyACM*` node is ever created. A host reboot had reloaded
`usb_debug` and re-established it as the binder. Unbinding it and binding
`cdc_acm` by hand made `/dev/ttyACM0` appear instantly; a
`/etc/modprobe.d/blacklist-usb_debug.conf` makes it permanent.

**Lesson:** when a tool that worked yesterday fails today and the *board*
looks healthy, check what changed on the **host** before theorizing about the
firmware you just wrote. "The gadget enumerates but no `ttyACM` appears" is a
driver-binding symptom, not a device symptom — `lsusb` + the sysfs `driver`
symlink tell you which layer to blame in ten seconds.

---

## 12. Four wrong answers about who wrote 64 KiB of console log

**2026-08-01.** `growfs` on the guest root wedged the board. Over the next few
hours I produced four confident diagnoses, in this order, and every one was
wrong:

1. **A lost virtqueue kick.** Refuted by the counters: `g_kicks_seen ==
   g_heads_popped == g_hdrs_read`, every kick seen and consumed.
2. **A lost completion in the ack window.** A real race — the ack clears
   `VRING` even for a completion published after the guest read
   InterruptStatus, and the used-lock cannot prevent it because the losing
   interleaving needs no torn write. I fixed it, wrote a test that caught a
   genuine bug in my own first attempt (a watermark sampled at injection is
   moved by the very event it must detect), shipped it — and it changed
   nothing. Slot [61] stayed 0 and the hang reproduced **at the identical
   sector**, which alone disproves a race. Kept in the tree with an explicit
   retraction in its commit message; the race is real, the credit was not.
3. **"The kick never reaches `vblk_kick()`."** Built on `g_dabt_seen` climbing
   while `g_kicks_seen` did not. But `g_dabt_seen` lives in vblk's window and
   increments **before** the window-address check, so it counts every guest
   data abort including other devices'. vblk's own in-window counter
   (`g_faults`) never moved at all. The guest was hammering virtio-**net**.
4. **"The filesystem is full."** Plausible — 106% used, `-51M` available, and
   `growfs` does need free blocks in the existing fs. Refuted by freeing
   137 MB and watching it wedge again at the same LBA.

### What actually made these possible

Not one of the four failed on reasoning. They failed on **measurement
hygiene**, in three recurring ways:

- **Reading a frozen value as a live one.** `last_fault_ipa` held
  `0xa001050` for minutes; I read it as "where the guest is faulting now". A
  last-value slot needs a delta, or a label saying it is not one.
- **Reading one subsystem's counter for another's problem.** See (3). A
  counter named `dabt_seen` sitting inside vblk's window must count vblk's
  aborts.
- **Non-hermetic experiments.** I zeroed 16 bytes and read 128, then called
  the untouched tail "the text came back". Later I zeroed before boot A and
  drew conclusions after boot B. Each time the fix was the same: zero, *verify
  the zeros*, then perturb exactly one thing.

### The instrument that ended it

The writer was found only after building one: `hwbp_set_wp_el2()`, a
watchpoint that matches **EL2** stores (`HMC=1, PAC=0b10`). Every watchpoint in
the tree until then matched EL1&EL0 only, so "which of our own instructions
wrote this?" was literally unanswerable. Its first version armed on CPU0 alone
and produced a fifth wrong answer, because `DBGWVR`/`DBGWCR` are **banked per
PE** and CPU1 — which owns EMAC, usbacm and therefore the console path — was
the one core guaranteed unwatched. That is the third time per-PE banking has
misled this project.

Armed on all four cores, with the region zeroed and the zeros verified, it
returned a *useful negative*: the bytes reappear and **no CPU instruction on
any core writes them**. The guest cannot either (stage-2 carves the whole 2 MiB
block; `hvscr_f=1`). That leaves a DMA master, and turns the next search from
"grep the source" into "check descriptor and buffer bases".

### The root cause of the collision itself

Separately from the hang: those windows were being clobbered because
vconsole's capture buffer was **derived** from its neighbour
(`RING_BASE + HDR_SIZE`) and later grown 3 KiB -> 64 KiB, so it marched through
six other subsystems' windows. Two things kept it invisible — a comment
asserting "vconsole ends 0x50001b10", true only of the 3 KiB version, and a
host constant saying 4 KiB while the firmware wrote 64 KiB, so nobody ever read
far enough to see the damage. The overlap itself was *documented and
deliberate* ("fine while chasing the guest root-mount"); the hunt ended, the
trade did not.

The fix worth copying: the buffer base is now **absolute, not derived**, the
header is **self-describing** (version, base, size) so host and firmware cannot
drift, and `hv_addrmap.h` grew a compile-time non-overlap chain for the low
block. The pre-existing chain covered only `0x50020000..`, which is precisely
why it missed this. Restoring the old derived address now fails the build.

### Rules this earned

- A breadcrumb window's address must never be computed from a neighbour's.
- Publish every counter as a real zero at init, or "never happened" and "this
  build has no such counter" (`0xffffffff`) are indistinguishable.
- Anything shared between host and firmware should be self-describing.
- Before concluding *anything* from a counter, sample it twice.
- When a hypothesis needs a fifth revision, stop and build the instrument.

## Further reading in this tree

- `docs/aw-mmc-dma-coherency.md` — the long-form, still-open DMA coherency
  investigation referenced in story 4.
- `docs/el2-nc-guest-dram.md` — the design for the non-cacheable EL2 remap
  proposed (but not yet enabled) as its fix.
- `docs/virtio-blk-design.md` / `docs/virtio-blk-integration.md` — the
  virtio-blk device stories 3 and 4 happened inside.
- `DEBUG_RULES.md` — the operating rules this project distilled from
  exactly these kinds of incidents (R2, in particular, is the rule story 8
  is a direct illustration of).
