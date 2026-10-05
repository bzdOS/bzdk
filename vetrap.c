/* SPDX-License-Identifier: BSD-2-Clause */
/* vetrap.c — see vetrap.h. */
#include <stdint.h>
#include "hv_addrmap.h"
#include "exceptions.h"
#include "smp.h"
#include "vetrap.h"

#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3Fu
#define ESR_EC_DABT_LOWER 0x24u
#define ESR_ISV_BIT       (1u << 24)
#define ESR_SAS_SHIFT     22
#define ESR_SAS_MASK      0x3u
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)
#define SRT_XZR           31u

const unsigned long vetrap_pages[VETRAP_NPAGES] = {
	VETRAP_PAGE_BASE,
#if HV_VETRAP >= 2
	0x01C00000UL, 0x01C20000UL, 0x01C62000UL,
#endif
};

#define VE_RING_N ((HVMAP_VETRAP_SIZE - 0x40u) / 8u)

static volatile uint32_t *hdr = (volatile uint32_t *)HVMAP_VETRAP;
static volatile uint32_t *ent = (volatile uint32_t *)(HVMAP_VETRAP + 0x40u);
static uint32_t inited;

static void init_once(void)
{
	if (inited)
		return;
	for (uint32_t i = 0; i < HVMAP_VETRAP_SIZE / 4u; i++)
		hdr[i] = 0;
	hdr[0] = 0x52544556u;   /* "VETR" */
	hdr[2] = VE_RING_N;
	inited = 1;
}

static void log_access(uint32_t off, uint32_t wnr, uint32_t sas, uint32_t v)
{
	uint32_t n = hdr[1];
	uint32_t w0 = off | (wnr << 16) | (sas << 17) |
	    ((smp_cpu_id() & 3u) << 19);

	if (!wnr && n) {
		volatile uint32_t *p = ent + ((n - 1u) % VE_RING_N) * 2u;
		uint32_t pw0 = p[0];
		if ((pw0 & 0x00FFFFFFu) == w0 && p[1] == v && (pw0 >> 24) < 255u) {
			p[0] = pw0 + (1u << 24);
			return;
		}
	}
	{
		volatile uint32_t *e = ent + (n % VE_RING_N) * 2u;
		e[0] = w0;
		e[1] = v;
		hdr[1] = n + 1u;
	}
}

int vetrap_handle_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint64_t hpfar, addr;
	uint32_t off, wnr, srt, sas, page;
	volatile void *p;

	if (((esr >> ESR_EC_SHIFT) & ESR_EC_MASK) != ESR_EC_DABT_LOWER)
		return 0;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);
	for (page = 0; page < VETRAP_NPAGES; page++)
		if ((addr & ~0xFFFUL) == vetrap_pages[page])
			break;
	if (page == VETRAP_NPAGES)
		return 0;

	init_once();
	off = (uint32_t)(addr & 0xFFFu);
	if (!(esr & ESR_ISV_BIT)) {
		hdr[3]++;
		frame->elr += 4;
		return 1;
	}
	wnr = (esr & ESR_WNR_BIT) ? 1u : 0u;
	srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;
	p = (volatile void *)(uintptr_t)addr;

	if (wnr) {
		uint32_t v = (srt == SRT_XZR) ? 0u : (uint32_t)frame->x[srt];
		if (page != 2u || off < 0x400u)
			log_access(off | (page << 12), 1, sas, v);
		switch (sas) {
		case 0: *(volatile uint8_t *)p = (uint8_t)v; break;
		case 1: *(volatile uint16_t *)p = (uint16_t)v; break;
		default: *(volatile uint32_t *)p = v; break;
		}
		__asm__ volatile("dsb sy" ::: "memory");
	} else {
		uint32_t v;
		switch (sas) {
		case 0: v = *(volatile uint8_t *)p; break;
		case 1: v = *(volatile uint16_t *)p; break;
		default: v = *(volatile uint32_t *)p; break;
		}
		if (page != 2u || off < 0x400u)
			log_access(off | (page << 12), 0, sas, v);
		if (srt != SRT_XZR)
			frame->x[srt] = v;
	}
	frame->elr += 4;
	return 1;
}
