# Snapshot/restore — integration snippets (APPLY LATER)

These are the exact edits to wire `snapshot.c` into the existing tree. **Do not
apply as part of the design task** — this file is the recipe. Each snippet cites
the file and the anchor to edit. Nothing here modifies guest semantics until a
`snap`/`rest` command is actually typed on the dbgmon console.

---

## 1. `Makefile` — build `snapshot.o` into the debugger build

`snapshot` needs the trap frame and runs on the dbgmon/CPU1 debug core, so it
belongs in **`DBG_OBJS`** (the build that already has `dbgmon.o`, `stage2.o`,
`emmc_bio.o`). Line ~99:

```make
# BEFORE
DBG_OBJS := start.o main_dbg.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
            gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
            emac.o dbgmon.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o

# AFTER  (append snapshot.o)
DBG_OBJS := start.o main_dbg.o exceptions.o el2_exc.o kload.o stage2.o guest.o \
            gic_timer.o sched.o timer.o wdt.o libmin.o vconsole.o gtrace.o \
            emac.o dbgmon.o reboot.o hwbp.o backtrace.o smp.o firstfault.o onebp.o vgic.o \
            musb.o usbacm.o emmc_bio.o snapshot.o
```

And add `snapshot.o` to the `clean` target's `rm -f` list (line ~126) for
hygiene. The generic `%.o: %.c` rule (line ~119) already covers compilation, so
no per-object rule is needed.

---

## 2. `dbgmon.c` — add `snap` / `rest` / `snapi` commands

`snapshot_save`/`snapshot_restore` take the live guest frame, and `dbgmon`'s
command handler already receives `struct el2_frame *frame` (used by `cmd_gr`,
`cmd_bp`, `ss`, ...). Add the header include near the top with the other module
headers:

```c
#include "snapshot.h"
```

Add the three command branches in the `streq(cmd, ...)` dispatch chain (the
block around `dbgmon.c:803-934`), e.g. right after the `ss` branch:

```c
if (streq(cmd, "snap")) {
    if (!frame) { err("no guest frame (run while guest is trapped)"); return; }
    int r = snapshot_save(frame);
    cputs("snapshot_save -> "); print_hex64((uint64_t)(long)r);
    cputs(r == 0 ? "  OK (guest resumes)\r\n" : "  FAIL\r\n");
    return;
}
if (streq(cmd, "rest")) {
    if (!frame) { err("no guest frame"); return; }
    if (!snapshot_present()) { err("no valid snapshot in store"); return; }
    int r = snapshot_restore(frame);   /* rewrites *frame; eret lands in guest */
    cputs("snapshot_restore -> "); print_hex64((uint64_t)(long)r); newline();
    return;   /* dbgmon returns -> el2_common erets into the restored guest */
}
if (streq(cmd, "snapi")) {             /* info: is a snapshot present? */
    cputs("snapshot_present="); print_hex32((uint32_t)snapshot_present());
    cputs(" store@0x80000000\r\n");
    return;
}
```

Add one line to `cmd_help()` (`dbgmon.c:634`) so the commands are discoverable:

```c
cputs("snap  - checkpoint guest (frame+sysregs+DRAM) to store\r\n");
cputs("rest  - restore guest from snapshot (erets into it)\r\n");
cputs("snapi - is a snapshot present?\r\n");
```

> **Note on `rest`:** because `dbgmon_service(frame)` is called from the
> `el2_trap` tick path and `snapshot_restore` rewrites `*frame` in place,
> returning normally from the command handler lets the existing `el2_common`
> restore-and-`eret` path carry the CPU into the restored guest. No new `eret`
> and no `el2_exc.c` change is required for the common case.

---

## 3. `el2_exc.c` — (OPTIONAL) auto-checkpoint hook

No change is **required** — the `snap` command drives everything from the
existing `dbgmon_service(frame)` call. If a future automatic checkpoint is
wanted (e.g. "snapshot the first time the guest reaches a target PC"), the hook
point is the guest-sync dispatch in `el2_trap` (`el2_exc.c:282`), gated so it
runs at most once:

```c
/* OPTIONAL auto-checkpoint: snapshot when the guest first hits a target PC
 * (e.g. the mountroot prompt's known ELR). Off unless snap_auto_pc is set. */
extern volatile uint64_t snap_auto_pc;   /* 0 = disabled */
if ((kind >> 2) == 2u && snap_auto_pc && frame->elr == snap_auto_pc) {
    snapshot_save(frame);
    snap_auto_pc = 0;                    /* one-shot */
}
```

This mirrors the existing one-shot patterns (`onebp`, `firstfault`) and needs
`snapshot.h` included in `el2_exc.c`. Leave it out for v1 — the manual `snap`
command is sufficient and avoids adding cost to the hot trap path.

---

## 4. Host side (`hvdbg.py` / `dbgmon` client) — optional convenience

No protocol change: `snap`/`rest`/`snapi` are ordinary dbgmon line commands,
already reachable over the EMAC/ACM console. Optionally add thin wrappers to
`hvdbg.py` next to the existing command helpers:

```python
def snap(self):  return self.cmd("snap")
def rest(self):  return self.cmd("rest")
def snapi(self): return self.cmd("snapi")
```

For the **eMMC** or **EMAC-stream** store back-ends (design §4), the host would
additionally pull/push the DRAM image via the existing `rd`/`wr` bulk memory
path — but the default DRAM back-end needs nothing host-side beyond the three
commands.

---

## 5. Sanity checklist before first board run (later)

- [ ] Confirm the **2 GiB** board variant (store lives at `0x80000000`); on a
      1 GiB board switch `SNAP_STORE_KIND` to eMMC/EMAC first.
- [ ] Verify `SNAP_DRAM_BASE/SIZE` still equal `stage2.h`'s
      `STAGE2_DRAM_BASE/SIZE` (add a `_Static_assert` if `stage2.h` is included).
- [ ] Take the first `snap` at `mountroot>` (quiescent I/O — respects the
      device-state risk in design §7.4).
- [ ] After `rest`, watch the vconsole ring (`0x50000f10`) for the guest
      resuming from the snapshot PC, and confirm no timer-IRQ storm (design §7.3
      re-base working).
