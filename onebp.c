/* SPDX-License-Identifier: BSD-2-Clause */

/* onebp.c — one-shot HVC software breakpoint. See onebp.h. */
#include "onebp.h"

#define ONEBP_BC_BASE 0x50007000UL
#define ONEBP_MAGIC   0x31425031u   /* "1BP1" */

/* HVC #imm16 encoding: 0xD4000002 | (imm16 << 5). Same as gtrace.c. */
static inline uint32_t onebp_hvc_insn(uint32_t imm16)
{
	return 0xD4000002u | ((imm16 & 0xFFFFu) << 5);
}

static uint64_t onebp_pa;
static uint32_t onebp_orig;
static uint32_t onebp_imm;
static int      onebp_armed;
static int      onebp_fired;

static inline void onebp_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(ONEBP_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void onebp_bc64(int i, uint64_t v)
{
	onebp_bc(i, (uint32_t)v);
	onebp_bc(i + 1, (uint32_t)(v >> 32));
}

/* Write `word` to guest physical address `pa` and make it visible to the
 * instruction stream: clean D-cache to PoU, invalidate I-cache to PoU,
 * dsb+isb before it will ever be fetched. */
static void onebp_patch_word(uint64_t pa, uint32_t word)
{
	volatile uint32_t *p = (volatile uint32_t *)pa;

	*p = word;
	__asm__ volatile(
		"dc cvau, %0\n\t"
		"dsb ish\n\t"
		"ic ivau, %0\n\t"
		"dsb ish\n\t"
		"isb"
		:: "r"(p) : "memory");
}

void onebp_arm(uint64_t pa, uint32_t imm)
{
	onebp_bc(0, 0u);           /* clear magic while (re)arming */
	onebp_bc(1, 0u);           /* hit=0 */

	onebp_pa   = pa;
	onebp_orig = *(volatile uint32_t *)pa;
	onebp_imm  = imm & 0xFFFFu;
	onebp_fired = 0;

	onebp_patch_word(pa, onebp_hvc_insn(onebp_imm));
	onebp_armed = 1;

	onebp_bc(0, ONEBP_MAGIC);
}

int onebp_handle_hvc(struct el2_frame *frame)
{
	uint32_t iss;

	if (!onebp_armed || onebp_fired)
		return 0;

	iss = (uint32_t)(frame->esr & 0xFFFFu);
	if (iss != onebp_imm)
		return 0;

	onebp_bc64(2, frame->x[0]);
	onebp_bc64(4, frame->x[1]);
	onebp_bc64(6, frame->x[2]);
	onebp_bc64(8, frame->x[3]);
	onebp_bc64(10, frame->elr);   /* PC just after the hvc, for reference */
	onebp_bc64(12, frame->x[21]);
	onebp_bc64(14, frame->x[8]);
	onebp_bc(1, 1u);              /* hit=1, stamped last */

	/* Self-remove: put the original instruction back and rewind ELR by 4
	 * so the guest re-executes it for real on this same eret. */
	onebp_patch_word(onebp_pa, onebp_orig);
	frame->elr -= 4u;

	onebp_fired = 1;
	return 1;
}
