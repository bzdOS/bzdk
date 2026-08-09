/* SPDX-License-Identifier: BSD-2-Clause */

/* vconsole.c — trap-and-emulate virtual UART0 for the FreeBSD/arm64 EL1
 * guest (channel 0) AND, since the dual-guest milestone, the Zephyr EL1
 * guest running concurrently on CPU3 (channel 1). See vconsole.h for the
 * full rationale, the el2_trap wiring contract, and the capture-ring
 * layout.
 *
 * CHANNEL REFACTOR (dual-guest milestone): the original implementation was
 * a single, hard-coded set of static file-scope helpers/state addressing
 * VCONSOLE_RING_BASE/VCONSOLE_BUF_BASE directly. This file now
 * parameterizes that same logic over an explicit `struct vc_chan *`
 * channel-state pointer (see below), with two static instances: vc_chan0
 * (VCONSOLE_RING_BASE/VCONSOLE_BUF_BASE, unchanged addresses, `interactive`
 * = 1 -- RX injection + TX-tee + the wdt_note_progress() feed, exactly the
 * original behavior) and vc_chan1 (a NEW, smaller ring at
 * HVMAP_VCONSOLE_CHAN1_HDR/_BUF, `interactive` = 0 -- TX-only: Zephyr's
 * console never reads UART input in this design, so there is no RX ring, no
 * USB-ACM tee, and — the one correctness requirement that is NOT optional —
 * NO path to wdt_note_progress(), which must stay reachable ONLY from
 * channel 0. Every gated ("if (cs->interactive)") check below evaluates
 * exactly as the original UNCONDITIONAL code did when cs == &vc_chan0, so
 * channel 0's behavior is unchanged; grep for "cs->interactive" to see
 * every place this refactor touches, there are no others.
 *
 * Freestanding, no libc: only <stdint.h>. Every ring store is
 * cache-coherent (dc civac + dsb sy per store), the same pattern used by
 * every other breadcrumb/ring window in this tree (stage2.c's stg2_bc,
 * main_fbsd.c's FBS_BC, etc.) so the captured text survives past the WDT
 * reset that main_fbsd.c arms before entering the guest and is visible via
 * a plain physical memory dump afterward.
 */
#include <stdint.h>
#include "vconsole.h"
#include "stage2.h"     /* UART0_BASE, UART0_SIZE */
#include "exceptions.h" /* struct el2_frame */
#include "wdt.h"        /* wdt_note_progress() — feed the dead-man's switch */
#include "flightrec.h"  /* B4: flightrec_log(FLTR_K_CONSOLE, ...) per byte */
#include "hv_addrmap.h" /* HVMAP_VCONSOLE_CHAN1_* (channel 1's ring lane) */

/* ------------------------------------------------------------------ *
 * A64 UART0 (8250/16550-compatible) register byte offsets from
 * UART0_BASE, per the brief and cross-checked against the "snps,dw-apb-
 * uart" compatible + reg-shift/reg-io-width in
 * /opt/bzdos/build/bananapi-min.dtb's serial@1c28000 node (a standard
 * ns16550 register set, byte-addressed here since we only ever see 32-bit
 * word accesses from the guest's ISS decode anyway). The Zephyr guest
 * (channel 1) targets the SAME physical/IPA UART0_BASE with the SAME
 * register spacing (zephyr-guest/boards/bzdos/bpi_m64_hv/bpi_m64_hv.dts's
 * uart0 node: "ns16550", reg-shift=2, i.e. 4-byte register spacing,
 * identical to the offsets below) — the two channels never collide because
 * they run on different cores against different, disjoint stage-2 tables
 * (CPU0's stage2.c identity-maps this page away for a UART trap; CPU3's
 * stage2_zephyr.c leaves the ENTIRE MMIO gigabyte invalid, so a Zephyr
 * UART0 access reaches el2_exc.c's CPU3 dispatch instead — see that file's
 * header).
 * ------------------------------------------------------------------ */
