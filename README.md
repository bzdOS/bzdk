# bzdOS / Chimp — a from-scratch EL2 hypervisor for the Banana Pi M64

A bare-metal, hand-written type-1 hypervisor (no KVM/Xen/Jailhouse ancestry, no
vendor SDK) that runs **FreeBSD 15.1 arm64 as an EL1 guest** on a **Banana Pi
M64** — Allwinner A64, 4× Cortex-A53, 1 GiB of guest DRAM
(`STAGE2_DRAM_SIZE = 0x40000000`, `stage2.h:88`).

Everything below the guest is in this tree: the stage-2 MMU tables
(`stage2.c`), the GICv2 virtualisation (`vgic.c`, `gic_timer.c`), the eMMC PIO
driver (`emmc_bio.c`), the Ethernet MAC (`emac.c`), the USB-OTG gadget
(`musb.c`, `usbacm.c`), virtio-mmio block and net devices (`vblk_emmc.c`,
`vnet_emac.c`), a 16550 emulation (`vconsole.c`), and the DE2/HDMI display
pipeline (`hdmi.c`, `scanout.c`, `hud.c`). The board has **no physical UART**:
bring-up and every debugging session run over a raw-Ethernet debug protocol
(ethertype `0x88B5`) plus a USB CDC-ACM gadget, both written here.

## Why this exists

ROADMAP §0 states the project's identity, and it is not "another hypervisor":

> A hypervisor where **"the board cannot be lost"** and **"you can point a
> debugger at the crashed OS"** are feature #1, not a side effect.

Xen, Jailhouse and seL4 are more mature and better isolated. None of them lets
you debug a wedged guest kernel on a $30 board from the sofa with no serial
cable. Three properties follow from that, and they are what the substrate
actually buys:

1. **Trap-and-emulate on real silicon, with no firmware assistance.** The guest
   is unpatched: FreeBSD boots as shipped, and every bug found on the way was
   fixed on the *emulation* side (the ns8250 IIR latch, `MSR=0xB0`, the
   `DATA_OVER` race, the CNTV mask). That is a statement about the quality of
   the emulation rather than a workaround. Nothing here relies on EL3 services
   beyond PSCI passthrough.
2. **Isolation you can point at.** `stage2_isolation_selfcheck()` (`stage2.c`)
   asks the CPU's own table walker, via `AT S12E1W`, whether the guest's EL1
   regime can reach the hypervisor image and scratch windows, and latches
   `PAR_EL1.F` for each into breadcrumb `STG2[14..18]`. A passing boundary is
   `HVIMG.F == 1 && HVSCR.F == 1 && control.F == 0` — hardware answering, not
   software re-reading its own descriptors. It runs on every boot.
3. **A debug channel that outlives the guest.** CPU1 owns EMAC, the USB gadget,
   `dbgmon`, the GDB stub and the hardware watchdog, and it keeps answering when
   CPU0's guest is wedged or dead. `bzdctl.py console --postmortem` even
   recovers the *previous* run's console log, because reloading the hypervisor
   is the only way to get the channel back after a crash and is also what
   destroys the evidence (`vconsole_postmortem_carry()`, `vconsole.c:373`).

Core assignment (see ORIENTATION.md "Core layout"):

| Core | Role |
|---|---|
| CPU0 | runs the guest, takes its traps |
| CPU1 | EMAC/debug core; `dbgmon`, GDB stub, software BMC; owns the HW watchdog |
| CPU2 | async eMMC I/O offload (`vblk_async.c`) |
| CPU3 | WFI in `dbg`/`gdb`/`fbsd`; a genuine second guest (Zephyr) in `make dual` |

Interrupt policy, as the tree stands: **EL2 owns every physical IRQ/FIQ**
(`HCR_EL2.IMO=1, FMO=1`, set in `main_dbg.c:392`) and forwards each one into a
guest List Register in HW mode, so the guest's own virtual EOI deactivates the
physical source. This *supersedes* the earlier `IMO=0` policy rather than
reverting to it — `main_dbg.c:326-375` records why, including the 145 kHz
EHCI storm (INTID 106) that the incomplete first attempt produced and the
ordering constraint (GICC→GICV redirect and `vgic_init()` before the HCR write)
that the two prior reverts violated. Memory partitioning remains **static, à la
Jailhouse**: no dynamic guest creation, one fixed stage-2 layout per guest.

## State of things, with numbers

The v1 gate is ROADMAP §2 — numbers, not impressions. Current standing:

