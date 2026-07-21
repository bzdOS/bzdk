# GDB stub — integration edits (apply LATER, do not apply now)

Every edit below is a snippet against an **existing** file. This document is the
"how to wire it in" companion to `docs/gdbstub-design.md`; nothing here is
applied by the design task. New files already delivered:

* `gdbstub.c` / `gdbstub.h` — RSP core + SW breakpoints (already in tree).
* `gdbstub_hw.c` / `gdbstub_hw.h` — HW bp/wp lane (new, this task).
* `gdb-bridge.py` — host TCP↔EMAC relay (new, this task).

The current **DBG build reality** (from `main_dbg.c` + `smp.c`) drives the
wiring and differs from the tick-path model `gdbstub.c`'s header comment
assumes:

* IRQs are routed to the guest (`HCR_EL2.IMO=0`); **there is no EL2 timer tick**.
* **CPU1 is the debug core**: it owns EMAC, runs `dbgmon_service(&g_last_guest_frame)`
  in a loop, and pets the HW watchdog via `wdt_debug_kick()` every iteration
  (`smp.c` ~L363-404). CPU0 runs the guest and sets `dbg_core_active=1` so it
  stops touching EMAC.

So the stub is hosted on **CPU1** (Model B in the design doc), against the
`g_last_guest_frame` snapshot, with a small **cross-core stop/resume handshake**
for breakpoint/step traps that fire on CPU0.

---

## 1. Makefile — build the two new objects into the DBG product

`DBG_OBJS` (the `dbg:` target). Add `gdbstub.o gdbstub_hw.o`:

```make
DBG_OBJS := start.o main_dbg.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
            gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
            emac.o dbgmon.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o \
            gdbstub.o gdbstub_hw.o          # <-- add
```

The generic `%.o: %.c` rule already compiles them with the right freestanding
flags; no other Makefile change needed. (Optionally add them to the `fbsd:` /
`repl:` products too if you want the stub there; both already link `hwbp.o`.)

---

## 2. main_dbg.c — byte-transport shims + init

The console hooks (`console_getc/putc/poll/flush`) already exist (~L52). Add the
GDB stub's three externs next to them, mapped to the same EMAC console, and call
`gdbstub_init()` once after `dbgmon_init()`:

```c
/* --- GDB RSP transport: same EMAC 0x88B5 channel as dbgmon --------------- */
int  gdb_getc(void)   { emac_poll(); return emac_getc(); }  /* MUST pump RX  */
void gdb_putc(int c)  { emac_putc(c); }
void gdb_flush(void)  { emac_flush(); }
```

```c
    dbgmon_init();
    gdbstub_init();          /* <-- add: resets stub state, TDE on. No banner. */
    DBG_BC(1, 3);
```

Add the includes at the top of `main_dbg.c`:

```c
#include "gdbstub.h"
#include "gdbstub_hw.h"
```

---

## 3. Shared cross-core stop/resume state (new small block in el2_exc.c)

Guest debug traps (`BRK`/step/hwbp/watch) fire in `el2_trap` on **CPU0**, but the
RSP transport lives on **CPU1**. Coordinate with three cache-coherent globals
(SMPEN makes plain globals coherent, as the tree already relies on for
`g_last_guest_frame`). Add near `g_last_guest_frame` in `el2_exc.c`:

```c
/* GDB cross-core stop/resume handshake (see docs/gdbstub-integration.md).
 *   gdb_stop_pending : CPU0 sets when the guest hit a bp/step/wp; CPU1 serves.
 *   gdb_stop_signal  : the GDB signal (5 SIGTRAP / 2 SIGINT).
 *   gdb_resume_act   : CPU1 writes a GDB_RUN_* code; CPU0 applies it and clears.
 */
volatile uint32_t gdb_stop_pending;
volatile uint32_t gdb_stop_signal;
volatile uint32_t gdb_resume_act;
```

---

## 4. el2_exc.c — divert guest debug traps to the stub (CPU0 side)

