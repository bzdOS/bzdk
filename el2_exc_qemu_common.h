/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_qemu_common.h — the shared half of every QEMU-virt CI target's
 * el2_trap().
 *
 * WHY THIS EXISTS. There are nine hand-written el2_trap() implementations in
 * this tree, one per QEMU CI scenario (el2_exc_qemu.c, el2_exc_vgic_qemu.c,
 * el2_exc_zephyr_qemu.c, el2_exc_linux_qemu.c, el2_exc_snapshot_qemu.c,
 * el2_exc_dual_qemu.c, el2_exc_dual2_qemu.c, el2_exc_dual_rearm*, and
 * el2_exc_dual_zephyr_qemu.c). Each was written as "a NEW, minimal el2_trap(),
 * not a reuse of el2_exc.c", for a real reason: the board's el2_exc.c drags in
 * a dozen Allwinner-specific modules that do not exist under QEMU virt. But
 * each one ALSO re-implemented the parts that are not scenario-specific at all
 * — PSCI SYSTEM_OFF, the ESR/ELR/FAR fault dump, and the guest lower-EL
 * synchronous dispatch chain.
 *
 * That duplication is not a tidiness complaint, it caused a measured outage:
 * when stage2.h flipped STAGE2_WX_DYNAMIC on by default, guest DRAM became
 * execute-never pending a promotion fault, and NONE of the nine handlers had
 * the hook el2_exc.c has. Every QEMU target died on its guest's first
 * instruction fetch (ESR=0x8200000e) and the entire board-free gate — the
 * thing SESSION-RULES/BRING-UP.md say to pass BEFORE touching hardware — sat
 * red. Nine copies of a dispatch chain means a policy change reaches zero of
 * them.
 *
 * WHAT IS SHARED AND WHAT IS NOT. The split follows the actual fault line:
 *
 *   SHARED (here): PSCI poweroff, the fault dump, and the guest sync chain —
 *   dynamic W^X promotion, trap-emulated console, HVC acknowledgement, PSCI
 *   SMC forwarding, and the non-DRAM MMIO absorb. These are properties of
 *   "an EL1 guest running under this tree's stage-2", identical everywhere.
 *
 *   NOT SHARED (stays in each variant): everything that defines what the
 *   scenario PROVES — the IRQ/tick schedule, the pass criteria, the counters
 *   sampled, the PASS/FAIL strings, and any mechanism actually under test
 *   (holdtest's guest BRK interception, snapshot's save/restore ticks, the
 *   dual targets' per-core routing). A gate's verdict logic belongs in the
 *   gate, not in a shared library.
 *
 * Each variant therefore keeps its own el2_trap() — it still owns the async
 * branch and the ordering — and calls qemu_guest_sync() for the common part
 * and qemu_report_fault() for the terminal one.
 */
#ifndef BZDOS_EL2_EXC_QEMU_COMMON_H
#define BZDOS_EL2_EXC_QEMU_COMMON_H

#include <stdint.h>
#include "exceptions.h"

/* EC values the shared chain reasons about. Spelled out here once instead of
 * per file, which is what several of these variants each did locally. */
#define QEMU_EC_HVC64      0x16u
#define QEMU_EC_SMC64      0x17u
#define QEMU_EC_IABT_LOWER 0x20u
#define QEMU_EC_DABT_LOWER 0x24u

/* Per-scenario configuration for qemu_guest_sync(). Every field is optional;
 * a zeroed struct means "do only the things that are unconditionally correct
 * for any guest" (today: dynamic W^X promotion). */
struct qemu_guest_sync_ops {
	/* Tried FIRST, before anything below, so a scenario can claim an
	 * exception the shared chain would otherwise handle differently — this
	 * is where a mechanism UNDER TEST belongs (holdtest's EC 0x3C guest
	 * BRK, for instance). Nonzero return = handled, nothing else runs. */
	int (*claim_first)(struct el2_frame *frame, uint32_t ec);

	/* Trap-emulated guest console (vconsole.c). Set want_console to try
	 * vconsole_handle_fault(frame, console_chan) on a lower-EL DATA abort;
	 * after_console, if set, runs only when vconsole claimed the fault (the
	 * targets that echo guest output drain their tee ring there). */
	unsigned want_console;
	unsigned console_chan;
	void (*after_console)(void);

	/* Acknowledge a guest HVC. hvc_advance_elr distinguishes two REAL
	 * behaviours this tree has already had a bug about, so it is an explicit
	 * choice rather than a default: for an HVC trap the architecture sets
	 * ELR_EL2 to the instruction AFTER the HVC (it is a return address, not
	 * a faulting PC), so NO adjustment is correct — el2_exc_dual2_qemu.c's
	 * header documents el2_exc_qemu.c's own `frame->elr += 4u;` as a latent
	 * bug that is merely dead code there because that payload issues no HVC.
	 * Both behaviours are preserved verbatim during this refactor rather than
	 * silently "fixed", so no target's observable behaviour changes here; the
	 * +4 users are flagged in their own call sites. */
	unsigned want_hvc_ack;
	unsigned hvc_advance_elr;

	/* Forward a guest SMC (PSCI) to the target's own handler. */
	void (*on_smc)(struct el2_frame *frame);

	/* Absorb a lower-EL data abort OUTSIDE guest DRAM as read-as-zero /
	 * write-as-noop (mmio_absorb.c), for a guest whose DTS describes devices
	 * QEMU virt does not have — the real Zephyr image probing the A64 GIC at
	 * 0x01C81000 being the live example.
	 *
	 * The DRAM exclusion is not optional and not a micro-optimisation:
	 * mmio_absorb_fault() NEVER returns 0, it absorbs everything handed to
	 * it. That is safe in the dual build, where CPU3's stage2_zephyr table
	 * maps only its own 32 MiB slice so any data abort IS an MMIO probe. On a
	 * target using stage2.c's identity map, handing it everything would
	 * swallow a genuine guest bug touching a bad DRAM address and turn a real
	 * failure into a green run. qemu_guest_sync() therefore checks the IPA
	 * from HPFAR_EL2 and lets DRAM faults fall through to the caller's
	 * report_fault(), which is the entire purpose of a gate. */
	unsigned want_mmio_absorb;
};

/* Run the shared lower-EL synchronous chain. Returns nonzero if the exception
 * was handled and the caller must return without touching ELR/SPSR further;
 * zero means "not ours" and the caller should fall through to its own
 * reporting path exactly as before.
 *
 * `kind` and `ec` are passed in already-decoded because every caller has them
 * to hand; this function does NOT re-check that the exception came from a
 * lower EL or that it is synchronous — the caller's own dispatch already
 * established that, and pretending otherwise would just duplicate the test. */
int qemu_guest_sync(struct el2_frame *frame, uint32_t ec,
                    const struct qemu_guest_sync_ops *ops);

/* PSCI SYSTEM_OFF via SMC. QEMU's virt machine answers PSCI itself for a
 * bare-metal EL2 payload booted with -kernel and no -bios, and exits 0 on
 * SYSTEM_OFF; the wfi loop is only for the unexpected case where it does not
 * terminate the emulator. Was duplicated verbatim in every variant. */
void qemu_psci_poweroff(void) __attribute__((noreturn));

/* The shared fault dump: `<marker>: FAULT kind=.. ESR=.. ELR=.. FAR=..`.
 *
 * `marker` is the scenario's own greppable prefix ("QEMU-CI",
 * "QEMU-SNAPSHOT-CI", ...) — CI scripts grep for these, so they stay
 * per-target rather than being unified into one string.
 *
 * `poweroff` picks the terminal behaviour, and the difference is deliberate,
 * not incidental: a target that powers off gives its CI script an immediate
 * exit, while one that halts in wfi lets the wrapping `timeout` produce the
 * failure. Both are unambiguous; existing targets differ, and this preserves
 * each one's choice. */
void qemu_report_fault(struct el2_frame *frame, unsigned long kind,
                       const char *marker, int poweroff)
	__attribute__((noreturn));

#endif /* BZDOS_EL2_EXC_QEMU_COMMON_H */
