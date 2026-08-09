# The Zephyr guest (`make zephyr` / `make zephyr-qemu`)

## Status, 2026-08-04

| Claim | True? | How it was checked |
|---|---|---|
| `make zephyr` **links** | ✅ | from-scratch build, `rm -f *.o` first: 49 883 B text |
| A Zephyr guest image **builds** | ✅ | `zephyr-guest/build.sh bpi_m64_hv` → `zephyr.elf`, EXEC, one `PT_LOAD` @ `0x51000000`, entry `0x5100100c` |
| The hypervisor **loads** it | ✅ | `kload_parse_elf` + `kload_place_segments` on the real image, board-free under QEMU |
| It **boots** to its own `main()` | ✅ | `./zephyr-qemu-ci.sh` — banner, `hello from EL1`, heartbeats 0 and 1 |
| It boots **on the board** | ✅ **confirmed 2026-08-09, after the Finding-1 fix** | see "Confirmed on real hardware" below |

Those five rows are deliberately separate. "Links" is not "loads" and
"loads" is not "boots"; this project has burned time before on a report that
blurred them.

## Result on real hardware (2026-08-06)

Loaded via a reversible swap of `tftpboot/kernel` for `tftpboot/zephyr.elf`
(SHA256-verified before and after, chimpd's own `HYP_ELF`/`KERNEL` constants
monkeypatched at the Python object level rather than edited on disk — no
file in the git tree changed for this test, and the guest-image TFTP name is
a hardcoded string literal inside `chimpd.py`'s closures, not sourced from
its `KERNEL` global, so that half had to be a real file swap regardless of
any patch). `loady`+`bootelf` completed normally ("go sent"). EMAC never
answered — not a patience problem: polled continuously for ~80s across two
separate attempts, zero responses both times. After the first attempt the
board had already dropped back to U-Boot's own USB gadget (`1f3a:efe8`),
consistent with the project's own watchdog-auto-recovery design firing
because nothing pets it — i.e. the HV hung or crashed early enough that it
never reached its own EMAC/dbgmon service loop, or `bootelf` never actually
jumped for a reason that has no analogue under QEMU.

Not chased further live on hardware without a plan — this is a first
finding, not a diagnosed bug. The board-free gate above stays a valid,
narrower claim (loads and boots *under QEMU*); the gap between that and real
hardware is itself the useful signal. Candidates worth checking before the
next hardware attempt, none confirmed: `main_zephyr.c` initialises real A64
peripherals `main_zephyr_qemu.c` never touches (nothing under QEMU exercises
that code at all), and the GIC/vGIC path is a known black hole under QEMU
specifically — a divergence there would be invisible to every board-free
run to date.

The board was restored to its normal `dbg`/FreeBSD configuration after this
test (`tftpboot/kernel` restored from a SHA256-verified backup, reloaded via
plain `chimpd.py`). The reload landed with root read-only (expected: the
running guest was hard-reset via `bzdctl.py power reset` to force it back to
U-Boot, not a cooperative `shutdown -p`, so the mid-session A2 guarantee
does not apply here) — cleared with the documented `fsck_ffs -y` +
`mount -u -o reload` path, then `service netif restart` + a manual default
route, since `/etc/rc` had stopped before reaching network configuration too.
Confirmed recovered: ssh answers, root `rw`, filesystem intact at 15% used.

## The two halves

**The guest image** is built out of tree from `zephyr-guest/` against a
Zephyr v4.4.1 checkout (565 MB, deliberately *not* vendored into this repo):

```sh
ZEPHYR_BASE=/opt/bzdos/zephyr-work/zephyr ./zephyr-guest/build.sh bpi_m64_hv
```

It is a plain `EXEC` ELF with a single `PT_LOAD` at physical `0x51000000`
(`p_vaddr == p_paddr`, non-relocatable) and entry `0x5100100c`. See
`zephyr-guest/boards/bzdos/bpi_m64_hv/` for the board port and
`zephyr-guest/LOADING.md` for the full build/flash procedure, including why
`-mno-outline-atomics` is required with this host's Linux-hosted cross
compiler.

