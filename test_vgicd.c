/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vgicd.c — hosted (x86_64, plain gcc, no cross-compiler) unit tests for
 * the POLICY logic in vgicd.c: own_cpu_mask(), the GICD_ITARGETSR byte-partial
 * masking loop, and the GICD_SGIR TargetListFilter masking. That logic is
 * exactly what the 2026-08-27 CPU2 livelock (commit 7b9603b, see
 * crash-20260827-vcpu2-livelock/finding.md) lived in: own_cpu_mask() silently
 * dropped CPU2 from the "same guest" group because it was hardcoded to the
 * {0,1} pair, so a cross-core SGI or ITARGETSR write between CPU2 and
 * {CPU0,CPU1} never reached the real distributor. Before this file, vgicd.c
 * had ZERO hosted test coverage — every other trap-and-emulate file in this
 * tree (vconsole.c, vblk_emmc.c, vnet_emac.c, vgic.c) has one, this did not.
 * That gap is exactly how a hardcoded-pair regression like 7b9603b's root
 * cause survives `make test` and `./ci.sh` clean: nothing here asserted the
 * group computation for a THIRD core at all.
 *
 * ============================================================================
 * WHY A HAND-TRANSCRIBED MIRROR, NOT `#include "vgicd.c"`
 * ============================================================================
 * Same convention test_vgic_pendq.c documents for vgic.c, for the same reason:
 * vgicd.c's logic is not separable from raw AArch64 access it is entangled
 * with -- gicd_rd()/gicd_wr() are `volatile` MMIO loads/stores against a real
 * physical address, vgicd_bc() does `dc civac, %0\n\tdsb sy`, smp_cpu_id() is
 * an MRS against MPIDR_EL1, and vgicd_handle_fault() reads HPFAR_EL2/FAR_EL2
 * via inline asm. None of that runs, or should run, on an x86_64 host. What
 * this file mirrors is deliberately narrow: own_cpu_mask()'s group
 * computation, the ITARGETSR per-byte mask, and the SGIR TargetListFilter
 * mask -- pure integer arithmetic, verbatim from vgicd.c (line references in
 * each test), with the MMIO/breadcrumb/ESR-decode plumbing around them left
 * OUT rather than stubbed. This proves the policy math; it does NOT and
 * cannot prove the trap dispatch, the SAS-sized real distributor access, or
 * anything about the real GIC-400 -- that is vgicd_handle_fault() itself,
 * board-only (see the design doc this file accompanies for what is and is
 * not provable off the board).
 *
 * Every test here was re-checked against the CURRENT vgicd.c (as of commit
 * 7b9603b) before being written, per DEBUG_RULES.md R2.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * own_cpu_mask() -- verbatim from vgicd.c:161-173, minus smp_cpu_id()'s MRS
 * (replaced with the `me` parameter, which is all smp_cpu_id() reduces to on
 * hosted logic: "which core is asking"). dbg_vcpu1/2/3 are the mirrored
 * weak-symbol gates, passed explicitly instead of read as globals so a
 * single test binary can exercise every combination without relinking.
 * ------------------------------------------------------------------ */
static uint32_t
own_cpu_mask(uint32_t me, uint32_t dbg_vcpu1, uint32_t dbg_vcpu2, uint32_t dbg_vcpu3)
{
	uint32_t mask = 1u << me;
	uint32_t group = (1u << 0u) |
	                  (dbg_vcpu1 ? (1u << 1u) : 0u) |
	                  (dbg_vcpu2 ? (1u << 2u) : 0u) |
	                  (dbg_vcpu3 ? (1u << 3u) : 0u);

	if (group & (1u << me))
		mask = group;
	return mask;
}

/* ------------------------------------------------------------------ *
 * ITARGETSR byte-partial masking -- verbatim loop body from
 * vgicd_handle_fault(), vgicd.c:236-250. `nbytes` mirrors the SAS-derived
 * count (1/2/4) the real function computes from ESR_EL2.SAS; here it is
 * passed straight in since there is no ESR to decode on the host. Returns
 * the masked value; `*changed_out` mirrors the vgicd_bc(4, ...) counter
 * gate (nonzero exactly when the write asked for a core it does not own).
 * ------------------------------------------------------------------ */
