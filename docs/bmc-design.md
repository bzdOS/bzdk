# bzdOS software-BMC — out-of-band management plane design

**Status:** design + skeleton (v1.0). Code-only; nothing here has been built or
run on the board.
**Files:** `bmc.h`, `bmc.c` (on-board dispatch + health record), `bmc_client.py`
(host CLI), `docs/bmc-integration.md` (exact edits to apply later).

---

## 1. Motivation

The Banana Pi M64 has **no UART**. The only out-of-band paths to a running (or
wedged) board are:

- **EMAC** raw-Ethernet debug console — ethertype `0x88B5`, board MAC
  `02:bd:05:00:00:01`, serviced by `dbgmon.c` on the **CPU1 debug core**
  (survives a guest wedge on CPU0 because CPU1 runs an independent poll loop —
  see `smp.c` `smp_secondary_main`).
- **USB-OTG CDC-ACM** gadget — `usbacm.c`, also polled on CPU1, bridged to the
  same console hooks.

Management capability already exists but is **ad-hoc**: it's spread across
`dbgmon.c` (memory/register/breakpoint verbs), `reboot.c` (`reboot_clean`),
`wdt.c` (`wdt_debug_hold`/`wdt_arm`/`wdt_disarm`), `smp.c` (the `dbg_*` runtime
flags), `vconsole.c` (console capture ring + RX-inject ring + TX tee), and a
dozen fixed DRAM **breadcrumb windows** each with its own magic and word
layout. To use any of it you must already know the exact address, offset and
magic. There is **no named, versioned, self-describing contract** — that is
what a BMC gives you, and what this design consolidates.

The **software BMC** is a thin, well-labelled façade: one dispatch entry point
(`bmc_dispatch`), a versioned verb catalog grouped by domain, a structured
health record, and an explicit safety gate. Every handler calls an **existing**
primitive. The only genuinely new hardware touch is an optional A64 thermal
sensor read for a temperature metric.

---

## 2. Transport & framing

The BMC does **not** own a transport. It reuses the console `dbgmon` already
services:

- **Wire:** a raw L2 Ethernet frame on `br0`, ethertype `0x88B5`, payload =
  the ASCII command line, `\r`-terminated, zero-padded to the 46-byte minimum.
  Board replies are frames from `02:bd:05:00:00:01` with the same ethertype;
  the host reads until the `dbg> ` prompt re-appears (see `hvdbg.HV._drain`).
- **USB-ACM path:** identical line protocol, just carried over the CDC-ACM
  bulk endpoints instead (`usbacm.c` feeds the same `console_*` hooks). No BMC
  code changes between the two — pick the channel by which host device you open.
- **Invocation:** `dbgmon.c`'s `exec_line()` gets **one** added branch — when
  the first token is `bmc`, it calls `bmc_dispatch(tok+1, nt-1, frame)`. All
  framing, MAC filtering, non-blocking drain and the prompt round-trip are
  unchanged. (Exact snippet in `docs/bmc-integration.md`.)
- **Context:** runs in tick/poll context on CPU1 with the guest preempted.
  Every handler is **non-blocking and bounded**, exactly like `dbgmon_service`.

Two reply forms:

1. **Text** — human/tool-readable lines, like every other dbgmon command.
2. **Breadcrumb** — `bmc health` also **latches** its record to a fixed
   cache-coherent DRAM window (`BMC1` @ `0x50006000`), so a host can read the
   same structured status with a plain `r 0x50006000 32` even when the text
   path is flaky or the board just warm-reset (the window survives a WDT reset,
   same `dc civac + dsb sy` discipline as every other lane).

### Versioning

`BMC_PROTO_MAJOR.MINOR` (currently `1.0`), reported by `bmc ver` and embedded
in the health record's `version` word (`MAJOR<<16 | MINOR`). Bump **minor** to
add verbs (back-compatible); bump **major** to change an existing verb's
request/reply shape. `bmc_client.py` can refuse an unknown major.

