# GDB remote-serial-protocol stub for the bzdOS EL2 hypervisor

Debug the **FreeBSD arm64 EL1 guest** from a host `gdb`/`lldb`, over the same
raw-Ethernet EMAC channel (ethertype `0x88B5`) the REPL / dbgmon already use.
The host runs a tiny TCP↔EMAC relay (`gdb-bridge.py`) so that

```
aarch64-none-elf-gdb microkernel-dbg.elf
(gdb) target remote :1234
```

speaks bytes to the board with zero board-side networking beyond what already
exists.

> **Status of the tree.** `gdbstub.c` / `gdbstub.h` already exist and implement
> the RSP *core*: packet framing + checksum + `+/-` acks, `g/G/p/P`, `m/M`,
> `c/C/s/S`, `?`, `vCont`, `qSupported` / `qXfer:features:read:target.xml`, and
> **software** breakpoints (`Z0/z0`, patched `BRK #0x7d0`). What is **missing**
> and what this design adds:
>
> 1. **Hardware** breakpoints (`Z1/z1`) and **watchpoints** (`Z2/Z3/Z4`) wired
>    to the existing `hwbp.c` DBGB\*/DBGW\* machinery — delivered as a new,
>    non-invasive companion module **`gdbstub_hw.c` / `gdbstub_hw.h`** (the
>    "code-only, no-edit" rule forbids touching `gdbstub.c`; the exact 6-line
>    `dispatch()` edit that calls into it is given in
>    `gdbstub-integration.md`, to apply later).
> 2. A **standalone** host bridge `gdb-bridge.py` (today the bridge only exists
>    as a comment block inside `gdbstub.c`).
> 3. The **el2_trap routing** and **dbgmon channel hand-off** wiring
>    (`gdbstub-integration.md`).
> 4. The **watchdog / SMP-debug-core** and **PSTATE.D early-boot** analysis
>    (below) — the parts that make or break attaching to a *live board*.

---

## 1. Architecture at a glance

```
   host                                  Banana Pi M64 (A64, Cortex-A53)
 ┌───────┐   TCP :1234    ┌──────────────┐  0x88B5   ┌──────────────────────────┐
 │  gdb  │◄──────────────►│ gdb-bridge.py│◄─────────►│ EMAC RX/TX ring (emac.c) │
 └───────┘  raw RSP bytes └──────────────┘  frames   └───────────┬──────────────┘
                                                                 │ emac_getc/putc
                                                     ┌───────────▼──────────────┐
                                                     │ gdb_getc/putc/flush       │
                                                     │  (main_dbg.c shims)       │
                                                     ├───────────────────────────┤
                                                     │ gdbstub.c   RSP core       │
                                                     │  ├─ g/G/m/M/p/P/c/s/?/vCont│
                                                     │  ├─ Z0/z0 sw-bp (BRK)      │
                                                     │  └─ Z1/z1/Z2-4 ─► gdbstub_hw│
                                                     │ gdbstub_hw.c  ─► hwbp.c     │
                                                     │  DBGBVR/BCR, DBGWVR/WCR     │
                                                     │  MDSCR_EL1.SS, MDCR_EL2.TDE │
                                                     └───────────────────────────┘
                                              acts on struct el2_frame (x0..x30,
                                              elr=PC, spsr=CPSR) captured in el2_trap
```

The stub never opens a socket, never allocates, never uses libc or FP/SIMD
(`-mgeneral-regs-only`). It is a pure byte-stream state machine plus a handful
of `MRS`/`MSR` to the guest's **banked EL1 debug registers**, issued from EL2 —
exactly as `hwbp.c` and the single-step lane in `el2_exc.c` already do.

---

## 2. Transport

### 2.1 Wire framing (identical to the existing console)

The RSP payload rides **raw** in the Ethernet payload — no length prefix of our
own; GDB's own `$…#cc` framing *is* the framing:

| direction     | dst               | src               | etype    | payload                               |
|---------------|-------------------|-------------------|----------|---------------------------------------|
| host → board  | `ff:ff:ff:ff:ff:ff`| host MAC          | `0x88B5` | raw RSP bytes, zero-padded to 46      |
| board → host  | `ff:ff:ff:ff:ff:ff`| `02:bd:05:00:00:01`| `0x88B5` | raw RSP bytes, zero-padded to 46      |

