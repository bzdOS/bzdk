/* SPDX-License-Identifier: BSD-2-Clause */

/* sd_bio.h — Allwinner A64 SMHC0 / SD-card (mmc0 @ 0x01c0f000) block-I/O
 * helper for the bzdOS EL2 microkernel. Contract for sd_bio.c.
 *
 * PURPOSE: give the HYPERVISOR direct, PIO block access to the microSD in the
 * board's SD slot, driven on the debug core via dbgmon `call <pa> ...`, so the
 * card can be read/written over the network WITHOUT the FreeBSD guest touching
 * the native aw_mmc0 path (which wedges its DMA/IDMAC engine — see
 * `aw-mmc-cache-coherency-hypothesis`). This is the HV-owned-SD substrate for
 * ROADMAP G-3 (SD offload / eMMC read-only) and the eventual second virtio-blk
 * backing the SD to the guest.
 *
 * This is the SD-card sibling of emmc_bio.c/.h: SAME SMHC IP block, so the
 * controller mechanics (clock config, CMDR command engine, 128-word FIFO
 * drain/fill, NTSR/SAMP_DL new-mode latch) are ported verbatim from
 * emmc_bio.c — see that file's header for the register-map citations. Only
 * these differ and are the SD-specific content of this file:
 *   - base 0x01c0f000 (SMHC0) vs 0x01c11000 (SMHC2);
 *   - pinmux PF0..PF5 -> func2 ("mmc0") vs the eMMC PC5 clock fix;
 *   - CCU clock gate/divider at 0x01c20088 (MMC0_CLK) vs 0x01c20090 (MMC2_CLK);
 *   - card identification: SD uses CMD8 (SEND_IF_COND) + ACMD41 (with the HCS
 *     high-capacity bit) + CMD3 where the CARD returns its own RCA — NOT the
 *     eMMC CMD1 + host-assigned RCA. SDHC/SDXC cards are block-addressed
 *     (CMD17/CMD24 argument = LBA); legacy SDSC is byte-addressed (arg = LBA*512).
 *     sd_bio_init() detects this from the OCR CCS bit and sd_bio_read/write()
 *     apply the right addressing automatically.
 *
 * ABI: plain C, callable via dbgmon `call` (AAPCS x0-x3, ret in x0). All polls
 * are iteration-capped — a call can never hang the debug core; a timeout is a
 * nonzero/negative return, not a spin.
 */
#ifndef BZDOS_SD_BIO_H
#define BZDOS_SD_BIO_H
#include <stdint.h>

/* Real hardware SPI for mmc@1c0f000 (SMHC0/SD), per the guest DTB's own
 * `interrupts = <0 0x3c 0x04>` (bananapi-min.dts) -- SPI 60, INTID 32+60.
 * Same "EL2-owned unconditionally" reasoning as EMMC_DMA_IRQ_INTID
 * (emmc_bio.h): the guest's own dmesg confirms "no driver attached" for
 * this node (verified live 2026-09-30). */
#define SD_DMA_IRQ_SPI    60u
#define SD_DMA_IRQ_INTID  (32u + SD_DMA_IRQ_SPI)   /* 92 */

/* Called from gic_timer.c's IRQ dispatch (SD_DMA_IRQ_INTID arm) every time
 * the real hardware SPI fires -- see emmc_bio_dma_irq_note()'s comment,
 * same contract, same "no controller register touched here" safety. */
void sd_bio_dma_irq_note(void);

/* One-time (idempotent) SD-card bring-up: PF0..5 pinmux -> mmc0, 400 kHz init
 * clock, controller reset, then GO_IDLE / SEND_IF_COND / ACMD41(HCS) /
 * ALL_SEND_CID / SEND_RELATIVE_ADDR / SEND_CSD / SELECT / SET_BLOCKLEN(512).
 * Returns 0 on success, negative on failure (a bounded poll timed out, or the
 * card never reported OCR-ready within the retry budget). Distinct negative
 * codes per step (see sd_bio.c) aid diagnosis. Safe to call repeatedly. Must
 * return 0 before sd_bio_read()/sd_bio_write(). */
int sd_bio_init(void);

