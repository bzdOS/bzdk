/* SPDX-License-Identifier: BSD-2-Clause */

/* bmc.h — "software BMC": a BMC/IPMI-style out-of-band MANAGEMENT PLANE for
 * the bzdOS EL2 hypervisor on the Banana Pi M64 (Allwinner A64).
 *
 * WHY THIS EXISTS
 * ---------------
 * The board has NO UART. The only out-of-band channels to a running (or
 * wedged) board are:
 *   - EMAC raw-Ethernet debug console (ethertype 0x88B5, board MAC
 *     02:bd:05:00:00:01) serviced by dbgmon.c on the CPU1 debug core, and
 *   - the USB-OTG CDC-ACM gadget (usbacm.c), also on CPU1.
 * Over time a pile of ad-hoc management capability accreted across dbgmon.c
 * (memory/register/breakpoint verbs), reboot.c (reboot_clean), wdt.c
 * (wdt_debug_hold / wdt_arm / wdt_disarm), smp.c (the dbg_* runtime flags),
 * vconsole.c (console capture ring + RX-inject ring + TX tee) and a scatter of
 * fixed DRAM breadcrumb windows (EXC/GICT/SMP1/FFL1/FF1V/UART...). Each is
 * reachable, but only by someone who already knows the exact address, word
 * offset and magic — there is no NAMED, versioned, self-describing contract.
 *
 * bmc.c/bmc.h consolidate all of that into ONE coherent management interface:
 * a versioned verb catalog grouped by domain (power/reset, console, debug
 * flags, health, memory, breakpoints, fault/observability), a single dispatch
 * entry point, a structured health record, and an explicit safety gate for
 * destructive verbs. It is PURELY a thin, well-labelled façade: every handler
 * calls an EXISTING primitive (reboot_clean(), vconsole_rx_push(), the dbg_*
 * flags, the EXC/GICT/SMP1 breadcrumbs, ...). It adds no new device drivers
 * except an optional A64 thermal-sensor read for a temperature health metric.
 *
 * TRANSPORT
 * ---------
 * bmc.c does NOT own a transport. It is invoked from dbgmon.c's existing
 * command dispatch (one added line — see docs/bmc-integration.md) as the
 * handler for the `bmc` command word, so every BMC verb rides the SAME
 * non-blocking, line-oriented EMAC/USB-ACM console dbgmon already services on
 * CPU1. Framing, MAC filtering and the "dbg> " prompt round-trip are unchanged.
 * The verb reply is plain text (like every other dbgmon command) AND, for
 * `bmc health`, is also latched into a fixed breadcrumb (BMC_HEALTH_BASE) so a
 * host can read the same record with a raw `r <addr> <n>` even mid-wedge.
 *
 * FREESTANDING: <stdint.h> + exceptions.h (struct el2_frame) only, no libc,
 * -mgeneral-regs-only. Runs in tick/poll context on the CPU1 debug core with
 * the guest preempted — every handler is non-blocking and bounded, exactly
 * like dbgmon_service(). Console output uses the same extern console_* hooks.
 */
#ifndef BZDOS_BMC_H
#define BZDOS_BMC_H

#include <stdint.h>
#include "exceptions.h"

/* -------------------------------------------------------------------------
 * Protocol version. Bump MINOR when adding verbs (back-compatible), MAJOR
 * when changing an existing verb's request/reply shape (breaking). The host
 * client (bmc_client.py) queries this via `bmc ver` and refuses to talk to a
 * MAJOR it does not understand.
 * ------------------------------------------------------------------------- */
#define BMC_PROTO_MAJOR   1u
#define BMC_PROTO_MINOR   1u   /* +AXP803 battery telemetry (back-compat: */
                               /* fills what was reserved[23..28], see below) */

/* -------------------------------------------------------------------------
 * Structured health / status record ("BMC1"). Written by bmc_health_snapshot()
 * both to the console (as decoded text) and to a cache-coherent DRAM
 * breadcrumb at BMC_HEALTH_BASE, so it survives a warm WDT reset and is
 * readable over EMAC with a plain physical-memory read even when the guest is
 * wedged. Layout mirrors the rest of the tree's breadcrumb convention:
 * word[0] = magic, and every wide field is stored lo,hi (little-endian pair).
 *
 * Chosen at 0x50006000: clear of every existing window (MUSB 0x50000000 ..
 * FF1V 0x50005800..0x50005930, TX-tee 0x50004000, GTRC 0x50002000). 64 words
 * reserved.
 * ------------------------------------------------------------------------- */
#define BMC_HEALTH_BASE   0x50006000UL
#define BMC_HEALTH_MAGIC  0x424D4331u   /* "BMC1" */

/* Packed, fixed-width so the host can also parse it straight out of a raw word
 * read (see bmc_client.py health --raw). All multi-word fields little-endian.
 * struct field order == breadcrumb word order for a trivial 1:1 mapping. */
