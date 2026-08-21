/* SPDX-License-Identifier: BSD-2-Clause */

/* gic_timer_qemu.c — GICv2 + ARM Generic Timer (CNTP/INTID 30) tick driver
 * for the QEMU `virt` CI target. See gic_timer_qemu.h for why this is a
 * separate file from gic_timer.c rather than an #ifdef inside it.
 *
 * ------------------------------------------------------------------
 * GICv2 MMIO bases — cited, not guessed: QEMU's hw/arm/virt.c
 * `a15memmap[]` (with `-machine virt,gic-version=2`, as this target's
 * documented invocation requests):
 *   VIRT_GIC_DIST = { 0x08000000, 0x00010000 }   (GICD)
 *   VIRT_GIC_CPU  = { 0x08010000, 0x00010000 }   (GICC)
 * Both are inside stage2.c's existing 1 GiB MMIO identity block
 * (0x00000000..0x40000000, Device-nGnRE — see stage2.h), unmodified, so no
 * stage-2 change is needed to reach them from the EL1 guest either.
 *
 * INTID 30 = the non-secure EL1 physical timer (CNTP): same PPI the
 * real-board driver uses, same reasoning (this is the timer non-secure
 * software is handed as a Group 1 interrupt on essentially every GICv2
 * platform, QEMU virt included).
 * ------------------------------------------------------------------
 */
#include <stdint.h>
#include "gic_timer_qemu.h"
#include "timer.h"
#include "cntpct.h"

#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

#define GICD_CTLR         (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4u * (n)))
/* GICD_IPRIORITYR is byte-addressable, one byte per interrupt ID. */
#define GICD_IPRIORITYR_BYTE(id) (*(volatile uint8_t *)(GICD_BASE + 0x400 + (id)))

#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile uint32_t *)(GICC_BASE + 0x00c))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

#define TIMER_INTID    30u
#define TIMER_PRIORITY 0x00u
#define GIC_SPURIOUS_MIN 1020u

static uint64_t g_period_ticks;
static uint64_t g_ticks;

static inline void
write_cntp_cval(uint64_t v)
{
	__asm__ volatile("msr cntp_cval_el0, %0" :: "r"(v) : "memory");
}

static inline void
write_cntp_ctl(uint32_t v)
{
	__asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)v) : "memory");
}

static inline uint64_t
read_cntpct(void)
{
	uint64_t v;
	__asm__ volatile("isb sy" ::: "memory");
	v = cntpct_read();   /* cntpct.h: Allwinner counter erratum */
	return v;
}

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

#define HCR_EL2_IMO (1ull << 4)

void
gic_timer_qemu_init(uint32_t period_us)
{
	uint64_t freq = timer_freq();

	/* Guard against a bogus/zero CNTFRQ_EL0 the same way timer.c's
	 * timer_delay_us() does, falling back to QEMU virt's typical default
	 * (62.5 MHz) rather than dividing by zero or spinning forever. */
	if (freq < 1000000ull || freq > 1000000000ull)
		freq = 62500000ull;

	g_period_ticks = (freq / 1000000ull) * (uint64_t)period_us;

	GICD_CTLR = 1u;                          /* enable distributor */
	GICD_IPRIORITYR_BYTE(TIMER_INTID) = TIMER_PRIORITY;
	GICD_ISENABLER(TIMER_INTID / 32u) = (1u << (TIMER_INTID % 32u));
	GICC_PMR  = 0xffu;                       /* priority mask wide open */
	GICC_CTLR = 1u;                          /* enable CPU interface */

	write_cntp_cval(read_cntpct() + g_period_ticks);
	write_cntp_ctl(1u);                      /* ENABLE=1, IMASK=0 */

	/* HCR_EL2.IMO: route physical IRQs to EL2 (read-modify-write, every
	 * other bit — in particular RW, set later by guest_config() —
	 * preserved untouched, same contract as gic_timer.c). */
	write_hcr_el2(read_hcr_el2() | HCR_EL2_IMO);
}

int
gic_timer_qemu_irq(struct el2_frame *frame)
{
	uint32_t iar = GICC_IAR;
	uint32_t id  = iar & 0x3ffu;

	(void)frame;

	if (id >= GIC_SPURIOUS_MIN)
		return 0;               /* spurious: nothing pending, no EOI */

	if (id != TIMER_INTID) {
		GICC_EOIR = iar;        /* not ours: EOI anyway, don't wedge the GIC */
		return 0;
	}

	write_cntp_cval(read_cntpct() + g_period_ticks);   /* re-arm next period */
	GICC_EOIR = iar;
	g_ticks++;
	return 1;
}

uint64_t
gic_timer_qemu_ticks(void)
{
	return g_ticks;
}