| Criterion | State |
|---|---|
| Stage-2 isolation cut HV out of the guest map | **CLOSED** — `AT S12E1W` self-check, hardware-proven |
| W^X on guest DRAM | **CLOSED 2026-08-20** — dynamic W^X is now the default |
| 100 clean boots in a row | **CLOSED 2026-08-04** — streak 100 (`boot_ledger.py`) |
| 20 survived break-glass resets in a row | **CLOSED 2026-08-11** — 20/20, 0 failures |
| Clean shutdown (`shutdown -p` → PSCI SYSTEM_OFF → fs always clean) | **CLOSED 2026-08-05** — `a2_cycle.py --cycles 10`, 10/10 |
| Hosted unit tests on virtqueue + stage-2 builder in CI | **CLOSED** — 17 C suites + 8 Python suites in `make test` |
| Second target builds and boots (QEMU-virt minimum) | **CLOSED** — `qemu-ci.sh`, plus vGIC, Zephyr and Linux QEMU targets |
| License + pinned toolchain | **CLOSED** — BSD-2 `LICENSE`, `make toolchain-check` |
| **72 h soak under continuous disk load** | **NOT CLOSED.** Harness is done and resumable (`soak72.py`); the hardware run so far is 0.06 h of 0.05 requested, 7/7 healthy samples — minutes, not 72 hours. The degradation (early-rate) check has never had time to fire. See `docs/soak-and-breakglass.md`. |
| Bring-up guide reproducible by a third party | **Cannot self-certify.** Known gaps listed in `BRING-UP.md` §"What's missing": FEL flashing of SPL/U-Boot is not automated, the br0/TFTP/dnsmasq bench is described but not scripted, the host USB path is hardcoded. |

**Dynamic W^X, measured under the real workload** (ROADMAP §2, closed
2026-08-20). `STAGE2_WX_DYNAMIC` now defaults to 1 (`stage2.h:291`): guest DRAM
is W+XN by default, the first instruction fetch from a 4 KiB page takes a
stage-2 permission fault, the page is split to L3 on demand and turned RO+X, and
a subsequent write fault turns it back. The question that kept it off — whether
a bounded pool of 64 L3 tables survives the *real* physical fragmentation of
this guest's `kldload` — was answered by reading `HVMAP_WXDYN_BC` (0x50093000)
after a full boot to multiuser with `drm.ko` + `lima.ko` + `bzfb.ko` loaded and
`limabench` running:

```
pool_used      = 27 of 64      (2.4x headroom)
pool_exhausted = 0
flip_count     = 4045          (the mechanism works under load, it does not idle)
```

Pool exhaustion still fails **open** — one 2 MiB block degrades to W+X, the
guest is not killed — so a heavier workload loses enforcement on a block rather
than breaking. Image size 954912 B against `link.ld`'s 2 MiB ceiling.

**Graphics** (the driver-side work lives in the sibling repo
`/opt/bzdos/bsdOS`, not here — this repo owns the EL2 side: `hdmi.c`,
`scanout.c`, `hud.c`, `fbdump.c`):

- **Mali-400 MP2 renders.** `hal/lima/tests/limabench.c`: a sampled texture,
  2420 draw calls per run, depth testing, alpha blending, 512×512 with a depth
  buffer, ~125 ms per run (~80 fps), 4/4 runs, zero GPU MMU faults, no
  `LIMA_DEBUG` flag. Measured 2026-08-19.
- **Zero-copy presentation, 1030.7 fps.** A client renders into a
  `gbm_surface`, `eglSwapBuffers`, and hands the locked front buffer's dma-buf
  fd to `BZFB_IOC_PRESENT_DMABUF`; EL2 validates the extent and repoints the DE2
  guest-window layer at it. 12369 frames in 12.0 s at 1120×276, 0 refused,
  153471 accepted addresses and 0 rejected over the session
  (`bsdOS/hal/bzfb/tests/README-zerocopy.md`). The HUD's own framebuffer was
  sampled from EL2 at the same time and was untouched.
- **A real DRM/KMS device in the guest, since 2026-08-20.** `bsdOS/hal/bzkms`
  puts the same doorbell behind the interface X11's `modesetting` and every
  Wayland compositor insist on: `drmModeAddFB2` / PRIME import →
  `drmModePageFlip` → doorbell → `DRM_EVENT_FLIP_COMPLETE`. Hardware:
  `/dev/dri/card1`, connector 35, crtc 33, mode 1120x276@59, **1194 frames in
  20.0 s = 59.7 fps**, presented 1194, refused 0; EL2 saw `guestwin_count` rise
  187 in 3 s. The drop from 1047 to 59.7 fps **is the point**: every frame now
  waits for flip completion instead of firing blind.
- **The guest gets a window inside the HUD, zero copy.** Two DE2 layers in one
  UI channel, blended by the mixer: layer 0 is the HV's 1280×720 HUD at
  0x4D000000, layer 1 is the guest's 1134×276 console at 0x4B000000, which is
  ordinary guest DRAM declared `simple-framebuffer` and reserved `no-map` in its
  DTB. Isolation is untouched — the guest writes only its own DRAM. Measured:
  the HUD's non-zero count stayed at 1200 through everything the guest did while
  the window's moved 113 → 8 → 80 (`docs/guest-display.md`).