Two facts about the EMAC console constrain the encoding, and RSP satisfies both:

* **RX stops at the first NUL.** `emac_getc()` feeds payload bytes up to the
  first `0x00`. RSP packet text (`$`, `#`, hex, `+`, `-`, `}`-escape, `*`
  run-length) **never contains a NUL**, so zero-padding to the 46-byte Ethernet
  minimum is transparent. The bridge strips trailing NULs
  (`payload.split(b"\0",1)[0]`).
* **Single-byte control chars work.** GDB's `+`/`-` acks and the `0x03`
  interrupt are 1-byte payloads; padded to 46 NULs they decode to exactly one
  byte board-side.

### 2.2 Byte hooks (board side)

`gdbstub.c` consumes three externs, wired in `main_dbg.c` to EMAC:

```c
int  gdb_getc(void);   /* { emac_poll(); return emac_getc(); }  — pumps RX */
void gdb_putc(int c);  /* emac_putc(c)                                     */
void gdb_flush(void);  /* force queued TX out as a frame now               */
```

`gdb_getc()` **must** call `emac_poll()` so RX keeps flowing while the stub
spins in its stopped-state command loop with IRQs masked (see §6).

### 2.3 Channel ownership — RSP *xor* dbgmon

The `0x88B5` channel carries **either** dbgmon's line-oriented text protocol
**or** binary RSP, never both at once: a stray `dbg> ` prompt injected into a
GDB stream corrupts a packet, and vice-versa. Two options, both documented in
`gdbstub-integration.md`:

* **Build-time**: a debugger build calls `gdbstub_poll()` on the tick path
  *instead of* `dbgmon_service()`. Simple, but you lose the text REPL.
* **Runtime hand-off (recommended)**: dbgmon gains a `gdb` command. Typing
  `gdb` at the `dbg>` prompt sets a latch; from then on the tick path routes to
  `gdbstub_poll()` and prints no more prompts. `gdb-bridge.py --arm` sends that
  one text line before it flips to raw-RSP relaying. Detach (`D`/`k`) drops the
  latch and dbgmon resumes.

---

## 3. Register model — the aarch64 g-packet

GDB's `g`/`G` (and `p`/`P`) exchange a fixed register block whose **order and
sizes** must match the target description we advertise via
`qXfer:features:read:target.xml`. We advertise **exactly** the AArch64 *core*
feature (`org.gnu.gdb.aarch64.core`) and nothing else, so GDB does not expect
the FP/SIMD block and our `g` reply is accepted verbatim.

### 3.1 g-packet layout (this stub)

| g-index | reg    | bytes | source in `struct el2_frame`                    |
|--------:|--------|------:|-------------------------------------------------|
| 0..30   | x0..x30| 8 ea. | `frame->x[0..30]`                               |
| 31      | sp     | 8     | `MRS sp_el1` (guest SP; **banked**, not in frame)|
| 32      | pc     | 8     | `frame->elr` (ELR_EL2 == guest PC)              |
| 33      | cpsr   | 4     | low 32 of `frame->spsr` (SPSR_EL2 == guest PSTATE)|

Total = 31·8 + 8 + 8 + 4 = **268 bytes → 536 hex chars**. Each value is
**little-endian** hex (LSB first) per RSP convention — see `emit_le`/`read_le`
in `gdbstub.c`.

Notes / gotchas:

* **`sp` is banked.** At EL2 the guest's stack pointer is `SP_EL1`, not any
  field of the saved frame. `reg_get`/`reg_set` `MRS`/`MSR sp_el1`. Because the
  guest is stopped (preempted) when GDB reads/writes it, this is the guest's
  live SP.
* **`pc` write** (`P 20=…` or a `c addr`) rewrites `frame->elr`; the guest
  resumes there on `eret`.
* **`cpsr` is 32-bit** in the g-packet even though `SPSR_EL2` is 64-bit; we
  splice only the low 32 on write so we never clobber the reserved high half.
* **FP/SVE deliberately omitted.** FreeBSD kernel debugging rarely needs V-regs;
  advertising them would force us to save/restore the SIMD file at every trap
  (and the build is `-mgeneral-regs-only`). If needed later, add an
  `org.gnu.gdb.aarch64.fpu` feature and append `v0..v31`(128b), `fpsr`, `fpcr`
  to the g-block — see §9.