**The hypervisor firmware** comes in two flavours that run the *same*
guest-loading sequence:

| Target | Runs on | Guest console | Progress readable via |
|---|---|---|---|
| `make zephyr` (`main_zephyr.c`) | real board | vconsole 16550 emulation | EMAC debug channel / breadcrumb `0x50008000` |
| `make zephyr-qemu` (`main_zephyr_qemu.c`) | QEMU `virt` | vconsole 16550 emulation | PL011, straight to stdout |

Sequence, identical in both: `kload_parse_elf(0x44000000)` →
`kload_place_segments(…, 0x51000000)` → `guest_config()` → `stage2_init()` /
`stage2_enable()` → `kload_enter(entry, 0, …)`. `kload.c`, `stage2.c`,
`guest.c`, `vconsole.c` and `exceptions.S` are used **unmodified** and are
the same objects the board build links — that reuse is itself the evidence
that they are guest-OS-agnostic rather than FreeBSD-specific.

## Running it without a board

```sh
./zephyr-qemu-ci.sh      # ~1.6 s; also part of ./ci.sh
```

Expected tail:

```
HV: segments placed at 0x51000000, entry_pa=0x000000005100100c
HV: stage-2 AT S12E1R self-check PASS (PAR_EL1.F=0)
[guest] *** Booting Zephyr OS build v4.4.1 ***
[guest] bzdOS/Zephyr: hello from EL1 (board: bpi_m64_hv)
[guest] heartbeat 0
[guest] heartbeat 1
ZEPHYR-QEMU-CI: PASS …
```

The guest ELF is staged at physical `0x44000000` by QEMU's generic loader
(`-device loader,file=…,addr=0x44000000,force-raw=on`). `force-raw=on`
matters: QEMU drops the file as an opaque memory image and does **not**
interpret the ELF — `kload.c` does, exactly as on the board, where
`tftpboot 0x44000000 zephyr.elf` fills the same role.

### Why the *real board's* image runs under QEMU at all

The image is not a QEMU port. Its MMIO addresses are Allwinner A64
addresses. Two independent reasons it still works:

- **UART0 (`0x01c28000`) never reaches QEMU's memory system.** stage-2
  leaves exactly that 4 KiB page invalid (`stage2.h`, `UART0_BASE`), so
  Zephyr's stock `ns16550` driver faults into EL2 on every access and
  `vconsole.c` emulates a 16550. Whether QEMU has a device there is
  irrelevant. This is the *real* console mechanism, and it is the most
  valuable thing this gate exercises: a third-party driver, written with no
  knowledge of us, driving our emulation.
- **The GIC (`0x01c81000`) is a black hole.** It *is* identity-mapped, so
  guest accesses do go out to the memory system, where QEMU virt has
  nothing. Measured on QEMU 10.1.5: dropped silently (reads return 0), no
  external abort, so `arm_gic_init()` completes and boot continues.

### What a pass therefore does NOT prove

- **Nothing about GIC programming** (see above). Harmless today — this build
  is tickless and no interrupt is ever enabled by anyone — but the day the
  guest gets a real timer tick, this gate needs a GIC that answers.
- **Nothing about real-hardware timing**: no eMMC, no EMAC, no USB gadget,
  no watchdog, and `CONFIG_BUSYWAIT_CPU_LOOPS_PER_USEC=400` is an untested
  guess at a ~1.2 GHz Cortex-A53 that only ever gets exercised against TCG.
- **Nothing about the A64 watchdog path** `main_zephyr.c` arms and
  `main_zephyr_qemu.c` omits.

## Next step (needs the board)

1. `tftpboot 0x44000000 zephyr.elf` (already staged at
   `/opt/bzdos/tftpboot/zephyr.elf`); **no DTB transfer needed** — Zephyr's
   devicetree is compiled into the image.
2. `loady` + `bootelf -p` `microkernel-zephyr.elf` (`make zephyr`).
3. Expect the same console text the QEMU run prints, and breadcrumb
   `0x50008000` word[1] climbing to 7 with word[6] == `0x5100100c`.