#define UART_REG_THR   0x00u   /* write: transmit holding register       */
#define UART_REG_IER   0x04u   /* interrupt enable (ignored)             */
#define UART_REG_IIR   0x08u   /* IIR/FCR (ignored)                      */
#define UART_REG_LCR   0x0Cu   /* line control (ignored)                 */
#define UART_REG_MCR   0x10u   /* modem control (ignored)                */
#define UART_REG_LSR   0x14u   /* line status: bit5 THRE, bit6 TEMT      */
#define UART_REG_MSR   0x18u   /* modem status (ignored)                 */
#define UART_REG_USR   0x7Cu   /* Allwinner UART status (busy indicator) */

#define UART_LSR_THRE_TEMT   0x60u   /* bits 5+6 set: always tx-ready    */
#define UART_LSR_DR          0x01u   /* bit0: RX data ready              */

/* ------------------------------------------------------------------ *
 * RX INJECTION ring (CHANNEL 0 ONLY — see vconsole.h's channel-1 contract:
 * TX-only, no RX). Lets the HOST feed keystrokes to the guest's UART0
 * console over EMAC (dbgmon writes the buffer + advances head; the guest,
 * polling LSR/reading RBR through this trap, drains it). Makes the guest
 * console INTERACTIVE (mountroot> prompt, single-user shell, login) instead
 * of output-only. Fixed DRAM region in the 0x50000e00 breadcrumb block,
 * clear of DBG1 (0x50000e00-0x50000e08) and the vconsole ring (0x50000f00).
 *   0x50000e40 head (host write ptr, host increments)
 *   0x50000e44 tail (guest read ptr, guest increments)
 *   0x50000e48 buf[128]
 * Cross-core coherent (SMPEN): host store+dc civac on CPU1 is seen by the
 * guest's load on CPU0 with no explicit invalidate. */
#define VC_RX_HEAD   (*(volatile uint32_t *)0x50000e40UL)
#define VC_RX_TAIL   (*(volatile uint32_t *)0x50000e44UL)
#define VC_RX_BUF    0x50000e48UL
#define VC_RX_SZ     128u
#define VC_RX_MASK   (VC_RX_SZ - 1u)

static inline int vc_rx_pending(void)
{
	return VC_RX_HEAD != VC_RX_TAIL;
}

static inline uint8_t vc_rx_getc(void)
{
	uint32_t t = VC_RX_TAIL;
	uint8_t c;
	if (t == VC_RX_HEAD)
		return 0xffu;                    /* empty */
	/* Order the HEAD gate-load before the data load: the producer (usbacm.c
	 * on CPU1) writes the byte, THEN bumps HEAD, so a data load reordered
	 * ahead of the HEAD check could return a byte the producer hasn't stored
	 * yet. dmb ishld pins the ordering (ring.c uses LDAR for the same reason). */
	__asm__ volatile("dmb ishld" ::: "memory");
	c = *(volatile uint8_t *)(VC_RX_BUF + (t & VC_RX_MASK));
	VC_RX_TAIL = t + 1u;
	__asm__ volatile("dsb sy" ::: "memory");
	return c;
}

/* Producer side of the SAME ring, called by the USB-ACM bridge (usbacm.c),
 * expected to run on the CPU1 debug core (see smp.c smp_secondary_main) --
 * i.e. host keystrokes arriving over the gadget's bulk-OUT endpoint. Ring
 * full (guest not draining fast enough): drop the new byte rather than
 * overwrite an undelivered one or block -- bounded, matches every other poll
 * path in this tree. Store-then-barrier-then-publish-head ordering matches
 * vc_rx_getc()'s tail-side convention so the guest never observes a head
 * advance before the byte it points at is visible (SMPEN cache coherency
 * handles cross-core visibility; no dc civac needed for this live,
 * non-persisted ring, unlike the vconsole capture ring below). This
 * remains channel-0-only: nothing ever calls it for channel 1 (Zephyr's
 * console has no RX path — see vconsole.h). */
