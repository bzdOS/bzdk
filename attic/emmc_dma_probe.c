/* emmc_dma_probe.c — READ-ONLY live probe of the guest's SMHC2/eMMC IDMAC ring,
 * for confirming the aw_mmc IDMAC DMA-corruption failure mode. See
 * docs/aw-mmc-dma-coherency.md (root cause) and docs/aw-mmc-dma-instrument.md
 * (this harness). NEW file; instrumentation only.
 *
 * It does NOT touch the SMHC controller state (no register writes), does NOT
 * change stage-2, and does NOT enter the guest. It only:
 *   1. reads the IDMAC descriptor-list base (DLBA) + LBA (CAGR) + byte count,
 *   2. walks the descriptor ring to recover the data-buffer PA + length,
 *   3. optionally invalidates each segment's cache lines to PoC (dc ivac) so a
 *      subsequent physical read reflects true DRAM, not EL2's stale cache,
 *   4. lays a snapshot into the 0x50000d00 breadcrumb window for the host.
 *
 * Wire-in (parent-owned; NOT done here — existing files are read-only for this
 * task): add one dispatch line in dbgmon.c's command table, e.g.
 *     case 'Q': emmc_dma_probe(1); break;
 * then read the crumbs over the net with `bc 0x50000d00`.
 *
 * Freestanding: <stdint.h> only. Cache-coherent breadcrumb stores (dc civac +
 * dsb sy), same pattern as the rest of the tree.
 */
#include <stdint.h>

/* Guest eMMC = SMHC2 = aw_mmc1. Base + offsets verbatim from emmc_bio.c /
 * sys/arm/allwinner/aw_mmc.h. a64_emmc_conf has dma_desc_shift == 0, so DLBA and
 * the descriptor buf_addr/next fields are UNSHIFTED physical addresses. */
#define EMMC_BASE   0x01c11000UL
#define REG_BYCR    0x14u   /* byte count of current transfer            */
#define REG_CAGR    0x1Cu   /* command argument == LBA (sector index)    */
#define REG_DMAC    0x80u   /* IDMAC control (bit7 IDMA_ON)              */
#define REG_DLBA    0x84u   /* IDMAC descriptor-list base PA            */
#define REG_IDST    0x88u   /* IDMAC status (bit0 TX,bit1 RX, FSM[16:13])*/

#define DESC_OWN    (1u << 31)
#define DESC_LD     (1u << 2)   /* last descriptor      */
#define DESC_ER     (1u << 5)   /* end of ring          */
#define DMA_XFERLEN 0x2000u     /* buf_size==0 means this (a64_emmc)     */

#define PROBE_BC_BASE  0x50000d00UL
#define PROBE_MAGIC    0x454d4d31u   /* "EMM1" */
#define MAX_DESCS      512u          /* walk cap — never trust the ring blindly */

static inline uint32_t rd32(uint32_t off)
{
	return *(volatile uint32_t *)(EMMC_BASE + off);
}

static inline uint32_t pread32(uint32_t pa)
{
	return *(volatile uint32_t *)(uintptr_t)pa;
}

/* Invalidate one cache line covering `pa` to the Point of Coherency, so a
 * following load returns what DRAM (and therefore the IDMAC) actually holds,
 * not a stale EL2/CPU1 cacheable copy. 64-byte line on A53. */
static inline void dc_ivac(uint32_t pa)
{
	__asm__ volatile("dc ivac, %0" :: "r"((uintptr_t)pa) : "memory");
}

static inline void probe_bc(int i, uint32_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(PROBE_BC_BASE + (uint32_t)i * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* Snapshot the in-flight IDMAC ring for the host. If `invalidate` is nonzero,
 * dc-ivac each 64-byte line of the FIRST data segment to PoC before sampling its
 * first word, so the host can compare a cacheable read vs this PoC-accurate read
 * (see docs/aw-mmc-dma-instrument.md §4). Returns the first data-buffer PA (0 if
 * the ring looked invalid).
 *
 * Breadcrumb layout at 0x50000d00:
 *   [0] magic "EMM1"     [1] DLBA (ring PA)     [2] CAGR (LBA)
 *   [3] BYCR (bytes)     [4] IDST               [5] DMAC
 *   [6] desc0.config     [7] desc0.buf_size     [8] desc0.buf_addr (data PA)
 *   [9] desc0.next       [10] nsegs walked      [11] total bytes across segs
 *   [12] first data word @ buf_addr (cacheable read as EL2 sees it)
 *   [13] first data word @ buf_addr AFTER dc ivac (PoC-accurate) — same as [12]
 *        if EL2 holds no stale line; DIFFERS => EL2 stale-line hazard proven
 */
uint32_t emmc_dma_probe(int invalidate)
{
	uint32_t dlba = rd32(REG_DLBA);
	uint32_t cagr = rd32(REG_CAGR);
	uint32_t bycr = rd32(REG_BYCR);
	uint32_t idst = rd32(REG_IDST);
	uint32_t dmac = rd32(REG_DMAC);

	probe_bc(0, PROBE_MAGIC);
	probe_bc(1, dlba);
	probe_bc(2, cagr);
	probe_bc(3, bycr);
	probe_bc(4, idst);
	probe_bc(5, dmac);

	/* Basic sanity: DLBA must point into DRAM (>= 0x40000000). */
	if (dlba < 0x40000000u || dlba >= 0x80000000u) {
		probe_bc(10, 0xBADD1BA5u);
		return 0;
	}

	uint32_t p = dlba;
	uint32_t first_buf = 0, first_size = 0, cfg0 = 0, next0 = 0;
	uint32_t nsegs = 0, total = 0;

	for (nsegs = 0; nsegs < MAX_DESCS; nsegs++) {
		uint32_t cfg  = pread32(p + 0x0);
		uint32_t size = pread32(p + 0x4);
		uint32_t buf  = pread32(p + 0x8);
		uint32_t next = pread32(p + 0xC);

		uint32_t seg = size ? size : DMA_XFERLEN;

		if (nsegs == 0) {
			cfg0 = cfg; first_size = size; first_buf = buf; next0 = next;
		}
		total += seg;

		if ((cfg & (DESC_LD | DESC_ER)) || next == 0u ||
		    next < 0x40000000u || next >= 0x80000000u)
			break;
		p = next;
	}
	nsegs++;

	probe_bc(6, cfg0);
	probe_bc(7, first_size);
	probe_bc(8, first_buf);
	probe_bc(9, next0);
	probe_bc(10, nsegs);
	probe_bc(11, total);

	if (first_buf >= 0x40000000u && first_buf < 0x80000000u) {
		uint32_t cached = pread32(first_buf);   /* as EL2's cache sees it   */
		probe_bc(12, cached);
		if (invalidate) {
			/* Invalidate the whole first segment to PoC, then re-read. */
			uint32_t seg = first_size ? first_size : DMA_XFERLEN;
			for (uint32_t o = 0; o < seg; o += 64u)
				dc_ivac(first_buf + o);
			__asm__ volatile("dsb ish\n\tisb" ::: "memory");
			probe_bc(13, pread32(first_buf));   /* PoC-accurate            */
		} else {
			probe_bc(13, cached);
		}
	}

	return first_buf;
}