4. Then re-time `CONFIG_BUSYWAIT_CPU_LOOPS_PER_USEC`, and only after that
   consider wiring `vgic_init()` + the GICC-IPA → GICV stage-2 redirect to
   give the guest a real tick.

Full procedure and failure signatures: `zephyr-guest/LOADING.md`.

## Deepened root-cause investigation (2026-08-09, board-free)

Static analysis only — no board access this pass (see this session's
constraints). Result: **two independent, code-confirmed bugs in
`main_zephyr.c` that each fully explain "EMAC never answered" without
requiring Zephyr itself to have failed to boot.** Both predate the
2026-08-06 hardware attempt (confirmed via `git log`/`git status` — the code
on disk is bit-for-bit what was tested), so both were live during that test.
Ranked by confidence.

### Finding 1 (HIGH confidence, deterministic — verified by reading the code, no board needed): the HV never starts any live debug service, and self-destructs ~6s after entering the guest regardless of what Zephyr does

`main_zephyr.c`'s `main()` never calls `emac_init()`, `smp_init()`,
`musb_init()`, `usbacm_init()`, or `dbgmon_init()` — compare against
`main_dbg.c` (line 106 `emac_init()`, line 168 `usbacm_init()`, line 182
`dbgmon_init()`, line 446 `smp_init()`) and `main_gdb.c` (same shape, lines
107/124/132/195). `emac.o`/`musb.o`/`usbacm.o` are linked into `ZEPHYR_OBJS`
(Makefile line 283) but nothing in `main_zephyr.c` calls their init
functions, so the EMAC block's clocks/PHY/RX-ring are never programmed and
CPU1 (the debug core that runs `dbgmon`/hosts the EMAC console — see
`CLAUDE.md`'s core layout) never comes up. **There is no server on the
other end of the EMAC protocol this build's own tooling (`hvdbg.py`,
`chimpd.py`) expects to poll.** Polling it for 80s was certain to see zero
responses no matter what Zephyr's guest code did — this is a test-harness/
firmware mismatch, not evidence of a guest-side hang. (`main_fbsd.c`, the
template `main_zephyr.c` explicitly mirrors "almost step for step," has the
identical gap — this is inherited, not Zephyr-specific.)

Separately, and independently sufficient to explain "board back in U-Boot
after ~80s": `main_zephyr.c` arms the A64 hardware watchdog with **raw MMIO
writes** right before `kload_enter()` (lines 147–149):

```c
*(volatile uint32_t *)0x01c20cb4UL = 1u;    /* WDOG_CFG:  reset-whole-system */
*(volatile uint32_t *)0x01c20cb8UL = 0x61u; /* WDOG_MODE: interval idx 6, EN=1 (~6s) */
*(volatile uint32_t *)0x01c20cb0UL = 0x14AFu; /* WDOG_CTRL: (re)start counter   */
```