void
vconsole_rx_push(uint8_t c)
{
	uint32_t h = VC_RX_HEAD;
	uint32_t n = h + 1u;

	if ((n & VC_RX_MASK) == (VC_RX_TAIL & VC_RX_MASK))
		return;                          /* ring full: drop */

	*(volatile uint8_t *)(VC_RX_BUF + (h & VC_RX_MASK)) = c;
	__asm__ volatile("dsb sy" ::: "memory");
	VC_RX_HEAD = n;
	__asm__ volatile("dsb sy" ::: "memory");

	/* B4 flight recorder: a captured console byte, direction RX (host ->
	 * guest, i.e. an operator keystroke injected over the USB-ACM bridge).
	 * Low volume (human typing speed) so this never competes with fault/IRQ
	 * events for ring space. a1=1 marks the direction for the host decoder. */
	flightrec_log(FLTR_K_CONSOLE, c, 1);
}

/* ------------------------------------------------------------------ *
 * TX TEE ring (CHANNEL 0 ONLY, same reasoning as the RX ring above): a
 * second, small, ring that mirrors every byte the guest writes to THR (see
 * vconsole_capture_byte() below) so the USB-ACM bridge (usbacm.c, polled on
 * the CPU1 debug core) can drain it out the gadget's bulk-IN endpoint
 * without ever touching MUSB registers from CPU0's guest-fault path
 * (single-core-owns-the-hardware discipline -- see usbacm.c's file banner
 * for why). Deliberately NOT the same ring as the big 64 KiB
 * capture-for-postmortem window at VCONSOLE_RING_BASE (0x50000f00): that one
 * is written-only/never drained and exists purely for a post-WDT memory
 * dump, whereas this one is actively consumed every poll and must never
 * silently grow unbounded. Channel 1 (Zephyr, no USB-ACM bridge) never
 * pushes to this ring — see vconsole_capture_byte()'s `cs->interactive`
 * gate.
 *
 * Fixed DRAM window 0x50004000 -- distinct from every breadcrumb window
 * listed in smp.h's map (MUSB 0x50000000 .. HDMI 0x50003000); chosen as the
 * next free 4 KiB-aligned slot.
 *   0x50004000 head (producer: vconsole_capture_byte, CPU0)
 *   0x50004004 tail (consumer: vconsole_tx_tee_getc, CPU1)
 *   0x50004008 buf[256]
 * ------------------------------------------------------------------ */
#define VC_TXTEE_BASE   0x50004000UL
#define VC_TXTEE_HEAD   (*(volatile uint32_t *)(VC_TXTEE_BASE + 0x00))
#define VC_TXTEE_TAIL   (*(volatile uint32_t *)(VC_TXTEE_BASE + 0x04))
#define VC_TXTEE_BUF    (VC_TXTEE_BASE + 0x08)
#define VC_TXTEE_SZ     256u
#define VC_TXTEE_MASK   (VC_TXTEE_SZ - 1u)

static inline void
vc_txtee_push(uint8_t c)
{
	uint32_t h = VC_TXTEE_HEAD;
	uint32_t n = h + 1u;

	if ((n & VC_TXTEE_MASK) == (VC_TXTEE_TAIL & VC_TXTEE_MASK))
		return;                          /* USB side isn't draining: drop */

	*(volatile uint8_t *)(VC_TXTEE_BUF + (h & VC_TXTEE_MASK)) = c;
	__asm__ volatile("dsb sy" ::: "memory");
	VC_TXTEE_HEAD = n;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Consumer side, called from usbacm.c (CPU1). Returns 1 and stores the next
 * tee'd byte into *out, or returns 0 if the tee ring is currently empty.
 * Bounded/non-blocking, like every other accessor in this file. */
int
vconsole_tx_tee_getc(uint8_t *out)
{
	uint32_t t = VC_TXTEE_TAIL;

	if (t == VC_TXTEE_HEAD)
		return 0;                         /* empty */

	/* Order the HEAD gate-load before the data load (see vc_rx_getc): the
	 * producer stores the byte then bumps HEAD, so the data load must not be
	 * reordered ahead of the HEAD check that gated it. */
	__asm__ volatile("dmb ishld" ::: "memory");
	*out = *(volatile uint8_t *)(VC_TXTEE_BUF + (t & VC_TXTEE_MASK));
	__asm__ volatile("dsb sy" ::: "memory");
	VC_TXTEE_TAIL = t + 1u;
	__asm__ volatile("dsb sy" ::: "memory");
	return 1;
}

#define UART_IIR_RXRDY       0x04u   /* received data available            */
#define UART_IIR_TXRDY       0x02u   /* transmit holding register empty    */
#define UART_IER_ERXRDY      0x01u   /* enable RX-ready interrupt          */
#define UART_IER_ETXRDY      0x02u   /* enable TX-ready interrupt          */

#define UART_IIR_NOPEND      0x01u   /* bit0: 1 = no interrupt pending (ns16550.h
                                      * IIR_NOPEND) -- synthesizing 0 here made
                                      * ns8250_clrint()'s "while ((iir &
                                      * IIR_NOPEND) == 0)" loop forever (iir=0
                                      * masks to IIR_MLSC, reads MSR, re-reads
                                      * IIR, repeat), verified against the exact
                                      * matching uart_dev_ns8250.c source. */

