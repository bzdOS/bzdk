# Test: FreeBSD Boot with All Fixes (2026-07-15)

## Quick Start

```bash
cd /opt/bzdos/microkernel
./loady_over_acm.py microkernel-dbg.bin --log /tmp/dbg-boot-$(date +%s).log
```

**Expected**: Boot console output starting with `EHCI...` (from previous run with 18KB captured).

## What Was Fixed

1. **MODINIOMD tag off-by-one** (kload.h): Metadata records now decode correctly
2. **UART FAR_EL2 IPA reconstruction** (vconsole.c): Console works after guest MMU enables
3. **IIR infinite loop fix** (vconsole.c): UART driver loop unblocked
4. **vgic revert**: Removed timer injection that caused regression

## Test Expectations

### Success Path
- U-Boot `=>` prompt responds (should already be there from reset)
- Script sends microkernel-dbg.bin via YMODEM
- Microkernel boots FreeBSD as guest
- Network console appears (may take 10-15s for enumeration)
- See FreeBSD boot output: device probes, `usbus` enumeration, CPU setup
- **Either:**
  - Guest continues booting successfully (best case)
  - Guest hangs at a known point (provides diagnostics for next iteration)

### Known Issues
- **Potential hang**: Previous run showed guest wedging in `DELAY()` after `usbus3: EHCI version 1.0` output (~20s at that point)
- **DEBUG_RULES R1**: A snapshot of frozen ELR isn't the same as liveness. If appears hung, capture log output and timing.

## Debugging Tools Available

Once network console is up, use the debugger (EMAC, port 0x88B5):

### Read Memory (guest physical)
```
gpa <PA> <bytes>        # Read guest physical memory
d <PA> [count]          # Dump with default size
```

### Read Registers
```
gr                      # Guest registers (live ELR, SPSR, etc.)
sr <SYSREG> <value>     # Set/read sysregs
```

### Hardware Breakpoints
⚠️ **Note**: Early guest boot has `PSTATE.D=1` (debug masked), so HW breakpoints won't fire until after `cninit()`. Once console is up, can set them:
```
ba <PA> <imm>          # Arm breakpoint at PA with HVC immediate
```

### Set Watchpoints / Single-Step
Same limitation (only after debug unmasked by guest).

## Output Locations

- **Stdout**: Live streaming (CSI codes stripped for readability)
- **Log file**: Full raw output with timing, pass via `--log FILE`
- **Hypervisor diagnostics**: Breadcrumbs at `0x500xxxxx` (see el2_exc.c for map)
  - `0x50000e00` = DBG_BC(0-15) boot checkpoints
  - `0x50000f00` = vconsole capture ring (if console touched)
  - `0x50005800` = firstfault ring (early panics)

## What To Do If Hang

1. **Check the log**: Look for last console output before silence
2. **Check guest ELR**: `gr` to see where guest PC froze
3. **Measure time**: How long until hang? (DEBUG_RULES R1)
4. **If DELAY() hang** (like previous run):
   - `sr 0` to read guest `CNTFRQ_EL0` (expected ~24 MHz for this SoC)
   - Check if timer counter advancing: `sr 3` (CNTVCT_EL0) read twice with delay
   - If frozen: timer not working; if advancing: miscalibration

## Abort / Reset

Press `Ctrl-C` to stop the script. Board stays powered. To return to U-Boot:
```bash
# Poke watchdog timer to reset to U-Boot
# (from dbgmon, if network console up; or retry loady script)
wb 0x01c20cb4 1; wb 0x01c20cb8 1; w 0x01c20cb0 0x14af
```

## After Test

Regardless of outcome, save the log and report:
- **Last console output** (copy from log)
- **Boot time** (how long until hang, if any)
- **ELR when hung** (if hung, use `gr` to read guest PC)

For next iteration, this data drives the diagnosis.