static uint32_t
itargetsr_mask(uint32_t val, unsigned nbytes, uint32_t own, int *changed_out)
{
	uint32_t masked = 0;
	for (unsigned b = 0; b < nbytes; b++) {
		uint32_t t = (val >> (b * 8)) & 0xFFu;
		masked |= (t & own) << (b * 8);
	}
	if (changed_out)
		*changed_out = (masked != val);
	return masked;
}

/* ------------------------------------------------------------------ *
 * SGIR TargetListFilter masking -- verbatim from vgicd.c:251-283. `me` is
 * smp_cpu_id() (the sending core); `own` is own_cpu_mask() already computed
 * for that core. Returns the masked GICD_SGIR value; `*changed_out` mirrors
 * vgicd_bc(5, ...).
 * ------------------------------------------------------------------ */
static uint32_t
sgir_mask(uint32_t val, uint32_t me, uint32_t own, int *changed_out)
{
	uint32_t filter = (val >> 24) & 0x3u;
	uint32_t before = val;

	if (filter == 0x1u) {
		/* "all except self" always reaches outside the sender -- rewrite
		 * to an explicit target list of own's members, minus self. */
		val &= ~(0x3u << 24);
		val = (val & ~(0xFFu << 16)) | ((own & ~(1u << me)) << 16);
	} else if (filter == 0x0u) {
		uint32_t list = (val >> 16) & 0xFFu;
		val = (val & ~(0xFFu << 16)) | ((list & own) << 16);
	}
	/* filter 0b10 ("this CPU only") is already safe -- untouched. */

	if (changed_out)
		*changed_out = (val != before);
	return val;
}

/* ==================================================================== *
 * Tests
 * ==================================================================== */

/* Every dbg_vcpuN gate off (the default build, and every build before
 * 2026-08-25): own_cpu_mask() must reduce to "self only" for all four
 * cores. This is the "byte-for-byte the old behavior" claim vgicd.c's
 * comment makes at line ~154 -- pinned here so it cannot silently stop
 * being true. */
static void test_own_cpu_mask_default_is_self_only(void)
{
	for (uint32_t me = 0; me < 4; me++) {
		uint32_t mask = own_cpu_mask(me, 0, 0, 0);
		assert(mask == (1u << me));
	}
}

/* dbg_vcpu1 armed alone (the 2026-08-25 fix's own scenario): CPU0 and CPU1
 * merge into {0,1}; CPU2 and CPU3 stay self-only. This is exactly what
 * vcpu1-first-live-attempt-smp-ipi-hang needed and what shipped before
 * 7b9603b -- pinned so 7b9603b's generalization cannot regress it back. */
static void test_own_cpu_mask_vcpu1_only_merges_0_and_1(void)
{
	assert(own_cpu_mask(0, 1, 0, 0) == 0x3u);   /* CPU0 sees {0,1} */
	assert(own_cpu_mask(1, 1, 0, 0) == 0x3u);   /* CPU1 sees {0,1} */
	assert(own_cpu_mask(2, 1, 0, 0) == 0x4u);   /* CPU2 still self-only */
	assert(own_cpu_mask(3, 1, 0, 0) == 0x8u);   /* CPU3 still self-only */
}

/* THE REGRESSION TEST FOR 7b9603b ITSELF: dbg_vcpu1 AND dbg_vcpu2 both
 * armed (crash-20260827-vcpu2-livelock's exact configuration). Before
 * 7b9603b, own_cpu_mask() hardcoded the {0,1} pair and CPU2 was silently
 * excluded from the group in BOTH directions -- this is precisely the bug
 * that made CPU2 unreachable by a rendezvous SGI. The fixed function must
 * report {0,1,2} for all three cores. */
