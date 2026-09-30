/* SPDX-License-Identifier: BSD-2-Clause */

/* emmc_bio.h — Allwinner A64 SMHC2/eMMC (aw_mmc1 @ 0x01c11000) block-I/O
 * helper for the bzdOS EL2 microkernel. Contract for emmc_bio.c.
 *
 * PURPOSE: draining the eMMC controller's 128-word FIFO by hand over the
 * EMAC debug channel (one `rd`/`wr` dbgmon round-trip per 32-bit register)
 * costs 128+ network round-trips per 512-byte block — far too slow for a
 * `gpart recover` pass over the whole disk. These three functions live IN
 * the hypervisor's own .text and are invoked directly on the debug core via
 * dbgmon's `call <pa> <x0> <x1> <x2> <x3>` (see dbgmon.c cmd_call): the
 * FIFO drain loop runs entirely on-board, and only ONE round-trip per block
 * is needed (the host then bulk-reads/writes the DRAM scratch buffer over
 * the same coherent debug channel).
 *
 * Register offsets, bit values, command flags and the bring-up/read
 * sequence below are ported VERBATIM from a hardware-verified Python
 * sequence run over the debug channel against THIS controller (not from a
 * generic datasheet) — see emmc_bio.c for the register map and citations.
 * The write path (emmc_bio_write / CMD24) mirrors the read path structurally
 * but has NOT been hardware-verified yet (the read path has).
 *
 * ABI: plain C, callable via dbgmon `call` (AAPCS x0-x3, up to 4 uint64_t
 * args, ret in x0). All internal polls are iteration-capped — a call can
 * never hang the debug core; a timeout is reported as a nonzero/negative
 * return instead of spinning forever.
 */
#ifndef BZDOS_EMMC_BIO_H
#define BZDOS_EMMC_BIO_H
#include <stdint.h>

/* Real hardware SPI for mmc@1c11000 (SMHC2/eMMC), per the guest DTB's own
 * `interrupts = <0 0x3e 0x04>` (bananapi-min.dts) -- SPI 62, INTID 32+62.
 * The guest's own DTB node for this exact device is "okay" but FreeBSD's
 * dmesg confirms "no driver attached" (verified live 2026-09-30: aw_mmc
 * only ever attaches to mmc@1c10000, the WiFi SDIO controller) -- so this
 * SPI is EL2-owned unconditionally, same reasoning gic_timer.c already
 * gives MUSB_IRQ_INTID/HDMI_TCON1_IRQ_INTID for their real device SPIs. */
#define EMMC_DMA_IRQ_SPI    62u
#define EMMC_DMA_IRQ_INTID  (32u + EMMC_DMA_IRQ_SPI)   /* 94 */

/* Called from gic_timer.c's IRQ dispatch (EMMC_DMA_IRQ_INTID arm) every
 * time the real hardware SPI fires -- purely a diagnostic counter (EBIO
 * breadcrumb slot [31]), proving on hardware that the IDMAC completion IRQ
 * actually fires once per DMA transfer. Does NOT touch REG_IDST/any
 * controller register (that stays exclusively the job of whichever core
 * holds the eMMC controller lock and is inside emmc_dma_wait_complete()) --
 * safe to call from ANY core's IRQ context with no locking at all. */
void emmc_bio_dma_irq_note(void);

/* One-time (idempotent) controller bring-up: PC5 pinmux -> func3, 400 kHz
 * init clock, controller reset, GO_IDLE/SEND_OP_COND/ALL_SEND_CID/SET_RCA/
 * SEND_CSD/SELECT/SET_BLOCKLEN(512) card-identification sequence.
 * Returns 0 on success (OCR "ready" bit seen from CMD1), nonzero on failure
 * (a bounded poll timed out, or CMD1 never reported ready within the
 * retry budget). Safe to call more than once; each call re-runs the whole
 * sequence from scratch. Must be called (and must return 0) before
 * emmc_bio_read()/emmc_bio_write(). */
int emmc_bio_init(void);

/* Read one 512-byte block (CMD17, single-block read) at 512-byte-sector
 * address `lba` into DRAM at physical address `buf_pa`, written as 128
 * little-endian 32-bit words (buf_pa need not be aligned strictly beyond
 * natural uint32_t alignment). Returns 0 on success, negative on a bounded
 * timeout (FIFO never delivered/DATA_OVER never seen). */
int emmc_bio_read(uint32_t lba, uint64_t buf_pa);