This is the *exact same idiom* `reboot.c`'s `reboot_clean()` uses to
deliberately force a reboot ("arm a short interval, then spin without
petting") — so this pattern is a well-understood, intentional
"self-destruct in N seconds" primitive elsewhere in this tree. The
difference is `reboot_clean()` *wants* the reset. `main_zephyr.c` does not:
it then calls `kload_enter()`, jumping into Zephyr, hoping the watchdog
stays fed.

It won't. `wdt.c`'s real petting design (`wdt_arm()` + `wdt_pet()`, used by
`main_dbg.c` line 192, `main_gdb.c` line 155, `main_net.c` line 52,
`main_repl.c` line 113) tracks a software window (`wdt_window_ticks`,
`wdt_last_progress`) that only `wdt_arm()` initializes. `el2_trap()`
(`el2_exc.c:593`) unconditionally calls `wdt_pet()` on every EL2
exception — its own comment even says so: *"The WDT is armed (~16s) in
`main_dbg.c`."* But `main_zephyr.c` never calls `wdt_arm()`, so
`wdt_window_ticks` stays at its `.bss` value of `0`. `wdt_pet()`'s guard is:

```c
if (rd_cntpct() - wdt_last_progress < wdt_window_ticks)   /* window_ticks == 0 here */
    WDOG_CTRL = WDOG_CTRL_RESTART;
```

With `wdt_window_ticks == 0` this is false for any nonzero elapsed time, so
`wdt_pet()` is a permanent no-op for this build — it can *never* re-arm the
counter `main_zephyr.c` started by hand. **The raw ~6s watchdog armed at
line 149 is guaranteed to fire and hard-reset the board roughly 6 seconds
after `kload_enter()`, whether Zephyr boots perfectly, hangs, or crashes.**
That is fully consistent with "board had already dropped back to U-Boot's
own USB gadget" after the first attempt — that is the watchdog firing on
schedule, not necessarily a guest crash.

### Finding 2 (concrete mechanism, MEDIUM-HIGH confidence as an actual guest-side hang cause): the guest's "GICC" IPA is silently redirected to the real GICV *without* the pairing that redirect requires — real hardware only, invisible under QEMU

`main_zephyr.c`'s own header comment (written 2026-07-23, commit `f059acd`)
says stage-2 setup is reused "verbatim, no Zephyr-specific branches
anywhere," and that `vgic_init()` is "deliberately never called here,
exactly like `main_fbsd.c` doesn't call it today." That was true when
written. It stopped being the whole story two days later:

- `stage2.c`'s `stage2_build_mmio_tables()` (called unconditionally by
  `stage2_init()`, which both `main_dbg.c` and `main_zephyr.c` call with
  zero divergence) redirects **two 4 KiB pages at the guest's GICC IPA
  (`VGIC_GICC_BASE` = `0x01c82000`, `vgic.h:136`) to the real GICV physical
  pages (`VGIC_GICV_BASE` = `0x01c86000`, `vgic.h:142`)** — see
  `stage2.c`'s block comment right above the `j == 130u || j == 131u`
  branch. This is the standard KVM/GICv2-virtualization trick, and it is
  **unconditional**: no flag, no check for whether the caller also set up
  the rest of the virtualization story.