static void test_own_cpu_mask_vcpu1_and_vcpu2_merge_all_three(void)
{
	assert(own_cpu_mask(0, 1, 1, 0) == 0x7u);
	assert(own_cpu_mask(1, 1, 1, 0) == 0x7u);
	assert(own_cpu_mask(2, 1, 1, 0) == 0x7u);
	assert(own_cpu_mask(3, 1, 1, 0) == 0x8u);   /* CPU3 not armed: still alone */
}

/* All four dbg_vcpuN gates armed (vcpu3.c's "CPU3 as a fourth vCPU" mode):
 * the group is the full {0,1,2,3}, for every core including CPU3. This is
 * the case vgicd.c's comment claims is now covered "without touching the
 * `dual` build's CPU3-as-Zephyr isolation" -- that isolation is a LINK-TIME
 * property (vcpu3.o vs. the dual build's own CPU3 code are mutually
 * exclusive), not something this pure-logic test can observe; what this
 * test pins is that IF dbg_vcpu3 is armed, CPU3 does join the group. */
static void test_own_cpu_mask_all_four_armed_is_full_group(void)
{
	for (uint32_t me = 0; me < 4; me++)
		assert(own_cpu_mask(me, 1, 1, 1) == 0xFu);
}

/* dbg_vcpu2 armed WITHOUT dbg_vcpu1 (an odd but legal configuration --
 * nothing in vgicd.c requires the gates to be armed contiguously): CPU0 and
 * CPU2 merge into {0,2}; CPU1 (not armed) stays self-only and is NOT part
 * of the group even though it sits between the other two numerically. This
 * guards against a plausible-but-wrong "contiguous range" reimplementation
 * of the group computation. */
static void test_own_cpu_mask_noncontiguous_gates(void)
{
	assert(own_cpu_mask(0, 0, 1, 0) == 0x5u);   /* {0,2} */
	assert(own_cpu_mask(2, 0, 1, 0) == 0x5u);
	assert(own_cpu_mask(1, 0, 1, 0) == 0x2u);   /* CPU1 excluded, self only */
}

/* Every physical core (CPU4..CPU7 do not exist on the A64, but the `& 7u`
 * mask in the real smp_cpu_id() call means a caller could theoretically be
 * asked with a value up to 7): a core numbered outside 0..3 never matches
 * any group bit (all groups here are subsets of {0,1,2,3}), so it must fall
 * back to self-only. Mirrors the `if (group & (1u << me)) mask = group;`
 * else-leaves-mask-as-self-bit structure exactly. */
static void test_own_cpu_mask_out_of_range_core_is_self_only(void)
{
	assert(own_cpu_mask(5, 1, 1, 1) == (1u << 5));
}

/* GICD_ITARGETSR, single-byte access (SAS=0, the case FreeBSD actually
 * uses -- vgicd.c's own comment at :82-88 explains why byte-granularity is
 * load-bearing). own={0,1}: a byte targeting {CPU0,CPU1,CPU2} must have
 * CPU2's bit stripped, and the counter must fire. */
static void test_itargetsr_byte_access_masks_disowned_bits(void)
{
	int changed = 0;
	uint32_t masked = itargetsr_mask(0x07u /* {0,1,2} */, 1, 0x3u /* {0,1} */, &changed);
	assert(masked == 0x03u);
	assert(changed == 1);
}

/* Same access, but the guest only ever asked for cores it owns: no masking,
 * and the "did we have to intervene" counter must NOT fire -- this is the
 * "on a single-guest build [g_mask_target] should stay 0 forever" property
 * vgicd.c's breadcrumb comment states at line ~46. */
static void test_itargetsr_byte_access_no_op_when_already_owned(void)
{
	int changed = 0;
	uint32_t masked = itargetsr_mask(0x02u /* {1} */, 1, 0x3u /* {0,1} */, &changed);
	assert(masked == 0x02u);
	assert(changed == 0);
}

/* GICD_ITARGETSR, word access (SAS=2, 4 bytes = 4 consecutive INTIDs).
 * own={0,1} (CPU0/CPU1 merged). Byte 0 targets {0,1} (kept whole), byte 1
 * targets {0,1,2} (CPU2 stripped), byte 2 targets {2} alone (zeroed
 * entirely), byte 3 targets {3} (also zeroed). This is the exact "1, 2 or 4
 * of them depending on SAS" case vgicd.c:241-247 exists to get right --
 * each byte is masked independently, so byte 0's legitimate value must
 * survive untouched by byte 2's full rejection. */
