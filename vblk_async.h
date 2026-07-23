/* vblk_async.h — CPU2 bring-up for the ROADMAP C2 milestone: move eMMC
 * block-I/O PIO off the guest's synchronous trap path onto a THIRD core.
 *
 * ============================================================================
 * BACKGROUND / WHY A NEW FILE
 * ============================================================================
 * vblk_emmc.c's QueueNotify handler used to call emmc_bio_read()/write()
 * directly, INLINE, inside the guest's MMIO trap — so a vCPU issuing a disk
 * request blocked for the full PIO transfer (hundreds of microseconds to
 * several milliseconds at the eMMC's 400 kHz-to-~25 MHz controller clock,
 * per block) before the trap could return control to the guest. This
 * milestone moves that PIO work onto CPU2 (previously idle — see smp.c's
 * old "Other secondaries (2,3): unused for now" comment), so the trap can
 * post a small request and return immediately, and the vCPU keeps running
 * while CPU2 does the transfer in the background, injecting a completion
 * IRQ when it's done.
 *
 * This file is ONLY the CPU2 bring-up loop (mirrors smp.c's CPU1
 * dedicated-debug-core style deliberately and closely — see smp.h's
 * SMPEN/coherency notes and smp.c's smp_secondary_main() CPU1 block). The
 * mailbox itself (the producer half in vblk_kick()/vblk_request(), the
 * consumer half in vblk_async_poll()) lives IN vblk_emmc.c, NOT here,
 * because both halves need vblk_emmc.c's already-static helpers
 * (gmem_read/gmem_write/gmem_cmo, the vring push/pop helpers, serve_data(),
 * vblk_inject_irq()) and duplicating those across files would itself be a
 * correctness hazard (two copies of the same cache-maintenance-sensitive
 * guest-memory-access code drifting apart over time). See vblk_emmc.h's
 * "ASYNC I/O OFFLOAD" section for the one function this file calls every
 * loop iteration: vblk_async_poll().
 *
 * ============================================================================
 * THE THIRD ACTOR ON THE eMMC CONTROLLER
 * (read vblk_emmc.h's design §8.1 FIRST — this file does not change that
 * lock's contract, it adds a user of it)
 * ============================================================================
 * The single aw_mmc controller at 0x01c11000 was already driven from TWO
 * cores (vblk_emmc.h design §8.1):
 *   - CPU0: virtio-blk's trap handler — historically its ONLY path, now its
 *     SYNCHRONOUS FALLBACK path (still present, unchanged, for whenever the
 *     mailbox is busy or unavailable — see vblk_async_post()'s contract).
 *   - CPU1: the debug core, via a dbgmon `call emmc_bio_*`.
 * This milestone adds a THIRD:
 *   - CPU2: draining the async mailbox, calling emmc_bio_read()/write() for
 *     the common case (mailbox free, chain fits the single slot).
 * All three serialize through the SAME VBLK_EMMC_LOCK_PA test-and-set word
 * (vblk_emmc_trylock()/vblk_emmc_unlock(), ldaxr/stlxr, plain DRAM rather
 * than .bss so a warm WDT reset can never leave it stuck locked). NOTHING
 * about that lock's discipline changes here — CPU2's serve_data() call
 * takes it with the exact same bounded-spin acquire
 * (emmc_lock_acquire_bounded()) every other caller already uses. There is
 * still only ONE lock guarding the ONE controller; CPU0's fallback path and
 * CPU2's async path can never run a controller transaction concurrently, by
 * construction — the "first cut" scope explicitly does not add a queue of
 * outstanding requests, so at most one of {CPU0 fallback, CPU2 async} is
 * ever mid-transfer at a time in practice, and even if that assumption were
 * ever violated the trylock still makes it SAFE (the loser just retries/
 * bails cleanly), only not necessarily fast.
 *
 * ============================================================================
 * MAILBOX PROTOCOL (single slot, single producer / single consumer)
 * ============================================================================
 * See the "ASYNC I/O OFFLOAD" block comment in vblk_emmc.c (directly above
 * vblk_async_post()/vblk_async_poll()) for the wire format and the exact
 * memory-ordering reasoning (dsb sy bracketing the payload writes before the
 * state flag flips, and again before the slot is freed). Summary:
 *
 *   CPU0 (vblk_request(), inside the guest's QueueNotify trap):
 *     For a T_IN/T_OUT request, if g_vblk_async_ready, the mailbox is EMPTY,
 *     and the chain's data-descriptor count fits the single slot:
 *       write {head, is_read, sector, status_gpa, ndesc, data[]} -> dsb sy
 *       -> state = POSTED -> dsb sy -> RETURN 0 (the request was NOT
 *       completed synchronously; vblk_kick() must not count it toward its
 *       own end-of-batch IRQ — CPU2 injects its own, later).
 *     Else: fall back to the original synchronous per-sector loop,
 *     UNCHANGED (always correct; this is the ROADMAP's own "only one
 *     request in flight is fine for a first cut" simplification — a second
 *     I/O request arriving before the first completes just executes the old
 *     way instead of queueing behind it).
 *
 *   CPU2 (vblk_async_poll(), called every iteration of this file's loop):
 *     If state != POSTED: return immediately (cheap, non-blocking poll).
 *     Else: dsb sy (acquire) -> read the payload -> run serve_data() per
 *     data descriptor (the actual emmc_bio_read()/write() PIO, itself
 *     iteration-capped in emmc_bio.c, so this can never hang) -> write the
 *     status byte -> push the used-ring entry -> raise VBLK_INTID pending
 *     in the REAL GICD (vblk_inject_irq() — the SAME GICD_ISPENDR mechanism
 *     the synchronous path already uses under HCR_EL2.IMO=0; see
 *     vblk_emmc.h's IRQ-injection comment and main_dbg.c's "IRQ POLICY" —
 *     there is no vgic/GICH involved, on purpose) -> dsb sy -> state = EMPTY
 *     -> dsb sy, so CPU0 may post the next request.
 *
 * ============================================================================
 * CPU2 BRING-UP SAFETY (mirrors CPU1 exactly — smp.c's smp_secondary_main())
 * ============================================================================
 * CPU2 is already PSCI CPU_ON'd generically by smp_init() before this file's
 * code ever runs: every secondary reloads the SAME captured
 * MAIR/TCR/TTBR0/VBAR/HCR/SCTLR and sets its own CPUECTLR_EL1.SMPEN=1 in
 * start.S's _start_secondary, per smp.h's block comment on why that exact
 * ordering matters for cache coherency (the A53 SMPEN gotcha). This file's
 * loop, once smp_secondary_main() dispatches into it for cpu==2:
 *   - NEVER writes CPUECTLR_EL1 or touches SMPEN itself — that is
 *     start.S's/ATF's job, done once, before smp_secondary_main() is even
 *     reached. Touching it again from EL2 here would trap to ATF and hang
 *     the core (the exact hazard this design brief was written to avoid).
 *   - is a TIGHT, BOUNDED poll: vblk_async_poll() itself does at most one
 *     mailbox request's worth of work, and that work is itself
 *     iteration-capped end-to-end (emmc_bio.c's polls; serve_data()'s
 *     bounded eMMC-lock acquire) — there is no unbounded wait, no
 *     WFE-without-timeout, nothing that can wedge this core.
 *   - leaves IRQ MASKED on this core (never unmasked, no timer armed) —
 *     identical to CPU1's debug loop; CPU2 has no need for a preemptive
 *     tick, it is a pure polling server with nothing worth preempting mid-
 *     request.
 *   - never returns (like CPU1's loop / the plain WFI park it replaces for
 *     CPU2 specifically — CPU3 still parks in WFI, untouched).
 *
 * ============================================================================
 * WEAK-SYMBOL LINKAGE (why smp.c does not hard-call this file, and why that
 * is NOT the async path's real on/off switch — see g_vblk_async_ready)
 * ============================================================================
 * smp.o is linked into every Makefile target (stage0/net/repl/fbsd/gdb/dbg —
 * several of which are ALREADY broken at baseline for unrelated missing-
 * object reasons, see the delivery notes), but vblk_async.o is added ONLY to
 * DBG_OBJS. smp.c therefore defines a WEAK default vblk_async_cpu2_run() (a
 * plain WFI park, byte-for-byte the old CPU2/3 "unused" behaviour) that THIS
 * file's STRONG definition overrides only when both are linked together
 * (the dbg build). No other Makefile target's CPU2 behaviour changes at all.
 *
 * This alone is NOT sufficient for correctness, though: the gdb build links
 * vblk_emmc.o (so vblk_request() CAN attempt vblk_async_post()) but not
 * vblk_async.o (so nothing would ever drain a posted mailbox — a silent,
 * permanent guest I/O hang). That is exactly what g_vblk_async_ready (set to
 * 1 only by this file's vblk_async_cpu2_run(), checked first thing by
 * vblk_async_post()) prevents: see vblk_emmc.h's comment on that flag.
 */
#ifndef BZDOS_VBLK_ASYNC_H
#define BZDOS_VBLK_ASYNC_H

/* CPU2 C entry: called once from smp.c's smp_secondary_main() when cpu==2
 * (via the weak/strong vblk_async_cpu2_run() indirection described above).
 * Never returns. Safe to call unconditionally — it only touches the vblk
 * mailbox (vblk_async_poll(), vblk_emmc.c) and its own tiny breadcrumb
 * (0x50020600, "VBA1" — see vblk_async.c). */
void vblk_async_cpu2_run(void) __attribute__((noreturn));

#endif /* BZDOS_VBLK_ASYNC_H */
