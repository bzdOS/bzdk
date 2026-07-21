/* guest.c — EL1 guest-execution infrastructure for bzdOS becoming a Type-1
 * hypervisor on the Allwinner A64 (Cortex-A53). See guest.h for the API
 * contract and the big picture; this file is the implementation plus the
 * exact register bit rationale.
 *
 * Preemption path this all sets up (already exists on the EL2 side — we do
 * not modify it, we just make it observable at EL1):
 *   1. guest_start_demo() drops the CPU to EL1 (guest_enter -> eret).
 *   2. guest_demo_el1() runs at EL1, spinning and bumping a counter.
 *   3. The CNTP timer fires. Because gic_timer.c set HCR_EL2.IMO=1, this
 *      physical IRQ is taken to EL2 (not EL1) even though EL1 is currently
 *      executing. The CPU takes the "lower EL, AArch64" IRQ vector (VEC 9 in
 *      exceptions.S), i.e. kind = (2<<2)|EL2_KIND_IRQ = 9, so
 *      (kind >> 2) == 2 identifies exactly this case.
 *   4. el2_common (exceptions.S) saves the full EL1 guest context (all
 *      x0..x30, ELR_EL2 = the guest PC it was interrupted at, SPSR_EL2 = the
 *      guest's PSTATE) and calls el2_trap(frame, kind).
 *   5. el2_trap dispatches IRQ/FIQ to gic_timer_irq() as it already does for
 *      the host's own tick handling. The integration lane additionally calls
 *      guest_note_preempt() here when (kind >> 2) == 2, bumping the
 *      breadcrumb preemption counter — see guest_note_preempt() below.
 *   6. el2_common restores the saved context and `eret`s — back into EL1,
 *      resuming the guest exactly where it was interrupted.
 * No part of this file masks that IRQ or touches HCR_EL2.IMO/VBAR_EL2: the
 * whole point is that EL2 regains control on every tick regardless of what
 * EL1 is doing.
 *
 * Freestanding, no libc: only <stdint.h>. Cache-coherent breadcrumb stores
 * (dc civac + dsb sy) so the record is visible to a live network peek/`bc`
 * even though the D-cache is on at EL2 and DRAM is shared with the guest.
 */
#include <stdint.h>
#include "guest.h"

/* ------------------------------------------------------------------ *
 * Breadcrumb window: 0x50000b00 ("GST1"). Distinct from every other window
 * in the tree (MUSB 0x50000000, EMAC 0x50000100, REPL 0x50000300, EL2
 * exceptions 0x50000400, jitter/TIMR 0x50000500, GIC timer 0x50000800).
 *   [0] magic       0x47535431 ("GST1")
 *   [1] loop_count  guest (EL1) loop counter — written BY guest_demo_el1;
 *                   climbing proves EL1 code is actually executing.
 *   [2] preempt_cnt bumped by guest_note_preempt(), meant to be called from
 *                   el2_trap on every (kind>>2)==2 IRQ/FIQ; climbing proves
 *                   EL2 is preempting the guest on the timer tick.
 *   [3] guest_el    CurrentEL as sampled BY the EL1 payload (should read 1).
 *   [4] hcr_el2     HCR_EL2 readback (lo 32 bits) after guest_config().
 *   [5] spsr_used   SPSR_EL2 value guest_enter() actually wrote before eret.
 * ------------------------------------------------------------------ */
#define GUEST_BC_BASE   0x50000b00UL
#define GUEST_BC_MAGIC  0x47535431u   /* "GST1" */

enum {
	GBC_MAGIC = 0,
	GBC_LOOP_COUNT,
	GBC_PREEMPT_CNT,
	GBC_GUEST_EL,
	GBC_HCR_EL2,
	GBC_SPSR_USED,
};

static inline void
guest_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(GUEST_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline uint32_t
guest_bc_read(int i)
{
	volatile uint32_t *p = (volatile uint32_t *)(GUEST_BC_BASE + (uint32_t)i * 4u);
	return *p;
}

/* ------------------------------------------------------------------ *
 * HCR_EL2 — EL2 host-control register. We only ever OR in the bit we own
 * (RW) and otherwise preserve whatever gic_timer.c (or U-Boot/BL31) already
 * set, in particular IMO, which MUST stay 1.
 * ------------------------------------------------------------------ */
#define HCR_EL2_VM  (1ull << 0)   /* stage-2 translation enable — OFF (flat first cut) */
#define HCR_EL2_RW  (1ull << 31) /* EL1 (and EL0 under it) is AArch64, not AArch32 */
#define HCR_EL2_IMO (1ull << 4)  /* physical IRQ routed to EL2 — set by gic_timer.c; preserved, never cleared here */

static inline uint64_t
read_hcr_el2(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, hcr_el2" : "=r"(v));
	return v;
}

static inline void
write_hcr_el2(uint64_t v)
{
	__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(v) : "memory");
}

