/* SPDX-License-Identifier: BSD-2-Clause */

/* gtrace.c — guest early-locore tracing (see gtrace.h for the full design
 * rationale + trace-ring layout). Two instruments:
 *
 *   1. gtrace_handle_sysreg() — HCR_EL2.TVM trap-and-emulate for the guest's
 *      SCTLR_EL1/TTBRn_EL1/TCR_EL1/ESR_EL1/FAR_EL1/AFSRn_EL1/MAIR_EL1/
 *      AMAIR_EL1/CONTEXTIDR_EL1 accesses.
 *   2. gtrace_handle_hvc() + gtrace_vbar_el1() — a guest VBAR_EL1
 *      trampoline of bare `hvc #<vector>` instructions so an early,
 *      un-vectored EL1 fault becomes a visible EL2 trap.
 *
 * Freestanding: <stdint.h> only, no libc, no FP/SIMD (-mgeneral-regs-only).
 */
#include <stdint.h>
#include "gtrace.h"
#include "exceptions.h"
#include "flightrec.h"

/* ---- cache-coherent trace-ring store, same pattern as the rest of the
 * tree: *p = v; dc civac, p; dsb sy. ---- */
static inline void gtr_wr(uint32_t word_idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(GTRACE_BASE + (uint32_t)word_idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline uint32_t gtr_rd(uint32_t word_idx)
{
	volatile uint32_t *p = (volatile uint32_t *)(GTRACE_BASE + (uint32_t)word_idx * 4u);
	return *p;
}

/* ---- first-fault latch store (own DRAM window at GTRACE_FF_BASE, same
 * cache-coherent pattern). One-shot: ff_latched gates it to the FIRST guest
 * EL1 sync fault only, so the recursion storm can't overwrite it. ---- */
static int ff_latched;

static inline void ff_wr(uint32_t word_idx, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(GTRACE_FF_BASE + (uint32_t)word_idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void ff_wr64(uint32_t word_idx, uint64_t v)
{
	ff_wr(word_idx, (uint32_t)v);
	ff_wr(word_idx + 1u, (uint32_t)(v >> 32));
}

/* Append one (tag, value) event. Once the log fills, event_count keeps
 * counting but the log itself stops growing — the FIRST GTRACE_MAX_EVENTS
 * events (the earliest, most diagnostic activity) are preserved rather
 * than being overwritten by later spin/re-fault noise. */
static void gtrace_record(uint32_t tag, uint32_t value)
{
	uint32_t n = gtr_rd(1);

	if (n < GTRACE_MAX_EVENTS) {
		uint32_t widx = 4u + n * 2u;
		gtr_wr(widx, tag);
		gtr_wr(widx + 1u, value);
	}
	gtr_wr(1, n + 1u);
}

/* ---------------------------------------------------------------------
 * Instrument 1 — HCR_EL2.TVM sysreg trap-and-emulate (EC==0x18).
 *
 * ISS[24:0] layout (ARM ARM D17.2.124 for trapped MSR/MRS to EL1 regs via
 * TVM/TVM-like traps): Direction=bit0 (0=write/MSR,1=read/MRS), CRm=[4:1],
 * Rt=[9:5], CRn=[13:10], Op1=[16:14], Op2=[19:17], Op0=[21:20].
 *
 * All 11 TVM-trappable EL1 registers share Op0==3, Op1==0; they differ
 * only in CRn/CRm/Op2, so the switch below keys on a packed
 * (CRn<<8)|(CRm<<4)|Op2 value. AArch64 has no indirect msr/mrs, so each
 * register gets its own explicit case doing the concrete instruction.
 * --------------------------------------------------------------------- */
int gtrace_handle_sysreg(struct el2_frame *frame)
{
	uint32_t esr  = (uint32_t)frame->esr;
	uint32_t iss  = esr & 0x1FFFFFFu;      /* ISS[24:0] */
	uint32_t dir  = iss & 0x1u;            /* 0=write, 1=read */
	uint32_t rt   = (iss >> 5)  & 0x1Fu;
	uint32_t crn  = (iss >> 10) & 0xFu;
	uint32_t crm  = (iss >> 1)  & 0xFu;
	uint32_t op1  = (iss >> 14) & 0x7u;
	uint32_t op2  = (iss >> 17) & 0x7u;
	uint32_t op0  = (iss >> 20) & 0x3u;
	uint32_t key  = (crn << 8) | (crm << 4) | op2;
	uint32_t regid = GTRACE_REG_UNKNOWN;
	uint64_t val;

	val = (rt == 31u) ? 0ULL : frame->x[rt];   /* Rt==31 (write) = XZR = 0 */

	if (op0 == 3u && op1 == 0u) {
		if (dir == 0u) {
			/* WRITE: the TVM trap means the real msr did NOT happen —
			 * emulate it by actually performing it. */
			switch (key) {
			case (1u  << 8) | (0u << 4) | 0u:
				__asm__ volatile("msr sctlr_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_SCTLR_EL1;
				break;
			case (2u  << 8) | (0u << 4) | 0u:
				__asm__ volatile("msr ttbr0_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_TTBR0_EL1;
				break;
			case (2u  << 8) | (0u << 4) | 1u:
				__asm__ volatile("msr ttbr1_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_TTBR1_EL1;
				break;
			case (2u  << 8) | (0u << 4) | 2u:
				__asm__ volatile("msr tcr_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_TCR_EL1;
				break;
			case (5u  << 8) | (2u << 4) | 0u:
				__asm__ volatile("msr esr_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_ESR_EL1;
				break;
			case (6u  << 8) | (0u << 4) | 0u:
				__asm__ volatile("msr far_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_FAR_EL1;
				break;
			case (5u  << 8) | (1u << 4) | 0u:
				__asm__ volatile("msr afsr0_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_AFSR0_EL1;
				break;
			case (5u  << 8) | (1u << 4) | 1u:
				__asm__ volatile("msr afsr1_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_AFSR1_EL1;
				break;
			case (10u << 8) | (2u << 4) | 0u:
				__asm__ volatile("msr mair_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_MAIR_EL1;
				break;
			case (10u << 8) | (3u << 4) | 0u:
				__asm__ volatile("msr amair_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_AMAIR_EL1;
				break;
			case (13u << 8) | (0u << 4) | 1u:
				__asm__ volatile("msr contextidr_el1, %0\n\tisb" :: "r"(val) : "memory");
				regid = GTRACE_REG_CONTEXTIDR_EL1;
				break;
			default:
				break;   /* unknown encoding: fall through, still record */
			}

			if (regid == GTRACE_REG_SCTLR_EL1)
				gtr_wr(2, (uint32_t)val);   /* [2] last_sctlr */
		} else {
			/* READ: emulate the mrs the trap suppressed. */
			switch (key) {
			case (1u  << 8) | (0u << 4) | 0u:
				__asm__ volatile("mrs %0, sctlr_el1" : "=r"(val));
				regid = GTRACE_REG_SCTLR_EL1;
				break;
			case (2u  << 8) | (0u << 4) | 0u:
				__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(val));
				regid = GTRACE_REG_TTBR0_EL1;
				break;
			case (2u  << 8) | (0u << 4) | 1u:
				__asm__ volatile("mrs %0, ttbr1_el1" : "=r"(val));
				regid = GTRACE_REG_TTBR1_EL1;
				break;
			case (2u  << 8) | (0u << 4) | 2u:
				__asm__ volatile("mrs %0, tcr_el1" : "=r"(val));
				regid = GTRACE_REG_TCR_EL1;
				break;
			case (5u  << 8) | (2u << 4) | 0u:
				__asm__ volatile("mrs %0, esr_el1" : "=r"(val));
				regid = GTRACE_REG_ESR_EL1;
				break;
			case (6u  << 8) | (0u << 4) | 0u:
				__asm__ volatile("mrs %0, far_el1" : "=r"(val));
				regid = GTRACE_REG_FAR_EL1;
				break;
			case (5u  << 8) | (1u << 4) | 0u:
				__asm__ volatile("mrs %0, afsr0_el1" : "=r"(val));
				regid = GTRACE_REG_AFSR0_EL1;
				break;
			case (5u  << 8) | (1u << 4) | 1u:
				__asm__ volatile("mrs %0, afsr1_el1" : "=r"(val));
				regid = GTRACE_REG_AFSR1_EL1;
				break;
			case (10u << 8) | (2u << 4) | 0u:
				__asm__ volatile("mrs %0, mair_el1" : "=r"(val));
				regid = GTRACE_REG_MAIR_EL1;
				break;
			case (10u << 8) | (3u << 4) | 0u:
				__asm__ volatile("mrs %0, amair_el1" : "=r"(val));
				regid = GTRACE_REG_AMAIR_EL1;
				break;
			case (13u << 8) | (0u << 4) | 1u:
				__asm__ volatile("mrs %0, contextidr_el1" : "=r"(val));
				regid = GTRACE_REG_CONTEXTIDR_EL1;
				break;
			default:
				val = 0;
				break;   /* unknown encoding: fall through, still record */
			}

			if (rt != 31u)
				frame->x[rt] = val;   /* Rt==31 (read) = discard into XZR */
		}
	}
	/* else: op0/op1 outside the TVM-trappable set — genuinely unexpected;
	 * still fall through and record as unknown rather than loop. */

	gtrace_record((dir ? 0x40000000u : 0u) | regid, (uint32_t)val);

	/* Self-disabling trace (found live 2026-07-28): gtrace_init()'s own
	 * header comment calls this "guest early-locore tracing" — meant to
	 * observe pmap_bootstrap/locore, a one-time early-boot event. But
	 * HCR_EL2.TVM was only ever turned ON (gtrace_init()), never OFF, so
	 * EVERY later TTBR0_EL1 write for the guest's entire remaining
	 * lifetime -- i.e. every process-address-space context switch, for as
	 * long as the guest runs -- kept paying a full trap-and-emulate EL2
	 * round trip. Root mount worked but then a normal multiuser boot
	 * (rc.d spawning many short-lived processes) looked hung: virtio-blk
	 * breadcrumbs frozen, zero network traffic, ever. Confirmed live: a
	 * one-off build with TVM force-disabled (-DDBG_NO_TVM) sailed straight
	 * through to a real `login:` prompt in the same capture window that
	 * the TVM-on build never got past "Mounting local filesystems".
	 * gtrace_record() above already stops accumulating NEW events once
	 * the ring fills (GTRACE_MAX_EVENTS) -- the trap itself has delivered
	 * zero additional diagnostic value past that point, so this is the
	 * natural, permanent-cost-free place to turn TVM back off: once the
	 * ring is full, clear HCR_EL2.TVM (same read-modify-write pattern as
	 * gtrace_init()'s set) so every subsequent guest sysreg write runs
	 * natively for the rest of this boot. */
	if (gtr_rd(1) >= GTRACE_MAX_EVENTS) {
		uint64_t hcr;
		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr &= ~(1ULL << 26);
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
	}

	frame->elr += 4u;   /* skip the trapped instruction; access is emulated */
	return 1;
}

/* ---------------------------------------------------------------------
 * Instrument 2 — guest VBAR_EL1 trampoline (EC==0x16 / HVC).
 * --------------------------------------------------------------------- */

#define GTRACE_VEC_COUNT   16u
#define GTRACE_VEC_STRIDE  0x80u   /* bytes per architectural EL1 vector */

/* 16 entries x 0x80 bytes = 0x800 = 2048 bytes, matching the mandatory
 * VBAR_EL1 alignment (low 11 bits must be zero). Lives in .bss; identity
 * stage-2-mapped like the rest of hypervisor RAM, so the EL1 guest can
 * fetch instructions from it directly. */
static uint32_t gtrace_vbar_table[(GTRACE_VEC_COUNT * GTRACE_VEC_STRIDE) / 4u]
	__attribute__((aligned(2048)));

uint64_t gtrace_vbar_el1(void)
{
	return (uint64_t)(unsigned long)gtrace_vbar_table;
}

/* AArch64 HVC #imm16 encoding: 1101_0100_000<imm16>000_10
 *   bits[31:24] = 0xD4, [23:21] = 000, [20:5] = imm16, [4:2] = 000, [1:0] = 10
 * i.e. word = 0xD4000002 | (imm16 << 5). HVC #0 == 0xD4000002. */
static inline uint32_t gtrace_hvc_insn(uint32_t imm16)
{
	return 0xD4000002u | ((imm16 & 0xFFFFu) << 5);
}

/* Handle an ESR_EL2.EC==0x16 trap from the EL1 guest's own HVC (issued by
 * one of our vbar-table entries in response to a real EL1 exception). */
int gtrace_handle_hvc(struct el2_frame *frame)
{
	uint32_t iss = (uint32_t)(frame->esr & 0xFFFFu);   /* HVC immediate */
	uint32_t vec = iss & 0xFu;                          /* vector index 0..15 */
	uint64_t elr1, esr1, far1, spsr1;
	uint32_t ec1;
	uint32_t tag_info, tag_far;
	uint32_t fault_count;

	/* EL2 can read EL1 registers directly (more privileged) — no trap. */
	__asm__ volatile("mrs %0, elr_el1"  : "=r"(elr1));
	__asm__ volatile("mrs %0, esr_el1"  : "=r"(esr1));
	__asm__ volatile("mrs %0, far_el1"  : "=r"(far1));
	__asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr1));

	/* FIRST-FAULT LATCH (one-shot): before recording into the rolling ring,
	 * freeze the FULL guest state of the VERY FIRST EL1 sync fault into the
	 * FFL1 breadcrumb and never re-arm. frame->x[] are the guest's GPRs as of
	 * the fault (the trampoline entry is a bare `hvc #vec`, so it clobbers no
	 * GPRs), and ELR/ESR/FAR/SP/SPSR_EL1 are the guest's live EL1 fault
	 * state. This captures the ORIGINAL fault PC + FAR before the recursive
	 * storm masks them. */
	if (!ff_latched) {
		uint64_t sp1;
		uint32_t i;

		__asm__ volatile("mrs %0, sp_el1" : "=r"(sp1));

		ff_wr(0, 0u);              /* clear magic while writing */
		ff_wr(1, 0u);              /* valid=0 during the write window */
		ff_wr(2, vec);
		ff_wr(3, 0u);
		ff_wr64(4,  esr1);
		ff_wr64(6,  elr1);
		ff_wr64(8,  far1);
		ff_wr64(10, sp1);
		ff_wr64(12, spsr1);
		for (i = 0; i < 31u; i++)
			ff_wr64(14u + i * 2u, frame->x[i]);
		ff_wr(0, GTRACE_FF_MAGIC); /* stamp magic + valid last */
		ff_wr(1, 1u);
		ff_latched = 1;
	}

	ec1 = (uint32_t)((esr1 >> 26) & 0x3Fu);

	/* Full-width record of this fault into the flightrec.
	 *
	 * The two existing records below are lossy in ways that mattered: the FFL1
	 * latch above keeps only the FIRST fault of the boot (an early harmless
	 * kernel fault claims it), and gtrace_record() truncates the PC to 32 bits,
	 * discarding the top half of a 0xffff0000........ kernel address. Both stay
	 * as they are — dbgmon reads them — and this adds a lossless one alongside.
	 * SPSR_EL1.M[3:2] gives the originating level, so one bit says EL0 vs EL1.
	 *
	 * SCOPE: this function only runs while OUR trampoline is the guest's
	 * VBAR_EL1, i.e. early boot. Once FreeBSD installs its own vectors nothing
	 * here is reached again, and VBAR_EL1 writes cannot be trapped (TVM covers
	 * memory-management registers only). Verified on hardware 2026-08-03:
	 * deliberately crashing growfs in a running guest produced ZERO records.
	 * Do not expect userland-crash visibility from here — see FLTR_K_GFAULT's
	 * comment in flightrec.h for why, and for the hardware-breakpoint route
	 * that does work. */
	{
		uint32_t from_el = (uint32_t)((spsr1 >> 2) & 0x3u);
		uint64_t a0 = ((uint64_t)(from_el ? 1u : 0u) << 40) |
		              ((uint64_t)ec1 << 32) | (uint32_t)esr1;

		flightrec_log(FLTR_K_GFAULT, a0, elr1);
		/* FAR only means something for aborts (EC 0x20/0x21 instruction,
		 * 0x24/0x25 data). Skip it otherwise so the common case stays at one
		 * ring slot. */
		if (ec1 == 0x20u || ec1 == 0x21u || ec1 == 0x24u || ec1 == 0x25u)
			flightrec_log(FLTR_K_GFAR, a0, far1);
	}

	tag_info = 0x80000000u | ((vec & 0xFu) << 24) | ((ec1 & 0x3Fu) << 8);
	tag_far  = 0xA0000000u | ((vec & 0xFu) << 24);

	gtrace_record(tag_info, (uint32_t)elr1);
	gtrace_record(tag_far,  (uint32_t)far1);

	fault_count = gtr_rd(3);
	gtr_wr(3, fault_count + 1u);

	/* Deliberately do not touch frame->elr / attempt to resume: an early,
	 * un-vectored EL1 fault is the smoking gun this instrument exists to
	 * catch. Recording is the point; the ~6s WDT resets the board so the
	 * ring can be read with `md.l 0x50001000`. */
	return 1;
}

/* ---------------------------------------------------------------------
 * gtrace_init() — arm both instruments. Must run BEFORE the guest is
 * entered: HCR_EL2.TVM has to be set before the guest's first SCTLR_EL1/
 * TTBRn_EL1/... write, and VBAR_EL1 has to point at our trampoline before
 * the guest can take its first (early, un-vectored) exception.
 * --------------------------------------------------------------------- */
void gtrace_init(void)
{
	uint32_t i;
	uint64_t hcr;

	/* Zero the whole ring (header + event log) before re-stamping the
	 * magic, so a stale ring from a previous run can't be misread. */
	for (i = 0; i < 4u + GTRACE_MAX_EVENTS * 2u; i++)
		gtr_wr(i, 0u);
	gtr_wr(0, GTRACE_MAGIC);

	/* Re-arm the one-shot first-fault latch and clear its magic/valid so a
	 * stale FFL1 from a previous run can't be misread as this run's. */
	ff_latched = 0;
	ff_wr(0, 0u);
	ff_wr(1, 0u);

	/* Build the 16-entry VBAR_EL1 trampoline: one `hvc #i` per vector,
	 * with explicit D/I-cache maintenance since this is self-modifying
	 * code the guest will later fetch. */
	for (i = 0; i < GTRACE_VEC_COUNT; i++) {
		uint32_t *slot = &gtrace_vbar_table[(i * GTRACE_VEC_STRIDE) / 4u];
		*slot = gtrace_hvc_insn(i);
		__asm__ volatile(
			"dc cvau, %0\n\t"
			"dsb ish\n\t"
			"ic ivau, %0\n\t"
			"dsb ish\n\t"
			"isb"
			:: "r"(slot) : "memory");
	}

	/* HCR_EL2.TVM (bit 26) — read-modify-write so VM/RW/IMO/etc (owned by
	 * guest_config()/stage2_enable()) are preserved. */
	__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
	hcr |= (1ULL << 26);
	__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
}