- **1080p locks**, hardware-verified 2026-08-20 with
  `-DHDMI_MODE_1080P`: `PHY_STATUS = 0x000ac0f4` (bit 7 LOCK = 1),
  `DE_GLB_SIZE = 0x0437077f`, guest reached a login prompt on the same image,
  6089 frames presented with 0 refusals. This withdrew the document's own
  "probably an analog limit at 1.485 Gbps" expectation
  (`docs/hdmi-1080p-phy.md`).

Also proven on hardware, outside the gate: **two guests concurrently** — real
Zephyr RTOS v4.4.1 on CPU3 printing heartbeats while FreeBSD on CPU0 serves
`ssh` and a 384 MB `dd` off `vtbd0p3` under load 2.00 with zero `dmesg` errors,
each under its own disjoint stage-2 table (`stage2_zephyr.c`), `ISOL 1/1` per
boot (`docs/dual-guest.md`).

**Honest caveat on the ledger.** `boot-ledger.txt` is append-only and its
counters are live, so they do not match the gate snapshot above: 193 of 233
entries predate the supervisor fix of 2026-08-05 (`df611e3`), when `chimpd`
condemned healthy boots after 40 s of console silence. Those entries *understate*
reliability. The file says so itself, in the report it prints.

## Usage

### First command of every session

```sh
cd /opt/bzdos/microkernel && python3 orient.py
```

Read-only; it samples counters and asks the guest harmless questions, never
writes. Four sections, in this order: `HOST-SIDE PRECONDITIONS`, `HYPERVISOR`,
`GUEST (FreeBSD)`, `WHERE TO LOOK NEXT`. It exists because the expensive
failures in this project were never hard bugs — they were sessions spent
debugging a stale image, a tty already held by another process, a guest shell
parked in an unterminated quote, or an idle guest read as wedged
(`orient.py:4-23`). Do not skip it.

### One tool for the board lifecycle

```sh
python3 bzdctl.py status                # one screen of state; EMAC only, safe anytime
python3 bzdctl.py health [--raw]        # the BMC health record
python3 bzdctl.py power reset|hold|release|arm|disarm
python3 bzdctl.py power uboot           # catch the board in U-Boot  (needs the tty)
python3 bzdctl.py console [-n N] [--follow]
python3 bzdctl.py console --inject TEXT
python3 bzdctl.py console --postmortem [--all|--at OFF]   # the PREVIOUS run's log
python3 bzdctl.py boot-watch [--timeout S]
python3 bzdctl.py crash [--out DIR]     # bundle status + triage + crash report
python3 bzdctl.py ledger                # both numeric v1 gates from one file
python3 bzdctl.py serve [--port 8088]   # read-only web dashboard, EMAC only
```

`bzdctl` is a façade (ROADMAP B1 DoD): every subcommand delegates to
`bmc_client.py`, `board_ctl.py`, `chimpd.py`, `boot_ledger.py`,
`coredump-recv.py` or `triage.py`. The split that matters is transport:
`status`, `health`, `serve`, `ledger` and the postmortem console ride EMAC and
**never open the tty**, so a dashboard left in a browser tab cannot lock out a
reload. `power uboot` is the subcommand that does need it.

`--postmortem` prints `no carry-over held` when the previous run captured
nothing or the layout version changed — the firmware refuses to guess across a
layout change rather than presenting the wrong region as console text
(`vconsole.c:390`).

### Diagnosis: `triage.py` before `gr`

```sh
python3 triage.py            # full report
python3 triage.py --gap 4    # longer settle for a slow or degraded link
```

Read-only: no `call`, no `w`, nothing that changes board state; safe against a
wedged board and safe to repeat. It exists because one 2026-07-29 freeze took
~15 ad-hoc queries and the two decisive facts were both available in the first
30 seconds and both went unnoticed for an hour — the board was running a
*different build* than the source tree (`79ae093f109c+` vs `08688afa045f+`), and
`GICH_HCR = 0`, i.e. the vGIC's virtual CPU interface was off. Its most
load-bearing trick is **sampling twice**: everything time-varying is read twice
with a gap and reported as `MOVING`/`frozen`, because a frozen counter and a
spinning one are identical in one snapshot. `gr`, by contrast, returns a *stale
saved frame*, not the live PC.

### Talking to the guest

```sh
python3 guest_sh.py 'uname -a'          # over the console; works with no network
python3 guest_sh.py -f script.sh        # feed a file line by line
ssh -i /root/.ssh/chimp_ed25519 root@192.168.88.82     # once /etc/rc has run
```

`guest_sh.py` frames output between two unique markers echoed by the guest's own
shell and discards everything outside them, because the console drops and
interleaves bytes; a partially corrupted line is then visible instead of
silently swallowed. It opens the stable udev symlink (`bzd_board.CHIMP_TTY`), not
`/dev/ttyACM0` — the CDC-ACM node renumbers on USB re-enumeration, and the
resulting bare `OSError: [Errno 5]` reads exactly like dead hardware.

