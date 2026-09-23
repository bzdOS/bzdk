# internal-note: EMAC-dark + block_reset 0x27 — forensic findings

**Reported:** 2026-09-01. **Status:** hv iron-side forensics (internal-note), facts for chimp-hal code patch.

## Symptom

Board alive (USB gadget `1d6b:0010` enumerated on workstation, 4 cores moving, 44–45 °C) but EMAC dark for ~17 h (2026-08-31 22:38 → 2026-09-01 15:59). Remote reset blocked. Recovery: WDOG fired after petting stopped; fresh boot won PHY lottery; EMAC + guest network up.

## Configuration at time of dark window

```
flags = 0x27 = usbacm(1) | core_enable(1) | cpu1_wdog(1) | block_reset(1)
```

- `block_reset` (bit 5, `BMC_FLAG_BLOCK_RESET` in `bmc.h:132`) = **SET**
- `dbg_block_reset` volatile in `el2_exc.c:225`, init value 1
- `block_reset` gates remote reset in `el2_exc.c:1212` and `main_dbg.c:480`

## Forensic data (live reads 2026-09-02 03:01 UTC)

### BZDBG pipeline breadcrumbs @0x5009E4C0

```
0x5009e4c0: 00000007 00000007 00000007 00000003
0x5009e4d0: ffffffff × 12
```

| Word | Value | Meaning |
|------|-------|---------|
| [0] | 0x7 | lines posted |
| [1] | 0x7 | lines executed |
| [2] | 0x7 | lines replied |
| [3] | 0x3 | phase (3 = monitor poll) |

7 dbgmon operations were driven over USB-ACM (`~BZDBG<line>`, usbacm.c) during the dark boot. All 7 completed (posted == executed == replied). Phase=3 = monitor poll was the last activity, consistent with EXC breadcrumb.

### EXC breadcrumb @0x50000A00

```
word[0] = 0x00000003          (phase=3, monitor poll)
word[1] = 0x78302072          (ASCII: "r 0x")
word[2..7] = 0x0
```

Last dbgmon command before the dark window: `r 0x...` — a register read, not a reset attempt. This is consistent with a monitor poll being the last activity, not a mid-command wedge.

### GICT breadcrumb @0x50000800

```
all 0xffffffff
```

GICT timer breadcrumb not written by this build (retired — see `bmc.c BMC_GICT_BASE` comment). This is expected, not a sign of failure.

### BMC1 health snapshot (live, after EMAC recovery)

```
uptime      ~32665 s (~9 h at time of read, board rebooted at 15:59)
exc_count   107422    (historical, may predate this boot)
last_kind   0x8       (stage-2 fault traffic)
last_esr    0xf20007d0
flags       0x27      (usbacm + core_enable + cpu1_wdog + block_reset)
wdt_hold    0
cores       0xf       (all 4 online)
heartbeat   [3863800 5688795 3812842 472209]
```

`block_reset` is STILL SET on the recovered board. The WDOG reset did NOT clear it — `dbg_block_reset` init value is 1 (`el2_exc.c:225`), so every boot starts with block_reset ON.

### USB gadget persistence (workstation kernel journal)

- 2026-08-31 22:00:12 — HV gadget `1d6b:0010` (dev 24) → ttyACM0, stable
- 2026-09-01 15:59:35 — petting stopped, WDOG fired
- **17 h with zero USB disconnects** — proves WDOG was being petted the whole time

## Root cause: block_reset + EMAC dark = no recovery path

### The trap

1. EMAC goes dark during SDIO WiFi bring-up (reason TBD — PHY crash, clock gate, or pinmux conflict)
2. `block_reset` is SET (init=1, never cleared) → all remote reset commands (`bmc reset`, watchdog arm/release) are blocked at `el2_exc.c:1212`
3. The only recovery path is hardware WDOG (unconditional, bypasses block_reset)
4. But CPU1 is petting the WDOG via `wdt_debug_kick()` → WDOG never fires
5. Board stays alive but dark indefinitely

### Why block_reset was designed this way

