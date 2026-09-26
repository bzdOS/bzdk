/* SPDX-License-Identifier: BSD-2-Clause */

/* rsbtrap.c — see rsbtrap.h. */
#include <stdint.h>
#include "hv_addrmap.h"
#include "exceptions.h"
#include "smp.h"       /* smp_cpu_id() */
#include "cntpct.h"
#include "rsb.h"
#include "rsbtrap.h"

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

/* Controller registers, offsets inside rsb@1f03400 (FreeBSD aw_rsb.c names). */
#define R_CTRL    0x00u
#define R_CCR     0x04u
#define R_INTE    0x08u
#define R_INTS    0x0Cu
#define R_DADDR0  0x10u
#define R_DADDR1  0x14u
#define R_DLEN    0x18u
#define R_DATA0   0x1Cu
#define R_DATA1   0x20u
#define R_PMCR    0x28u
#define R_CMD     0x2Cu
#define R_DAR     0x30u

#define CTRL_SOFT_RESET  (1u << 0)
#define CTRL_START       (1u << 7)
#define INT_TRANS_OVER   (1u << 0)
#define INT_TRANS_ERR    (1u << 1)
#define PMCR_START       (1u << 31)

#define CMD_WR8   0x4Eu
#define CMD_WR16  0x59u
#define CMD_WR32  0x63u

#define AXP_RTA        0x2Du
#define AXP_REG_OUT1   0x10u     /* DCDC1..6 enable, bits 0..5 */
#define AXP_REG_OUT2   0x12u     /* DC1SW bit 7, DLDO4..1 bits 6..3, ELDO3..1 */
#define AXP_REG_POFF   0x32u     /* bit 7: power off */
#define OUT1_KEEP      ((1u << 0) | (1u << 1) | (1u << 4) | (1u << 5))
#define OUT2_DC1SW     (1u << 7)
#define OUT2_DLDO1     (1u << 3)
#define POFF_OFF       (1u << 7)

static struct {
	uint32_t ctrl, ccr, inte, ints, daddr0, daddr1, dlen, data0, data1,
	    pmcr, cmd, dar;
} g_sh;
static uint32_t g_inited;

static inline void bc(unsigned idx, uint32_t v)
{
	((volatile uint32_t *)HVMAP_RSBTRAP_BC)[idx] = v;
}
static inline uint32_t bc_get(unsigned idx)
{
	return ((volatile uint32_t *)HVMAP_RSBTRAP_BC)[idx];
}
static inline void bc_inc(unsigned idx)
{
	bc(idx, bc_get(idx) + 1u);
}

static void init_once(void)
{
	if (g_inited)
		return;
	for (unsigned i = 0; i < HVMAP_RSBTRAP_BC_SIZE / 4u; i++)
		bc(i, 0);
	for (unsigned i = 0; i < RSBTRAP_LOG_N * 2u; i++)
		((volatile uint32_t *)HVMAP_RSBTRAP_LOG)[i] = 0;
	bc(0, 0x54425352u);   /* "RSBT" */
	g_inited = 1;
}

static uint8_t police(uint8_t reg, uint8_t v)
{
	if (reg == AXP_REG_OUT1)
		v |= (uint8_t)OUT1_KEEP;
	else if (reg == AXP_REG_OUT2) {
		v |= (uint8_t)OUT2_DC1SW;
#ifdef HV_HDMI
		v |= (uint8_t)OUT2_DLDO1;
#endif
	} else if (reg == AXP_REG_POFF)
		v &= (uint8_t)~POFF_OFF;
	return v;
}

static void log_write(uint8_t reg, uint8_t asked, uint8_t sent)
{
	uint32_t head = bc_get(11);
	volatile uint32_t *e = (volatile uint32_t *)HVMAP_RSBTRAP_LOG +
	    (head % RSBTRAP_LOG_N) * 2u;

	e[0] = (uint32_t)reg | ((uint32_t)asked << 8) | ((uint32_t)sent << 16) |
	    ((smp_cpu_id() & 0xFFu) << 24);
	e[1] = (uint32_t)cntpct_read();
	bc(11, head + 1u);
}

/* A guest write to the PMIC, byte by byte (WR16/WR32 cover consecutive
 * registers starting at DADDR0, little-endian in DATA0). */
static uint32_t police_write(uint32_t cmd, uint32_t reg0, uint32_t data)
{
	unsigned n = (cmd == CMD_WR8) ? 1u : (cmd == CMD_WR16) ? 2u : 4u;
	uint32_t out = data;

	for (unsigned i = 0; i < n; i++) {
		uint8_t reg = (uint8_t)(reg0 + i);
		uint8_t asked = (uint8_t)(data >> (8u * i));
		uint8_t sent = police(reg, asked);

		if (sent != asked) {
			out = (out & ~(0xFFu << (8u * i))) | ((uint32_t)sent << (8u * i));
			bc_inc(6);
		}
		log_write(reg, asked, sent);
	}
	return out;
}

static void do_transfer(void)
{
	struct rsb_guest_xfer x = {
		.dar = g_sh.dar, .daddr0 = g_sh.daddr0, .daddr1 = g_sh.daddr1,
		.dlen = g_sh.dlen, .data0 = g_sh.data0, .data1 = g_sh.data1,
		.cmd = g_sh.cmd,
	};
	uint32_t stat = 0, d0 = 0, d1 = 0;
	int is_write = (x.cmd == CMD_WR8 || x.cmd == CMD_WR16 || x.cmd == CMD_WR32);

	if (is_write && ((x.dar >> 16) & 0xFFu) == AXP_RTA)
		x.data0 = police_write(x.cmd, x.daddr0 & 0xFFu, x.data0);

	bc_inc(4);
	if (rsb_guest_transfer(&x, &stat, &d0, &d1) != 0)
		bc_inc(5);
	g_sh.ints |= stat;
	if (!is_write) {
		g_sh.data0 = d0;
		g_sh.data1 = d1;
	}
}

