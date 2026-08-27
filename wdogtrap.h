/* SPDX-License-Identifier: BSD-2-Clause */

/* wdogtrap.h — trap-and-police the CCU/PIO/WDOG page (0x01C20000-0x01C20FFF).
 *
 * WHY THIS EXISTS
 *
 * CCU (`ccu@1c20000`, clocks/PLLs), PIO (`pio@1c20800`, pinmux/GPIO) and the
 * A64 hardware watchdog (`watchdog@1c20ca0`, at SOC_A64_WDOG_NODE_BASE inside
 * the `timer@1c20c00` block) all live in the SAME 4 KiB physical page —
 * verified against the live tftpboot DTB (`dtc -I dtb -O dts
 * bananapi-min.dtb`): `clock@1c20000` reg size 0x400, `pinctrl@1c20800` reg
 * size 0x400, `watchdog@1c20ca0` reg size 0x20, all inside
 * 0x01C20000-0x01C20FFF. That page is currently one of the ordinary
 * identity-mapped 4 KiB entries `stage2_build_mmio_tables()` (stage2.c) falls
 * through to — it happens to already live inside `stage2_l3_uart[]`, the same
 * table split for the UART0 and GICD traps (UART_L2_IDX==14 covers both), so
 * it is NOT trapped at all today: a guest that reaches it (any way at all —
 * its own driver, `/dev/mem`, a bug) has full read/write access to the
 * hardware watchdog registers, i.e. to this project's entire automatic-
 * recovery story (SESSION-RULES.md R2). See docs/wdog-ccu-pio-stage2.md for
 * the full analysis this header is the implementation of.
 *
 * THE ONE THING THAT MAKES THIS SAFE TO EVEN CONTEMPLATE: EL2's own accesses
 * to this page (wdt.c, reboot.c, the PC5 pinmux enforcement in main_dbg.c/
 * smp.c/gic_timer.c) are NOT subject to stage 2 at all — VTTBR_EL2/VTCR_EL2
 * govern translations performed by a lower EL (EL1/EL0, i.e. the guest)
 * only, never EL2's own loads/stores (stage2.c's A1 comment states this
 * explicitly and it is the same fact vgicd.c's `gicd_rd`/`gicd_wr` rely on to
 * reach real hardware after the guest's identical access just faulted). So
 * trapping this page for the GUEST cannot, structurally, touch EL2's own
 * watchdog kicks or reboot sequence — there is nothing in EL2's own code path
 * for a stage-2 table edit to break.
 *
 * WHAT THIS DOES: three sub-ranges, not one policy for the whole page --
 *
 *   1. WDOG_NODE (SOC_A64_WDOG_NODE_BASE, 0x20 bytes) — EL2's alone. Guest
 *      WRITES are refused (absorbed as a no-op, exactly the "read-as-
 *      something/write-as-noop" contract mmio_absorb.c already uses
 *      elsewhere in this tree for a fault that must not propagate) and
 *      counted loudly (HVMAP_WDOGTRAP_BC word 4: must stay 0 on a correctly
 *      behaving guest). Guest READS pass through to real hardware, same
 *      reasoning vgicd.c gives for its own read policy: "knowing... is not
 *      the threat here, changing it is" -- a read tells the guest nothing it
 *      can act on once every write is refused.
 *   2. PIO_PC_CFG0 (SOC_A64_PIO_PC_CFG0, one word) — the single register the
 *      PC5/eMMC-clock-pin fight (main_dbg.c, smp.c, gic_timer.c's triplicate
 *      reassertion) is entirely about. Writes pass through with nibble 5
 *      (bits[23:20], the PC5 function field) forced to 0b0011 (func 3/mmc2)
 *      regardless of what the guest asked for; every other nibble in the
 *      word (PC0/1/2/3/4/6/7's function fields) is whatever the guest wrote,
 *      unmodified. This is what makes the three reassertion sites
 *      obsoletable -- see docs/wdog-ccu-pio-stage2.md for why removing them
 *      is NOT bundled with this trap. Reads pass through unmodified.
 *   3. Everything else on the page (the rest of CCU -- PLL/clock-gate
 *      registers the guest's real EHCI/OHCI/MMC drivers genuinely need, per
 *      docs/dma-bypass-stage2.md's own inventory; the rest of PIO -- every
 *      other port's pinmux; `timer@1c20c00`) — PASSTHROUGH, full read/write,
 *      real hardware, SAS-sized exactly like vgicd.c's gicd_rd/gicd_wr. NOT a
 *      "pass through anything unrecognised" catch-all reached by falling off
 *      the end of an if/else chain: the WDOG check runs FIRST and returns
 *      unconditionally on a match (see wdogtrap_handle_fault()'s own
 *      comment) specifically so a later refactor that adds more classified
 *      ranges to this function cannot silently reorder past it and reopen
 *      the WDOG hole this file exists to close.
 *
 * WHAT THIS DOES NOT DO (see docs/wdog-ccu-pio-stage2.md §"What's landed"):
 * the stage-2 table edit that actually TRAPS this page is compiled only
 * under `STAGE2_TRAP_WDOG_PAGE` (stage2.c), default UNDEFINED. Every
 * shipping target links this file's fault handler already (same "always
 * linked, only ever invoked if something upstream actually faults here"
 * shape as vblk_sd_mmio_fault()/scanout_mmio_fault()) and el2_exc.c calls it
 * unconditionally in the guest data-abort dispatch chain, same as every
 * other handler there -- that is NOT byte-identical to the pre-existing
 * binary (one more linked object, one more address-range check per guest
 * MMIO fault, same negligible per-fault cost every earlier addition to that
 * chain already carries), but it IS behaviourally inert: with the page left
 * identity-mapped by default (stage2.c's own #ifdef), the fault this handler
 * exists to service simply cannot occur, so guest-visible MMIO behaviour on
 * this page is unchanged. This page sits on the critical path of eMMC
 * clocking (PC5); per DEBUG_RULES.md this is exactly the class of change
 * that must not be guessed at without hardware validation, so the flag stays
 * off until someone flips it and boots the real board (see the design doc's
 * hardware-validation section).
 */
#ifndef BZDOS_WDOGTRAP_H
#define BZDOS_WDOGTRAP_H

#include <stdint.h>
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated */

struct el2_frame;

/* The whole 4 KiB page CCU/PIO/WDOG share. SOC_A64_CCU_BASE is the page's
 * own base address (0x01C20000, 4 KiB aligned) -- not a coincidence, it's
 * literally where the `ccu@1c20000` DTB node starts. */
#define WDOGTRAP_PAGE_BASE   SOC_A64_CCU_BASE
#define WDOGTRAP_PAGE_SIZE   0x00001000UL

/* Handle a stage-2 fault that landed on the CCU/PIO/WDOG page.
 *
 * Returns 1 if the access was inside this page and has been emulated (ELR
 * already advanced past the instruction), 0 if the fault was not ours --
 * same "not ours, keep looking" contract as vgicd_handle_fault() and
 * vconsole_handle_fault(). Safe to call unconditionally from the fault
 * dispatch chain regardless of whether STAGE2_TRAP_WDOG_PAGE is compiled in:
 * without it, this page stays identity-mapped and the fault this function
 * services never happens, so the address check below simply never matches. */
int wdogtrap_handle_fault(struct el2_frame *frame);

#endif /* BZDOS_WDOGTRAP_H */