Inside `el2_trap`, in the lower-EL SYNC block (`(kind>>2)==2 && (kind&3)==SYNC`),
**before** the existing `hwbp_handle()` / fault-record paths, add the GDB divert.
It only fires while GDB is attached; otherwise the existing `hwbp.c` one-shot
path and fault breadcrumb are untouched.

```c
    if ((kind >> 2) == 2u && (kind & 3u) == EL2_KIND_SYNC) {
        uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

        /* ---- GDB divert (only while a host gdb is attached) -------------- */
        if (gdbstub_attached() &&
            (ec == 0x3Cu ||                        /* guest BRK  (SW bp)   */
             ec == 0x32u ||                        /* single-step complete */
             ((ec == 0x30u || ec == 0x34u) &&      /* HW bp / watchpoint    */
              gdbstub_hw_active()))) {
            int sig = 5;                           /* SIGTRAP */
            /* Publish the stop to CPU1 and park until it resumes us. The frame
             * is snapshotted for CPU1 to read/edit; CPU1 writes gdb_resume_act
             * and any register edits back into g_last_guest_frame, which we
             * copy back before eret. */
            g_last_guest_frame = *frame;
            gdb_stop_signal  = (uint32_t)sig;
            gdb_resume_act   = 0xffffffffu;        /* "no decision yet"        */
            __asm__ volatile("dsb sy" ::: "memory");
            gdb_stop_pending = 1;
            while (gdb_resume_act == 0xffffffffu)  /* CPU1 runs the RSP loop   */
                __asm__ volatile("wfe");           /* woken by CPU1's sev      */
            *frame = g_last_guest_frame;           /* apply CPU1's reg edits   */
            gdb_stop_pending = 0;
            /* CPU1 already armed/disarmed MDSCR_EL1.SS + SPSR.SS in the frame
             * for STEP vs CONTINUE (gdbstub apply()), so just return. Do NOT
             * advance ELR — for a BRK the stub rewinds/reprograms as needed. */
            return;
        }
        /* ... existing ec==0x17 SMC / 0x32 el2_ss / firstfault / hwbp_handle
         *     / gtrace paths continue unchanged below ... */
```

**Ordering constraint (critical).** This block must sit **before** the existing
`if ((ec==0x30||ec==0x34) && hwbp_handle(...))` line so that, while GDB owns the
slots, `hwbp.c`'s *one-shot* handler does not clear them (GDB wants them sticky
until an explicit `z`). When GDB is **not** attached, the guard falls through and
`hwbp_handle` behaves exactly as today.

> **PSTATE.D note**: `ec==0x3C` (BRK) fires even during early locore
> (`PSTATE.D=1`); `ec==0x30/0x32/0x34` only fire once the guest clears
> `PSTATE.D`. That is the software-vs-hardware attach story from design §7 — no
> code change needed, it falls out of which ECs the CPU can raise.

---

## 5. smp.c — CPU1 debug-core loop hosts the RSP command loop

In `smp_secondary_main`, the CPU1 `for(;;)` loop (~L367) currently calls
`dbgmon_service(&g_last_guest_frame)`. Gate that on a latch and, when GDB has
taken the channel, service the stub instead. Two service points: (a) a pending
cross-core stop from CPU0 (§4), and (b) async traffic (a `$…` command or Ctrl-C
arriving while the guest runs).

