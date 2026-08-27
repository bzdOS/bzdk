/* SPDX-License-Identifier: BSD-2-Clause */

/* vgicd.c — trap-and-police the GIC distributor. See vgicd.h for why.
 *
 * Structure deliberately mirrors vconsole.c's MMIO fault handler: same ESR
 * decode, same HPFAR_EL2 + FAR_EL2 IPA reconstruction, same "return 0 if it is
 * not our page" contract, same `frame->elr += 4` on the way out. Copying that
 * shape rather than inventing one keeps the two trap handlers auditable side by
 * side, and it is already hardware-proven for the UART page.
 */

#include <stdint.h>
#include "hv_addrmap.h"
#include "exceptions.h"
#include "smp.h"     /* smp_cpu_id() */
#include "vgicd.h"

/* ESR_EL2 ISS fields for a Data Abort, same names/values vconsole.c uses. */
#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3Fu
#define ESR_EC_DABT_LOWER 0x24u   /* data abort from a lower EL */
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SAS_SHIFT     22
#define ESR_SAS_MASK      0x3u
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u     /* SRT==31 is the zero register, not x[31] */

/* smp_cpu_id() is a static inline over MPIDR_EL1 affinity 0 (smp.h), not an
 * exported symbol -- include the header rather than declaring it extern. */

/* ------------------------------------------------------------------ *
 * Breadcrumbs (HVMAP_VGICD_BC). Published as REAL zeros at first use so a
 * reader can tell "never happened" from "this build has no such counter" —
 * the window reads back 0xFFFFFFFF until written, and that ambiguity has
 * cost this project real time before.
 *
 *   [0] magic "VGCD"
 *   [1] total GICD accesses trapped
 *   [2] reads passed through
 *   [3] writes passed through unmodified
 *   [4] ITARGETSR writes whose affinity field was MASKED (the interesting one:
 *       nonzero means a guest tried to target a core it does not own)
 *   [5] SGIR writes whose target list was masked
 *   [6] last offset faulted
 *   [7] last value written (post-mask)
 *   [8] accesses with ISV==0 (no register info; see the comment at that site)
 *   [9] last CPU id that faulted here
 * ------------------------------------------------------------------ */
#define VGICD_BC_MAGIC 0x56474344u   /* "VGCD" */

