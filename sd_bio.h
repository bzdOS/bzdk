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
 * direction (CMD24); like emmc_bio_write() this is structurally correct but
 * treat a 0 return with a read-back compare until confirmed on silicon. */
int sd_bio_write(uint32_t lba, uint64_t buf_pa);

#endif /* BZDOS_SD_BIO_H */