/* ESR_EL2.ISS field positions for a data-abort EC (0x24), applicable to
 * both frame->esr's overall bit numbering (ISS occupies bits[24:0], and
 * every field below already lies inside that window so the absolute ESR
 * bit position and the "ISS-relative" position the brief quotes coincide). */
#define ESR_EC_SHIFT      26
#define ESR_EC_MASK       0x3Fu
#define ESR_EC_DABT_LOWER 0x24u

#define ESR_ISV_BIT       (1u << 24)
#define ESR_SAS_SHIFT     22
#define ESR_SAS_MASK      0x3u
#define ESR_SRT_SHIFT     16
#define ESR_SRT_MASK      0x1Fu
#define ESR_WNR_BIT       (1u << 6)

#define SRT_XZR   31u   /* SRT==31 means the zero register, not x[31] */

/* ------------------------------------------------------------------ *
 * Channel state -- see this file's header comment for the full refactor
 * rationale. `ring_base`/`buf_base`/`buf_size` are fixed (set once, at
 * static-init time, never written again); `uart_ier`/`txrdy_pend` are the
 * per-channel mutable latches the original code kept as file-scope statics
 * (vc_uart_ier/vc_txrdy_pend) -- now one copy per channel instead of one
 * shared copy, which is actually a LATENT BUG FIX for channel 0 alone
 * (there was only ever one channel before, so this changes nothing
 * observable for it) but is load-bearing for channel 1: Zephyr's IER
 * writes must never be visible to/from FreeBSD's channel and vice versa.
 * `interactive` gates every RX-ring/TX-tee/wdt_note_progress() access —
 * see the file header comment for the exact list.
 * ------------------------------------------------------------------ */
struct vc_chan {
	uint64_t ring_base;
	uint64_t buf_base;
	uint32_t buf_size;
	int      interactive;   /* 1 = chan0 (RX ring + TX tee + wdt feed) */
	uint32_t uart_ier;
	uint32_t txrdy_pend;
};

static struct vc_chan vc_chan0 = {
	VCONSOLE_RING_BASE, VCONSOLE_BUF_BASE, VCONSOLE_BUF_SIZE, 1, 0, 0
};
static struct vc_chan vc_chan1 = {
	HVMAP_VCONSOLE_CHAN1_HDR, HVMAP_VCONSOLE_CHAN1_BUF,
	(uint32_t)HVMAP_VCONSOLE_CHAN1_BUF_SIZE, 0, 0, 0
};

/* ------------------------------------------------------------------ *
 * Capture-ring low-level accessors. Cache-coherent single-word/single-byte
 * stores, matching the rest of the tree's breadcrumb convention. Now
 * parameterized over the explicit channel-state pointer -- for `cs ==
 * &vc_chan0` these address EXACTLY VCONSOLE_RING_BASE/VCONSOLE_BUF_BASE,
 * i.e. channel 0's behavior is unchanged.
 * ------------------------------------------------------------------ */