The guest is also a build host: it has `/usr/bin/cc` and free space, so a
misbehaving base tool is usually faster to rebuild there than to debug. To copy
files *in*, the **guest listens and the host connects** (`nc -l` on the guest) —
inbound TCP to this host is firewalled. Never change the guest MAC: it must stay
`02:bd:05:00:00:01` (the guest relies on `VIRTIO_NET_F_MAC`).

### Loading a build, and the resident supervisor

```sh
make microkernel-dbg.elf
python3 -u chimpd.py                    # persistent; NO --once
```

`chimpd` is the supervisor: it waits for `/dev/ttyCHIMP`, catches the U-Boot
prompt, runs a TFTP preflight (starting `dnsmasq` if :69 is dead), TFTPs the
FreeBSD kernel (16 MB → 0x44000000) and DTB (→ 0x4a000000), `loady`s the
hypervisor ELF to 0x48000000 and `bootelf -p`s it, then monitors: GICT ticks,
the vconsole ring's `total_bytes`, `ff`/EXC for a guest panic, `gr` for the guest
PC. Its verdicts are deliberately conservative — **console silence alone is not
a hang**, because a booting FreeBSD is quiet for well over 40 s while hammering
the eMMC, so vblk's request counters count as progress too, and a guest that
reached `mountroot`/`login` and went quiet is left alive. That heuristic used to
be wrong and reset healthy boots (fixed 2026-08-05, `df611e3`); every ledger
number older than that fix is suspect for exactly this reason.

Run **one** supervisor. Several instances fight over `ttyACM0` and corrupt the
load. `reliable_load.py` blocks forever against a live `chimpd` —
`acquire_port_lock()` is `fcntl.flock(LOCK_EX)` with no timeout, so the symptom
is total silence right after `[serial] TFTP preflight` (SESSION-RULES R3; it cost
50 minutes on 2026-08-18). If a running `chimpd` is present it will pick up a
rebuilt ELF on its next watchdog turn; a second loader is not needed.

```sh
python3 reliable_load.py --expect-vbk --cycles 1
```