---

## 3. Verb catalog (v1.0)

Grouped by domain. `[ARMED]` = destructive, requires a preceding `bmc arm`
(see §5). Everything else is read-only/observational.

### 3.1 Power / reset / watchdog
| Verb | Effect | Backed by |
|------|--------|-----------|
| `bmc reset` `[ARMED]` | Clean reboot to U-Boot (drops USB pull-up, arms WDOG, spins). Never returns. | `reboot.c` `reboot_clean()` |
| `bmc wdt hold` `[ARMED]` | CPU1 stops petting → HW WDOG fires ≤16 s → reset. | `wdt.c` `wdt_debug_hold=1` |
| `bmc wdt release` | Resume petting, board stays resident. | `wdt.c` `wdt_debug_hold=0` |
| `bmc wdt arm` `[ARMED]` | (Re)start the dead-man progress window. | `wdt.c` `wdt_arm()` |
| `bmc wdt disarm` | Clear WDOG enable (keep a wedge inspectable indefinitely). | `wdt.c` `wdt_disarm()` |

### 3.2 Console
| Verb | Effect | Backed by |
|------|--------|-----------|
| `bmc con read [n]` | Dump up to `n` bytes of the guest console **capture** ring as ASCII (post-mortem log). | `UART` ring `0x50000f00` hdr + buf `0x50000f10` |
| `bmc con inject <text…>` | Push host keystrokes into the guest's virtual UART0 **RX** ring + CR. | `vconsole.c` `vconsole_rx_push()` |
| `bmc con tee [n]` | Drain up to `n` bytes pending in the guest→host **TX tee** ring (one-shot pull; host loops for a stream). | `vconsole.c` `vconsole_tx_tee_getc()` |

### 3.3 Debug flags
| Verb | Effect | Backed by |
|------|--------|-----------|
| `bmc flags` | List all `dbg_*` flags + current values. | `smp.c` / `el2_exc.c` globals |
| `bmc flag <name> <0\|1>` `[ARMED]` | Set one flag live. | direct write to the `volatile uint32_t` |

