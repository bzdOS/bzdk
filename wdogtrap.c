/* SPDX-License-Identifier: BSD-2-Clause */

/* wdogtrap.c — trap-and-police the CCU/PIO/WDOG page. See wdogtrap.h for why.
 *
 * Structure deliberately mirrors vgicd.c (itself mirroring vconsole.c): same
 * ESR decode, same HPFAR_EL2 + FAR_EL2 IPA reconstruction, same "return 0 if
 * it is not our page" contract, same `frame->elr += 4` on the way out.
 * Copying that shape rather than inventing one keeps every trap handler in
 * this tree auditable side by side.
 */

#include <stdint.h>
#include "hv_addrmap.h"
#include "exceptions.h"
#include "smp.h"     /* smp_cpu_id() */
#include "wdogtrap.h"

/* ESR_EL2 ISS fields for a Data Abort — identical names/values vgicd.c and
 * vconsole.c already use for the same decode. */
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

/* ------------------------------------------------------------------ *
 * Breadcrumbs (HVMAP_WDOGTRAP_BC). Field layout is documented at the
 * #define site in hv_addrmap.h.
 * ------------------------------------------------------------------ */
#define WDOGTRAP_BC_MAGIC 0x57445047u   /* "WDPG" */

static inline void wdogtrap_bc(unsigned i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(HVMAP_WDOGTRAP_BC + i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

#define WDOGTRAP_BC_NWORDS 10u
_Static_assert(WDOGTRAP_BC_NWORDS * 4u <= HVMAP_WDOGTRAP_BC_SIZE,
               "wdogtrap.c writes more breadcrumb slots than HVMAP_WDOGTRAP_BC_SIZE reserves");

static uint32_t g_inited;
static uint32_t g_total, g_reads, g_writes, g_wdog_refused, g_pc5_reforced, g_isv0;

static void wdogtrap_init_once(void)
{
	if (g_inited)
		return;
	g_inited = 1;
	for (unsigned i = 0; i < WDOGTRAP_BC_NWORDS; i++)
		wdogtrap_bc(i, 0);
	wdogtrap_bc(0, WDOGTRAP_BC_MAGIC);
}

/* ------------------------------------------------------------------ *
 * Real page access. EL2's own loads/stores are not subject to stage 2 (see
 * wdogtrap.h's header comment), so this reaches the hardware directly even
 * though the guest's identical access just faulted.
 *
 * SIZED, honouring ESR.SAS rather than rounding to a word — same reasoning
 * vgicd.c's gicd_rd/gicd_wr give: PIO's config registers are legitimately
 * accessed at sub-word granularity by real pinctrl drivers, and a mis-sized
 * emulated access would corrupt neighbouring fields exactly like the
 * ITARGETSR bug that comment describes.
 * ------------------------------------------------------------------ */
static uint32_t page_rd(uint32_t off, uint32_t sas)
{
	volatile void *p = (volatile void *)(WDOGTRAP_PAGE_BASE + off);
	switch (sas) {
	case 0: return *(volatile uint8_t *)p;
	case 1: return *(volatile uint16_t *)p;
	default: return *(volatile uint32_t *)p;
	}
}

static void page_wr(uint32_t off, uint32_t v, uint32_t sas)
{
	volatile void *p = (volatile void *)(WDOGTRAP_PAGE_BASE + off);
	switch (sas) {
	case 0: *(volatile uint8_t *)p  = (uint8_t)v;  break;
	case 1: *(volatile uint16_t *)p = (uint16_t)v; break;
	default: *(volatile uint32_t *)p = v;          break;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Offsets, relative to WDOGTRAP_PAGE_BASE, of the two classified sub-ranges.
 * Everything else on the page is the passthrough default (see wdogtrap.h
 * §"WHAT THIS DOES", case 3). */
#define WDOG_OFF_BASE  ((uint32_t)(SOC_A64_WDOG_NODE_BASE - WDOGTRAP_PAGE_BASE))
#define WDOG_OFF_END   (WDOG_OFF_BASE + (uint32_t)SOC_A64_WDOG_NODE_SIZE)
#define PC_CFG0_OFF    ((uint32_t)(SOC_A64_PIO_PC_CFG0 - WDOGTRAP_PAGE_BASE))
/* The one BYTE of PC_CFG0 that carries PC5's function field: nibble 5
 * (bits[23:20]) is the upper nibble of byte 2 (bits[23:16]) of the register,
 * i.e. PC_CFG0_OFF+2. Isolated so the write-path below can force it
 * correctly regardless of whether the guest's access is a byte, halfword or
 * word store — see that code's comment for why an exact-offset-only check
 * would miss a narrower store landing on just this byte. */
#define PC5_BYTE_OFF   (PC_CFG0_OFF + 2u)

_Static_assert(WDOG_OFF_BASE == 0xCA0u,
               "WDOG node offset drifted from the live-DTB-verified 0xCA0 -- "
               "re-derive against bananapi-min.dtb before trusting this trap");
_Static_assert(WDOG_OFF_END == 0xCC0u,
               "WDOG node span drifted -- re-check SOC_A64_WDOG_NODE_SIZE");
_Static_assert(PC_CFG0_OFF == 0x848u,
               "PIO_PC_CFG0 offset drifted from the verified 0x848");
_Static_assert(PC5_BYTE_OFF == 0x84Au,
               "PC5's byte offset arithmetic drifted");

int wdogtrap_handle_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec  = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	/* Same IPA reconstruction vconsole.c/vgicd.c document: FAR_EL2 is only
	 * good for the low 12 bits once the guest's stage-1 MMU is on, so the
	 * page comes from HPFAR_EL2 bits[39:4] = IPA[47:12]. */
	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	if (addr < WDOGTRAP_PAGE_BASE || addr >= WDOGTRAP_PAGE_BASE + WDOGTRAP_PAGE_SIZE)
		return 0;                       /* not ours — keep looking */

	wdogtrap_init_once();

	uint32_t off = (uint32_t)(addr - WDOGTRAP_PAGE_BASE);
	wdogtrap_bc(1, ++g_total);
	wdogtrap_bc(6, off);
	wdogtrap_bc(9, smp_cpu_id());

	/* ISV==0: no register/size info, cannot emulate faithfully. Same fake-a-
	 * benign-completion fallback vconsole.c/vgicd.c use, same reasoning:
	 * leaving the guest to re-fault forever on an unemulatable access is
	 * worse than a counted, visible fib. */
	if (!(esr & ESR_ISV_BIT)) {
		wdogtrap_bc(8, ++g_isv0);
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;

	/* ---- THE POLICY ----
	 *
	 * WDOG range FIRST, unconditionally, with a hard return on any match —
	 * reads included. This is deliberate, not merely "the natural order":
	 * it means a later refactor that adds more classified ranges to this
	 * function (another PIO field, a CCU gate someone decides needs
	 * policing) cannot silently reorder past this check and fall through to
	 * the passthrough default for a WDOG offset. Do not turn this into a
	 * table/switch that a later case could be inserted ahead of — the
	 * refuse-and-return must stay the first thing this function does after
	 * the ISV check above.
	 *
	 * A plain start-offset range check (no nbytes/overlap math, unlike the
	 * PC5 check below) is CORRECT here, not an oversight: Device-nGnRE
	 * memory guarantees an unaligned access always takes an alignment
	 * fault (ARM ARM), so every access this handler ever sees is naturally
	 * aligned to its own size. WDOG_OFF_BASE/END are both 4-byte aligned
	 * (0xCA0, 0xCC0), so no naturally-aligned 1/2/4-byte access can straddle
	 * either boundary — unlike PC5, which is a sub-register FIELD in the
	 * middle of a 4-byte register and can be hit by a validly-aligned access
	 * that does not start at the register's own base. */
	if (off >= WDOG_OFF_BASE && off < WDOG_OFF_END) {
		if (!wnr) {
			/* Reads pass through — see wdogtrap.h's policy comment for why
			 * this is not a hole: nothing the guest can act on, since every
			 * write below is refused regardless of what a read revealed. */
			uint32_t v = page_rd(off, sas);
			if (srt != SRT_XZR)
				frame->x[srt] = (uint64_t)v;
			wdogtrap_bc(2, ++g_reads);
			frame->elr += 4;
			return 1;
		}
		/* Write: REFUSED. Absorbed as a no-op (mmio_absorb.c's own
		 * "write-as-noop" contract) rather than propagated — the guest's
		 * store instruction completes normally from its point of view, but
		 * nothing on the real WDOG_IRQ_EN/STA/CTRL/CFG/MODE registers
		 * changes. Counted loudly: g_wdog_refused must stay 0 on a
		 * correctly-behaving guest, and any nonzero value here is exactly
		 * the signal this whole file exists to make visible. */
		wdogtrap_bc(4, ++g_wdog_refused);
		wdogtrap_bc(7, (srt == SRT_XZR) ? 0u : (uint32_t)frame->x[srt]);
		frame->elr += 4;
		return 1;
	}

	if (!wnr) {
		uint32_t v = page_rd(off, sas);
		if (srt != SRT_XZR)
			frame->x[srt] = (uint64_t)v;
		wdogtrap_bc(2, ++g_reads);
		frame->elr += 4;
		return 1;
	}

	uint32_t val = (srt == SRT_XZR) ? 0u : (uint32_t)frame->x[srt];

	/* PC5 (bits[23:20] of PIO_PC_CFG0, i.e. the upper nibble of byte
	 * PC5_BYTE_OFF) is forced to function 3 (mmc2) regardless of what the
	 * guest wrote there — this is the field the PC5 reassertion triplicate
	 * (main_dbg.c, smp.c, gic_timer.c) exists to fight after the fact;
	 * forcing it here means FreeBSD's pinctrl driver literally cannot clear
	 * it, no matter how many times it tries. Every other nibble in the
	 * register (every other PC pin's function field) passes through exactly
	 * as the guest wrote it — this is NOT a PIO-wide policy, only this one
	 * field. See wdogtrap.h's policy comment, case 2.
	 *
	 * Checked against the access's actual BYTE RANGE, not just "off ==
	 * PC_CFG0_OFF": a real pinctrl driver is expected to RMW the whole
	 * 32-bit register (the case `bit_in_val==16` below covers), but nothing
	 * stops a narrower byte/halfword store from landing on just
	 * PC5_BYTE_OFF, and an offset-equality check alone would let that one
	 * through unmasked -- silently reopening exactly the hole this trap
	 * exists to close. */
	unsigned nbytes = (sas == 0) ? 1u : (sas == 1) ? 2u : 4u;
	if (off < PC5_BYTE_OFF + 1u && off + nbytes > PC5_BYTE_OFF) {
		unsigned bit_in_val = (PC5_BYTE_OFF - off) * 8u;
		uint32_t before = val;
		val = (val & ~(0xFu << (bit_in_val + 4u))) | (3u << (bit_in_val + 4u));
		if (val != before)
			wdogtrap_bc(5, ++g_pc5_reforced);
	} else {
		wdogtrap_bc(3, ++g_writes);
	}

	page_wr(off, val, sas);
	wdogtrap_bc(7, val);
	frame->elr += 4;
	return 1;
}