### 3.2 target.xml

Already embedded in `gdbstub.c` as `target_xml[]` and served through
`xfer_reply()`. It lists `x0..x30`, `sp` (`data_ptr`), `pc` (`code_ptr`),
`cpsr` — matching the table above 1:1. **Do not reorder** without editing both.

---

## 4. Memory access — `m` / `M`

`m addr,len` / `M addr,len:data` resolve every guest address through
`resolve(addr)`:

1. `AT S1E1R, addr` → `PAR_EL1`. On success the guest **stage-1** translation
   gives the IPA; the guest runs stage-2-identity (IPA==PA) so the result is
   directly addressable under our flat EL2 map. This is the same primitive as
   dbgmon's `gva` command.
2. On a stage-1 fault (`PAR_EL1.F==1`) fall back to treating `addr` as a **flat
   physical** address — lets GDB also poke the EL2 kernel itself, or guest
   physical RAM before the guest MMU is on.

Byte granularity, little-endian hex both ways. `M` follows every written word
with `dc cvau; ic ivau` (`isync_patch`) in case GDB is writing **code** (e.g.
`set {int}pc = …`, or GDB inserting its own breakpoint via `M` on a target that
declined `Z0`). This is the same self-modifying-code flush `onebp.c`/`gtrace.c`
use.

> **Risk**: `AT S1E1R` uses the *current* `TTBR0/1_EL1`, which is the guest's
> only while the guest context is loaded. On the tick path that is true (guest
> just trapped). If the stub ever runs on CPU1 against a *snapshot* (see §6),
> `AT S1E1R` on CPU1 translates through **CPU1's** TTBR, not the guest's — a
> real hazard flagged in §6 and §10.

---

## 5. Breakpoints, watchpoints, single-step

### 5.1 Software breakpoints `Z0/z0` — already implemented

`bp_insert(addr)`: `resolve()` → save original word → write `BRK #0x7d0`
(`0xD420FA00`) → `isync_patch`. On execution the guest takes a `BRK` synchronous
exception, `ESR_EL2.EC==0x3C`, routed to EL2 by `MDCR_EL2.TDE=1`. `z0` restores
the saved word. Up to `GDB_MAX_BP` (16) live at once.

**Why SW bp are the early-boot workhorse**: `BRK` is a *synchronous* exception,
**not gated by `PSTATE.D`** (the debug-exception mask). So a `Z0` fires even in
the guest's earliest locore where HW debug is masked (§7). This is the same
insight `onebp.c` exploits with `HVC`.

### 5.2 Hardware breakpoints `Z1/z1` — NEW (`gdbstub_hw.c`)

GDB packet `Z1,addr,kind`. Map to a `hwbp.c` **breakpoint** slot:

```
gdbstub_hw_insert(1 /*type*/, addr, kind)
  → allocate a free DBGB slot (0..n_bp-1)
  → hwbp_set(slot, addr, /*is_write_wp=*/0)     // arms DBGBVR/BCR + MDE + TDE
  → record {addr,slot,type} in the hw shadow table
```

`z1,addr,kind` → find the slot for `addr` → `hwbp_clear(slot, 0)` → free the
shadow entry. When a HW bp hits, `ESR_EL2.EC==0x30`; the stop reply carries
`hwbreak:;` (§5.5).

**Why HW bp in addition to SW bp**: (a) breakpoints in **read-only / XIP**
guest memory that `M`/`BRK` cannot patch; (b) breakpoints the guest itself must
not see when it reads its own code (SW `BRK` is visible to the guest's I-fetch
integrity checks). Trade-off: only `n_bp` (6 on A64, clamped from
`ID_AA64DFR0_EL1`) exist.

### 5.3 Watchpoints `Z2` (write) / `Z3` (read) / `Z4` (access) — NEW

GDB packet `Z2,addr,kind` (kind = length in bytes: 1/2/4/8). Map to a `hwbp.c`
**watchpoint** slot. `hwbp_set(slot, va, 1)` currently hard-codes
`LSC=0b10` (store-only) and `BAS=0xff` (whole doubleword). For full GDB
semantics the stub needs the LSC to follow the GDB type:

| GDB packet | meaning | DBGWCR.LSC (bits[4:3]) |
|-----------|---------|------------------------|
| `Z2`      | write   | `0b10`                 |
| `Z3`      | read    | `0b01`                 |
| `Z4`      | access  | `0b11`                 |

Because `hwbp_set()` takes no LSC argument, `gdbstub_hw.c` provides
`gdbstub_hw_set_wp(slot, va, lsc, len)` that programs `DBGWVR/DBGWCR` itself
(same field builders as `hwbp.c`, LSC and a `BAS` byte-mask derived from
`len`+alignment) — **or**, preferably, a 1-line extension to `hwbp_set` adding
an `lsc` parameter (given in `gdbstub-integration.md`; not applied here since
`hwbp.c` is existing code). Until that lands, `Z3`/`Z4` degrade to `Z2`
store-watch and this is called out to the user with a `qXfer`/`E` note.

On a watchpoint hit `ESR_EL2.EC==0x34`; `FAR_EL2` holds the accessed data
address. The stop reply carries `watch:ADDR;` / `rwatch:` / `awatch:` (§5.5).

### 5.4 Single-step `s` / `vCont;s` — already implemented

`arm_step()` sets `MDCR_EL2.TDE=1`, `MDSCR_EL1.SS=1`, and on the resumed frame
`SPSR.SS=1` (`bit21`) + clears `SPSR.D` (`bit9`, unmask). The guest executes one
instruction and takes a **Software-Step** exception, `ESR_EL2.EC==0x32`, back to
EL2. `el2_trap` routes `EC==0x32` **to the stub** (not the `el2_ss` ring logger)
when `gdbstub_step_active()` — see integration hook. The stub sends the stop
reply and re-enters the command loop. `disarm_step()` clears SS on `c`.

