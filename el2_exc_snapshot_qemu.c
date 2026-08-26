/* SPDX-License-Identifier: BSD-2-Clause */

/* el2_exc_snapshot_qemu.c — EL2 trap handler for the QEMU `virt` snapshot
 * exercise (ROADMAP D1: "freeze the guest, dump RAM, restore it").
 *
 * This is a NEW, minimal el2_trap(), following el2_exc_qemu.c's own
 * precedent exactly (see that file's header for why a dozen board-specific
 * modules are deliberately not pulled in here) — just with a different
 * payload: instead of only proving "stage-2 + EL1 guest + timer
 * preemption", this one additionally calls the REAL, UNMODIFIED
 * snapshot_save()/snapshot_restore() (snapshot.c/.h) at two specific ticks
 * and watches the guest's own breadcrumb loop counter
 * (guest_snapshot_payload.h's SNAPGP_BC_BASE) to see whether the guest
 * actually resumes at its pre-snapshot state afterwards.
 *
 * Sequence (ticks are gic_timer_qemu_ticks(), 100 ms period from
 * main_snapshot_qemu.c, same as the plain qemu-ci.sh target):
 *   tick SNAP_TICK      -- call snapshot_save(frame). This is exactly the
 *                          "guest trap boundary" snapshot.h's own API
 *                          contract calls for: the guest is quiesced right
 *                          here (mid-IRQ, EL2 has full control), frame is
 *                          the live trap frame, no device DMA exists on
 *                          this target at all (no MMC/EMAC/USB — see
 *                          el2_exc_qemu.c's header for why), so the
 *                          preconditions snapshot_save() documents as
 *                          "caller-enforced" are trivially satisfied here.
 *   tick RESTORE_TICK   -- call snapshot_restore(frame). If this returns
 *                          and the guest is still alive, el2_common's eret
 *                          (using the frame snapshot_restore() just
 *                          rewrote) lands the CPU back at the SAVED PC —
 *                          so the very next samples of the guest's loop
 *                          counter should show it near where it was at
 *                          SNAP_TICK, not where it grew to by RESTORE_TICK.
 *   tick PASS_AFTER_TICKS -- print the verdict (did the counter actually
 *                          rewind and then resume climbing?) and power off.
 *
 * HONESTY: this is exactly the "freeze -> dump -> restore" sequence the
 * ROADMAP asks for, run for real (not just arithmetic) against a real,
 * running EL1 guest under QEMU. It is NOT the real board (no EMAC bulk
 * transport, no eMMC store, no A64 watchdog — see wdt_qemu_stub.c) and it
 * does NOT paper over the known "the snapshotted DRAM range contains the
 * HV's own image" issue documented in test_snapshot_fmt.c's
 * open_issue_snapshot_range_contains_hv_image: SNAP_DRAM_BASE/SNAP_DRAM_SIZE
 * (snapshot.h) are used completely unmodified, and this target's own HV
 * image loads at 0x40080000 (link_qemu.ld) — squarely inside that range,
 * same as the real board's 0x42000000. If snapshot_restore()'s DRAM copy
 * corrupts the running HV before this file gets to print a verdict, THAT
 * is itself the answer to "can restore be exercised end-to-end" — see the
 * report this shipped with for what was actually observed.
 */
#include <stdint.h>
#include "exceptions.h"
#include "guest.h"
#include "guest_snapshot_payload.h"
#include "gic_timer_qemu.h"
#include "pl011_qemu.h"
#include "snapshot.h"
#include "el2_exc_qemu_common.h"  /* the shared half of this dispatch — see that header */

/* Tick schedule. 100 ms period (main_snapshot_qemu.c) so this whole run is
 * ~1.3 s of guest time even before accounting for however long the 1 GiB
 * dram_copy() itself takes under TCG emulation (unmeasured — part of what
 * this exercise is for). */
#define SNAP_TICK          3u   /* freeze + dump here */
#define RESTORE_TICK       6u   /* restore from the dump here */
#define OBSERVE_TICKS      4u   /* ticks to watch post-restore before verdict */
#define PASS_AFTER_TICKS   (RESTORE_TICK + OBSERVE_TICKS)

/* A small allowance for "how much can the guest's loop counter have grown
 * between the exact DRAM-copy pass inside snapshot_save() and the read we
 * take right after it returns" — should be 0 (the guest is fully quiesced
 * for the whole call, nothing else runs), but a slack of a few counts costs
 * nothing and keeps the verdict from being pathologically strict about an
 * implementation detail this exercise isn't trying to pin down to the
 * instruction. */