`el2_exc.c:225-265`: block_reset was added after the 2026-07-18 failure where a stray dbgmon reset frame rebooted the board during live debugging. The gate prevents accidental remote resets. But the init value of 1 means every boot starts in "blocked" state, and nothing clears it automatically.

### Why the board eventually recovered

At 15:59:35, petting stopped for unknown reasons (possibly a guest hang on CPU1 triggered by the WiFi SDIO state, or a transient CPU1 fault). After ~16 s, WDOG fired. Fresh boot won PHY lottery. EMAC came up. block_reset remained SET (init=1) but EMAC being alive meant the board was reachable again.

## Patch proposal

### A. Clear block_reset after successful boot (conservative)

After the guest boots and EMAC is confirmed alive, clear block_reset so remote recovery works. This is the minimal fix.

```diff
--- a/bmc.c
+++ b/bmc.c
@@ -xxx,6 +xxx,12 @@
+ * After guest boot completes and EMAC is confirmed alive, clear block_reset
+ * so remote recovery is possible if EMAC goes dark again.  The gate protected
+ * against stray resets during live debugging; once the system is up, the risk
+ * shifts to "board alive but unreachable" (internal-note).
+ */
+void bmc_clear_block_reset_after_boot(void) {
+    extern volatile uint32_t dbg_block_reset;
+    dbg_block_reset = 0;
+}
```

Call this from the boot completion path (e.g., after first successful `bmc health` response, or from `vnet_emac.c` after first TX).

### B. Timeout block_reset (structural)

Make block_reset time-limited: set a timestamp when it's armed, clear after N seconds. This prevents both "stuck blocked" and "accidentally re-armed" scenarios.

```diff
--- a/el2_exc.c
+++ b/el2_exc.c
@@ -225,7 +225,12 @@
-volatile uint32_t dbg_block_reset = 1;
+volatile uint32_t dbg_block_reset = 1;
+volatile uint64_t dbg_block_reset_arm_time;  /* set when armed */
+#define BLOCK_RESET_TIMEOUT_S  300  /* 5 minutes */
```

In the reset gate check:
```c
if (dbg_block_reset) {
    uint64_t now = ...; /* read timer */
    if (now - dbg_block_reset_arm_time > BLOCK_RESET_TIMEOUT_S * TICKS_PER_S)
        dbg_block_reset = 0;  /* expired, allow reset */
    else
        return "blocked";     /* still within window */
}
```

### C. Do NOT change init value

Changing `dbg_block_reset = 1` to `0` would remove protection against stray resets during early boot. The gate exists for a real reason (2026-07-18 incident). Options A or B preserve the protection while fixing the stuck state.

## Files involved

| File | Role |
|------|------|
| `el2_exc.c:225` | `dbg_block_reset` declaration and gate check |
| `el2_exc.c:1212` | Gate enforcement point |
| `bmc.h:132` | `BMC_FLAG_BLOCK_RESET` bit definition |
| `bmc.c:63,292` | flag read + reporting |
| `bmc.c:501` | flag set handler |
| `main_dbg.c:480` | Gate blocks reset, returns "fake success" |

## Open questions for chimp-hal

1. **What clears block_reset today?** Only `bmc flag block_reset 0` via remote command — but if EMAC is dark, you can't send that command. Is there any internal timeout?
2. **Why did petting stop at 15:59:35?** The USB gadget stayed enumerated until then. Was it a CPU1 fault, a guest hang, or something else? The WDOG timeout is ~16 s, and petting stopped ~16 s before the reset.
3. **What caused EMAC to go dark at ~22:38?** The board was doing WiFi SDIO bring-up via SSH. Was it a PHY crash, clock gate, or pinmux conflict with SMHC1/EMAC shared pins?

## Verification (board needed)

- Confirm block_reset clears after boot (patch A)
- Confirm block_reset timeout works (patch B)
- Reproduce EMAC dark with block_reset=0 → verify remote reset succeeds
- Measure WDOG petting interval to confirm 16 s timeout