static uint32_t sh_read(uint32_t r)
{
	switch (r) {
	case R_CTRL:   return g_sh.ctrl;
	case R_CCR:    return g_sh.ccr;
	case R_INTE:   return g_sh.inte;
	case R_INTS:   return g_sh.ints;
	case R_DADDR0: return g_sh.daddr0;
	case R_DADDR1: return g_sh.daddr1;
	case R_DLEN:   return g_sh.dlen;
	case R_DATA0:  return g_sh.data0;
	case R_DATA1:  return g_sh.data1;
	case R_PMCR:   return g_sh.pmcr;
	case R_CMD:    return g_sh.cmd;
	case R_DAR:    return g_sh.dar;
	default:       return 0;
	}
}

static void sh_write(uint32_t r, uint32_t v)
{
	switch (r) {
	case R_CTRL:
		if (v & CTRL_SOFT_RESET) {
			/* The guest resets ITS view; the bus stays as EL2 set it. */
			g_sh.ints = 0;
			bc_inc(10);
		}
		g_sh.ctrl = v & ~(CTRL_SOFT_RESET | CTRL_START);
		if (v & CTRL_START)
			do_transfer();
		break;
	case R_CCR:    g_sh.ccr = v; break;
	case R_INTE:   g_sh.inte = v; break;
	case R_INTS:   g_sh.ints &= ~v; break;          /* write 1 to clear */
	case R_DADDR0: g_sh.daddr0 = v; break;
	case R_DADDR1: g_sh.daddr1 = v; break;
	case R_DLEN:   g_sh.dlen = v; break;
	case R_DATA0:  g_sh.data0 = v; break;
	case R_DATA1:  g_sh.data1 = v; break;
	case R_PMCR:
		/* Device-mode switch: EL2 switched the PMIC to RSB mode at its own
		 * init (rsb_init()); make sure of that and report it done. */
		g_sh.pmcr = v & ~PMCR_START;
		if (v & PMCR_START) {
			if (rsb_init() == 0)
				g_sh.ints |= INT_TRANS_OVER;
			else
				g_sh.ints |= INT_TRANS_ERR;
		}
		break;
	case R_CMD:    g_sh.cmd = v; break;
	case R_DAR:    g_sh.dar = v; break;
	default:       break;
	}
}

static uint32_t page_rd(uint32_t off, uint32_t sas)
{
	volatile void *p = (volatile void *)(RSBTRAP_PAGE_BASE + off);
	switch (sas) {
	case 0: return *(volatile uint8_t *)p;
	case 1: return *(volatile uint16_t *)p;
	default: return *(volatile uint32_t *)p;
	}
}

static void page_wr(uint32_t off, uint32_t v, uint32_t sas)
{
	volatile void *p = (volatile void *)(RSBTRAP_PAGE_BASE + off);
	switch (sas) {
	case 0: *(volatile uint8_t *)p  = (uint8_t)v;  break;
	case 1: *(volatile uint16_t *)p = (uint16_t)v; break;
	default: *(volatile uint32_t *)p = v;          break;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

int rsbtrap_handle_fault(struct el2_frame *frame)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint64_t hpfar, addr;
	uint32_t off, wnr, srt, sas;

	if (((esr >> ESR_EC_SHIFT) & ESR_EC_MASK) != ESR_EC_DABT_LOWER)
		return 0;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);
	if (addr < RSBTRAP_PAGE_BASE || addr >= RSBTRAP_PAGE_BASE + RSBTRAP_PAGE_SIZE)
		return 0;

	init_once();
	off = (uint32_t)(addr - RSBTRAP_PAGE_BASE);
	bc_inc(1);
	bc(8, smp_cpu_id());
	bc(9, off);

	if (!(esr & ESR_ISV_BIT)) {
		bc_inc(7);
		frame->elr += 4;
		return 1;
	}
	wnr = esr & ESR_WNR_BIT;
	srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;

	if (off < RSBTRAP_RSB_OFF || off >= RSBTRAP_RSB_END) {
		/* Not the RSB controller (R_PWM etc.): plain passthrough. */
		if (wnr)
			page_wr(off, (srt == SRT_XZR) ? 0u : (uint32_t)frame->x[srt], sas);
		else if (srt != SRT_XZR)
			frame->x[srt] = page_rd(off, sas);
		frame->elr += 4;
		return 1;
	}

	/* The driver does 32-bit accesses only; a narrower one is emulated on
	 * the containing register. */
	{
		uint32_t r = (off - RSBTRAP_RSB_OFF) & ~3u;
		uint32_t sh = ((off & 3u) * 8u);

		if (wnr) {
			uint32_t v = (srt == SRT_XZR) ? 0u : (uint32_t)frame->x[srt];
			if (sas < 2u) {
				uint32_t m = (sas == 0u) ? 0xFFu : 0xFFFFu;
				v = (sh_read(r) & ~(m << sh)) | ((v & m) << sh);
			}
			bc_inc(3);
			sh_write(r, v);
		} else {
			uint32_t v = sh_read(r) >> sh;
			if (sas == 0u)
				v &= 0xFFu;
			else if (sas == 1u)
				v &= 0xFFFFu;
			bc_inc(2);
			if (srt != SRT_XZR)
				frame->x[srt] = v;
		}
	}
	frame->elr += 4;
	return 1;
}