A bounded state machine, not a retry loop: `ensure_uboot()` (keyed on USB
VID:PID, because `/dev/ttyCHIMP` maps *both* U-Boot's download gadget
`1f3a:efe8` and the running HV's own console `1d6b:0010`) → `serial_load()` →
`verify()` (EMAC answers, and with `--expect-vbk` the VBK1 breadcrumb at
0x50005000 is present, proving the virtio-blk build actually ran). Note that
`--cycles` defaults to 5, i.e. five full reload-and-verify cycles; pass
`--cycles 1` for one.

### The gate harnesses

```sh
python3 soak72.py --hours 72            # v1 gate: 72 h of cumulative HEALTHY load time
python3 breakglass_cycle.py --attempts 20
python3 boot_ledger.py [--tail 20]      # both numeric gates, from one append-only file
```

Both harnesses are resumable and idempotent: state in `soak72-state.json` /
`breakglass-state.json` (atomically replaced), every event appended and
`fsync`'d to a `.jsonl`. Re-run the identical command to continue; `--restart` is
the only way to zero a streak.

`soak72.py` measures **cumulative healthy load time**, not wall clock — time it
could not verify does not count toward 72 h. It inverts `soak.py`, whose unit of
work is "reload and dwell" and which never asks the guest to do anything and so
cannot say whether a *running* system degrades. A watchdog reset is recorded and
recovered from rather than failing the run (SESSION-RULES R2), and it is
*counted*, because a 72 h soak that needed nine resets is a different result
from one that needed none.

`breakglass_cycle.py` exercises the recovery of last resort: send
`\x00~BZRST\x00` to the ACM tty → the hardware watchdog resets the board to
U-Boot in ~16 s → reload → the standard uncooperative-stop repair (`fsck_ffs -y
/dev/vtbd0p3`, `mount -u -o reload /`, `mount -u -o rw /`, `service netif
restart`, default route, `service sshd start`) → verify `ssh` answers, root is
`rw`, `fsck` clean. It stops loudly on a failed attempt by default: 20 **in a
row**, not 20 out of 30.

`boot_ledger.py` is the accrual mechanism. Nothing runs a dedicated soak to rack
up the boot number: every reload done for any reason records its outcome, plus
`isol_pass` (the A1 `STG2[18]` self-check) when the caller has it, so the ledger
simultaneously proves isolation held on every counted boot. Break-glass attempts
land in the same file under `source=breakglass` and are excluded from the
clean-boot streak by design — one ledger, two gates.

## How we debug

This is the project's real asset. `SESSION-RULES.md` (R0–R7) and
`DEBUG_RULES.md` (R1–R7) are mandatory reading; both are in Russian, and both
were distilled from specific incidents rather than from principles. The rules
that bite hardest:

- **R0: never make the user press reset or re-plug power.** The whole
  infrastructure exists so the agent fixes things live over EMAC or performs a
  network reload itself. Live fixes come first: one sysreg via
  `hv.cmd('sw hcr 0x84000003')`, one instruction via `hv.patch(pa, insn)`, a
  function call via `hv.call(pa, ...)`, memory via `w`/`wb`.
- **The hardware watchdog resetting the board is BY DESIGN** (R2). It is the
  automatic recovery path back to U-Boot, not a fault; harnesses count and
  recover from it. Guest writes below `VBLK_BOOT_GUARD_LBA` (18432 sectors =
  9 MB) are rejected by the hypervisor so the guest cannot destroy the SPL that
  guarantee rests on — measured: 0 rejections over 227 guest writes, so the
  guard neither fires falsely nor is needed in practice.
- **One process on the tty at a time.** Two readers steal each other's bytes and
  a healthy channel looks completely dead. `fuser -v /dev/ttyACM0` before
  blaming the board.
- **Per-PE registers are banked.** `HCR_EL2`, `MDSCR_EL1`, `DBGB*`/`DBGW*`, GIC
  PPI state — a read over the debug channel is serviced by **CPU1** and says
  nothing about the guest's core. This has produced at least four confidently
  wrong diagnoses here, including a "dead guest timer" that was CPU1's own
  `HCR`, and a watchpoint armed on CPU0 alone that left CPU1 — the one core that
  owns the console path — guaranteed unwatched.
- **A guest-written DRAM word read from another core is not a liveness probe.**
  A guest with its stage-1 MMU off makes its own stores Device-typed, and a
  Device write does not invalidate another core's cached line, so the reader can
  return a stale value forever. QEMU models no caches and will happily let the
  same probe "work" while lying about hardware; it did exactly that on
  2026-08-10 (`docs/dual-guest.md`, RETRACTED section). Trust progress observed
  *through a stage-2 fault* — console bytes, or an EL2 breadcrumb written with an
  explicit `dc civac`.
- **A snapshot is not liveness** (DEBUG_RULES R1). "Alive in a poll loop" and
  "dead" give bit-identical snapshots. Until you have a measure of progress over
  time, the only defensible sentence is "the last captured frame was X".
- **The symptom may be an artifact of your own instrument** (R3). Change exactly
  one mechanism per experiment; `-DDBG_NO_TVM` is the model A/B.
- **One variable per board cycle** (R4), and write down beforehand what you
  expect to see on each branch. If the snapshot cannot distinguish the branches,
  the experiment is worthless — do not burn the cycle.

### Breadcrumb windows, and their hygiene rules

Nearly all EL2 observability lands in `hv-scratch`, a DTB-reserved DRAM window
at `0x50000000` that is cross-core coherent and survives a warm reset.
`hv_addrmap.h` is the single authoritative map, and it exists because the
addresses used to be literals scattered across a dozen `.c` files: on
2026-07-24 `VBLK_USED_LOCK_PA` was placed at `0x50020200`, which already belonged
to `emmc_bio.c`'s failure-diagnostics window whose word[0] is the read-failure
counter — so the first eMMC read error stamped a nonzero value into the lock
word, wedging the used-ring lock permanently "held" and reopening the exact
CPU0-vs-CPU2 race it had been added to close, precisely when I/O errors start.
A comment can be wrong and nobody notices; a `_Static_assert` cannot.

The hygiene rules, earned in `docs/war-stories.md` §12 (four wrong answers about
who wrote 64 KiB of console log):

- **A window's address must never be computed from a neighbour's.** vconsole's
  capture buffer was `RING_BASE + HDR_SIZE` and then grew 3 KiB → 64 KiB, so it
  marched through six other subsystems' windows. It is absolute now, the header
  is self-describing (version, base, size) so host and firmware cannot drift, and
  restoring the derived address fails the build.
- **Publish every counter as a real zero at init**, or "never happened" and
  "this build has no such counter" (`0xffffffff`) are indistinguishable.
- **Sample twice before concluding anything from a counter.** A last-value slot
  (`last_fault_ipa`) needs a delta, or a label saying it is not a live one.
- **Read the right subsystem's counter.** `g_dabt_seen` sits in vblk's window but
  increments *before* the window-address check, so it counts every guest data
  abort including other devices'. It sent one investigation after virtio-blk when
  the guest was hammering virtio-net.
- **Zero, verify the zeros, then perturb exactly one thing.** `dbgmon`'s `w`/`wb`
  were a bare store with no `dc civac`/`dsb` while every breadcrumb writer in the
  tree has always done both — so host writes landed in CPU1's cache, the
  read-back hit that same cache and *confirmed* the write, and a warm reset
  discarded the line and restored the original bytes. Read-back verification
  cannot catch this; `hvdbg.write_word_verified()` could not either. The tell was
  that the "rewritten" text came back byte-for-byte identical. Fixed; but every
  host-side write before that fix could silently have failed to reach DRAM.

### The instruments

| Instrument | What it answers |
|---|---|
| `dbgmon.c` + `hvdbg.py` | the EMAC debug monitor: `gr sr sr2 r rb d gva w wb t ff ffv ss pt bp wp bt sw call patch`, serviced by CPU1. `hv.call()` invokes a hypervisor function by address over the network, with `.text` validation and crash recovery (returns `0xDEAD` if the callee faults). |
| `flightrec.c` | the first *generalised* instrument here: a `(kind, a0, a1)` ring at `0x50012000`, magic `FLTR`, for post-mortem timelines. Every breadcrumb before it (EXC1, BTR1, SST1, FF1V, VBK1…) was a bespoke per-subsystem ring. |
| `firstfault.c` | catches the guest's *original* first EL1 fault: with the guest's vector page unmapped at stage-2, its first vector fetch aborts to EL2 and the still-pristine `ELR_EL1`/`ESR_EL1`/`FAR_EL1`/`SPSR_EL1`/`SP_EL1` and GPRs are latched into `FF1V` at `0x50005800` before the guest's own handler can overwrite them. |
| `hwbp.c` | EL2-controlled hardware breakpoints and watchpoints on the guest (`DBGBVR/DBGBCR`, `DBGWVR/DBGWCR`, `MDCR_EL2.TDE`). Single-step is exhaustive but glacial; these catch "PC reaches X" or "this byte is written" for free. `hwbp_set_wp_el2()` (`HMC=1, PAC=0b10`) answers the question no other watchpoint in the tree could: *which of **our** instructions wrote this address*. Arm it per core — the registers are banked. |
| `gdbstub.c` / `gdbstub_hw.c` | a GDB RSP stub on CPU1 over the same `0x88B5` channel, bridged to `target remote :1234` by `gdb-bridge.py`. Live-verified: `$?` → `T05`, `$g` → registers. Breakpoint paths are the weak part — prefer software `Z0` over `Z1`, and check the notes before trusting either. |
| `bmc.c` / `bmc_client.py` | the software BMC: a versioned, self-describing verb catalog (power/reset, console, debug flags, health, memory, breakpoints, fault) over the same CPU1-serviced transport, replacing a pile of "you must already know the address, word offset and magic". `bmc health` also latches its record into a breadcrumb so a host can read it with a raw `r <addr> <n>` mid-wedge. |
| `coredump.c` / `crash_report.py` | guest crash forensics. Note the honest gap in ROADMAP §2: the fixed `p_vaddr` bug is regression-tested (`test_coredump_elf`, 75/75) and `crash_report.py selftest` runs `readelf` and a scripted `gdb` against a real `kernel.debug`, but **no path here has yet seen a genuine live FreeBSD panic**. |
| `bzdctl.py console --postmortem` | one generation of console log carried across the reload that destroyed it. |

### docs/ index

Deep dives, all in this tree:

| Doc | Subject |
|---|---|
| `war-stories.md` | bring-up on real, unfamiliar silicon — twelve incidents, each with the wrong answers that preceded the right one |
| `soak-and-breakglass.md` | the two long unattended runs: 72 h soak and 20 break-glass resets |
| `wx-enforcement.md` | W^X on guest DRAM: can it be enforced, and is it |
| `autoboot-no-cable.md` | the board boots the whole stack with no host interaction |
| `brick-recovery.md` | the 2026-08-12 "brick" that was not one, and what it taught |
| `dual-guest.md` | FreeBSD (CPU0) + Zephyr (CPU3), genuinely concurrent |
| `phase2-all-cores.md` | making all four cores available to guests (not started) |
| `zephyr-guest.md` / `linux-guest.md` | the other two guests, and what their gates do not cover |
| `virtio-blk-design.md` / `-integration.md` / `-dtb.md` | the eMMC-backed block device |
| `virtio-net-dtb.md` | the guest's virtio-net node |
| `emmc-silent-write-corruption.md` | silent eMMC write corruption: a clock that goes backwards |
| `aw-mmc-dma-coherency.md` / `-instrument.md` | the guest-side IDMAC corruption investigation |
| `dma-bypass-stage2.md` | DMA bypass of stage-2 (finding H2(b)): fixable vs inherent |
| `el2-nc-guest-dram.md` | the non-cacheable EL2 remap of guest DRAM |
| `zero-copy-scanout.md` / `guest-display.md` | the display path, hypervisor side and guest window |
| `hdmi-1080p-phy.md` | why 1080p was thought not to lock, and the run that withdrew it |
| `gdbstub-design.md` / `-integration.md` | the GDB stub |
| `bmc-design.md` / `-integration.md` | the software BMC |
| `snapshot-restore-design.md` / `-integration.md` | guest checkpoint/restore |
| `guest-dtb.md` | what is authoritative about the guest's DTB, and how to edit it (never `dtc`) |
| `guest-rootfs-persistence.md` / `guest-ro-root.md` | guest rootfs layout and the read-only-root variant |
| `security-notes.md` | the debug protocol has no authentication, by default — and what that means |
| `qemu-ci.md` | the board-free QEMU `virt` target |
| `sessions/` | frozen session snapshots, including `SESSION-HANDOFF.md` |

Adjacent, in the sibling repo `/opt/bzdos/bsdOS`: `hal/lima` (the Mali-400
driver, `MALI-STATUS.md`, `tests/README.md`), `hal/bzfb` (the doorbell ioctl and
`tests/README-zerocopy.md`), `hal/bzkms` (the DRM/KMS device).

## Build and load

```sh
make dbg            # the hardware image: microkernel-dbg.elf / .bin
make test           # board-free gate
./ci.sh             # 10 stages: make test, then the QEMU targets
```

`dbg` is the current milestone build and is **`HV_HDMI=1` by default**
(`Makefile:274`); `make dbg HV_HDMI=0` builds display-less. `HV_HDMI` was
briefly made opt-in because it appeared to reset the board every few minutes —
that was wrong: the resets were `wdt_debug_hold` left set in `hv-scratch` by a
previous `reboot_clean`, surviving the very reset it asked for, on *every* image
(fixed in `wdt_arm()`, `wdt.c`). With that fixed the default configuration ran
six hours with the guest at a login prompt and 887364 accepted scanout flips.

Other targets: `dual` (FreeBSD on CPU0 + Zephyr on CPU3, `main_dbg.c` reused
unchanged, minus the snapshot objects that claim the same DRAM window), `gdb`
(`gdbstub` instead of `dbgmon` on the tick path), and `stage0`/`net`/`repl`/
`fbsd`/`hdmi`/`zephyr`, which are earlier, narrower milestones kept for
bisection — all six were verified to link from clean on 2026-08-04. The QEMU
targets are **board-free**: `qemu`, `zephyr-qemu`, `linux-qemu`, `vgic-qemu`,
`snapshot-qemu`, `dual-qemu`, `dual2-qemu`, `dual-zephyr-qemu`, `holdtest`.

`make test` depends on `toolchain-check` (which fails on a *missing* tool and
only warns on an unverified version — see `TOOLCHAIN.md`), then runs 17 hosted C
suites plus `test_automount.py`, `coredump-recv.py selftest`,
`snapshot_net.py selftest`, `bmc_client.py selftest`, `bzdctl.py selftest`,
`crash_report.py selftest`, `soak72.py --dry-run` (36/36) and
`breakglass_cycle.py --dry-run` (13/13). The C suites deliberately *mirror*
rather than `#include` the real `.c` files — those are full of raw ARMv8 inline
asm (cache maintenance, exclusive-monitor spinlocks, banked debug registers)
that host gcc cannot assemble for x86_64. `test_automount.py` is the exception
and the stronger kind of test: it drives the real
`reliable_load.auto_mount_root()` over a pty, so it *can* catch a transcription
error.

### Loading

**The board now autoboots the whole stack with no host interaction** —
hardware-verified 2026-08-20 including a genuine cold power-on with the host not
touching the console at all (`docs/autoboot-no-cable.md`). U-Boot's `bootcmd`
TFTPs kernel + DTB + hypervisor and `bootelf`s it, retrying ten times and arming
the SoC watchdog if it never succeeds. So swapping the hypervisor is:

```sh
install -m 0644 microkernel-dbg.elf /opt/bzdos/tftpboot/microkernel-dbg.elf
# then write 1 to HVMAP_WDT_DEBUG_HOLD (0x50095000) over EMAC:
# CPU1 stops petting, the watchdog fires within 16 s, the board returns on the new image
```

Use `install`, not `cp` — `cp` is aliased to `cp -i` on this host and silently
declined to overwrite the TFTP image for five weeks, which is how a stale
371856-byte ELF came to be blamed on a U-Boot dcache bug that does not exist.
Check the size in U-Boot's transfer log.

Three things in that U-Boot environment are load-bearing and non-obvious: the
`autostart` split (off for the transfers, on immediately before `bootelf`, which
only *jumps* when it is on), `bootdelay=3` and never `-1` (the delay is the only
window in which the prompt can still be caught), and a fallback that arms the
**watchdog** rather than calling `reset` — `reset` at this U-Boot prompt prints
"System reset not supported on this platform" and calls `hang()`, killing the CLI
and the USB gadget with no remote lever left. The Ethernet cable is still
required (the boot fetches over TFTP from 192.168.88.2); only the USB cable is
now optional.

## Layout

Hypervisor core:

| Path | Role |
|---|---|
| `start.S`, `exceptions.S`, `link.ld` | EL2 entry, vectors, the fixed link at 0x42000000 with `ASSERT(__bss_end - 0x42000000 <= 0x200000)` |
| `main_dbg.c` | the hardware build's entry point: EMAC, USB-ACM, SMP, kernel load, stage-2, vGIC, guest entry |
| `el2_exc.c` | the trap handler — every stage-2 abort, sysreg trap, SMC and debug exception |
| `stage2.c`, `stage2.h` | stage-2 tables, the `AT S12E1W` isolation self-check, dynamic W^X |
| `stage2_zephyr.c` | the second guest's disjoint stage-2 table |
| `kload.c` | loads the FreeBSD kernel ELF directly — no bootloader stage for the guest |
| `smp.c`, `sched.c`, `ksync.c` | core bring-up and the per-core roles |
| `vgic.c`, `vgicd.c`, `gic_timer.c` | GICv2 virtualisation, HW-mode injection, the vtimer |
| `wdt.c` | the hardware watchdog, petted by CPU1 |
| `hv_addrmap.h` | the authoritative `hv-scratch` map with its `_Static_assert` chain |

Emulated devices and drivers:

| Path | Role |
|---|---|
| `vblk_emmc.c`, `vblk_async.c`, `emmc_bio.c`, `sd_bio.c` | virtio-blk backed by the real eMMC, with CPU2 offload |
| `vnet_emac.c`, `emac.c` | virtio-net bridged over the board's EMAC, sharing the link with the debug protocol |
| `vconsole.c` | 16550 emulation: capture ring, RX injection, TX tee, postmortem carry-over |
| `musb.c`, `usbacm.c` | the USB-OTG CDC-ACM gadget, including the break-glass sequence |
| `hdmi.c`, `scanout.c`, `hud.c`, `fb.c`, `fbdump.c` | DE2/HDMI, the HUD, the guest window and its flip doorbell |
| `rsb.c`, `axp803.c` | RSB and the PMIC (which is how the HDMI PHY supply gets reclaimed) |
| `el2_ncmap.c` | the non-cacheable EL2 view of guest DRAM (designed, gated off) |

Observability: `dbgmon.c`, `bmc.c`, `flightrec.c`, `firstfault.c`, `hwbp.c`,
`onebp.c`, `gtrace.c`, `backtrace.c`, `ksym.c`, `coredump.c`, `gdbstub.c`,
`gdbstub_hw.c`, `netcon.c`, `snapshot.c`, `snapshot_net.c`.

Host-side tooling: `orient.py`, `bzdctl.py`, `triage.py`, `hvdbg.py`,
`bmc_client.py`, `chimpd.py`, `reliable_load.py`, `supervise.py`, `guest_sh.py`,
`board_ctl.py`, `bzd_board.py`, `loady_over_acm.py`, `boot_ledger.py`,
`soak72.py`, `soaklib.py`, `breakglass_cycle.py`, `a2_cycle.py`,
`crash_report.py`, `coredump-recv.py`, `snapshot_net.py`, `gdb-bridge.py`,
`gdbstub_client.py`, `armdec.py`, `screenshot.py`, plus the UFS2 surgery tools
(`ufs2walk.py`, `ufs_clean.py`, `ufs_sblock_search.py`, `gpt_recover.py`) kept
from before the guest had its own shell.

Hosted tests: `test_*.c` (built with host gcc, run by `make test`) and
`test_automount.py`.

Docs: `ORIENTATION.md` (entry point), `SESSION-RULES.md` and `DEBUG_RULES.md`
(operating rules), `ROADMAP.md` (identity, honest assessment, v1 gate, feature
map, beyond-v1 research bets), `PROGRESS.md` (checkpoint log),
`WOW_FEATURES.md` (§0 is a list of hard prohibitions — read rule 7 before
patching anything on disk), `BRING-UP.md`, `TOOLCHAIN.md`,
`REVIEW-2026-07-24.md`, `PROJECT.md`, `docs/`.

## Known gaps

- **72 h soak is the last open numeric gate.** Everything else in ROADMAP §2 is
  closed or cannot be self-certified.
- **The debug protocol has no authentication.** Anyone on the LAN can peek and
  poke memory over ethertype `0x88B5`. `hmac_sha256.c` exists; the two options
  (HMAC on the protocol, or compiling it out of a "prod" build) are named in
  `docs/security-notes.md` and not yet chosen.
- **One SoC, one board.** Pins are hardcoded for the A64. QEMU-virt is the second
  target and it models no caches, which is exactly the class of bug the hardware
  keeps finding.
- **Crash forensics has never seen a real panic** (ROADMAP §2, B3): the code
  paths are unit-tested and self-tested against a real `kernel.debug`, but not
  end-to-end on hardware.
- **No vsync on the guest-window flip path.** `bzkms`'s vblank source is a
  periodic callout at the mode's refresh rate, so pacing works but its phase is
  not locked to real scanout. Fixing it properly means EL2 publishing a vblank
  counter; noted, not done.
- **Per-file license headers.** BSD-2 is in `LICENSE`; individual `.c`/`.h` files
  are being stamped with `SPDX-License-Identifier: BSD-2-Clause` incrementally,
  not retroactively across all ~90 of them.

## License

BSD 2-Clause — see `LICENSE`.
