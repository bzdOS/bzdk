/* SPDX-License-Identifier: BSD-2-Clause */

/* main_stage0.c — bzdOS microkernel Stage-0 self-test.
 *
 * Proves the U-Boot load/exec/return chain works with NO USB involved:
 * writes a known signature + incrementing pattern to a fixed physical
 * scratch address. The host then does, in U-Boot:
 *     go 0x42000000
 *     md.l 0x42010000 10
 * and checks for 0xB2D05000 followed by 0xB2D05001..0xB2D05008. A clean
 * return to the U-Boot prompt (via start.S) additionally proves the ABI
 * contract (callee-saved regs preserved, stack untouched, MMU untouched).
 *
 * Freestanding: no libc, only <stdint.h>.
 */
#include <stdint.h>

#define STAGE0_SCRATCH_BASE 0x42010000u
#define STAGE0_SIGNATURE    0xB2D05000u
#define STAGE0_PATTERN_LEN  8

int main(void)
{
	volatile uint32_t *p = (volatile uint32_t *)STAGE0_SCRATCH_BASE;

	/* Word 0: signature. */
	p[0] = STAGE0_SIGNATURE;

	/* Words 1..8: incrementing pattern 0xB2D05001..0xB2D05008. */
	for (int i = 0; i < STAGE0_PATTERN_LEN; i++)
		p[1 + i] = STAGE0_SIGNATURE + 1u + (uint32_t)i;

	return 0;
}