#define REWIND_SLACK 8u

static uint32_t s_loop_at_snapshot      = 0xFFFFFFFFu;
static uint32_t s_loop_before_restore   = 0xFFFFFFFFu;
static uint32_t s_loop_after_restore    = 0xFFFFFFFFu;
static int      s_save_rc               = 1;   /* not-yet-called sentinel */
static int      s_restore_rc            = 1;   /* not-yet-called sentinel */

/* qemu_poweroff()/report_fault() used to be defined here, verbatim-duplicated
 * across every QEMU CI handler; they now live in el2_exc_qemu_common.c as
 * qemu_psci_poweroff()/qemu_report_fault(). Behaviour is unchanged, including
 * this target's choice to HALT in wfi rather than power off after a fault
 * (this exercise wants FAIL to look exactly like a hang under the wrapping
 * `timeout`, same as el2_exc_qemu.c's QEMU-CI target). */
#define qemu_poweroff qemu_psci_poweroff

/* This target's own scenario config for the shared guest-sync chain: a
 * zeroed struct, because this payload (guest_snapshot_payload.c) issues no
 * HVC, has no trap-emulated console, forwards no SMC, and probes no
 * non-DRAM MMIO — the ONLY thing the lower-EL sync path here has ever needed
 * to handle is the unconditional dynamic W^X promotion qemu_guest_sync()
 * already does for every target regardless of ops (see the "ONE EXCEPTION"
 * comment on the call site below, and el2_exc_qemu_common.h's own comment on
 * what a zeroed struct means). */
static const struct qemu_guest_sync_ops snapshot_qemu_ops = { 0 };

static inline uint32_t
snap_loop_read(void)
{
	return *(volatile uint32_t *)(SNAPGP_BC_BASE + SNAPGP_BC_LOOP_IDX * 4u);
}

static inline uint32_t
snap_magic_read(void)
{
	return *(volatile uint32_t *)(SNAPGP_BC_BASE + SNAPGP_BC_MAGIC_IDX * 4u);
}

static void
print_rc(const char *what, int rc)
{
	pl011_puts(what);
	if (rc == 0) {
		pl011_puts(" OK\n");
	} else {
		pl011_puts(" FAILED rc=");
		if (rc < 0) {
			pl011_puts("-");
			pl011_put_udec((uint32_t)(-rc));
		} else {
			pl011_put_udec((uint32_t)rc);
		}
		pl011_puts("\n");
	}
}

/* report_fault() used to be defined here (identical ESR/ELR/FAR dump to every
 * other QEMU CI handler, halting in wfi rather than powering off — this
 * target's own choice, preserved verbatim below); it is now
 * qemu_report_fault() in el2_exc_qemu_common.c, called at this file's one
 * call site with this target's own greppable marker "QEMU-SNAPSHOT-CI" and
 * poweroff=0. See el2_exc_qemu_common.h for why the marker stays per-target. */

static void
do_verdict(void)
{
	uint32_t final = snap_loop_read();
	int rewound_then_resumed =
		(s_save_rc == 0) && (s_restore_rc == 0) &&
		(s_loop_after_restore != 0xFFFFFFFFu) &&
		(s_loop_at_snapshot   != 0xFFFFFFFFu) &&
		(s_loop_after_restore <= s_loop_at_snapshot + REWIND_SLACK) &&
		(final > s_loop_after_restore);

	pl011_puts("HV: final guest loop_count=");
	pl011_put_udec(final);
	pl011_puts(" (snapshot-time=");
	pl011_put_udec(s_loop_at_snapshot);
	pl011_puts(", pre-restore=");
	pl011_put_udec(s_loop_before_restore);
	pl011_puts(", first-post-restore=");
	pl011_put_udec(s_loop_after_restore);
	pl011_puts(")\n");

	if (rewound_then_resumed) {
		pl011_puts("QEMU-SNAPSHOT-CI: PASS (loop_count rewound from a "
		           "later value back near the snapshot-time value and "
		           "then resumed climbing -- guest state was rolled "
		           "back by snapshot_restore())\n");
	} else {
		pl011_puts("QEMU-SNAPSHOT-CI: FAIL (did not observe a "
		           "rewind-then-resume; see printed rc's/counters "
		           "above)\n");
	}
	qemu_poweroff();
}

