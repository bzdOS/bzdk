/* SPDX-License-Identifier: BSD-2-Clause */

/* panic.h — persistent (warm-reset-surviving) panic log for the bzdOS EL2
 * microkernel.
 *
 * On a genuinely fatal / unhandled EL2 fault, panic_record() snapshots the
 * fault state (ESR/ELR/FAR/SP/SPSR + all 31 GPRs + a short reason string + a
 * compact frame-pointer backtrace) into a fixed DRAM window and cache-cleans
 * it (dc civac + dsb sy, the same trick the exception breadcrumb uses) so the
 * record survives a WDT / warm reset with the D-cache on.
 *
 * On the NEXT boot, panic_dump_if_present() (called early from a main) bumps a
 * persistent boot counter, notices that the stored record came from a PREVIOUS
 * boot, prints it over the network/console + HUD, and marks it consumed. This
 * turns "the board silently died and reset" into "the board reboots and tells
 * you exactly where it died last time".
 *
 * DRAM window: 0x50005000 (magic "PANC" = 0x50414E43). 1 KiB reserved
 * (0x50005000..0x50005400). Chosen to avoid every breadcrumb listed in
 * PROJECT/agent notes (musb/emac/repl/exc/hwbp/btr/dbg/vconsole/vgic/gtrace/
 * ffl1/sst1/hdmi/trace/prof).
 */
#ifndef BZDOS_PANIC_H
#define BZDOS_PANIC_H

#include <stdint.h>

struct el2_frame;   /* exceptions.h */

#define PANIC_BASE      0x50005000UL
#define PANIC_MAGIC     0x50414E43u   /* "PANC" (LE bytes P,A,N,C) */
#define PANIC_REASON_MAX 48u
#define PANIC_BT_MAX     16u

/* On-DRAM record. Fixed layout (written field-by-field, then cache-cleaned).
 * Keep POD / no implicit padding surprises — all fields are naturally aligned
 * and the trailing arrays are word/byte aligned. */
struct panic_record {
	uint32_t magic;        /* PANIC_MAGIC once the region is initialized      */
	uint32_t boot_seq;     /* bumped once per boot by panic_dump_if_present() */
	uint32_t valid;        /* 1 = an unconsumed panic is stored               */
	uint32_t panic_seq;    /* boot_seq value at the time the panic was taken  */

	uint64_t esr;
	uint64_t elr;
	uint64_t far;
	uint64_t sp;
	uint64_t spsr;
	uint64_t kind;         /* EL2_KIND_* + group<<2 (as el2_trap sees it)     */

	uint64_t x[31];        /* x0..x30 */

	uint32_t bt_count;
	uint32_t _pad;
	uint64_t bt[PANIC_BT_MAX];

	char     reason[PANIC_REASON_MAX];
};

/* Snapshot `frame` + `reason` into the persistent region and cache-clean it so
 * it survives a warm reset. Safe from the fault path (bounded, no allocation,
 * never faults on its own state). `reason` may be NULL. */
void panic_record(struct el2_frame *frame, const char *reason);

/* Called EARLY from a main (after the network/console is up so it can print).
 * Bumps the persistent boot counter; if a valid record from a PREVIOUS boot is
 * present, dumps it (network + console + HUD) and marks it consumed. Idempotent
 * within a boot (only bumps / dumps on the first call). */
void panic_dump_if_present(void);

/* 1 if a valid, unconsumed panic record is currently stored, else 0. */
int panic_present(void);

/* Accessor for dbgmon/HUD: read-only view of the stored record (never NULL —
 * points at the DRAM window; check ->magic / panic_present() for validity). */
const volatile struct panic_record *panic_last(void);

#endif /* BZDOS_PANIC_H */