This is the **same** MDSCR_EL1.SS mechanism `el2_ss_toggle()` uses; the stub and
`el2_ss` are mutually exclusive owners of the SS lane (they must not both arm SS
for the same run — documented in `hwbp.c`'s TDE note).

### 5.5 Trap → stop-reply mapping

`el2_trap`, when `gdbstub_attached()`, diverts these guest EC values to
`gdbstub_on_debug_event(frame, sig)` **before** the normal fault/log path:

| ESR_EL2.EC | event                | GDB stop reply                    |
|-----------:|----------------------|-----------------------------------|
| `0x3C`     | guest `BRK` (SW bp)  | `T05swbreak:;thread:01;`          |
| `0x30`     | HW breakpoint        | `T05hwbreak:;thread:01;`          |
| `0x34`     | watchpoint           | `T05watch:<FAR>;thread:01;` (rwatch/awatch per type) |
| `0x32`     | single-step complete | `T05thread:01;`                   |
| (Ctrl-C)   | host `0x03`          | `T02thread:01;` (SIGINT)          |
| other fault| guest data/instr abort | `T0Bthread:01;` (SIGSEGV) — optional, lets GDB catch a guest panic |

`sig 5 = SIGTRAP`, `2 = SIGINT`, `0x0B = SIGSEGV`. The `swbreak:`/`hwbreak:`/
`watch:` stop-reason keys require the matching `qSupported` features
(`swbreak+;hwbreak+`) which `gdbstub_hw.c` appends. `gdbstub.c`'s `send_stop()`
currently emits only `T{sig}thread:01;` — `gdbstub_hw.c` supplies
`gdbstub_hw_stop_reason(frame, buf)` that produces the richer reply for the
HW/watch cases; the integration hook chooses which sender to call by EC.

**GDB's sticky-vs-oneshot mismatch (important).** `hwbp.c::hwbp_handle()`
**one-shot-disables** the slot on a hit (so a *free-running* guest makes forward
progress). GDB expects a HW bp/wp to persist until an explicit `z`. Therefore
under GDB ownership we must **not** let `hwbp_handle()` run for these ECs;
instead the integration hook, while `gdbstub_attached()`, calls the stub
directly and the stub **re-arms** the same slot on the next `c`/`s` (the slot
address is still in the shadow table). This is spelled out in
`gdbstub-integration.md` as an ordering constraint: *stub-divert before
hwbp_handle*.

---

## 6. Where does the stub run? Tick-path (CPU0) vs debug-core (CPU1)

This is the single most consequential design choice and interacts with the
hardware watchdog (§ memory: *"Watchdog + persistent chimpd"*, *"SMP debug core
working"*).

### Model A — CPU0 tick path (what `gdbstub.c` is written for today)

`gdbstub_poll(frame)` runs inside `el2_trap` on the guest's preemption tick,
against the **live** frame. Halting the guest = **not** returning from
`el2_trap`; we spin in `command_loop()` polling EMAC.

* ✔ The frame is the real guest context; `AT S1E1R` uses the guest's TTBR;
  register writes hit the frame that is about to `eret`.
* ✔ Simple; already implemented.
* ✘ **Watchdog.** `wdt_pet()` is called *once* per `el2_trap` entry. While we
  spin in the stopped command loop (a human is typing in GDB, or GDB is idle at
  a breakpoint), **no new EL2 exceptions are taken**, so the WDT is never petted
  again and the ~16 s watchdog fires → board resets to U-Boot. **A breakpoint
  held longer than the watchdog timeout resets the board.** This is THE feasibility
  risk (§10).
  *Mitigation*: pet the WDT from inside `command_loop()`'s spin
  (`wdt_pet()` per iteration) **and/or** keep CPU1 petting the shared HW WDOG
  via `wdt_debug_kick()` (CPU1 already owns it per the SMP-debug-core memory).
  The spin-pet is a 3-line change (given in integration doc) and is the minimum
  needed to make live GDB sessions usable.

### Model B — CPU1 debug core (matches the SMP memory)

CPU1 (`smp_secondary_main`) already runs `dbgmon` over EMAC independently of the
guest, snapshots the guest frame into `g_last_guest_frame` on every CPU0 guest
trap, and **owns the HW watchdog** (`wdt_debug_kick`). Running the RSP loop on
CPU1 means:

* ✔ The watchdog keeps being petted by CPU1's own loop regardless of how long
  GDB dwells at a breakpoint — the §10 risk largely evaporates.
* ✔ CPU0/the guest can be genuinely *parked* (spun in EL2) while CPU1 serves
  GDB, a cleaner "target stopped" than freezing an interrupt handler.
* ✘ `g_last_guest_frame` is a **snapshot**: register writes (`G`/`P`) must be
  written **back** into the frame that CPU0 will `eret`, which needs a
  handshake (CPU0 must re-load the frame after CPU1 edits it, or CPU1 edits the
  in-place frame CPU0 is parked on). Memory `m/M` and `AT S1E1R` must run in a
  context that sees the **guest's** TTBR — on CPU1 that means either loading the
  guest `TTBR0/1_EL1`+`VTTBR_EL2` before `AT`, or doing the translation on CPU0.
* ✘ More moving parts (cross-core stop/resume signalling).

**Recommendation**: ship Model A **with the spin-pet mitigation** first (works
today, minimal change, matches the existing `gdbstub.c`), and treat Model B as
the hardening path once cross-core register write-back is in place. The
`gdbstub.h` API (`gdbstub_poll`/`gdbstub_on_debug_event`) is the same in both;
only *who calls it* and the frame's provenance differ.

---

## 7. The PSTATE.D=1 early-boot limitation — when can you attach?

Per the tree's *"chimp-guest-debug-masked"* note: the FreeBSD guest runs its
earliest locore with **`PSTATE.D=1`**, which **masks watchpoint, breakpoint and
software-step debug exceptions** (it does **not** mask `BRK`, `HVC`, `SVC` — those
are *instruction-generated* synchronous exceptions, not debug exceptions).

Consequences for attach:

| capability                     | works while guest PSTATE.D=1? |
|--------------------------------|-------------------------------|
| `Z0` software breakpoint (BRK) | **yes** — BRK isn't gated by D |
| `Z1` HW breakpoint (DBGBVR)    | no — masked until guest clears D |
| `Z2-4` watchpoint (DBGWVR)     | no — masked until guest clears D |
| `s` single-step (MDSCR.SS)     | no — Software-Step is a debug exception, masked |
| `g/G/m/M/p/P`, `?`, `c`        | **yes** — these don't depend on debug exceptions |

So the practical attach story is:

1. **Any time**: connect, read/write registers and memory, and set **software**
   breakpoints. This alone covers most guest-kernel debugging.
2. **HW bp/wp + single-step become viable once the guest clears `PSTATE.D`** —
   which FreeBSD does around `cninit`/early `machdep` once it installs its own
   exception vectors and enables debug. To *stop at that boundary*, set a **`Z0`
   software breakpoint** on the guest symbol that runs just after D is cleared
   (e.g. an early `initarm`/`cninit` address), let the guest hit it, and *then*
   place HW bp/wp/step — by then `PSTATE.D==0` (visible in `cpsr`, bit 9) and
   they arm normally.
3. The stub can also **force** debug on for the resumed instruction by clearing
   `SPSR.D` in the frame before `eret` (as `arm_step()` already does). That
   unmasks debug for the guest *from that point*; use with care — it changes
   guest-visible PSTATE. Only do it when the user explicitly steps/continues
   into a HW-armed run.

The stub advertises this by **accepting** `Z1-4` at any time but, if it detects
`cpsr.D==1` on the stopped frame, replying `E01` (or a textual `qXfer` note) so
GDB reports "cannot set hardware breakpoint yet" rather than silently arming a
slot that will never fire.

---

## 8. RSP subset implemented

| packet                         | handler                | notes                        |
|--------------------------------|------------------------|------------------------------|
| `?`                            | `send_stop`            | last stop reason             |
| `g` / `G`                      | reg_get/set loop       | §3 layout                    |
| `p n` / `P n=v`                | single reg             |                              |
| `m a,l` / `M a,l:d`            | resolve + LE hex       | §4                           |
| `Z0,a,k` / `z0,a,k`            | `bp_insert/remove`     | SW BRK                       |
| `Z1,a,k` / `z1,a,k`            | **gdbstub_hw**         | HW bp → DBGB slot            |
| `Z2/Z3/Z4,a,k` / `z…`          | **gdbstub_hw**         | watch → DBGW slot, LSC by type|
| `c [a]` / `C sig[;a]`          | disarm SS, CONTINUE    |                              |
| `s [a]` / `S sig[;a]`          | arm SS, STEP           |                              |
| `vCont?` / `vCont;c;C;s;S`     | first action           | single thread                |
| `H`, `qC`, `qfThreadInfo`…     | 1 thread (`01`)        |                              |
| `qSupported`                   | `PacketSize;qXfer:features:read+;swbreak+;hwbreak+` |
| `qXfer:features:read:target.xml`| `xfer_reply(target_xml)` | §3.2                     |
| `D` / `k`                      | remove all, detach     | drops channel latch (§2.3)   |
| `qAttached`                    | `1`                    | attach to existing "process" |

Everything else → empty reply (`$#00`), the RSP "unsupported" signal, which GDB
handles gracefully.

---

## 9. Optional future extensions

* **FP/SVE registers** — add `org.gnu.gdb.aarch64.fpu`, save/restore V-file at
  each stop (build must drop `-mgeneral-regs-only` for that lane).
* **Non-stop / multiprocess** — not needed; single "thread" (`01`) is the guest
  CPU0. If guest SMP is debugged, map each guest vCPU to a GDB thread id.
* **`qRcmd` (monitor)** — bridge `monitor` commands to dbgmon's text commands so
  `monitor gr`, `monitor sr2`, etc. work from inside GDB without leaving RSP.
* **`vFlashWrite`/kernel image load** — out of scope; `kload.c` owns that.

---

## 10. Biggest feasibility risk

**Holding the guest stopped at a breakpoint versus the hardware watchdog.** On
the CPU0 tick-path model, the RSP command loop spins in `el2_trap` with IRQs
masked; `wdt_pet()` runs only on `el2_trap` *entry*, so any human-scale dwell at
a breakpoint (or GDB simply sitting idle at a stop) exceeds the ~16 s WDT and
resets the board to U-Boot mid-session. The fix is small but **mandatory** for a
usable session — pet the WDT inside the command-loop spin **and** keep CPU1's
`wdt_debug_kick()` running (Model B) so the debug core, not the frozen guest
tick, owns liveness. Secondary risk: `AT S1E1R`-based memory access is only
valid in a context holding the guest's TTBR, which constrains a CPU1-hosted stub
(§4, §6). Both are addressed above; neither blocks the software design, but the
watchdog interaction must be wired correctly before the first live attach.