Flag names ↔ owners: `usbacm`→`dbg_usbacm`, `core_enable`→`dbg_core_enable`,
`cpu1_wdog`→`dbg_cpu1_wdog`, `isolate_noemac`→`dbg_isolate_no_emac`,
`no_guest`→`dbg_no_guest`, `block_reset`→`dbg_block_reset`. (Gated because
`core_enable=0` / `isolate_noemac=1` can sever the very channel you're on.)

### 3.4 Health / observability
| Verb | Effect | Backed by |
|------|--------|-----------|
| `bmc health` | Structured status record (text + latch to `BMC1`). | §4 |
| `bmc temp` | SoC temperature, milli-°C. | A64 THS `0x01C25000` (new read) |
| `bmc ver` | Protocol version. | compile-time constants |

### 3.5 Memory / registers / breakpoints — **kept in dbgmon**
These already form a clean set and are *not* re-wrapped, only referenced:
`gr` `sr` `sr2` `r/rb/d` `w/wb` `gva` `pt` `call` `patch` `bp/wp/bpc/bpl` `ss`
`bt` `ff` `ffv` `t`. The host CLI exposes the raw ones it needs via
`bmc_client.py dump` (which just calls `HV.read_words`). A future minor version
could add `bmc mem`/`bmc bp` aliases if a uniform namespace is wanted, but the
value/risk ratio today is low — see §7.

---

## 4. Health / status record

`struct bmc_health` (bmc.h), 32 words, latched to `BMC1` @ `0x50006000`
(`magic 0x424D4331`). Struct field order == breadcrumb word order for a 1:1
host decode (see `bmc_client.py health --raw`).

| Word | Field | Source primitive |
|------|-------|------------------|
| 0 | magic `BMC1` | — |
| 1 | version `MAJOR<<16\|MINOR` | compile-time |
| 2–3 | `uptime` = CNTPCT_EL0 | `mrs cntpct_el0` (÷CNTFRQ ≈ seconds) |
| 4–5 | `ticks` = guest-tick counter | `GICT` `0x50000800[1..2]` |
| 6 | `tick_delta` since last snapshot | computed (timer-liveness signal) |
| 7 | `exc_count` | `EXC1` `0x50000400[1]` |
| 8 | `last_exc_kind` | `EXC1[2]` |
| 9 | `last_exc_esr` | `EXC1[3]` |
| 10–11 | `guest_pc` = guest ELR | `el2_exc.c` `g_last_guest_frame.elr` |
| 12 | `online_map` (per-core bitmap) | `SMP1` `0x50000900[1]` |
| 13–16 | `hb_cpu0..3` heartbeats (per-core liveness) | `SMP1[6..9]` |
| 17 | `cons_bytes` (console progress) | `UART` `0x50000f00[1]` |
| 18 | `cons_faults` | `UART[2]` |
| 19 | `temp_mc` milli-°C (0 = n/a) | A64 THS |
| 20 | `flags` bitmap | `dbg_*` globals |
| 21 | `wdt_hold` | `wdt.c` `wdt_debug_hold` |
| 22 | `ffv_count` vector first-fault hits | `FF1V` `0x50005800[1]` |
| 23–31 | reserved | — |

This single record answers the operator's core questions at a glance: *is the
timer advancing* (`tick_delta`), *is the guest making progress* (`cons_bytes`,
`guest_pc`), *did it fault* (`exc_count`/`last_exc_*`/`ffv_count`), *are the
cores alive* (`online_map`/`hb_*`), *how hot* (`temp_mc`), *what's the mgmt
config* (`flags`/`wdt_hold`). It is exactly the set `chimpd.py` today scrapes
piecemeal from GICT/EXC/VCON — see §6.

### Temperature (the one new read)

A64 THS controller at `0x01C25000`; `THS0_DATA` @ `+0x80`. ATF/U-Boot leave it
running, so an EL2 read reaches it directly. The A64 calibration is affine with
**negative** slope; using the `sun50i-a64-ths` coefficients (Linux
`sun8i_thermal.c`):

```
T_milliC ≈ (2170 − raw) * 1000000 / 8560
```

Reported as `0 = n/a` when `raw ∈ {0, 0xFFF}` (controller idle). **The exact
coefficients must be validated on-board** — the formula is documented inline in
`bmc.c` so it's a one-line fix once measured against a known ambient.

---

## 5. Auth / safety

The channel is **L2-local and unauthenticated by design** — this is a
trust-the-LAN lab debug plane, not a production BMC. But several verbs are
**destructive**:

- `bmc reset` drops the board to U-Boot.
- `bmc wdt hold` / `bmc wdt arm` lead to a reset.
- `bmc flag core_enable 0` / `bmc flag isolate_noemac 1` can **sever the
  management channel itself**.

To stop a stray, duplicated, or replayed frame from firing one of these on its
own, destructive verbs require an explicit **two-step arm**, like a BMC's
chassis-power confirm:

```
bmc arm <nonce>      # latches nonce + CNTPCT timestamp
bmc reset            # allowed once, if armed within ~10 s; arm is CONSUMED
```

- **Time-boxed:** the arm expires after `BMC_ARM_WINDOW_S` (≈10 s) measured in
  real time (CNTPCT), so a stale arm can't linger.
- **One-shot:** `bmc_check_armed()` consumes the arm, so exactly one
  destructive verb fires per arm — a replayed `reset` after the fact is refused.
- **Client ergonomics:** `bmc_client.py` auto-arms immediately before a
  destructive send, so the human types one command while the on-board window
  still guards against spontaneous/replayed frames.

This is a **foot-gun guard, not security.** Hardening path if this ever leaves
the lab (all future-minor, non-breaking):

1. Per-command HMAC over a shared secret (nonce already in the frame).
2. Bind the arm to the requesting host's source MAC (drop the arm if the next
   destructive frame comes from a different MAC).