/* Read one 512-byte block at 512-byte-sector address `lba` into DRAM at
 * physical `buf_pa` (128 LE 32-bit words). Returns 0 on success, negative on a
 * bounded timeout. Addressing (block vs byte) is chosen from the card type
 * detected in sd_bio_init(). */
int sd_bio_read(uint32_t lba, uint64_t buf_pa);

/* Write one 512-byte block at `lba` from DRAM at `buf_pa`. Returns 0 on
 * success, negative on timeout. Mirrors sd_bio_read()'s sequence in the write
 * direction (CMD24). CONFIRMED on silicon 2026-08-24 (45/45 write/read-back
 * round trips + a guest-level virtio dd/md5 round trip) -- but only AFTER
 * sd_bio_set_highspeed()'s 25 MHz reclock; at the 400 kHz identification
 * clock this same function was observed to either time out outright or
 * report success while the data never actually landed. */
int sd_bio_write(uint32_t lba, uint64_t buf_pa);

/* CMD25 multi-block write of nblk (2..SD_MULTI_MAX_BLOCKS) sectors from
 * buf_pa, controller auto-CMD12. Non-zero on any error, after stopping the
 * card; the caller rewrites the run with sd_bio_write(). */
#define SD_MULTI_MAX_BLOCKS 128u   /* one whole SEG_MAX=16 x 4 KiB request */
int sd_bio_write_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk);
/* CMD18 counterpart, same contract. */
int sd_bio_read_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk);

/* Same contract as sd_bio_read_multi()/write_multi() (nblk in
 * [2, SD_MULTI_MAX_BLOCKS], same packed-failure-code convention), but the
 * data phase moves through the controller's own IDMAC (descriptor-chain
 * DMA) instead of a CPU FIFO-drain loop -- see emmc_bio.c's
 * emmc_bio_read_dma()/write_dma() for the design (same controller IP,
 * confirmed byte-identical CMD17/CMD24 CMDR encoding, same IDMAC register
 * map at a different base). NOT YET hardware-validated: validate with
 * dbgmon `call` (same standalone cross-check methodology as
 * test_emmc_dma.py) before wiring into any guest-facing path. */
int sd_bio_read_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk);
int sd_bio_write_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk);

/* Split-phase form of the pair above, same contract as emmc_bio.h's
 * emmc_bio_dma_start()/poll()/hook: SD_DMA_RUNNING until finished, then 0
 * or the synchronous pair's own failure code; -103 = none / busy. The SD
 * lock must be held from start() until poll() != SD_DMA_RUNNING. */
#define SD_DMA_RUNNING 1
#define SD_DMA_IRQ_SPIN_US 1000u
int sd_bio_dma_start(uint32_t is_read, uint32_t lba, uint64_t buf_pa,
                     uint32_t nblk);
int sd_bio_dma_poll(uint32_t spin_us);
void sd_bio_set_dma_done_hook(void (*fn)(uint32_t spin_us));
void sd_bio_dma_tick(void);

/* Best-effort reclock from the 400 kHz identification clock to SD Default
 * Speed (25 MHz), FAIL-SAFE by construction: unlike eMMC's HS_TIMING switch,
 * SD cards support the whole 0-25 MHz default-speed range with NO CMD6
 * SWITCH_FUNC negotiation, so this is a controller-side-only reclock plus a
 * mandatory post-switch test read; any failure reclocks straight back to
 * 400 kHz and returns nonzero. Called automatically, once, at the end of a
 * successful sd_bio_init() -- callers never need to call this themselves.
 * Written in response to sd_bio_write() being confirmed (2026-08-24, live
 * hardware) to fail outright or silently not persist data at 400 kHz; see
 * sd_bio.c's block comment above it for the fix rationale. */
int sd_bio_set_highspeed(void);

/* Real capacity in 512-byte sectors, parsed from the card's CSD during
 * sd_bio_init() (see sd_bio.c's parse site). Falls back to a conservative
 * 1 GiB stub if the card's CSD wasn't CSD-version-2.0 or init never ran.
 * Callers must not call this before a successful sd_bio_init(). */
uint64_t sd_bio_capacity_sectors(void);

#endif /* BZDOS_SD_BIO_H */
