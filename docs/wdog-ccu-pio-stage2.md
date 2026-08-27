# CCU/PIO/WDOG page: the watchdog trap (finding H3, name TBD)

Same spirit as `docs/dma-bypass-stage2.md`: a clear-eyed writeup of a known,
previously-unenforced gap, with the arithmetic verified against the live DTB
(`/opt/bzdos/tftpboot/bananapi-min.dtb`, `dtc -I dtb -O dts`), a real
implementation (`wdogtrap.c`/`wdogtrap.h`) that is compiled into every
real-hardware target, and a hosted test (`test_wdogtrap.c`, wired into `make
test`) — but the stage-2 table edit that actually traps the page stays behind
a default-OFF flag (`STAGE2_TRAP_WDOG_PAGE`, `stage2.c`), **NOT hardware
validated**. See "What was actually changed" at the bottom for the precise
landed/not-landed line.

## The finding, precisely

`CCU` (`clock@1c20000`), `PIO` (`pinctrl@1c20800`) and the watchdog
(`watchdog@1c20ca0`, inside the `timer@1c20c00` block) all live in the same
4 KiB physical page, `0x01C20000`-`0x01C20FFF`. Verified two ways:

- **Address arithmetic**: `SOC_A64_CCU_BASE` = `0x01C20000` is itself the page
  base (`soc_a64.h:53`). `SOC_A64_WDOG_CTRL` = `0x01C20CB0` (`soc_a64.h:70`)
  is `0x01C20000 + 0xCB0`, inside the same 4 KiB page. This is the same L2
  block (index 14, `UART_L2_IDX`, `stage2.c:187`) and the same L3 page index
  (32) the task that started this investigation cited — re-derived
  independently here as `WDOGTRAP_L3_IDX` (`stage2.c:228`), with a
  `_Static_assert` pinning it (`stage2.c:229-232`, mirroring the existing
  `VGICD_L3_IDX == 129` assert at `stage2.c:201-204`).
- **Live DTB** (`dtc -I dtb -O dts /opt/bzdos/tftpboot/bananapi-min.dtb`):

  | Node | `reg` | Span | `status` |
  |---|---|---|---|
  | `clock@1c20000` | `<0x1c20000 0x400>` | `0x1C20000`-`0x1C203FF` | okay (implicit) |
  | `pinctrl@1c20800` | `<0x1c20800 0x400>` | `0x1C20800`-`0x1C20BFF` | okay (implicit) |
  | `timer@1c20c00` | `<0x1c20c00 0xa0>` | `0x1C20C00`-`0x1C20C9F` | okay (implicit) |
  | `watchdog@1c20ca0` | `<0x1c20ca0 0x20>` | `0x1C20CA0`-`0x1C20CBF` | **disabled** |

  All four fit inside one page with 0x340 bytes spare at the top
  (`0x1C20CC0`-`0x1C20FFF`, no node claims it).

That page is one of the ordinary identity-mapped entries
`stage2_build_mmio_tables()` (`stage2.c`) falls through to today — it already
lives inside `stage2_l3_uart[]` (the same table UART0 and GICD are trapped
from, since `UART_L2_IDX == 14` covers this whole 2 MiB block), so trapping
it is **one more entry in an existing table**, exactly the argument
`stage2.c`'s own GICD-trap comment makes (`stage2.c:190-194`) and
`dma-bypass-stage2.md` makes for the DMA controller. No new table, no new
level.

**Consequence, unchanged from the prior pass's framing**: the guest can write
`WDOG_CTRL`/`WDOG_CFG`/`WDOG_MODE` — the entire hardware lever this project's
automatic-recovery story depends on (SESSION-RULES.md R2) — and nothing
stage-2 does today stops it.

## The DTB-disabled watchdog node: deliberate, and still not enough