3. A compile-time `BMC_READONLY` build that omits every `[ARMED]` handler for
   field images.

Additionally, two existing guards remain in force and are surfaced (not
replaced) by the BMC: `dbg_block_reset` (el2_exc.c intercepts the *guest's* own
PSCI SYSTEM_RESET so a guest can't drop the board off the USB bus), and CPU1's
unconditional WDOG petting (`wdt_debug_kick`) which keeps a wedged board
inspectable until an operator explicitly `wdt hold`s it.

---

## 6. What already exists vs what is new

| Capability | Status | Backing |
|------------|--------|---------|
| EMAC/USB-ACM line transport + framing | **Exists** | `dbgmon.c`, `usbacm.c`, `hvdbg.HV` |
| Clean reset (`reset`) | **Exists** | `reboot_clean()`; also `hvdbg.wdt_reset`, `chimpd.nc.wdt_reset` |
| Watchdog hold/release/arm/disarm | **Exists** | `wdt.c` `wdt_debug_hold`/`wdt_arm`/`wdt_disarm` |
| Guest regs / sysregs / memory / bp / faults | **Exists** | `dbgmon.c` verbs + breadcrumbs |
| Console **capture** read | **Exists** | `UART` ring; `hvdbg.vconsole_text` |
| Console **inject** (RX) | **Exists (primitive)** | `vconsole_rx_push()` — *no verb wired yet* |
| Console **TX tee** drain | **Exists (primitive)** | `vconsole_tx_tee_getc()` — *no verb wired yet* |
| Debug-flag list/toggle | **Partly** — flags exist, no named list/set | `dbg_*` globals in `smp.c`/`el2_exc.c` |
| **Structured health record** | **New (aggregation)** | reads existing GICT/EXC/SMP1/UART/FF1V |
| **Temperature metric** | **New (small driver)** | A64 THS `0x01C25000` |
| **Named/versioned verb namespace** | **New** | `bmc.c` dispatch + `bmc.h` catalog |
| **Destructive-verb arm gate** | **New** | `bmc.c` `bmc_arm`/`bmc_check_armed` |

**Net:** ~90% of the *capability* already exists as callable primitives; the
BMC's contribution is **consolidation** — one named/versioned verb surface, the
console inject/tee primitives finally exposed as verbs, a structured health
aggregate, a temperature read, and a safety gate. New on-board code is small
and additive (`bmc.o` + one dispatch line).

### Breadcrumb-address caveat (found while cataloging `chimpd.py`)

`chimpd.py` reads the EXC and GICT breadcrumbs at **SRAM** `0x00018100` /
`0x00018200`, whereas `dbgmon.c`, `el2_exc.c` and this design use **DRAM**
`0x50000400` / `0x50000800` (the SRAM copies get wiped by BROM/U-Boot on a warm
reset; the DRAM ones survive — see the comment in `el2_exc.c`). The BMC health
record standardizes on the **DRAM** addresses. `chimpd.py` should migrate to
`bmc health --raw` (or the DRAM addresses) so host and board agree; noted for
`docs/bmc-integration.md`.

---

## 7. Non-goals / future

- **Not** re-wrapping the dbgmon memory/bp verbs — they're already clean; a
  `bmc mem`/`bmc bp` alias set is a future minor if a uniform namespace wins.
- **Not** a binary/TLV protocol — v1 stays ASCII-line so it drops straight into
  the existing dbgmon console and stays greppable by hand. A binary `bmc`
  ethertype sub-protocol is possible later without disturbing v1 text verbs.
- **Not** authenticated (see §5) — deliberately, for the lab.
- Future minors: `bmc log` (stream + follow the capture ring with wrap
  handling), `bmc bp`/`bmc mem` aliases, `bmc watch <verb> <interval>`
  server-side sampling, `BMC_READONLY` field build.
```