- That redirect was reapplied in commit `58e92fc` ("Reapply 'vgic: wire the
  interrupt-virtualization milestone...'") on **2026-07-25** — two days
  *after* `main_zephyr.c` was written and never revisited since (confirmed:
  `git log -- main_zephyr.c` shows no commits after the Jul 25 mechanical
  license-stamp pass; the file's substance is unchanged since Jul 23).
- The redirect's own comment is explicit about the precondition:
  *"CORRECT ONLY paired with IMO=1 + COMPLETE HW-mode LR forwarding... under
  the old IMO=0 policy the guest still needed the REAL GICC to service
  interrupts directly, so redirecting it to GICV left it unable to ack
  anything"* — describing almost verbatim the bug this project already hit
  and reverted once (`814d0fa`, "FIX-1", 2026-07-16) for a *different*
  consumer (the plain EL2 debug tick). `main_dbg.c` pairs the redirect
  correctly: it calls `vgic_init()` (line 353) and then explicitly ORs
  `HCR_EL2.IMO|FMO` (lines 335-337, 375-377) — with a long ordering comment
  explaining exactly why both are required together.
- `main_zephyr.c` does neither. `guest_config()` (`guest.c`) — the shared,
  unmodified function both builds call — only sets `HCR_EL2.RW`; it
  explicitly preserves-but-does-not-set IMO (`guest.c:81`, `:85`: "IMO...
  MUST stay 1" is a preservation comment assuming someone else set it, not
  a setter). Nothing in `main_zephyr.c` sets IMO. So for the Zephyr guest,
  the redirect is live (baked into the identity-mapped page tables
  unconditionally by `stage2_init()`) but the pairing it requires is
  absent — reproducing, for a new guest, exactly the failure mode this
  project already diagnosed and fixed once for the old one.

If Zephyr's ARM GICv2 driver touches the CPU-interface registers at all
during `arm_gic_init()` — plausible even in a tickless config; most Zephyr
SoC ports bring up the GIC's distributor *and* CPU interface unconditionally
before checking whether any interrupt source is actually enabled — those
accesses land on the physical GICV block instead of GICC, with IMO=0. GICV
without IMO=1 is documented in this same codebase as producing wrong
behaviour ("unable to ack anything"); it is a plausible *silent, very-early
hang* (before any console output) on real hardware.

**Why this is invisible under QEMU and could not have been caught by
`zephyr-qemu-ci.sh`:** QEMU's `virt` machine has nothing mapped at either
the real A64 GICC address or wherever `VGIC_GICV_BASE` would physically be
— every access, redirected or not, is silently dropped (reads return 0), so
`main_zephyr_qemu.c` cannot distinguish "redirect is fine" from "redirect is
broken." This is the exact gap the existing "GIC is a black hole under
QEMU" section already flagged in the abstract; this pass turns it into a
specific, dated, line-numbered divergence rather than a hypothetical one.

**Not attempted, and explicitly out of scope this pass:** editing
`main_zephyr.c` to add `vgic_init()` + the `HCR_EL2.IMO|FMO` write, mirroring
`main_dbg.c`. That change cannot be validated under QEMU (the GIC black hole
swallows it either way), which is exactly the case this session's
instructions say not to guess at. It's recorded here as the leading
candidate fix, not applied.

### Finding 3 (ruled out this pass): a `bootelf`/load-address/reload-sequence mistake

Checked and found consistent, no smoking gun: `Z_ELF`/`Z_PABASE` in
`main_zephyr.c` match the documented TFTP/staging addresses exactly (same
values `main_zephyr_qemu.c` and this doc's own "Next step" section use);
`git status` shows the tree was clean before and after the 2026-08-06
attempt (no stray uncommitted edits could have changed behaviour between
what was reviewed here and what was flashed); the reversible-swap procedure
described above (SHA256-verified `tftpboot/kernel` swap, restored after) is
consistent with normal `chimpd`-driven reload. Given Finding 1 alone already
guarantees a watchdog-triggered return to U-Boot within seconds of any
`bootelf` that actually jumped, "board already back in U-Boot" is fully
explained without needing a `bootelf`-didn't-jump theory. This doesn't
*prove* `bootelf` jumped — it just means the observed symptom carries no
information either way, so this candidate is downgraded from "worth
checking" to "moot until Finding 1 is fixed and re-tested."

### What actually changed vs. the original two candidates

The original write-up (2026-08-06, above) flagged (a) "real A64 peripheral
inits under `main_zephyr.c` `main_zephyr_qemu.c` never touches" and (b) "GIC
is a black hole under QEMU, so a real divergence would be invisible" as
*unconfirmed* candidates. This pass confirms both, concretely:
(a) turned out to be the watchdog-arm MMIO writes specifically (not a vague
"some peripheral"), and its actual failure mode is a guaranteed timed
self-reset, not a hang; (b) turned out to be a real, dated, line-numbered
code divergence (the GICC→GICV redirect landed two days after
`main_zephyr.c` was written and was never propagated to it), not just a
theoretical blind spot.

### Recommended next live check (cheap, uses tooling that already works, no new code)

Do **not** re-run the EMAC-polling test as before — Finding 1 explains why
it cannot succeed regardless of Finding 2's outcome. Instead:

1. Flash `microkernel-zephyr.elf` exactly as before (`loady` + `bootelf`).
2. Let the ~6s self-armed watchdog fire on its own — this is now expected,
   not a symptom of failure. `chimpd`'s persistent supervisor already
   catches the board back in U-Boot on the next watchdog cycle (this is its
   normal job, per `SESSION-RULES.md` R2/R3).
3. The instant U-Boot regains control, **before** any new TFTP/load
   overwrites DRAM, dump the breadcrumb/capture regions directly over
   U-Boot's serial console (this needs no EMAC, no `dbgmon`, no code
   changes — a warm watchdog reset preserves DRAM, same property already
   relied on elsewhere in this project):
   - `md.l 0x50008000 8` — the `ZEP_BC` stage counter. Word[1] reaching `7`
     means `main_zephyr.c` completed setup and called `kload_enter()`;
     stalling below that pinpoints exactly which init step (`kload_parse_elf`
     / `kload_place_segments` / `stage2_init` / …) never returned. Word[6]
     should read `0x5100100c` once word[1] reaches 5.
   - `md.l 0x50000f00 4` then `md 0x50000f10 <n>` — `vconsole`'s capture
     ring (`VCONSOLE_RING_BASE`, see `vconsole.h`): word[1] (`total_bytes`)
     nonzero means Zephyr's `printk` reached the trapped UART0 at least
     once (i.e. got *past* whatever Finding 2 might be gating on); the byte
     dump is Zephyr's own console text, unread until now. Word[2]
     (`fault_count`) nonzero with `total_bytes == 0` would be the single
     strongest confirmation of Finding 2 (a stage-2 fault before any
     console output).
   - `md.l 0x50000400` region — `gtrace`'s EL2 exception record (ESR/ELR/FAR)
     if the guest ever actually faulted rather than silently hanging in a
     poll loop.

This distinguishes the two findings cheaply: stage counter stuck early with
no console bytes and no fault record points at Finding 2 (a silent hang in
GIC init, before `vconsole_init()`'s trap path is even exercised — check
whether the hang is before or after `ZEP_BC(1,2)`, i.e. before or after
`vconsole_init()`/`gtrace_init()`); stage counter reaching 7 with console
bytes present means the guest ran fine and Finding 1's watchdog is the
entire story.

## Finding 1 fixed (2026-08-09, board-free)

`main_zephyr.c` now calls `emac_init()`, `dbgtools_init()`, `usbacm_init()`
(brings up `musb_init()` internally), `gtrace_init()`, `dbgmon_init()`, and
`bmc_init()` — in that order, mirroring `main_dbg.c`/`main_gdb.c` — and
`smp_init()` right before `kload_enter()`. There is now a live EMAC/dbgmon
service and a running CPU1 for the host's existing tooling (`hvdbg.py`,
`chimpd.py`, `bzdctl.py`) to actually talk to.

The former raw `WDOG_CFG`/`WDOG_MODE`/`WDOG_CTRL` MMIO pokes (the "~6s
bring-up watchdog" comment, now removed) are replaced with a real call to
`wdt_arm()` — moved EARLY (before `kload_parse_elf()`, same placement
`main_dbg.c`/`main_gdb.c` use and justify: "before any of the risky setup").
`wdt_arm()` sets `wdt_window_ticks`/`wdt_last_progress` (`wdt.c`'s static
state), which is exactly what the old raw pokes never did and why
`wdt_pet()` — called unconditionally from every `el2_trap()` regardless of
build — could never re-arm the hardware timer before. This build now gets
the same real behavior `main_dbg.c` has: the ~16s hardware timer keeps
getting re-armed as long as the guest keeps making "progress" (defined by
`wdt_note_progress()` — a console byte), not a fixed unconditional ~6s
self-destruct.

`ZEPHYR_OBJS` (Makefile) grew by six objects to satisfy the new calls'
real dependency chain, each one discovered by an actual link failure, not
guessed in advance: `dbgmon.o` itself, plus its own hard dependencies
`bmc.o` (`exec_line()`'s `bmc_dispatch()`) and `dbgtools.o`
(`dbgtools_hold_set()`/`_release_set()`), plus `bmc.o`'s own dependency on
`rsb.o`/`axp803.o` (`bmc_init()`'s AXP803 PMIC health read). `main_zephyr.c`
also now defines the four `console_getc`/`console_putc`/`console_poll`/
`console_flush` hooks `dbgmon.c` calls as `extern` — the same hooks
`main_dbg.c` defines, previously absent here because nothing needed them.
The stand-in `dbgmon_call_active` definition this file carried (for builds
that skip `dbgmon.o` entirely, e.g. `main_gdb.c`) was removed — now that
`dbgmon.o` is genuinely linked, its own definition applies and a second one
would be a multiple-definition link error.

**Verified board-free**: `make zephyr` builds and links clean (no new
warnings beyond the pre-existing RWX-segment linker note every target
gets). `./zephyr-qemu-ci.sh` and the rest of `./ci.sh` are unaffected and
stay green — they exercise `main_zephyr_qemu.c`, a separate file, untouched
by this fix. **Not verified, and cannot be without the board**: whether
`emac_init()`/`musb_init()` actually bring up real EMAC/USB hardware
correctly in this specific file's call context (they are proven correct in
`main_dbg.c`'s context, but this is this exact sequence's first real
compile+link, let alone first hardware boot), and — separately — Finding 2
(the GICC→GICV redirect / IMO mismatch) is **NOT fixed by this pass** and
remains a real, live risk: if Zephyr's GIC init touches CPU-interface
registers, this fix alone does not address that. Recommended next real
hardware attempt: flash this rebuilt `microkernel-zephyr.elf` and check for
actual EMAC responses / `bzdctl.py status` liveness — if EMAC still doesn't
answer, that's new information (points squarely at Finding 2 or something
else entirely); if it does answer, use it to read the exact stage where any
remaining hang occurs, same breadcrumb addresses as the "next live check"
above.

## Confirmed on real hardware (2026-08-09)

The Finding-1 fix was flashed to the real board the same day it landed,
using the same reversible `tftpboot/kernel` swap (SHA256-verified before
and after, restored afterward) and `chimpd.HYP_ELF` monkeypatch as the
2026-08-06 attempt.

**Result: EMAC answered immediately** ("EMAC живой" within 7s of `loady`+
`bootelf`'s "go sent") — the exact channel that never produced a single
response on 2026-08-06. `chimpd`'s monitor loop then read real, growing
`vconsole` byte counts every poll for over 100s (137→985→1210 bytes across
this session's checks) and `hvdbg.vconsole_text()` read back the guest's
actual console output verbatim:

```
*** Booting Zephyr OS build v4.4.1 ***
bzdOS/Zephyr: hello from EL1 (board: bpi_m64_hv)
heartbeat 0
heartbeat 1
...
heartbeat 59
```

Sustained past heartbeat 59 with `bzdctl.py status` showing `exceptions
none recorded`, `guest_pc` moving and staying inside the Zephyr image's own
load range (`0x51000000`+), and `console advancing` on every check across
more than two minutes of continuous operation before the board was
deliberately reset back to FreeBSD. `chimpd`'s monitor loop did emit one
`timer=FROZEN` heuristic warning and one spurious "guest exception" line
during this run — both are the hang-detector reading a tick/EXC breadcrumb
this build's tickless Zephyr config never touches (see the file header's
own "KNOWN GAP" note: `CONFIG_SYS_CLOCK_EXISTS=n` / `CONFIG_ARM_ARCH_TIMER=n`),
not evidence of a real problem; a fresh `bzdctl.py status` moments later
showed `exceptions none recorded` and the console still climbing.

**This resolves Finding 2 in practice, if not in theory**: the un-paired
GICC→GICV redirect / missing `vgic_init()`+IMO=1 (still genuinely absent
from `main_zephyr.c`, still a real latent gap on paper) evidently never
gets exercised by this exact Zephyr build, because it never touches GIC
CPU-interface registers at all in a tickless, no-arch-timer configuration.
The moment this guest's Kconfig grows a real timer tick or any interrupt
source, that gap stops being theoretical — revisit before then, not after.

Restored to the normal FreeBSD/`dbg` configuration immediately afterward
(`tftpboot/kernel` restored from a SHA256-verified backup, reloaded via
plain `chimpd.py`, `fsck_ffs -y` + `netif restart` + `sshd start` — the
same read-only-root-after-hard-reset recovery this project has needed every
time a running guest gets a hard `power reset` rather than a cooperative
shutdown). Confirmed recovered: `ssh` answers, root mounted `rw`, `fsck`
found nothing to repair.