static inline void vgicd_bc(unsigned i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_VGICD_BC + i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

#define VGICD_BC_NWORDS 10u
_Static_assert(VGICD_BC_NWORDS * 4u <= HVMAP_VGICD_BC_SIZE,
               "vgicd.c writes more breadcrumb slots than HVMAP_VGICD_BC_SIZE reserves");

static uint32_t g_inited;
static uint32_t g_total, g_reads, g_writes, g_mask_target, g_mask_sgi, g_isv0;

static void vgicd_init_once(void)
{
	if (g_inited)
		return;
	g_inited = 1;
	for (unsigned i = 0; i < VGICD_BC_NWORDS; i++)
		vgicd_bc(i, 0);
	vgicd_bc(0, VGICD_BC_MAGIC);
}

/* ------------------------------------------------------------------ *
 * Real distributor access. EL2's own loads/stores are not subject to stage 2,
 * so this reaches the hardware directly even though the guest's identical
 * access just faulted.
 * ------------------------------------------------------------------ */
/* SIZED access. This MUST honour ESR.SAS rather than rounding to a word:
 * GICD_ITARGETSR and GICD_IPRIORITYR are BYTE-addressable, one byte per INTID,
 * and FreeBSD does write them a byte at a time. An earlier draft of this file
 * did `off & ~3u` with a 32-bit access, which would have taken a guest's
 * single-byte write to INTID N and splattered it across the word -- silently
 * rewriting the targets of INTIDs N-1..N+2. That is the exact shape of bug this
 * tree has been bitten by before, so the size decode is not optional. */
static uint32_t gicd_rd(uint32_t off, uint32_t sas)
{
	volatile void *p = (volatile void *)(VGICD_BASE + off);
	switch (sas) {
	case 0: return *(volatile uint8_t *)p;
	case 1: return *(volatile uint16_t *)p;
	default: return *(volatile uint32_t *)p;
	}
}

static void gicd_wr(uint32_t off, uint32_t v, uint32_t sas)
{
	volatile void *p = (volatile void *)(VGICD_BASE + off);
	switch (sas) {
	case 0: *(volatile uint8_t *)p  = (uint8_t)v;  break;
	case 1: *(volatile uint16_t *)p = (uint16_t)v; break;
	default: *(volatile uint32_t *)p = v;          break;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* CORRECTED 2026-08-25 (confirmed live): "the faulting core's own affinity
 * bit, nothing else" was right for static partitioning (each PHYSICAL core
 * is its own guest, so cross-core == cross-guest, always), but it is wrong
 * the moment CPU1 runs as a SECOND vCPU of the SAME guest as CPU0
 * (vcpu1.c/vcpu1.h) rather than a separate guest. FreeBSD's own SMP
 * AP-release rendezvous (smp_rendezvous(), right after "Release APs...")
 * sends a real inter-processor SGI from the BSP (CPU0) to the AP (CPU1)
 * via GICD_SGIR; this function's old masking reduced that write's target
 * list to CPU0's own bit, so CPU1 never received it and both cores hung
 * forever waiting on a rendezvous that could never complete — reproduced
 * and root-caused live, 2026-08-25 (see vcpu1-first-live-attempt-smp-ipi-
 * hang memory). The SAME masking also applies to GICD_ITARGETSR below, so
 * a cross-core interrupt-affinity write between CPU0 and CPU1 was equally
 * broken, not just SGIs.
 *
 * EXTENDED 2026-08-27 (crash-20260827-vcpu2-livelock): the fix above only
 * ever merged CPU0/CPU1, hardcoded, because CPU2 was not a guest vCPU yet
 * when it was written. It has been one since `vcpu2.c` started calling
 * `vgic_init()` (commit 3a5e5e2), and this function was never told: with
 * `dbg_vcpu1` and `dbg_vcpu2` both armed, a rendezvous SGI or an
 * ITARGETSR write issued by CPU0 (or CPU1) still computed
 * `own_cpu_mask() == {0,1}` — CPU2's bit was stripped from the target list
 * before it ever reached the real distributor, in BOTH directions between
 * CPU2 and {CPU0,CPU1}. That reproduces the exact measured signature: CPU2
 * never receives (or can send) a cross-core IPI, so it sits outside the
 * rendezvous entirely and just keeps taking its own local CNTV ticks at a
 * normal, healthy rate forever (~1174/s measured — FreeBSD's ordinary
 * per-CPU clock load, not a storm), while {CPU0,CPU1} block inside
 * `smp_rendezvous_action()` waiting for an ack from a core that was never
 * asked. See crash-20260827-vcpu2-livelock/finding.md for the measurement
 * that led here; the "livelock" it describes on CPU2 is this core running
 * completely normally, just cut off from the other two.
 *
 * Generalized rather than hardcoding a third case: the "same guest" group is
 * now every core whose own dbg_vcpuN gate is armed (CPU0 is always in it —
 * it is the guest's vCPU0 unconditionally), each read through a weak symbol
 * (0 in any build that does not link that core's vcpuN.o) exactly like
 * dbg_vcpu1 already was. This covers vcpu3.c's CPU3-as-a-fourth-vCPU mode
 * too, WITHOUT touching the `dual` build's CPU3-as-Zephyr isolation: the two
 * are link-time exclusive (vcpu3.c's own header), so in a `dual` build
 * vcpu3.o is never linked, dbg_vcpu3 stays weak-0, and CPU3 keeps a
 * self-only mask exactly as before — the isolation this file exists for is
 * untouched.
 *
 * When no dbg_vcpuN beyond CPU0 itself is armed (the default build, and
 * every build that predates this change), `group` reduces to `{0}` and this
 * function is byte-for-byte the old behavior for every core. */
__attribute__((weak)) volatile uint32_t dbg_vcpu1;
__attribute__((weak)) volatile uint32_t dbg_vcpu2;
__attribute__((weak)) volatile uint32_t dbg_vcpu3;

static inline uint32_t own_cpu_mask(void)
{
	uint32_t me = smp_cpu_id() & 7u;
	uint32_t mask = 1u << me;
	uint32_t group = (1u << 0u) |
	                  (dbg_vcpu1 ? (1u << 1u) : 0u) |
	                  (dbg_vcpu2 ? (1u << 2u) : 0u) |
	                  (dbg_vcpu3 ? (1u << 3u) : 0u);

	if (group & (1u << me))
		mask = group;
	return mask;
}

int vgicd_handle_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec  = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	/* Same IPA reconstruction vconsole.c documents: FAR_EL2 is only good for
	 * the low 12 bits once the guest's stage-1 MMU is on, so the page comes
	 * from HPFAR_EL2 bits[39:4] = IPA[47:12]. */
	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	if (addr < VGICD_BASE || addr >= VGICD_BASE + VGICD_SIZE)
		return 0;                       /* not ours — keep looking */

	vgicd_init_once();

	uint32_t off = (uint32_t)(addr - VGICD_BASE);
	vgicd_bc(1, ++g_total);
	vgicd_bc(6, off);
	vgicd_bc(9, smp_cpu_id());

	/* ISV==0 means the syndrome carries no register/size info, so the access
	 * cannot be emulated faithfully. vconsole.c hits the same case and fakes
	 * a benign completion rather than leaving the guest to re-fault forever.
	 * Do the same, and COUNT it — if [8] ever climbs, some guest access here
	 * is being silently mishandled and that needs to be visible, not assumed
	 * away. */
	if (!(esr & ESR_ISV_BIT)) {
		vgicd_bc(8, ++g_isv0);
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;

	if (!wnr) {
		/* Reads pass through untouched. A guest is allowed to SEE the whole
		 * distributor: knowing another partition's configuration is not the
		 * threat here, changing it is. */
		uint32_t v = gicd_rd(off, sas);
		if (srt != SRT_XZR)
			frame->x[srt] = (uint64_t)v;
		vgicd_bc(2, ++g_reads);
		frame->elr += 4;
		return 1;
	}

	uint32_t val = (srt == SRT_XZR) ? 0u : (uint32_t)frame->x[srt];

	/* ---- THE POLICY ---- Only two register ranges can reach across the
	 * partition boundary, and both are affinity fields. Mask them to the
	 * asking core rather than rejecting the write: rejecting would make the
	 * guest's own interrupt setup fail in ways it cannot diagnose, whereas
	 * masking gives it exactly what it is entitled to and nothing more. */

	if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR_END) {
		/* One BYTE per INTID. Mask each byte the access actually covers --
		 * 1, 2 or 4 of them depending on SAS -- down to the bits this core
		 * owns. Iterating a fixed 4 would corrupt the untouched bytes of a
		 * narrower access. */
		unsigned nbytes = (sas == 0) ? 1u : (sas == 1) ? 2u : 4u;
		uint32_t own = own_cpu_mask();
		uint32_t masked = 0;
		for (unsigned b = 0; b < nbytes; b++) {
			uint32_t t = (val >> (b * 8)) & 0xFFu;
			masked |= (t & own) << (b * 8);
		}
		if (masked != val)
			vgicd_bc(4, ++g_mask_target);
		val = masked;
	} else if (off == GICD_SGIR) {
		/* Bits[23:16] are the CPUTargetList when TargetListFilter (bits
		 * [25:24]) is 0b00. Filter 0b01 means "all except self", which by
		 * definition reaches the other partition, so force it to
		 * self-only. */
		uint32_t filter = (val >> 24) & 0x3u;
		uint32_t before = val;
		if (filter == 0x1u) {
			/* MUST exclude the sender. Rewriting "all except self" into an
			 * explicit target list of own_cpu_mask() was a real bug while
			 * dbg_vcpu1 is armed (introduced 2026-08-25, fixed 2026-08-26):
			 * own_cpu_mask() then returns {CPU0,CPU1}, so the SGI came back
			 * to the very core that asked for everyone BUT itself. An
			 * unexpected self-IPI is not harmless — FreeBSD's
			 * smp_rendezvous_action() would run a second, unrequested time
			 * on the initiator, against smp_rv_* state it did not expect to
			 * be re-entered for. With dbg_vcpu1 off own_cpu_mask() is just
			 * this core's own bit, so this clears the list to 0 — the same
			 * "reaches nobody else" outcome the old code intended, only now
			 * actually honouring the "except self" the guest asked for. */
			uint32_t me = smp_cpu_id() & 7u;

			val &= ~(0x3u << 24);                   /* -> use target list */
			val = (val & ~(0xFFu << 16)) |
			      ((own_cpu_mask() & ~(1u << me)) << 16);
		} else if (filter == 0x0u) {
			uint32_t list = (val >> 16) & 0xFFu;
			val = (val & ~(0xFFu << 16)) |
			      ((list & own_cpu_mask()) << 16);
		}
		/* filter 0b10 is "this CPU only" — already safe. */
		if (val != before)
			vgicd_bc(5, ++g_mask_sgi);
	} else {
		vgicd_bc(3, ++g_writes);
	}

	gicd_wr(off, val, sas);
	vgicd_bc(7, val);
	frame->elr += 4;
	return 1;
}