```c
        for (;;) {
            if (dbg_cpu1_wdog)
                wdt_debug_kick();          /* pets HW WDOG every iteration —
                                            * this is what makes a long GDB dwell
                                            * at a breakpoint safe (design §6/§10) */
            /* ... existing PC5 pinmux enforce ... */

            if (gdb_channel) {             /* latch set by dbgmon `gdb` command  */
                if (gdb_stop_pending) {
                    /* CPU0 is parked in el2_trap on a bp/step/wp. Run the RSP
                     * command loop against the shared frame; it edits
                     * g_last_guest_frame and picks an action. */
                    gdbstub_on_debug_event(&g_last_guest_frame,
                                           (int)gdb_stop_signal);
                    gdb_resume_act = 1;    /* any non-0xffffffff resumes CPU0   */
                    __asm__ volatile("dsb sy\n\tsev" ::: "memory"); /* wake CPU0 */
                } else {
                    /* Guest running: watch for an async $cmd / Ctrl-C. Bounded,
                     * non-blocking — returns immediately if nothing pending. */
                    gdbstub_poll(&g_last_guest_frame);
                }
            } else if (!dbg_isolate_no_emac) {
                dbgmon_service(&g_last_guest_frame);   /* text dbgmon as before */
            }

            if (dbg_usbacm)
                usbacm_poll();
            /* ... breadcrumb counters ... */
        }
```

Add the latch + include near the top of `smp.c`:

```c
#include "gdbstub.h"
extern int gdbstub_attached(void);
volatile uint32_t gdb_channel;      /* 0 = text dbgmon, 1 = binary RSP */
```

> **Caveat (design §4/§6)**: `g_last_guest_frame` is a *snapshot*, and `AT S1E1R`
> in `gdbstub.c::resolve()` runs on **CPU1** — it translates through CPU1's
> TTBR, not the guest's on CPU0. Consequences:
> * `m`/`M` on **guest VAs** need the guest translation. Two fixes, pick one:
>   (a) have CPU1 load the guest's `TTBR0_EL1/TTBR1_EL1/TCR_EL1` (snapshot them
>   into `g_last_guest_frame` at trap time) around the `AT`, or (b) do the m/M
>   translation on CPU0's trap path. Physical-address `m`/`M` (EL2 kernel, guest
>   phys) work as-is.
> * Register writes (`G`/`P`) land in `g_last_guest_frame`; the §4 handshake
>   copies them back into CPU0's real frame **only on a stop** (bp/step/wp). A
>   `G` issued while the guest is *free-running* (no stop pending) will not take
>   effect until the next stop — acceptable, since GDB writes registers while
>   stopped.

---

## 6. dbgmon.c — the `gdb` hand-off command

Give the operator (and `gdb-bridge.py --arm`) a way to flip the shared 0x88B5
channel from text dbgmon to binary RSP at runtime. In `dbgmon.c`'s
`exec_line()` command chain (near `cmd_bp`/`cmd_help`), add:

```c
    if (streq(cmd, "gdb")) {
        extern volatile uint32_t gdb_channel;
        cputs("entering gdb RSP mode (no more prompts)"); newline();
        console_flush();
        gdb_channel = 1;          /* CPU1 loop now routes to gdbstub_poll() */
        return;                   /* NB: emit nothing further on this channel */
    }
```

After this, dbgmon prints no `dbg> ` prompt (the CPU1 loop no longer calls
`dbgmon_service`), so nothing corrupts the RSP byte stream. `D`/`k` from GDB
(handled in `gdbstub.c::dispatch`) should clear the latch to return to text mode:
in `gdbstub.c`'s `D`/`k` cases you may optionally add `gdb_channel = 0;` (this is
the one place a later edit to `gdbstub.c` is worthwhile; left to the applier).

---

## 7. gdbstub.c — wire the HW bp/wp lane into dispatch (the one gdbstub.c edit)

The design task did not edit `gdbstub.c`. When you apply integration, replace the
`Z`/`z` cases' "hw unsupported → empty" arms with calls into `gdbstub_hw.c`, and
enrich the stop reply + `qSupported`. Exact diffs:

**7a. Include** at the top of `gdbstub.c`:

```c
#include "gdbstub_hw.h"
```

**7b. `Z` case** (replace the `else { gdb_send_empty(); }` hw arm):