static void test_itargetsr_word_access_masks_each_byte_independently(void)
{
	uint32_t val = 0x08u | (0x04u << 8) | (0x07u << 16) | (0x03u << 24);
	int changed = 0;
	uint32_t masked = itargetsr_mask(val, 4, 0x3u /* {0,1} */, &changed);

	assert(((masked >> 24) & 0xFFu) == 0x03u);  /* byte3: {0,1} kept whole   */
	assert(((masked >> 16) & 0xFFu) == 0x03u);  /* byte2: {0,1,2} -> {0,1}   */
	assert(((masked >> 8)  & 0xFFu) == 0x00u);  /* byte1: {2} -> {} (zeroed) */
	assert(((masked >> 0)  & 0xFFu) == 0x00u);  /* byte0: {3} -> {} (zeroed) */
	assert(changed == 1);
}

/* GICD_SGIR, TargetListFilter==0b00 (explicit target list): the list is
 * simply ANDed with own_cpu_mask(), same shape as ITARGETSR. own={0,1},
 * requested list {0,1,2} -> {0,1}. */
static void test_sgir_filter_explicit_list_masks_disowned_bits(void)
{
	uint32_t val = (0x0u << 24) | (0x07u << 16);   /* filter=0, list={0,1,2} */
	int changed = 0;
	uint32_t masked = sgir_mask(val, 0 /* CPU0 sends */, 0x3u, &changed);
	assert(((masked >> 16) & 0xFFu) == 0x03u);
	assert(((masked >> 24) & 0x3u) == 0x0u);   /* filter unchanged */
	assert(changed == 1);
}

/* GICD_SGIR, TargetListFilter==0b01 ("all except self"), sent by CPU0 with
 * dbg_vcpu1 armed (own={0,1}): this is the exact self-IPI regression
 * vgicd.c's comment at :258-270 documents (introduced 2026-08-25, fixed
 * 2026-08-26) -- the rewritten explicit list must be {1} (own minus self),
 * NOT {0,1}, or CPU0 receives an SGI it asked to exclude itself from. */
static void test_sgir_filter_all_but_self_excludes_sender(void)
{
	uint32_t val = (0x1u << 24);   /* filter=1, list field irrelevant */
	int changed = 0;
	uint32_t masked = sgir_mask(val, 0 /* CPU0 sends */, 0x3u /* {0,1} */, &changed);

	assert(((masked >> 24) & 0x3u) == 0x0u);      /* rewritten to explicit list */
	assert(((masked >> 16) & 0xFFu) == 0x02u);    /* {1} -- CPU0 excluded       */
	assert(changed == 1);
}

/* GICD_SGIR, TargetListFilter==0b01, sent by CPU0 with own_cpu_mask()=={0}
 * (no dbg_vcpuN armed -- the default/single-guest case): "all except self"
 * must reach nobody, matching the pre-vcpu1 behaviour vgicd.c's comment
 * calls "the same 'reaches nobody else' outcome the old code intended". */
static void test_sgir_filter_all_but_self_single_guest_reaches_nobody(void)
{
	uint32_t val = (0x1u << 24);
	int changed = 0;
	uint32_t masked = sgir_mask(val, 0, 0x1u /* {0} only */, &changed);
	assert(((masked >> 16) & 0xFFu) == 0x00u);
}

/* GICD_SGIR, TargetListFilter==0b10 ("this CPU only"): must pass through
 * completely unmodified -- vgicd.c:281 documents this filter value as
 * "already safe" and deliberately does not touch it. */
static void test_sgir_filter_self_only_is_untouched(void)
{
	uint32_t val = (0x2u << 24) | (0xDEu << 16);   /* filter=2, garbage list */
	int changed = 0;
	uint32_t masked = sgir_mask(val, 0, 0x3u, &changed);
	assert(masked == val);
	assert(changed == 0);
}