/* Write one 512-byte block (CMD24, single-block write) at 512-byte-sector
 * address `lba` from DRAM at physical address `buf_pa` (128 LE 32-bit
 * words). Returns 0 on success, negative on a bounded timeout. NOTE: unlike
 * emmc_bio_read(), this path mirrors the read sequence structurally but has
 * NOT been hardware-verified — treat a nonzero return (or a return of 0
 * that doesn't survive a read-back compare) with suspicion until confirmed
 * on real silicon. */
int emmc_bio_write(uint32_t lba, uint64_t buf_pa);

/* Best-effort reclock to HIGH-SPEED mode (CMD6 HS_TIMING + bus-width switch,
 * then controller clock 400 kHz -> ~25 MHz, per EMMC_HS_CLK_REG). Called
 * automatically at the END of emmc_bio_init() -- not required to be called
 * directly, but exposed (and dbgmon `call`-able) so a HS attempt can be
 * retried standalone without re-running the whole identification sequence.
 *
 * FAIL-SAFE BY CONSTRUCTION: every internal step (CMD6 timeout, clock-update
 * handshake timeout, post-switch test read of LBA 0) is guarded; on ANY
 * failure the controller AND card are restored to the known-good 400 kHz/
 * 1-bit configuration and this still returns 0 -- HS is a pure speed
 * optimization layered on top of the mandatory slow-path bring-up, never a
 * hard dependency. The eMMC keeps working at 400 kHz either way.
 *
 * Records the outcome in the EBIO breadcrumb window (see emmc_bio.c,
 * @0x50020200): word[6] (EBIO_BC_HS_STATE) = 0 never run / 1 HS active /
 * 2 fell back after a failure; word[7] (EBIO_BC_HS_STEP) = which step
 * failed when word[6]==2 (0 otherwise). Always returns 0. */
int emmc_bio_set_highspeed(void);

/* Read/write NBLK contiguous 512-byte blocks (CMD18/CMD25, the controller's
 * own AUTO_STOP raising CMD12 for us) in ONE controller transaction instead
 * of one emmc_bio_read()/emmc_bio_write() call per sector. Same command
 * encoding as sd_bio.c's read_multi/write_multi (identical controller IP),
 * but built on emmc_bio.c's own hardened timing/abort idioms -- see that
 * file's header comment above the functions themselves.
 *
 * nblk must be in [2, EMMC_MULTI_MAX_BLOCKS]; buf_pa holds nblk*512 bytes as
 * 128*nblk little-endian 32-bit words. Returns 0 on success, a nonzero
 * packed code on any failure -- the caller then falls back to one
 * emmc_bio_read()/emmc_bio_write() call per sector, exactly as it already
 * does for a single-block failure. HARDWARE-VALIDATED 2026-09-27
 * (test_emmc_multi.py) and wired into vblk_emmc.c's guest I/O path. */
#define EMMC_MULTI_MAX_BLOCKS 128u
int emmc_bio_read_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk);
int emmc_bio_write_multi(uint32_t lba, uint64_t buf_pa, uint32_t nblk);

/* Same contract as emmc_bio_read_multi()/write_multi() (nblk in
 * [2, EMMC_MULTI_MAX_BLOCKS], same packed-failure-code convention), but the
 * data phase is moved by the controller's own IDMAC (descriptor-chain DMA)
 * instead of a CPU FIFO-drain loop: the vCPU/debug core programs one
 * descriptor chain and polls IDST for completion, instead of reading/writing
 * REG_FIFO one word at a time. This is the actual fix for "a vCPU burns
 * cycles doing I/O" (see HANDOFF.md item 2) -- CMD18/CMD25 batching alone
 * only cut the number of controller *transactions*, not the per-transaction
 * PIO cost, which is what these functions remove.
 *
 * Descriptor format, register map and the exact enable/reset/wait sequence
 * are transcribed VERBATIM from the guest's own aw_mmc.c driver (FreeBSD
 * sys/arm/allwinner/aw_mmc.c, the real driver for this exact controller,
 * already the project's standing reference for anything not in the
 * hardware-verified Python sequence -- see this header's own top-of-file
 * note and emmc_bio.c's "REWRITTEN 2026-07-30 to match the reference
 * driver" precedent for GCTL_DMA_RST). NOT YET hardware-verified: validate
 * with dbgmon `call` (same standalone cross-check methodology as
 * test_emmc_multi.py) before wiring into any guest-facing path. */
int emmc_bio_read_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk);
int emmc_bio_write_dma(uint32_t lba, uint64_t buf_pa, uint32_t nblk);

