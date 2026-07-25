/* SPDX-License-Identifier: BSD-2-Clause */

/* vconsole.c — trap-and-emulate virtual UART0 for the FreeBSD/arm64 EL1
 * guest. See vconsole.h for the full rationale, the el2_trap wiring
 * contract, and the capture-ring layout.
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

/* ------------------------------------------------------------------ *
 * A64 UART0 (8250/16550-compatible) register byte offsets from
 * UART0_BASE, per the brief and cross-checked against the "snps,dw-apb-
 * uart" compatible + reg-shift/reg-io-width in
 * /opt/bzdos/build/bananapi-min.dtb's serial@1c28000 node (a standard
 * ns16550 register set, byte-addressed here since we only ever see 32-bit
 * word accesses from the guest's ISS decode anyway).
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
 * RX INJECTION ring: lets the HOST feed keystrokes to the guest's UART0
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
 * non-persisted ring, unlike the vconsole capture ring below). */
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
 * TX TEE ring: a second, small, ring that mirrors every byte the guest
 * writes to THR (see vconsole_capture_byte() below) so the USB-ACM bridge
 * (usbacm.c, polled on the CPU1 debug core) can drain it out the gadget's
 * bulk-IN endpoint without ever touching MUSB registers from CPU0's guest-
 * fault path (single-core-owns-the-hardware discipline -- see usbacm.c's
 * file banner for why). Deliberately NOT the same ring as the big 64 KiB
 * capture-for-postmortem window at VCONSOLE_RING_BASE (0x50000f00): that one
 * is written-only/never drained and exists purely for a post-WDT memory
 * dump, whereas this one is actively consumed every poll and must never
 * silently grow unbounded.
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

/* Latched guest IER value (one per emulated uart is overkill — the guest
 * only drives uart0's tty; uart1/2 never get IER!=0 programmed).
 *
 * WHY LATCH IT (found live 2026-07-20, "userland prints ONE byte then rc
 * hangs silently forever" hunt): FreeBSD's ns8250 TTY path transmits one
 * FIFO's worth, sets IER.ETXRDY and then waits for IIR to report TXRDY
 * before sending more (uart_intr -> bus_ipend reads IIR; in polled mode the
 * 50 Hz callout does the same read). Our old always-NOPEND IIR meant the
 * FIRST tty write stalled the output queue permanently: the KERNEL's own
 * printfs (low-level cnputc, LSR-polled) all worked, so the whole verbose
 * boot printed fine — and then userland/rc, whose /dev/console writes go
 * through the tty layer, emitted exactly one byte and went silent, with the
 * guest idling at a 6 Hz IIR poll (ELR pinned at one PC reading page offset
 * 0x008). RXRDY has the same shape: without it, userland tty reads never
 * notice injected input. NOTE ns8250_clrint() at PROBE time loops until IIR
 * reads no-pending — at probe IER==0, so the ier-gated logic below still
 * returns NOPEND there, exactly as the old stub did. */
static uint32_t vc_uart_ier;

/* THRE-interrupt EDGE latch. A real 16550 raises the TX interrupt when THR
 * becomes empty (for us: right after every THR write, and when ETXRDY gets
 * enabled while THR is already empty) and CLEARS it when IIR is read with
 * TXRDY as the reported source. The first version of this emulation returned
 * TXRDY on EVERY IIR read while ETXRDY was set — level, not edge — and
 * ns8250_clrint() (which loops reading IIR until NOPEND) then span forever,
 * hanging the guest at uart ATTACH, before mountroot, with the console mute.
 * One boot lost to that. */
static uint32_t vc_txrdy_pend;

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
 * Capture-ring low-level accessors. Cache-coherent single-word/single-byte
 * stores, matching the rest of the tree's breadcrumb convention.
 * ------------------------------------------------------------------ */