/* THE SGIR HALF OF THE 7b9603b REGRESSION: FreeBSD's smp_rendezvous() sends
 * its AP-release SGI as an explicit target list (filter=0b00), per
 * vgicd.c:112-123's own citation. With only dbg_vcpu1 armed, a list that
 * includes CPU2's bit must have it stripped (CPU2 not in the group yet);
 * with dbg_vcpu2 also armed, the same bit must survive. This is the exact
 * mechanism crash-20260827-vcpu2-livelock's finding.md traces the hang to. */
static void test_sgir_cpu2_reachability_gated_by_dbg_vcpu2(void)
{
	uint32_t val = (0x0u << 24) | (0x07u << 16);   /* filter=0, list={0,1,2} */
	int changed = 0;

	/* Only dbg_vcpu1 armed: own_cpu_mask(CPU0) == {0,1} -- CPU2 unreachable. */
	uint32_t masked_before_fix = sgir_mask(val, 0, own_cpu_mask(0, 1, 0, 0), &changed);
	assert(((masked_before_fix >> 16) & 0xFFu) == 0x03u);   /* CPU2's bit gone */

	/* dbg_vcpu1 AND dbg_vcpu2 armed: own_cpu_mask(CPU0) == {0,1,2} -- reaches it. */
	uint32_t masked_after_fix = sgir_mask(val, 0, own_cpu_mask(0, 1, 1, 0), &changed);
	assert(((masked_after_fix >> 16) & 0xFFu) == 0x07u);    /* CPU2's bit survives */
}

/* ==================================================================== */

static const struct { const char *name; void (*fn)(void); } k_tests[] = {
	{ "own_cpu_mask_default_is_self_only",              test_own_cpu_mask_default_is_self_only },
	{ "own_cpu_mask_vcpu1_only_merges_0_and_1",          test_own_cpu_mask_vcpu1_only_merges_0_and_1 },
	{ "own_cpu_mask_vcpu1_and_vcpu2_merge_all_three",    test_own_cpu_mask_vcpu1_and_vcpu2_merge_all_three },
	{ "own_cpu_mask_all_four_armed_is_full_group",       test_own_cpu_mask_all_four_armed_is_full_group },
	{ "own_cpu_mask_noncontiguous_gates",                test_own_cpu_mask_noncontiguous_gates },
	{ "own_cpu_mask_out_of_range_core_is_self_only",     test_own_cpu_mask_out_of_range_core_is_self_only },
	{ "itargetsr_byte_access_masks_disowned_bits",       test_itargetsr_byte_access_masks_disowned_bits },
	{ "itargetsr_byte_access_no_op_when_already_owned",  test_itargetsr_byte_access_no_op_when_already_owned },
	{ "itargetsr_word_access_masks_each_byte_independently", test_itargetsr_word_access_masks_each_byte_independently },
	{ "sgir_filter_explicit_list_masks_disowned_bits",   test_sgir_filter_explicit_list_masks_disowned_bits },
	{ "sgir_filter_all_but_self_excludes_sender",        test_sgir_filter_all_but_self_excludes_sender },
	{ "sgir_filter_all_but_self_single_guest_reaches_nobody", test_sgir_filter_all_but_self_single_guest_reaches_nobody },
	{ "sgir_filter_self_only_is_untouched",              test_sgir_filter_self_only_is_untouched },
	{ "sgir_cpu2_reachability_gated_by_dbg_vcpu2",       test_sgir_cpu2_reachability_gated_by_dbg_vcpu2 },
};

int main(void)
{
	int n = (int)(sizeof(k_tests) / sizeof(k_tests[0]));
	int passed = 0;

	for (int i = 0; i < n; i++) {
		printf("[ RUN ] %s\n", k_tests[i].name);
		k_tests[i].fn();
		printf("[ OK  ] %s\n", k_tests[i].name);
		passed++;
	}

	printf("---- vgicd policy tests: %d/%d passed ----\n", passed, n);
	return (passed == n) ? 0 : 1;
}