/* Arm/disarm WRITE fault injection. OFF by default; nothing in a normal boot
 * touches this.
 *
 * WHY IT EXISTS. ebio_fail_settle() waits for the card to finish programming
 * before its caller retries, and since the corruption fix there are no failures
 * left to enter that path -- so the safeguard has never actually executed. This
 * makes failures happen on demand, at the one moment that matters: after the
 * data phase has completed and DATA_OVER has latched, i.e. with the card
 * mid-program. That is precisely the state the old spurious busy-timeout bailed
 * out in, and retrying CMD24 from there is what corrupted ~0.1% of sectors.
 *
 *   every       inject on every Nth eligible write; 0 disarms.
 *   min_lba     SAFETY INTERLOCK -- never inject below this sector. Injection
 *               can corrupt real data; point this at scratch (the unused swap
 *               partition starts at 12863488) so the guest's root filesystem
 *               is unreachable regardless of how the run behaves.
 *   point       WHICH failure to inject. 0 = bail after DATA_OVER latched
 *               (the post-write CARD_BUSY path); 1 = bail on the first poll of
 *               the DATA-PHASE wait, before DATA_OVER, which is what the CNTPCT
 *               underflow actually did. Point 0 produced ZERO corruption with
 *               both waits -- correctly, since once DATA_OVER has latched all
 *               128 words have reached the card and no partial block is
 *               possible. Point 1 is where words are still undelivered in the
 *               FIFO and the settle's reset discards them.
 *   legacy_wait 1 = make the settle use the OLD iteration-bounded wait, so one
 *               build can show corruption reappearing with it and staying away
 *               without it. That is what validates the fix rather than merely
 *               exercising it.
 *
 * Breadcrumbs: [27] injections performed, [28] the armed configuration echoed
 * back (0 when disarmed, and distinguishable from an unwritten slot). */
void emmc_bio_fault_inject(uint32_t every, uint32_t min_lba, uint32_t point,
                           uint32_t legacy_wait);

/* ------------------------------------------------------------------ *
 * EXT_CSD / wear telemetry (HANDOFF item 4).
 * ------------------------------------------------------------------ */

/* Read the card's 512-byte EXT_CSD register (CMD8, SEND_EXT_CSD -- eMMC
 * meaning; not SD's CMD8/SEND_IF_COND) into DRAM at physical address
 * `buf_pa`, as 128 little-endian 32-bit words, byte order preserved (so
 * EXT_CSD byte N is at `((uint8_t *)buf_pa)[N]`). Structurally the exact
 * same single-block PIO drain as emmc_bio_read() (same FIFO/stall/DATA_OVER
 * handling, which is why it is a thin wrapper over a shared internal
 * helper, not a hand-copy) with a different command index and no block
 * address argument. Returns 0 on success, negative on a bounded timeout —
 * same convention as emmc_bio_read(). Caller must hold the eMMC controller
 * lock (VBLK_EMMC_LOCK_PA / vblk_emmc_trylock()), exactly like every other
 * function in this file. */
int emmc_bio_read_ext_csd(uint64_t buf_pa);

struct emmc_wear {
	uint32_t pre_eol_info;   /* EXT_CSD[267] PRE_EOL_INFO: 0=n/a/unread,
	                          * 1=normal, 2=warning (80% reserved used),
	                          * 3=urgent */
	uint32_t life_est_a;     /* EXT_CSD[268] DEVICE_LIFE_TIME_EST_TYP_A
	                          * (SLC or single type): 0=n/a, 1..10 = a BAND index,
	                          * 1=0-10% of rated lifetime used .. 10=90-100% */
	uint32_t life_est_b;     /* EXT_CSD[269] DEVICE_LIFE_TIME_EST_TYP_B
	                          * (MLC, 0 if the card has no separate MLC
	                          * region): 0=n/a, 1..10 = a BAND index,
	                          * same convention as life_est_a above */
	uint32_t ok;             /* 1 = EXT_CSD read succeeded this call; 0 =
	                          * timeout/error -- the three fields above are
	                          * left as 0, not stale, when this is 0 */
};

/* Fill *out from a fresh EXT_CSD read: pre_eol_info/life_est_a/life_est_b
 * plus the ok gate. Uses the fixed HVMAP_EMMC_EXTCSD_BUF landing buffer
 * internally (see hv_addrmap.h). Bounded, like every other call in this
 * file; on failure *out is all-zero (ok=0), never stale or partial. Caller
 * must hold the eMMC controller lock, same as emmc_bio_read_ext_csd(). */
void emmc_bio_read_wear(struct emmc_wear *out);

#endif /* BZDOS_EMMC_BIO_H */