static inline void
vc_store32(uint32_t word_idx, uint32_t v)
{
	volatile uint32_t *p =
	    (volatile uint32_t *)(VCONSOLE_RING_BASE + word_idx * 4u);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline uint32_t
vc_load32(uint32_t word_idx)
{
	volatile uint32_t *p =
	    (volatile uint32_t *)(VCONSOLE_RING_BASE + word_idx * 4u);
	return *p;
}

static inline void
vc_store_byte(uint32_t buf_off, uint8_t v)
{
	volatile uint8_t *p = (volatile uint8_t *)(VCONSOLE_BUF_BASE + buf_off);
	*p = v;
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void
vc_zero_byte(uint32_t buf_off)
{
	vc_store_byte(buf_off, 0);
}

/* ------------------------------------------------------------------ *
 * Public API (see vconsole.h for the full contract).
 * ------------------------------------------------------------------ */

void
vconsole_init(void)
{
	vc_store32(0, VCONSOLE_MAGIC);   /* word[0]: magic "UART"        */
	vc_store32(1, 0);                /* word[1]: total_bytes         */
	vc_store32(2, 0);                /* word[2]: fault_count         */
	vc_store32(3, 0);                /* word[3]: reserved/padding    */

	/* Zero the byte buffer for deterministic state on a fresh boot.
	 * Init-time only (never called from exception context), so a
	 * straightforward bounded loop is fine here. */
	for (uint32_t i = 0; i < VCONSOLE_BUF_SIZE; i++)
		vc_zero_byte(i);

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
	 * possibly fault on UART0 or the USB-ACM bridge can start draining. */
	VC_RX_HEAD = 0;
	VC_RX_TAIL = 0;
	VC_TXTEE_HEAD = 0;
	VC_TXTEE_TAIL = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Push one transmitted byte into the ring, wrapping at VCONSOLE_BUF_SIZE.
 * total_bytes (word[1]) counts every byte ever captured (unclamped); the
 * physical write offset is total_bytes % VCONSOLE_BUF_SIZE. */
static void
vconsole_capture_byte(uint8_t c)
{
	uint32_t total = vc_load32(1);
	uint32_t off = total % VCONSOLE_BUF_SIZE;
	vc_store_byte(off, c);
	vc_store32(1, total + 1);

	/* B4 flight recorder: a captured console byte, direction TX (guest ->
	 * host, i.e. the guest's own printf/tty output). a1=0 marks the
	 * direction. NOTE deliberate tradeoff: during a verbose boot this is by
	 * far the highest-frequency flightrec_log() caller and can dominate the
	 * 2048-slot ring, but that's the intended behavior, not overflow: the
	 * 64 KiB VCONSOLE_RING_BASE capture ring above already retains the full
	 * text separately, so flightrec's unique value here is INTERLEAVING the
	 * last console bytes with faults/IRQs/virtio ops in one time-ordered
	 * ring -- if the last ~2048 events before a crash are all console
	 * bytes, that itself is the finding (crashed mid-boot-spew, not
	 * mid-virtio-op). */
	flightrec_log(FLTR_K_CONSOLE, c, 0);

	/* Tee the same byte to the USB-ACM bridge's TX ring (see above) so the
	 * CPU1 debug core can push it out the gadget's bulk-IN endpoint. This
	 * runs on CPU0 inside the guest's fault-handling path, so it MUST stay
	 * O(1)/non-blocking -- it is (a bounded ring push, drops on overflow). */
	vc_txtee_push(c);
}

static void
vconsole_count_fault(void)
{
	vc_store32(2, vc_load32(2) + 1);
}

int
vconsole_handle_fault(struct el2_frame *frame)
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
	 * guest's stage-1 MMU is on. During early boot (stage-1 off) VA==IPA
	 * so far alone happened to equal the physical UART0 address, but once
	 * pmap_bootstrap_dmap() runs the guest reads UART0 via its DMAP VA
	 * (e.g. 0xffff007ffffff008), which never matches UART0_BASE and made
	 * every later console access fall through unhandled -- ELR is never
	 * advanced for a guest-group fault (el2_exc.c), so the guest re-faults
	 * on the same instruction forever (looks like a hang, isn't one of our
	 * making... except that it is: this comparison was wrong). Reconstruct
	 * the true IPA from HPFAR_EL2 (bits[39:4] = IPA[47:12]) OR'd with far's
	 * page offset, same as virtio.c's ipa_of(frame). */
	uint64_t hpfar;
	__asm__ volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
	uint64_t addr = ((hpfar & 0xFFFFFFFFF0ULL) << 8) | (frame->far & 0xFFFull);

	/* Only claim faults inside the UART0 4 KiB page; anything else in
	 * the stage-2 MMIO window is not ours. */
	if (addr < UART0_BASE || addr >= UART0_BASE + UART0_SIZE)
		return 0;

	vconsole_count_fault();

	uint32_t isv = esr & ESR_ISV_BIT;
	if (!isv) {
		/* No valid instruction-syndrome info -- we cannot tell which
		 * register/size/direction without decoding the faulting
		 * instruction ourselves, which we don't do. Skip it (advance
		 * past the faulting instruction) rather than spin forever
		 * re-taking the same fault, and count it as a "best effort"
		 * emulate above. */
		frame->elr += 4;
		return 1;
	}

	uint32_t wnr = esr & ESR_WNR_BIT;
	uint32_t srt = (esr >> ESR_SRT_SHIFT) & ESR_SRT_MASK;
	/* SAS (access size) is decoded for completeness/documentation but
	 * unused: every register we emulate is accessed as a byte or a
	 * 32-bit word by real 16550 drivers, and we only ever care about the
	 * low 8 bits of THR writes / synthesize a fixed LSR value, so the
	 * transfer width doesn't change our behavior. */
	uint32_t sas = (esr >> ESR_SAS_SHIFT) & ESR_SAS_MASK;
	(void)sas;

	uint32_t off = (uint32_t)(addr - UART0_BASE);

	/* UART0/1/2 share this one 4 KiB page at page offsets 0x000/0x400/0x800
	 * (DTB: snps,dw-apb-uart serial@1c28000 / @1c28400 / @1c28800), all
	 * 16550-compatible with an identical register layout inside each 0x400
	 * block. Fold the access onto a single register window so IIR/LSR/USR
	 * emulation applies to whichever UART the guest is driving. Without this,
	 * a read of UART1's IIR (page offset 0x408) misses the off==UART_REG_IIR
	 * check, falls through to val=0 (== "interrupt pending"), and FreeBSD's
	 * ns8250 interrupt-service path (clrint: read IIR, read MSR, re-read IIR)
	 * spins forever on the secondary UART. */
	uint32_t reg = off & 0x3FFu;

	if (wnr) {
		/* Guest is writing a UART register. */
		uint64_t val = (srt == SRT_XZR) ? 0 : frame->x[srt];

		if (reg == UART_REG_THR) {
			/* Transmit: capture the low byte -- this is a
			 * character FreeBSD's console is printing. */
			vconsole_capture_byte((uint8_t)(val & 0xffu));
			/* THR write -> THR "drains" instantly -> THRE edge. */
			vc_txrdy_pend = 1;
			/* Console output = observable forward progress: feed the
			 * dead-man's-switch watchdog's minutes-window (see wdt.c). */
			wdt_note_progress();
		} else if (reg == UART_REG_IER) {
			/* Latch: IIR reads below report TXRDY/RXRDY only for
			 * sources the guest actually enabled (see vc_uart_ier's
			 * comment for the userland-console-hang story). Enabling
			 * ETXRDY while THR is (always, in our model) empty raises
			 * the THRE edge immediately — that's what wakes a tty
			 * that armed the interrupt AFTER its last THR write. */
			uint32_t newier = (uint32_t)(val & 0xffu);
			if ((newier & UART_IER_ETXRDY) && !(vc_uart_ier & UART_IER_ETXRDY))
				vc_txrdy_pend = 1;
			vc_uart_ier = newier;
		}
		/* Every other write (IIR-FCR/LCR/MCR init-time
		 * programming) is silently accepted/ignored -- there's no
		 * real UART behind this page to configure. */
	} else {
		/* Guest is reading a UART register. */
		uint64_t val = 0;

		if (reg == UART_REG_THR) {
			/* This offset is RBR (receive buffer register) on a
			 * read -- THR/RBR share byte offset 0x00 on a real
			 * 16550, per the standard register map. Dequeue one
			 * host-injected byte from the RX ring (see the "RX
			 * INJECTION ring" section above / vconsole_rx_push(),
			 * fed by usbacm.c) if one is pending; otherwise 0
			 * (matches every other unpopulated read here). This is
			 * what makes the guest console INTERACTIVE: FreeBSD's
			 * ns8250 RX path polls LSR.DR then reads RBR exactly
			 * like this. */
			val = vc_rx_pending() ? (uint64_t)vc_rx_getc() : 0;
		} else if (reg == UART_REG_LSR) {
			/* THRE|TEMT: transmitter always ready, so FreeBSD's
			 * busy-wait-for-tx-ready loop never blocks. OR in DR
			 * (bit0) whenever the RX ring has a host-injected byte
			 * waiting, so the guest's poll loop knows to read RBR. */
			val = UART_LSR_THRE_TEMT;
			if (vc_rx_pending())
				val |= UART_LSR_DR;
		} else if (reg == UART_REG_USR) {
			/* Allwinner "busy" bit: always 0 (never busy). */
			val = 0;
		} else if (reg == UART_REG_IIR) {
			/* Priority-encode pending sources the guest ENABLED via
			 * IER (see vc_uart_ier's comment): RX data waiting beats
			 * TX-ready; TX is "ready" permanently in our model, so
			 * ETXRDY alone yields an endless TXRDY stream — exactly
			 * what keeps the ns8250 tty output queue draining. With
			 * IER==0 (probe-time ns8250_clrint() loop) this still
			 * reads NOPEND, as the old stub always did. */
			if ((vc_uart_ier & UART_IER_ERXRDY) && vc_rx_pending()) {
				val = UART_IIR_RXRDY;   /* level: clears as RBR drains */
			} else if ((vc_uart_ier & UART_IER_ETXRDY) && vc_txrdy_pend) {
				vc_txrdy_pend = 0;      /* EDGE: consumed by this read */
				val = UART_IIR_TXRDY;
			} else {
				val = UART_IIR_NOPEND;
			}
		} else if (reg == UART_REG_MSR) {
			/* Modem status: report DCD|DSR|CTS permanently asserted,
			 * like a board with the modem lines strapped. MSR==0 means
			 * "no carrier", and a tty open() without CLOCAL then sleeps
			 * in the carrier wait FOREVER -- observed live 2026-07-20:
			 * kernel boots (kernel printf bypasses the tty), init execs,
			 * then userland hangs silently before rc's first echo,
			 * blocked opening /dev/console; zero output, zero disk I/O,
			 * only the 50 Hz uart poll ticking. Bits: DCD=0x80 DSR=0x20
			 * CTS=0x10. */
			val = 0xB0;
		} else {
			/* Any other read (e.g. RBR/IER probed during
			 * init/detection) synthesizes 0. */
			val = 0;
		}

		if (srt != SRT_XZR)
			frame->x[srt] = val;
	}

	/* Skip the faulting load/store -- we've fully emulated its effect. */
	frame->elr += 4;
	return 1;
}