The prior pass noted the guest's own DTB marks `watchdog@1c20ca0` disabled
and no FreeBSD driver attaches. Verifying that claim (task's own instruction)
turned up more than "currently true, might drift":

**It is a deliberate, historical fix, not an accident of the base DTB.**
`PROGRESS.md:99` ("Watchdog Disable in DTB", 2026-07-15): *"Traced guest
hang/reset issues to FreeBSD's own watchdog driver `aw_wdog.c` attaching to
the hardware watchdog node and disabling the WDT we configured in EL2 as a
safety mechanism. Patched `/opt/bzdos/tftpboot/bananapi-min.dts` to add
`status = "disabled"` to `watchdog@1c20ca0`, preventing the guest from
disabling our safety net."* This is not "nothing touches it by luck" — a
real, live bug (`aw_wdog(4)` attaching and disabling the hardware watchdog
outright) was found and patched around. `gen_config.py` — the tool that DOES
programmatically manage DTB edits for this project (cpu@N nodes,
virtio_mmio@ device nodes, from `board-config.xml`) — has no watchdog logic
at all; the disabled status lives only in the hand-maintained `.dts` file,
one manual edit with no automated re-assertion.

**Why that is not enough on its own.** `docs/dma-bypass-stage2.md` already
makes this exact argument for the DMA controller case, and it applies here
verbatim: *"don't give the guest a device at all (DTB `disabled`, which is
convention only and unenforceable against a compromised guest — exactly the
reason stage-2 CPU-side denial has value...)"*. Concretely for the
watchdog: a `status = "disabled"` node stops FreeBSD's **driver-attach
framework** from probing it. It does nothing to a root shell that opens
`/dev/mem` (allowed by default at low `kern.securelevel`, which this image
does not appear to raise) and writes the three words directly, or to any
custom `kldload`ed module, or to a `sysctl`/`devmem`-style userland tool. The
guest already has a real root shell on this board (`guest-ssh-access-works`
memory) — "no in-tree driver attaches" says nothing about what a shell
command can do.

**So the correct framing (per the task) is defense in depth against a real
hole, and the two layers are NOT redundant**: the DTB edit stops the common,
accidental case (a stock driver innocently attaching, which is the actual bug
that was found and fixed); a stage-2 trap would be the only layer that also
stops a deliberate or buggy direct-physical-address access, and it is the
only layer that survives the DTB edit being silently lost (a DTB regenerated
from a fresher `sun50i-a64.dtsi`, a rebase of `bananapi-min.dts`, or simply a
future edit that touches the wrong node — this file has exactly one manual
`status = "disabled"` line and nothing re-asserts it).

## EL2's own use of these registers — and why a trap cannot break it

Every EL2 write to WDOG/CCU/PIO in this tree is enumerated below. The load-
bearing fact that makes trapping the page for the *guest* safe with respect
to *all* of these: **stage-2 translation governs only EL1/EL0 accesses.**
`stage2.c`'s own A1 comment states this directly (`stage2.c:493-495`,
restated for CCU/PIO/WDOG in `wdogtrap.h`'s header comment): `VTTBR_EL2`/
`VTCR_EL2` are consulted by the CPU only when it is executing at a lower EL.
EL2's own loads/stores use the flat, MMU-off-or-identity mapping U-Boot
leaves behind (same fact `musb.c` and `wdt.c` rely on already) and are never
subject to a stage-2 walk at all. A stage-2 table edit that makes the guest's
identical access fault therefore cannot, structurally, touch any EL2 code
path — there is nothing for the edit to break here, independent of whether
the trap's *emulation* logic (`wdogtrap.c`) is correct.

Concretely, everything EL2 does on this page:

- **`wdt.c`**: `wdt_arm()` (`wdt.c:74-105`, called from every `main*.c`'s
  boot path) programs `WDOG_CFG`/`WDOG_MODE`/`WDOG_CTRL` to start the 16 s
  hardware timer. `wdt_pet()` (`wdt.c:116-129`, called from `el2_trap()` on
  every EL2 exception, `el2_exc.c:798`) re-arms it while guest console
  progress is fresh. `wdt_debug_kick()` (`wdt.c:158-163`) is CPU1's
  unconditional kick — as of the 0.0.2 core-layout change
  (`ORIENTATION.md`/`vcpu1.h`), this runs from CPU1's unmaskable 10 ms EL2 tick
  (`gic_timer.c:1530`, gated `dbg_cpu1_wdog`), not a tight loop, specifically
  so a software hang of the guest running as CPU1's own vCPU cannot starve
  it.
- **`reboot.c`**: `reboot_clean()` (`reboot.c:36-46`) arms a fast ~2 s
  watchdog directly, for the clean-disconnect reboot path.
- **PC5 pinmux** (`SOC_A64_PIO_PC_CFG0`, inside the PIO sub-range): set once
  at boot (`main_dbg.c:551-566`) and re-asserted from two more places — see
  the next section.

None of this is disturbed by trapping the *guest's* view of the page.

## Policy design — three sub-ranges, not one page-wide rule

### 1. WDOG (`SOC_A64_WDOG_NODE_BASE`, offset `0xCA0`-`0xCBF`, 32 bytes)