struct bmc_health {
	uint32_t magic;          /* [0]  BMC_HEALTH_MAGIC                       */
	uint32_t version;        /* [1]  (MAJOR<<16)|MINOR                      */
	uint32_t uptime_lo;      /* [2]  CNTPCT_EL0 low  (÷CNTFRQ = seconds)    */
	uint32_t uptime_hi;      /* [3]  CNTPCT_EL0 high                        */
	uint32_t tick_lo;        /* [4]  RETIRED: always 0xffffffff (see bmc.c  */
	uint32_t tick_hi;        /* [5]  BMC_GICT_BASE comment -- the address   */
	uint32_t tick_delta;     /* [6]  this used to read is aliased with a    */
	                         /*      live backtrace.c ring, not tick data). */
	                         /*      Use hb_cpu0/hb_cpu1 for liveness.      */
	uint32_t exc_count;      /* [7]  EXC1 total EL2 exceptions seen; 0 means*/
	                         /*      "none recorded" (genuinely zero, or    */
	                         /*      EXC1 never written -- both are safe to */
	                         /*      collapse: see bmc.c bmc_health_snapshot)*/
	uint32_t last_exc_kind;  /* [8]  EXC1 last vector index; meaningless    */
	                         /*      when exc_count==0 (0 is EL2_KIND_SYNC, */
	                         /*      a real value -- don't display it then)*/
	uint32_t last_exc_esr;   /* [9]  EXC1 last ESR (low 32); ditto          */
	uint32_t guest_pc_lo;    /* [10] g_last_guest_frame ELR low (guest PC)  */
	uint32_t guest_pc_hi;    /* [11] g_last_guest_frame ELR high            */
	uint32_t online_map;     /* [12] SMP1 per-core online bitmap            */
	uint32_t hb_cpu0;        /* [13] SMP1 heartbeat, core 0                 */
	uint32_t hb_cpu1;        /* [14] SMP1 heartbeat, core 1 (debug core)    */
	uint32_t hb_cpu2;        /* [15] SMP1 heartbeat, core 2                 */
	uint32_t hb_cpu3;        /* [16] SMP1 heartbeat, core 3                 */
	uint32_t cons_bytes;     /* [17] vconsole UART capture total_bytes      */
	uint32_t cons_faults;    /* [18] vconsole stage-2 fault_count           */
	uint32_t temp_mc;        /* [19] SoC temperature, milli-°C (0 = n/a)    */
	uint32_t flags;          /* [20] snapshot of the dbg_* flag bitmap      */
	uint32_t wdt_hold;       /* [21] wdt_debug_hold (1 = reset gate armed)  */
	uint32_t ffv_count;      /* [22] FF1V vector first-fault hit count      */
	/* --- v1.1: AXP803 battery telemetry (see axp803.h for citations/
	 * confidence). Fills what was reserved[23..28] in v1.0 — additive,
	 * back-compatible; a v1.0 client just never reads these words. All 0
	 * if no AXP803 was confirmed present (batt_status bit BMC_BATT_CHIP_OK
	 * clear). */
	uint32_t vbat_mv;        /* [23] battery voltage, mV (0 = n/a)          */
	uint32_t ichg_ma;        /* [24] charge current, mA                     */
	uint32_t idischg_ma;     /* [25] discharge current, mA                  */
	uint32_t batt_ts_mv;     /* [26] TS-pin raw mV (NOT calibrated to °C)   */
	uint32_t batt_status;    /* [27] BMC_BATT_* bitmap (axp803.h)           */
	uint32_t axp_ok;         /* [28] 1 = AXP803 detected + REG03H verified  */
	uint32_t reserved[3];    /* [29..31] padding to a clean 32-word record  */
};

/* Bit assignments for the `flags` word above and for `bmc flags`. Each maps to
 * one live `volatile uint32_t` toggle owned elsewhere (smp.c / el2_exc.c). */
#define BMC_FLAG_USBACM         (1u << 0)  /* smp.c   dbg_usbacm            */
#define BMC_FLAG_CORE_ENABLE    (1u << 1)  /* smp.c   dbg_core_enable       */
#define BMC_FLAG_CPU1_WDOG      (1u << 2)  /* smp.c   dbg_cpu1_wdog         */
#define BMC_FLAG_ISOLATE_NOEMAC (1u << 3)  /* smp.c   dbg_isolate_no_emac   */
#define BMC_FLAG_NO_GUEST       (1u << 4)  /* smp.c   dbg_no_guest          */
#define BMC_FLAG_BLOCK_RESET    (1u << 5)  /* el2_exc.c dbg_block_reset     */

/* -------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */

/* One-time init: lay down the BMC1 breadcrumb magic + zero the record, and
 * (optionally) kick a first thermal reading. Call once from main_dbg.c after
 * the console hooks and dbgmon_init(). Idempotent. */
void bmc_init(void);

/* Dispatch one already-tokenized BMC verb. Called by dbgmon.c's exec_line when
 * the first token is "bmc": argv[0] is the verb, argv[1..argc-1] its operands,
 * `frame` is the live guest frame for this tick (may be the shared
 * g_last_guest_frame snapshot on the CPU1 debug core). Never blocks, never
 * crashes on bad input — prints an error line and returns. */
void bmc_dispatch(char **argv, int argc, struct el2_frame *frame);

/* Fill *out with a fresh health snapshot AND latch it into the BMC1
 * breadcrumb. Safe to call from any core/context; bounded. Returns *out for
 * convenience. Exposed so a supervisor could also invoke it via `call`. */
struct bmc_health *bmc_health_snapshot(struct bmc_health *out);

#endif /* BZDOS_BMC_H */