```c
    case 'Z': {
        int type = pkt[1] - '0';
        p = pkt + 2; if (*p == ',') p++;
        {
            uint64_t addr = parse_num(&p);
            int kind = 0;
            if (*p == ',') { p++; kind = (int)parse_num(&p); }
            if (type == 0) {                       /* SW bp (existing) */
                if (bp_insert(addr)) gdb_send_ok(); else gdb_send("E01");
            } else {                               /* HW bp / watchpoints */
                int r = gdbstub_hw_insert(type, addr, kind, g);
                if      (r == 1)  gdb_send_ok();
                else if (r == -1) gdb_send("E01"); /* refused: CPSR.D=1 */
                else              gdb_send_empty(); /* full -> GDB uses SW bp */
            }
        }
        return GDB_RUN_NONE;
    }
```

**7c. `z` case** (mirror):

```c
    case 'z': {
        int type = pkt[1] - '0';
        p = pkt + 2; if (*p == ',') p++;
        {
            uint64_t addr = parse_num(&p);
            if (type == 0) bp_remove(addr);
            else           gdbstub_hw_remove(type, addr);
            gdb_send_ok();
        }
        return GDB_RUN_NONE;
    }
```

**7d. Stop reply** — let a HW bp/wp name itself. In `gdbstub_on_debug_event()`
(or wherever `send_stop` is called for a debug event), prefer the rich reason:

```c
void gdbstub_on_debug_event(struct el2_frame *guest, int signal)
{
    char reason[48];
    disarm_step(guest);
    if (signal == 5 && gdbstub_hw_stop_reason(guest, reason)) {
        char s[64]; int n = 0;
        s[n++]='T'; s[n++]=nyb((unsigned)signal>>4); s[n++]=nyb(signal);
        { const char *r=reason; while(*r) s[n++]=*r++; }
        s[n++]='t';s[n++]='h';s[n++]='r';s[n++]='e';s[n++]='a';s[n++]='d';
        s[n++]=':';s[n++]='0';s[n++]='1';s[n++]=';'; s[n]='\0';
        gdb_send(s);
    } else {
        send_stop(signal);
    }
    command_loop(guest);
}
```

**7e. `qSupported`** — advertise the stop-reason keys so GDB believes the
`hwbreak:`/`watch:` fragments:

```c
    if (str_n_eq(pkt, "qSupported", 10)) {
        gdb_send("PacketSize=1024;qXfer:features:read+;swbreak+;hwbreak+");
    }
```

**7f. Watchdog spin-pet (mandatory for live sessions, design §6/§10).** If you
host the stub on CPU1 (this build), CPU1's loop already pets the WDOG *between*
command loops, but the RSP `command_loop()` itself spins with the guest stopped.
Pet the WDOG inside that spin so a long dwell can't reset the board. In
`gdbstub.c::command_loop_ex()`'s `for(;;)`:

```c
        for (;;) {
            extern void wdt_debug_kick(void);   /* CPU1-owned HW WDOG */
            wdt_debug_kick();                   /* keep the board alive while
                                                 * gdb dwells at a breakpoint  */
            n = first ? gdb_recv_body() : gdb_recv();
            /* ... unchanged ... */
```

(On a hypothetical CPU0 tick-path build instead, use `wdt_pet()` here.)

---

## 8. Apply / test order

1. `make dbg` with the Makefile edit (§1) — confirms `gdbstub.o` + `gdbstub_hw.o`
   compile and link into `microkernel-dbg.elf`.
2. Flash / load as usual (the session's chimpd/kload path). Board comes up in
   text dbgmon mode as today.
3. Host: `sudo ./gdb-bridge.py --arm` (sends the `gdb` line, then relays).
4. `aarch64-none-elf-gdb microkernel-dbg.elf` → `target remote :1234`.
5. Smoke test in order of increasing PSTATE.D dependence:
   `info registers` (g) → `x/8xb $sp` (m) → `break *<addr>` + `continue` (Z0, works
   even early) → after a Z0 stop past `cninit`: `hbreak` (Z1), `watch` (Z2).
6. Verify a breakpoint held for >20 s does **not** reset the board (confirms the
   §7f spin-pet and CPU1 `wdt_debug_kick` liveness).

Nothing above is applied by the design task; these are the exact later edits.