The DTB node's own declared span, not just the 3 registers `wdt.c` pokes —
wider on purpose, to also deny `WDOG_IRQ_EN`/`WDOG_IRQ_STA` at `0xCA0`/
`0xCA4`, which nothing in this tree uses but which a guest still could.

- **Writes: refused.** Absorbed as a no-op (the same "handled, nothing
  propagates" contract `mmio_absorb.c` already uses elsewhere in this tree)
  rather than rejected with a fault the guest would have to handle — the
  guest's store instruction completes normally from its point of view, the
  real register never changes. Counted loudly
  (`HVMAP_WDOGTRAP_BC` word 4, `g_wdog_refused`): must read 0 on a
  correctly-behaving guest; any nonzero value is exactly the signal this
  file exists to surface.
- **Reads: pass through.** Same reasoning `vgicd.c` gives for its own read
  policy (`vgicd.c:216-219`, *"knowing another partition's configuration is
  not the threat here, changing it is"*): once every write is refused, a
  read tells the guest nothing it can act on. (Considered and rejected: a
  synthetic always-disabled readback, to keep the guest's view consistent
  with the DTB's own "disabled" fiction. Passthrough was chosen for
  consistency with the one read-policy precedent already in this tree
  rather than inventing a second convention — see "What was not decided".)
- **Ordering is load-bearing.** The WDOG check runs FIRST, unconditionally,
  with a hard `return` on any match, reads included
  (`wdogtrap.c`, write-path comment). This is deliberate, not merely the
  natural order: it means a later refactor that adds more classified ranges
  to this function cannot silently reorder past it and fall through to the
  passthrough default for a WDOG offset. The comment in `wdogtrap.c` says so
  explicitly and asks that this never become a table/switch a case could be
  inserted ahead of.
- **Why a plain start-offset check is correct here** (no `nbytes`/overlap
  math, unlike PC5 below): Device-nGnRE memory guarantees an unaligned
  access always takes an alignment fault, so every access is naturally
  aligned to its own size. `0xCA0` and `0xCC0` are both 4-byte aligned, so no
  naturally-aligned 1/2/4-byte access can straddle either boundary. Pinned by
  a hosted test (`test_wdog_range_aligned_word_before_cannot_straddle`).

### 2. `PIO_PC_CFG0` (`SOC_A64_PIO_PC_CFG0`, one word, offset `0x848`) — the PC5 field only

This is the single register the PC5/eMMC-clock-pin fight is entirely about.
FreeBSD's `pinctrl` driver correctly muxes every other `mmc2` pin but leaves
PC5 at `gpio_in`, which kills the eMMC clock (`soc_a64.h:57-64`, `main_dbg.c:
551-559`). Today that is fought reactively, in three places doing the exact
same read-modify-write: `main_dbg.c:560-566` (once, before the guest boots),
`smp.c:586-596` (CPU1's old tight debug-core loop, live when `dbg_vcpu1` is
NOT armed), `gic_timer.c:1702-1707` (CPU1's tick, live when `dbg_vcpu1` IS
armed — the 0.0.2 default).

- **Writes: PC5's nibble (bits[23:20]) forced to `0b0011` (func 3/mmc2),
  unconditionally, on every write that touches it — every other nibble in
  the register passes through exactly as the guest wrote it.** This is a
  ONE-FIELD policy, not a PIO-wide one: PC0-PC4/PC6/PC7's function fields in
  the same register are the guest's to set.
- **Byte-partial correctness.** An offset-equality check (`off ==
  PC_CFG0_OFF`) is NOT sufficient: PC5's nibble is the upper nibble of BYTE 2
  of the register (offset `PC_CFG0_OFF + 2` = `0x84A`), and nothing stops a
  driver from touching just that byte or a halfword starting there, at a
  validly-aligned offset the equality check would miss entirely — silently
  reopening exactly the hole this trap exists to close. `wdogtrap.c`'s write
  path instead computes the access's actual byte range (`off`, `off+nbytes`
  from `ESR_EL2.SAS`) and forces the nibble at the correct bit position
  whenever that range overlaps `PC5_BYTE_OFF`. Six of the twelve hosted
  tests in `test_wdogtrap.c` are specifically about this overlap math (byte
  at the PC5 offset, halfword at the PC5 offset, word at the register base,
  a byte one earlier that must NOT match, a non-base-aligned word that must
  still compute the right bit shift).
- **Reads: pass through unmodified** — the real hardware value, which
  (because every write already forces PC5) should already read back func 3.

### 3. Everything else on the page — default: PASSTHROUGH, and why

The rest of CCU (PLL/clock-gate registers), the rest of PIO (every other
port's pinmux, including the `mmc0`/`mmc1`/`uart0-4`/`rmii`/`rgmii` pin
groups the live DTB's own `pio` phandle list advertises as in-use), and
`timer@1c20c00` — all real, full read/write, SAS-sized exactly like
`vgicd.c`'s `gicd_rd`/`gicd_wr`.

**This is the one sub-range with "no narrow fix known", and the task asked
for the default to be defended, not just stated.** `docs/dma-bypass-
stage2.md`'s own inventory already established why CCU cannot be
blanket-denied: *"Engines the guest genuinely needs (GIC, CCU, both
EHCI/OHCI pairs, all three MMC/eMMC controllers) cannot be stage-2-denied at
all without breaking the working guest."* The live DTB confirms PIO is the
same shape — every pin group a real, enabled driver (EHCI's `rgmii_pins`ish
neighbours aside, concretely `mmc0_pins`/`mmc2_pins`/`uart0-4_pins`) would
touch is enabled, not a candidate for denial.

**A blanket "pass through anything I don't recognise" reached by falling off
the end of an if/else chain is exactly the failure mode that would reopen
the WDOG hole** — if a future refactor turns the classification into a
table/switch and a new case gets inserted ahead of the WDOG check, or the
WDOG check is accidentally deleted during a merge, the "default" absorbs the
regression silently. The defense adopted here is structural, not
disciplinary: the WDOG check is not a case among cases, it is the first
thing the function does after decoding the access, with a hard `return`, and
a `_Static_assert` (`wdogtrap.c`) pins its offset window against the exact
DTB-verified numbers so the window itself cannot silently drift. The
passthrough default is therefore not "everything not otherwise handled";
it is "everything that reached this line, which the WDOG check has
already had first refusal on."

### What was decided vs. left open on read policy

Decided: WDOG reads pass through (§1). PC_CFG0 reads pass through (§2).
Left open: whether WDOG reads should instead return a synthetic
always-disabled value, matching the DTB's own "disabled" story more
tightly (considered, not implemented — see §1). This is a judgment call
with no board-observable difference (nothing legitimate reads this range
today), noted rather than silently decided.

## What was landed, and why not more

**Landed** (compiles for every real-hardware target, `make test` green):

- `soc_a64.h`: `SOC_A64_WDOG_NODE_BASE`/`SIZE`, the DTB-verified 32-byte
  watchdog-node span.
- `wdogtrap.h`/`wdogtrap.c`: the full trap-and-police handler, structured
  identically to `vgicd.c` (ESR decode, HPFAR/FAR reconstruction, SAS-sized
  real-hardware access, breadcrumbs at `HVMAP_WDOGTRAP_BC`). Linked into
  every real-hardware target (`DBG_OBJS`/`DUAL_OBJS`/`GDB_OBJS`/
  `FBSD_OBJS`/`ZEPHYR_OBJS`/`REPL_OBJS`, alongside `vgicd.o`) and called
  unconditionally from `el2_exc.c`'s guest data-abort dispatch, same
  "handled → return without recording" contract as every other device
  there.
- `stage2.c`: the L3-table classification arm that actually traps the page
  — but wrapped in `#ifdef STAGE2_TRAP_WDOG_PAGE`, default undefined. With
  the flag off, `stage2_build_mmio_tables()` emits the exact same descriptor
  for L3 index 32 as before this change (verified: see below).
- `test_wdogtrap.c`: 12 hosted tests covering the WDOG-range boundary
  (including the "cannot straddle" alignment argument) and every PC5
  byte/halfword/word overlap case, wired into `make test`
  (`Makefile:132,146` and the `test_wdogtrap` build rule).

**NOT landed**: `STAGE2_TRAP_WDOG_PAGE` is not defined in any shipping
target, so the page stays identity-mapped and no guest access to CCU/PIO/
WDOG faults at all — today's behaviour, unchanged.

**Why the flag stays off — the same hard constraint `dma-bypass-stage2.md`
named, plus one new risk specific to this page:**

1. **No hardware access this pass** (the task's own constraint, and
   `DEBUG_RULES.md`'s R4: iteration on the board is expensive and this page
   is on the critical path of eMMC clocking — wrong here means a board that
   does not boot). The trap's correctness for CCU/PIO/WDOG has been verified
   against the live DTB and by hosted test, never against real silicon.
2. **Trap-volume risk, not just correctness — this is the part that is
   genuinely new versus the GICD/DMA-controller precedents.** GICD is
   touched "during interrupt SETUP, not on the acknowledge/EOI hot path"
   (`vgicd.h`'s own COST section) — a per-configuration cost, safe to make
   unconditional. CCU/PIO have no equivalent citation in this tree. A
   plausible and NOT ruled out failure mode: Allwinner clock-enable
   sequences commonly poll a PLL LOCK bit in a tight loop after writing a
   PLL control register (a `while (!(reg & LOCK)) ;`-shaped wait, sometimes
   with a bounded iteration count rather than a wall-clock deadline). If any
   such loop exists in this board's FreeBSD `aw_ccu`/`aw_pio` driver path,
   turning every CCU/PIO access into a stage-2 fault-and-emulate round trip
   would multiply each poll iteration's cost by roughly the cost of a full
   trap (ESR decode, HPFAR/FAR read, breadcrumb store, SAS-sized real
   access) — potentially changing whether a bounded-iteration wait sees the
   lock bit set in time. This was NOT verified against the actual FreeBSD
   driver source in this pass (out of scope for a board-free, source-tree-
   only investigation) and is exactly the kind of thing that must be
   measured on hardware, not guessed at.

**What DOES make this safe to graduate once someone has board access**: flip
`STAGE2_TRAP_WDOG_PAGE`, `make dbg`, reload once, and specifically watch (a)
that the guest still reaches a mounted root with the eMMC intact — the
existing smoke-boot is sufficient for correctness — AND (b) the trap-volume
question §2 raises: read `HVMAP_WDOGTRAP_BC` word 1 (`g_total`) after boot to
see how many times this page was actually touched, and time-to-mounted-root
against a baseline run without the flag. If PC5's own reassertion sites
(`main_dbg.c`/`smp.c`/`gic_timer.c`) can then be observed to do nothing
(`g_pc5_reforced`, word 5, would be the count of times THEY would have had to
act — compare against those sites' own no-op branches) that is the concrete
evidence for §"PC5 reassert removal" below.

## Is PC5 reassert-removal a consequence of this work, or a separate change?

**A consequence, but not an automatic one — a second, separate change that
this work makes safe to make.** Landing `STAGE2_TRAP_WDOG_PAGE` does not by
itself delete the three reassertion sites; they would keep running
alongside the trap, redundantly (and harmlessly — RMW-if-drifted, so a
no-op once the trap holds PC5 at func 3). Removing them is a distinct,
smaller follow-up PR that depends on this one having been hardware-validated
first: only once the trap is confirmed to hold PC5 correctly on real
hardware, across FreeBSD's actual pinctrl-driver attach sequence, is it safe
to delete the code that currently does the same job reactively. Doing both
in one change would violate DEBUG_RULES.md R4 (one variable per board
cycle) — if something regressed, there would be no way to tell whether the
trap or the reassert-removal caused it.

## Hardware validation — concrete, board-specific

**Proves the WDOG refuse works without disabling the recovery EL2 depends
on:**

1. Boot with `STAGE2_TRAP_WDOG_PAGE` on. Confirm EL2's own recovery still
   works exactly as before: `bmc wdt disarm` / a deliberate hang, and watch
   the board come back via the hardware watchdog within its ~16 s window,
   same as today (SESSION-RULES.md R2's existing "watchdog = normal
   auto-recovery" behaviour, unaffected because EL2's own accesses never go
   through stage-2 — see the section above).
2. From the guest (root shell, `guest_sh.py`/ssh), attempt a direct write to
   the real physical addresses via `/dev/mem` (or a tiny loaded module) at
   `0x1C20CB0`/`0x1C20CB4`/`0x1C20CB8`. Confirm: the guest's write appears to
   succeed (no fault visible to it), but `HVMAP_WDOGTRAP_BC` word 4
   (`g_wdog_refused`) increments, and — the actual proof — the board's
   watchdog behaviour is unchanged (it still resets on schedule if EL2 stops
   petting it; the guest's write did not disarm anything).
3. Confirm `HVMAP_WDOGTRAP_BC` word 4 stays at 0 across an ORDINARY boot +
   idle period (no guest ever legitimately touches this range today, so any
   nonzero count outside the deliberate test in step 2 is itself a finding).

**Proves PC5 stays correct with the reassert hack removed** (a LATER,
separate validation, once the follow-up change lands — see above):

1. Boot with `STAGE2_TRAP_WDOG_PAGE` on and the three reassertion sites
   removed. Confirm the eMMC still enumerates (`CMD0` → `CMD_DONE`, `CMD1`
   OCR nonzero, same signature `main_dbg.c:555-556` already documents) and
   root mounts from `vtbd0p3` as today.
2. Read `HVMAP_WDOGTRAP_BC` word 5 (`g_pc5_reforced`) after boot: nonzero
   confirms the trap actually intervened (FreeBSD's pinctrl driver did try
   to clear PC5, and was overridden) rather than the test being vacuous
   because the driver never touched the field at all.
3. Run the project's existing soak/reload cycle a meaningful number of times
   (the ledger's "100 clean boots" gate, or a bounded subset) with the
   reassertion sites gone, watching specifically for the eMMC-clock-death
   failure mode PC5 protects against — this is the regression the whole
   design exists to prevent, so it is the one condition worth spending real
   board cycles on before calling the follow-up done.

## What could not be determined from the code alone

- **Whether any FreeBSD driver in the actual booted kernel build polls a
  CCU or PIO register in a tight loop** (the PLL-lock-wait concern in
  §"What was landed", point 2). This tree does not carry FreeBSD kernel
  source; resolving it needs either the actual kernel config/source that
  produced the boot image, or a live measurement (§"Hardware validation").
- **Whether PC5's nibble is ever touched by a sub-word (byte/halfword)
  access in practice.** The overlap-math fix in `wdogtrap.c` defends against
  this regardless, but whether real FreeBSD `pinctrl-sunxi`-style code ever
  issues anything narrower than a 32-bit RMW on this register was not
  confirmed from source in this tree.
- **The exact trap-volume this page would see in one boot** (how many CCU/
  PIO accesses total, and their timing) — only measurable on hardware
  (`HVMAP_WDOGTRAP_BC` word 1 exists specifically to answer this once the
  flag is flipped).
- **Whether `kern.securelevel` or any other guest-side hardening is high
  enough to block `/dev/mem` writes** in the shipped FreeBSD image — stated
  above as "not raised by default" based on this being a development/debug
  image with root ssh access enabled; not independently confirmed against
  actual `securelevel` sysctl output on the board (out of scope, no board
  access this pass).

## What was actually changed

- `soc_a64.h`: added `SOC_A64_WDOG_NODE_BASE`/`SOC_A64_WDOG_NODE_SIZE`.
- `hv_addrmap.h`: added the `HVMAP_WDOGTRAP_BC` breadcrumb window.
- `wdogtrap.h`, `wdogtrap.c`: new files, the full trap-and-police handler.
- `stage2.c`: `#include "wdogtrap.h"`; the `WDOGTRAP_L3_IDX` derivation +
  asserts (unconditional, cheap, just arithmetic); the actual table-mutating
  arm inside `#ifdef STAGE2_TRAP_WDOG_PAGE` (default undefined — no effect
  on any shipping target).
- `el2_exc.c`: `#include "wdogtrap.h"`; one more unconditional dispatch call
  in the guest data-abort chain, same shape as the seven devices already
  there.
- `Makefile`: `wdogtrap.o` added next to `vgicd.o` in every real-hardware
  target's object list; `test_wdogtrap` added to `make test`/`make clean`.
- `test_wdogtrap.c`: new hosted test file, 12 tests, all passing.
- **Verified board-free**: `make test` green (including the two new tests
  above alongside the existing 20-odd suites); `./ci.sh` green (one full
  run had three unrelated QEMU-timing flakes — `snapshot-qemu-ci.sh`,
  `dual-qemu-ci.sh`, `dual-zephyr-qemu-ci.sh` — each re-run in isolation and
  passed; two of the three don't even link any file this change touched,
  confirming host load, not this change, was the cause); a `make dbg
  EXTRA_CFLAGS=-DSTAGE2_TRAP_WDOG_PAGE` build compiles clean and links, and
  its `.elf` differs from the flag-off build starting exactly at
  `stage2_build_mmio_tables()` (confirmed via disassembly diff — every
  downstream symbol shifts by the same 32 bytes, consistent with one new
  branch block and nothing else changing) — i.e. the flag has a real,
  minimal, isolated effect and nothing else moved.
- **Not touched**: the board. No `reliable_load.py`/`board_ctl.py`/
  `bzdctl.py`/`chimpd.py`/`ttyACM0`/EMAC/dbgmon/ssh-to-guest/reset of any
  kind.
