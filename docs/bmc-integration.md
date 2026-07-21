# bzdOS software-BMC — integration guide (edits to apply LATER)

These are the **exact** edits to wire `bmc.c`/`bmc.h` into the existing tree.
**Nothing here has been applied** — this file is the change list to hand to the
build/integration step. All edits are additive; no existing behavior changes
unless the `bmc` command word is used.

Touch points: `Makefile` (build `bmc.o` into the dbg image), `dbgmon.c` (one
dispatch line), `main_dbg.c` (one init call). Optional: `chimpd.py` migration.

---

## 1. `Makefile` — add `bmc.o` to the debug image

`bmc.c` compiles under the generic `%.o: %.c` rule already present. Add the
object to **`DBG_OBJS`** (the debug/BMC image is where dbgmon + the CPU1 debug
core live). Change:

```make
DBG_OBJS := start.o main_dbg.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
            gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
            emac.o dbgmon.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o
```

to (append `bmc.o` on the `dbgmon.o` line):

```make
DBG_OBJS := start.o main_dbg.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
            gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
            emac.o dbgmon.o bmc.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o
```

(Optional) if the resident REPL image should also carry the BMC, append
`bmc.o` to `REPL_OBJS` the same way. Not required for the dbg workflow.

(Optional housekeeping) add `bmc.o` to the `clean:` rule's `rm -f` list.

**Link note:** `bmc.c` references `reboot_clean` (reboot.o ✓), `wdt_arm`/
`wdt_disarm`/`wdt_debug_hold` (wdt.o ✓), `vconsole_rx_push`/
`vconsole_tx_tee_getc` (vconsole.o ✓), the `dbg_*` flags (smp.o/el2_exc.o ✓),
`g_last_guest_frame` (el2_exc.o ✓) and the `console_*` hooks (main_dbg.o ✓) —
**all already in `DBG_OBJS`**, so no other objects need adding.

---

## 2. `dbgmon.c` — register the `bmc` command word

`bmc.c` is invoked from `dbgmon.c`'s existing `exec_line()` dispatch. Two small
additions.

### 2a. Add the extern near the other module externs (top of `dbgmon.c`, by the
`hwbp_*`/`backtrace_walk` externs ~line 43):

```c
/* bmc.c — software-BMC management-plane verbs. Extern decl only, per this
 * file's self-contained discipline (no bmc.h include). */
extern void bmc_dispatch(char **argv, int argc, struct el2_frame *frame);
```

### 2b. Add ONE branch in `exec_line()`, immediately before the final
`cputs("?\r\n");` fallthrough (~line 937). `tok`/`nt` are already in scope:

```c
	if (streq(cmd, "bmc")) {
		/* Hand tokens AFTER "bmc" to the management-plane dispatcher. tokenize()
		 * caps at MAX_TOKENS (4) so `bmc con inject <one-token>` works; multi-
		 * word inject can raise MAX_TOKENS or use an underscore-joined token. */
		bmc_dispatch(&tok[1], nt - 1, frame);
		return;
	}
```

> **Note on `MAX_TOKENS`:** `dbgmon.c` tokenizes at most 4 tokens, so
> `bmc con inject two words` currently sees only `inject two`. For richer
> inject, bump `#define MAX_TOKENS` (dbgmon.c ~line 154) to e.g. 8, or inject a
> single token and rely on the client to send words one at a time. `bmc.c`'s
> `bmc_con_inject` already re-joins whatever tokens it is handed with spaces, so
> raising `MAX_TOKENS` is the only change needed for full-line inject.

### 2c. (Optional) advertise it in `cmd_help()` — add one line to the dbgmon
help text:

```c
	cputs("  bmc <verb> ...     software-BMC mgmt plane (bmc help)\r\n");
```

---

## 3. `main_dbg.c` — call `bmc_init()` once at startup

`dbgmon_init()` is called at ~line 104 after the console hooks are wired and
`vconsole_init()` (~line 78) has run. Add the extern and the init call right
after `dbgmon_init();`:

```c
extern void bmc_init(void);   /* near the other extern prototypes at top */
```

```c
	dbgmon_init();
	bmc_init();                /* lay down BMC1 breadcrumb + first health record */
```

`bmc_init()` is idempotent and only reads breadcrumbs + writes the `BMC1`
window, so ordering relative to the guest launch is not critical — but placing
it after `vconsole_init()` means the first health snapshot already sees a valid
`UART` ring header.

---

## 4. (Optional) `chimpd.py` — migrate to the BMC health record

`chimpd.py` currently scrapes liveness from **SRAM** breadcrumbs
`0x00018200` (GICT) / `0x00018100` (EXC), which are **wiped on a warm WDT
reset** and disagree with the **DRAM** copies (`0x50000800` / `0x50000400`)
that `dbgmon.c`/`el2_exc.c` and the BMC use. Recommended (non-urgent) change:
replace those ad-hoc reads with a single BMC health poll.

Sketch (host side, uses `bmc_client.py`'s `BMC`):

```python
from bmc_client import BMC
b = BMC(iface="br0")
h = b.health_raw()                      # decoded struct bmc_health, or None
if h is None:
    verdict = "NO_EMAC"                  # board down / link lost
elif h["tick_delta"] == 0:
    verdict = "HANG"                     # timer not advancing
elif h["cons_bytes"] == prev_cons_bytes:
    verdict = "BOOT_STALL"              # timer live but no console progress
# reset path unchanged: b.reset()  (auto-arms + reboot_clean)
```

This collapses chimpd's GICT/EXC/VCON multi-read + `is_alive` ping into one
call, and fixes the SRAM-vs-DRAM address mismatch. `chimpd.py`'s serial-load
(ttyACM catch/loady/bootelf) phase is unaffected — only the monitor+reset phase
changes.

---

## 5. Build & smoke-check (to run LATER, on the board owner's turn)

```sh
make dbg                      # builds microkernel-dbg.elf with bmc.o linked
aarch64-linux-gnu-nm microkernel-dbg.elf | grep -E 'bmc_(init|dispatch|health)'
# then, over EMAC:
./bmc_client.py health
./bmc_client.py flags
./bmc_client.py health --raw   # verify BMC1 magic 0x424d4331 @ 0x50006000
```

No `make` is run as part of this design task.
```