/* ------------------------------------------------------------------ *
 * SCTLR_EL1 — guest's own system control register. MMU off (M=0), caches
 * off (C=0, I=0): flat physical execution, matching "no stage-2 yet, share
 * host DRAM directly". Only the architectural RES1 bits (ARMv8.0-A) are set
 * so the write is well-defined rather than leaving reserved fields at an
 * unknown reset value (EL1 has never been programmed before this point).
 * ------------------------------------------------------------------ */
#define SCTLR_EL1_RES1 \
	((1ull << 11) | (1ull << 20) | (1ull << 22) | (1ull << 23) | \
	 (1ull << 28) | (1ull << 29))

static inline void
write_sctlr_el1(uint64_t v)
{
	__asm__ volatile("msr sctlr_el1, %0\n\tisb" :: "r"(v) : "memory");
}

static inline void
write_vbar_el1(uint64_t v)
{
	__asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(v) : "memory");
}

static inline void
write_sp_el1(uint64_t v)
{
	/* SP_EL1 cannot be written directly with `msr sp_el1` while we ARE at
	 * EL1 (it aliases the live SP then); at EL2 it is a distinct banked
	 * register and this form is exactly right. */
	__asm__ volatile("msr sp_el1, %0" :: "r"(v));
}

/* ------------------------------------------------------------------ *
 * SPSR_EL2 — the PSTATE the guest starts with, restored by `eret`.
 *   M[3:0] = 0b0101 (EL1h: EL1, using SP_EL1 i.e. SPSel=1) — a guest kernel
 *            expects its own stack pointer, not SP_EL0.
 *   D = 1, A = 1: mask Debug and SError at guest entry (avoids surprises
 *            before the guest has set up its own handling of either).
 *   I = 0, F = 0: IRQ/FIQ UNMASKED at EL1. This looks like it would let the
 *            guest itself take the tick, but it does not: HCR_EL2.IMO=1
 *            means physical IRQs are steered to EL2 unconditionally, gated
 *            by EL2's OWN PSTATE.I, not EL1's. So this bit is cosmetic for
 *            our preemption story either way — left unmasked so a future
 *            real guest kernel (FreeBSD) that expects to run with IRQs
 *            enabled at EL1 is not surprised, and so a virtual-IRQ path
 *            (vGIC, later work) has somewhere sensible to deliver into.
 * ------------------------------------------------------------------ */
#define SPSR_EL1h (0x5ull)
#define SPSR_D    (1ull << 9)
#define SPSR_A    (1ull << 8)
#define SPSR_I    (1ull << 7)
#define SPSR_F    (1ull << 6)

#define GUEST_SPSR_EL2 (SPSR_EL1h | SPSR_D | SPSR_A)

static inline void
write_spsr_el2(uint64_t v)
{
	__asm__ volatile("msr spsr_el2, %0" :: "r"(v) : "memory");
}

static inline void
write_elr_el2(uint64_t v)
{
	__asm__ volatile("msr elr_el2, %0" :: "r"(v) : "memory");
}

static inline uint64_t
read_currentel(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return v >> 2;   /* CurrentEL.EL is bits [3:2] */
}

/* ------------------------------------------------------------------ *
 * Public API (see guest.h for the contract).
 * ------------------------------------------------------------------ */