static inline void
vc_store32(struct vc_chan *cs, uint32_t word_idx, uint32_t v)
{
	volatile uint32_t *p =
	    (volatile uint32_t *)(cs->ring_base + word_idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline uint32_t
vc_load32(struct vc_chan *cs, uint32_t word_idx)
{
	volatile uint32_t *p =
	    (volatile uint32_t *)(cs->ring_base + word_idx * 4u);
	return *p;
}

static inline void
vc_store_byte(struct vc_chan *cs, uint32_t buf_off, uint8_t v)
{
	volatile uint8_t *p = (volatile uint8_t *)(cs->buf_base + buf_off);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void
vc_zero_byte(struct vc_chan *cs, uint32_t buf_off)
{
	vc_store_byte(cs, buf_off, 0);
}

/* ------------------------------------------------------------------ *
 * Public API (see vconsole.h for the full contract).
 * ------------------------------------------------------------------ */

/* Shared init body: magic + counters + self-describing base/size tail +
 * zeroed buffer. Identical sequence to the original vconsole_init(), just
 * addressed through `cs`. */
static void
vconsole_init_chan(struct vc_chan *cs)
{
	vc_store32(cs, 0, VCONSOLE_MAGIC);              /* word[0]: magic "UART" */
	vc_store32(cs, 1, 0);                           /* word[1]: total_bytes  */
	vc_store32(cs, 2, 0);                           /* word[2]: fault_count  */
	vc_store32(cs, 3, VCONSOLE_LAYOUT_VER);         /* word[3]: layout version */
	vc_store32(cs, 4, (uint32_t)cs->buf_base);      /* word[4]: buffer base    */
	vc_store32(cs, 5, cs->buf_size);                /* word[5]: buffer size    */
	vc_store32(cs, 6, 0);                           /* word[6]: reserved       */
	vc_store32(cs, 7, 0);                           /* word[7]: reserved       */

	/* Zero the byte buffer for deterministic state on a fresh boot.
	 * Init-time only (never called from exception context), so a
	 * straightforward bounded loop is fine here. */
	for (uint32_t i = 0; i < cs->buf_size; i++)
		vc_zero_byte(cs, i);
}

void
vconsole_init(void)
{
	vconsole_init_chan(&vc_chan0);

	/* BUG FIX (found live on hardware): VC_RX_HEAD/TAIL (0x50000e40/
	 * 0x50000e44) and VC_TXTEE_HEAD/TAIL (0x50004000/0x50004004) are fixed
	 * DRAM windows, NOT .bss -- they are never zeroed by the C runtime and
	 * a warm WDT reset (which deliberately preserves DRAM, see wdt.c/
	 * SESSION-RULES.md) leaves them holding whatever a PRIOR run's counters
	 * ended at. Observed live: VC_RX_HEAD read 14 / VC_RX_TAIL read
	 * 0xffffffff on boot -- vc_rx_getc() then handed the guest bytes of
	 * garbage as "keystrokes". The exact same class of bug on the TX-tee
	 * ring made vconsole_tx_tee_getc() see an enormous bogus backlog
	 * (head/tail apart by whatever a previous session's byte count was)
	 * and replay stale physical buffer content cyclically for as long as
	 * it took the counters to numerically converge -- the "same early-boot
	 * lines repeating" flood + the CPU1 debug-core starvation it caused
	 * (usbacm_poll() never saw an empty tee ring, so it never stopped
	 * draining/flushing). Zero all four here, with plain stores + a single
	 * barrier (matching vc_rx_getc()/vc_txtee_push()'s no-dc-civac
	 * convention for these live, non-persisted rings), BEFORE the guest can
	 * possibly fault on UART0 or the USB-ACM bridge can start draining.
	 * CHANNEL 0 ONLY -- channel 1 has no RX ring / TX-tee ring at all
	 * (see vconsole_init_chan1() below). */
	VC_RX_HEAD = 0;
	VC_RX_TAIL = 0;
	VC_TXTEE_HEAD = 0;
	VC_TXTEE_TAIL = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

void
vconsole_init_chan1(void)
{
	/* Same init body as channel 0, addressed at the channel-1 ring
	 * instead. No RX ring / TX-tee ring to zero -- channel 1 (Zephyr) has
	 * neither (see vconsole.h: TX-only by design, no RX injection, no
	 * USB-ACM tee). */
	vconsole_init_chan(&vc_chan1);
}

/* Push one transmitted byte into `cs`'s ring, wrapping at cs->buf_size.
 * total_bytes (word[1]) counts every byte ever captured (unclamped); the
 * physical write offset is total_bytes % cs->buf_size. */
static void
vconsole_capture_byte(struct vc_chan *cs, uint8_t c)
{
	uint32_t total = vc_load32(cs, 1);
	uint32_t off = total % cs->buf_size;
	vc_store_byte(cs, off, c);
	vc_store32(cs, 1, total + 1);

	/* B4 flight recorder: a captured console byte, direction TX (guest ->
	 * host). Shared across both channels -- harmless/informational, lets a
	 * post-mortem see cross-guest console interleaving on one timeline.
	 * a1=0 marks the direction (same convention for both channels). See
	 * vconsole_rx_push()'s comment above for why this stays far below the
	 * 2048-slot ring's overflow point in practice. */
	flightrec_log(FLTR_K_CONSOLE, c, 0);

	if (cs->interactive) {
		/* Tee to the USB-ACM bridge's TX ring -- CHANNEL 0 ONLY. Channel 1
		 * (Zephyr) has no USB-ACM bridge; its console is EMAC/vconsole-ring
		 * only. This runs on the faulting core's own fault-handling path,
		 * so it MUST stay O(1)/non-blocking -- it is (a bounded ring push,
		 * drops on overflow). */
		vc_txtee_push(c);
	}
}

static void
vconsole_count_fault(struct vc_chan *cs)
{
	vc_store32(cs, 2, vc_load32(cs, 2) + 1);
}

/* Shared fault-handling body -- see vconsole_handle_fault() below for the
 * public, channel-selecting entry point. For `cs == &vc_chan0` every
 * `cs->interactive`-gated branch below evaluates exactly as the ORIGINAL,
 * unconditional code did (interactive == 1), so channel 0's behavior is
 * byte-for-byte the same logic as before this refactor. */
static int
vconsole_handle_fault_impl(struct el2_frame *frame, struct vc_chan *cs)
{
	uint32_t esr = (uint32_t)frame->esr;
	uint32_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

	/* Not a data abort at all -- not ours, let el2_trap's generic path
	 * record it. (The wiring contract in vconsole.h has the caller check
	 * this too, but we re-check defensively so this function is safe to
	 * call unconditionally.) */
	if (ec != ESR_EC_DABT_LOWER)
		return 0;

	/* frame->far is FAR_EL2 -- the faulting VIRTUAL address the guest
	 * used, valid only for its low 12 bits (the page offset) once the
	 * guest's stage-1 MMU is on. Reconstruct the true IPA from HPFAR_EL2
	 * (bits[39:4] = IPA[47:12]) OR'd with far's page offset, same as
	 * virtio.c's ipa_of(frame) -- see the original comment (preserved in
	 * spirit, condensed here) for the DMAP-VA bug this fixed. */
	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	/* Only claim faults inside the UART0 4 KiB page; anything else in
	 * the stage-2 MMIO window is not ours. */
	if (addr < UART0_BASE || addr >= UART0_BASE + UART0_SIZE)
		return 0;

	vconsole_count_fault(cs);

	uint32_t isv = esr & ESR_ISV_BIT;
	if (!isv) {
		/* No valid instruction-syndrome info -- skip past the faulting
		 * instruction rather than spin forever re-taking the same fault. */
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	/* SAS (access size) decoded for completeness/documentation but unused
	 * -- see the original comment: every register here is byte/word-sized
	 * and we never vary behavior on transfer width. */
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;
	(void)sas;

	uint32_t off = (uint32_t)(addr - UART0_BASE);

	/* UART0/1/2 share this one 4 KiB page at page offsets 0x000/0x400/0x800
	 * -- fold onto a single register window (see original comment for the
	 * secondary-UART IIR-spin bug this fixed). */
	uint32_t reg = off & 0x3FFu;

	if (wnr) {
		/* Guest is writing a UART register. */
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];

		if (reg == UART_REG_THR) {
			/* Transmit: capture the low byte. */
			vconsole_capture_byte(cs, (uint8_t)(val & 0xffu));
			/* THR write -> THR "drains" instantly -> THRE edge. */
			cs->txrdy_pend = 1;
			/* Console output = observable forward progress: feed the
			 * dead-man's-switch watchdog's minutes-window (see wdt.c).
			 * CHANNEL 0 ONLY -- this is the one correctness requirement
			 * that is NOT optional (see this file's header comment):
			 * Zephyr's channel-1 heartbeat output must never be able to
			 * feed the FreeBSD-guest-progress watchdog gate. */
			if (cs->interactive)
				wdt_note_progress();
		} else if (reg == UART_REG_IER) {
			/* Latch: IIR reads below report TXRDY/RXRDY only for
			 * sources the guest actually enabled. */
			uint32_t newier = (uint32_t)(val & 0xffu);
			if ((newier & UART_IER_ETXRDY) && !(cs->uart_ier & UART_IER_ETXRDY))
				cs->txrdy_pend = 1;
			cs->uart_ier = newier;
		}
		/* Every other write (IIR-FCR/LCR/MCR init-time programming) is
		 * silently accepted/ignored -- there's no real UART behind this
		 * page to configure, for either channel. */
	} else {
		/* Guest is reading a UART register. */
		uint64_t val = 0;

		if (reg == UART_REG_THR) {
			/* RBR (receive buffer register) on a read. Channel 0 only:
			 * dequeue a host-injected byte if one is pending. Channel 1
			 * (Zephyr) has no RX ring at all -- `cs->interactive` is 0,
			 * so this always synthesizes 0, matching "no RX support". */
			val = (cs->interactive && vc_rx_pending())
			      ? (uint64_t)vc_rx_getc() : 0;
		} else if (reg == UART_REG_LSR) {
			/* THRE|TEMT: transmitter always ready. OR in DR (bit0) only
			 * for channel 0 (channel 1 never has RX data waiting). */
			val = UART_LSR_THRE_TEMT;
			if (cs->interactive && vc_rx_pending())
				val |= UART_LSR_DR;
		} else if (reg == UART_REG_USR) {
			/* Allwinner "busy" bit: always 0 (never busy). */
			val = 0;
		} else if (reg == UART_REG_IIR) {
			/* Priority-encode pending sources the guest ENABLED via IER.
			 * RX-ready is gated on `cs->interactive` (always false for
			 * channel 1, so this branch can never fire there — the
			 * whole RXRDY path degrades to "never", exactly the TX-only
			 * shape vconsole.h documents for channel 1, with no separate
			 * code needed). */
			if ((cs->uart_ier & UART_IER_ERXRDY) && cs->interactive &&
			    vc_rx_pending()) {
				val = UART_IIR_RXRDY;   /* level: clears as RBR drains */
			} else if ((cs->uart_ier & UART_IER_ETXRDY) && cs->txrdy_pend) {
				cs->txrdy_pend = 0;      /* EDGE: consumed by this read */
				val = UART_IIR_TXRDY;
			} else {
				val = UART_IIR_NOPEND;
			}
		} else if (reg == UART_REG_MSR) {
			/* Modem status: DCD|DSR|CTS permanently asserted (see
			 * original comment for the carrier-wait hang this fixed). */
			val = 0xB0;
		} else {
			val = 0;
		}

		if (srt != SRT_XZR)
			frame->x[srt] = val;
	}

	/* Skip the faulting load/store -- we've fully emulated its effect. */
	frame->elr += 4;
	return 1;
}

int
vconsole_handle_fault(struct el2_frame *frame, unsigned channel)
{
	return vconsole_handle_fault_impl(frame,
	                                  (channel == 0) ? &vc_chan0 : &vc_chan1);
}