void
el2_trap(struct el2_frame *frame, unsigned long kind)
{
	unsigned t = (unsigned)(kind & 3u);

	if (t == EL2_KIND_IRQ || t == EL2_KIND_FIQ) {
		if (gic_timer_qemu_irq(frame)) {
			uint64_t ticks = gic_timer_qemu_ticks();

			if (ticks == 1u) {
				pl011_puts("HV: first tick -- EL2 preempted the EL1 guest\n");
				pl011_puts("HV: guest breadcrumb magic=0x");
				pl011_put_hex32(snap_magic_read());
				pl011_puts(" (expect 0x53474e50 \"SNGP\")\n");
			}

			if (ticks == (uint64_t)SNAP_TICK) {
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(" -- guest loop_count=");
				pl011_put_udec(snap_loop_read());
				pl011_puts(" -- calling snapshot_save()\n");

				s_save_rc = snapshot_save(frame);
				s_loop_at_snapshot = snap_loop_read();
				print_rc("HV: snapshot_save()", s_save_rc);
				pl011_puts("HV: snapshot_present()=");
				pl011_put_udec((uint32_t)snapshot_present());
				pl011_puts("\n");
			} else if (ticks > (uint64_t)SNAP_TICK && ticks < (uint64_t)RESTORE_TICK) {
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(" -- guest loop_count=");
				pl011_put_udec(snap_loop_read());
				pl011_puts(" (post-snapshot, pre-restore)\n");
			}

			if (ticks == (uint64_t)RESTORE_TICK) {
				s_loop_before_restore = snap_loop_read();
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(" -- guest loop_count=");
				pl011_put_udec(s_loop_before_restore);
				pl011_puts(" -- calling snapshot_restore()\n");

				s_restore_rc = snapshot_restore(frame);
				print_rc("HV: snapshot_restore()", s_restore_rc);
			} else if (ticks == (uint64_t)RESTORE_TICK + 1u) {
				s_loop_after_restore = snap_loop_read();
				pl011_puts("HV: first tick after restore -- guest loop_count=");
				pl011_put_udec(s_loop_after_restore);
				pl011_puts("\n");
			} else if (ticks > (uint64_t)RESTORE_TICK + 1u && ticks < (uint64_t)PASS_AFTER_TICKS) {
				pl011_puts("HV: tick ");
				pl011_put_udec((uint32_t)ticks);
				pl011_puts(" -- guest loop_count=");
				pl011_put_udec(snap_loop_read());
				pl011_puts(" (post-restore)\n");
			}

			if (ticks >= (uint64_t)PASS_AFTER_TICKS)
				do_verdict();
		}
		return;   /* never advance ELR/SPSR for an async exception */
	}

	/* Guest (lower-EL) synchronous exception: this payload never issues an
	 * HVC (unlike guest.c's guest_demo_el1()/el2_exc_qemu.c's ack path) and
	 * never should fault (flat, MMU-off EL1 writing to its own single
	 * fixed, valid stage-2-backed word) -- so ANY lower-EL sync exception
	 * here is unexpected and reported, not silently handled.
	 *
	 * ONE EXCEPTION, added 2026-08-26: "never should fault" stopped being
	 * true when stage2.h made STAGE2_WX_DYNAMIC's XN mapping the default.
	 * The payload's word is still valid and stage-2-backed, but its CODE now
	 * starts out execute-never, so its very first instruction fetch takes a
	 * permission fault that only stage2_wx_fault() can clear. That is normal
	 * guest progress under the current policy, not the unexpected fault this
	 * comment is about -- see stage2_wx_qemu.h (now reached indirectly, via
	 * qemu_guest_sync()'s unconditional stage2_wx_qemu_try() call — see
	 * el2_exc_qemu_common.h). Everything else still falls straight through
	 * to qemu_report_fault() exactly as report_fault() did before. */
	if ((kind >> 2) == 2u && t == EL2_KIND_SYNC) {
		uint32_t ec = ((uint32_t)(frame->esr >> 26)) & 0x3fu;

		if (qemu_guest_sync(frame, ec, &snapshot_qemu_ops))
			return;
	}

	qemu_report_fault(frame, kind, "QEMU-SNAPSHOT-CI", /*poweroff=*/0);
}