void
guest_config(void)
{
	guest_bc(GBC_MAGIC, GUEST_BC_MAGIC);

	/* HCR_EL2: read-modify-write, OR in only RW. IMO (and anything else
	 * gic_timer.c/firmware already set) is preserved untouched — in
	 * particular IMO must survive so the tick keeps preempting EL1. VM
	 * stays whatever it already is (expected 0 at this point: stage-2 is
	 * not enabled anywhere yet in this tree) — asserted, not forced,
	 * so a future stage-2 lane that sets VM=1 before calling us is not
	 * silently undone here.
	 */
	uint64_t hcr = read_hcr_el2();
	hcr |= HCR_EL2_RW;
	write_hcr_el2(hcr);
	guest_bc(GBC_HCR_EL2, (uint32_t)read_hcr_el2());

	/* SCTLR_EL1: MMU + caches off, flat physical, RES1 bits only. */
	write_sctlr_el1(SCTLR_EL1_RES1);

	/* VBAR_EL1: hook for a real guest's own vector table. 0 for this
	 * milestone — the demo payload never traps to EL1 (it either spins
	 * or issues HVC, which by definition traps to EL2, not EL1), so an
	 * unset guest VBAR is never actually exercised here. A real guest
	 * kernel image installs its own before it could take any EL1 trap. */
	write_vbar_el1(0);

	/* ---- Stage-2 hook (documented, INERT in this milestone) ----
	 * A real (FreeBSD) guest additionally needs stage-2 translation so
	 * the guest's view of physical memory is host-controlled/isolated:
	 *   - VTCR_EL2: stage-2 translation control (granule size, T0SZ, SL0,
	 *     cacheability/shareability attributes for the stage-2 walk).
	 *   - VTTBR_EL2: stage-2 translation table base + VMID.
	 *   - HCR_EL2.VM = 1 to actually enable stage-2 once the tables exist.
	 * None of that is touched here; VM is left as read, and VTCR_EL2/
	 * VTTBR_EL2 are not written at all — the guest currently sees flat
	 * host physical memory directly (MMU off at EL1), which is fine for
	 * a spinning test payload but not a real guest OS.
	 */
}

void
guest_enter(uint64_t entry, uint64_t sp_el1)
{
	write_sp_el1(sp_el1);
	write_spsr_el2(GUEST_SPSR_EL2);
	guest_bc(GBC_SPSR_USED, (uint32_t)GUEST_SPSR_EL2);
	write_elr_el2(entry);

	__asm__ volatile("eret" ::: "memory");

	__builtin_unreachable();
}

/* Bump every this many loop iterations so the periodic HVC doesn't dominate
 * (an HVC every single iteration would turn "the guest spins" into "the
 * guest traps continuously", which is a different demo). Pure busy-loop
 * count, not time — good enough for "prove it happens now and then". */
#define GUEST_HVC_PERIOD 200000u

void
guest_demo_el1(void)
{
	guest_bc(GBC_GUEST_EL, (uint32_t)read_currentel());

	uint32_t n = 0;
	for (;;) {
		/* Long, otherwise-pointless loop body: this is what the EL2
		 * timer tick is expected to interrupt mid-flight. The count
		 * itself is the proof-of-life the host reads back. */
		uint32_t cur = guest_bc_read(GBC_LOOP_COUNT);
		guest_bc(GBC_LOOP_COUNT, cur + 1u);

		if (++n == GUEST_HVC_PERIOD) {
			n = 0;
			/* Optional guest -> EL2 hypercall demonstration. This
			 * traps synchronously to EL2 as a "lower EL, AArch64"
			 * exception (exceptions.S VEC 8), i.e.
			 * kind = (2<<2)|EL2_KIND_SYNC = 8, so (kind>>2)==2
			 * again, and ESR_EL2.EC == 0x16 (HVC from AArch64).
			 * el2_trap's existing synchronous path already records
			 * ESR/ELR/FAR to its own breadcrumb and advances
			 * ELR_EL2 by 4 to skip the faulting (here: trapping)
			 * instruction, so the guest resumes cleanly with no
			 * further changes required for this alone. */
			__asm__ volatile("hvc #0" ::: "memory");
		}
	}
}

/* Private EL1 stack for the demo guest — owned entirely by this file, does
 * not collide with any other region (host stacks, breadcrumb windows, or
 * anything the integration lane owns). 16 KiB is generous for a payload
 * that only loops and stores a word. */
#define GUEST_STACK_WORDS (16384 / 8)
static uint64_t guest_el1_stack[GUEST_STACK_WORDS] __attribute__((aligned(16)));

void
guest_start_demo(void)
{
	guest_config();

	uint64_t sp_top = (uint64_t)&guest_el1_stack[GUEST_STACK_WORDS];
	guest_enter((uint64_t)guest_demo_el1, sp_top);

	__builtin_unreachable();
}

void
guest_note_preempt(void)
{
	uint32_t cur = guest_bc_read(GBC_PREEMPT_CNT);
	guest_bc(GBC_PREEMPT_CNT, cur + 1u);
}
