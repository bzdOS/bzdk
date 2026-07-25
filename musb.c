/* SPDX-License-Identifier: BSD-2-Clause */

/* musb.c — MUSB (Allwinner A64 USB-OTG) CDC-ACM gadget driver for bzdOS
 * microkernel. Implements musb.h. Freestanding, bare-metal AArch64, no
 * pmap/MMU games: U-Boot leaves the MMU on with a flat device mapping, so
 * MMIO is accessed via volatile pointers built from absolute physical
 * addresses.
 *
 * Register offsets, descriptors, and EP0 SETUP handling are ported from the
 * reference FreeBSD early console driver at
 * /opt/bzdos/qemu-test/usbgadget_console.c (the "reference"). See PROJECT.md
 * for the hardware facts and API contract.
 *
 * KEY FIX vs. the reference: the reference only calls its musb_poll()
 * equivalent from inside cnputc(), so if nothing is being printed the host
 * never gets serviced and enumeration stalls forever. Here, musb_poll()
 * unconditionally services EP0 SETUP and EP1-OUT RX on every single call,
 * with zero dependency on TX state. main.c is expected to spin on
 * musb_poll() continuously, which guarantees the host completes
 * enumeration regardless of whether the console is producing output.
 */
#include <stdint.h>
#include "musb.h"
#include "wdt.h"

/* ------------------------------------------------------------------ */
/* Physical addresses (PROJECT.md, verified on BPI-M64 "Chimp")        */
/* ------------------------------------------------------------------ */
#define MUSB_MMIO_BASE   0x01c19000UL
#define CCU_MMIO_BASE    0x01c20000UL

/* USB PHY0 "phy_ctrl" register block (A64 DT phy@1c19400) — this is the SAME
 * window the MUSB glue reaches as MUSB_base + 0x400 (so REG_ISCR below lands
 * here). ISCR = +0x00, PHYCTL (A33-style, bit-banged calibration) = +0x10,
 * OTGCTL (PHY0 host/gadget route) = +0x20. */
#define USBPHY_CTRL_BASE 0x01c19400UL
#define USBPHY_PMU0_BASE 0x01c1a800UL   /* DT "pmu0"; HCI_PHY_CTL at +0x10   */
#define SRAMC_MMIO_BASE  0x01c00000UL   /* SRAM controller (USB FIFO mapping) */

#define USBPHY_PHYCTL    (USBPHY_CTRL_BASE + 0x10)  /* REG_PHYCTL_A33          */
#define USBPHY_OTGCTL    (USBPHY_CTRL_BASE + 0x20)  /* REG_PHY_OTGCTL          */
#define USBPHY_HCI_CTL   (USBPHY_PMU0_BASE + 0x10)  /* REG_HCI_PHY_CTL         */

/* CCU clock/reset — A64 map (U-Boot drivers/clk/sunxi/clk_a64.c):
 *   CLK_BUS_OTG   = GATE (0x060, BIT23)   RST_BUS_OTG  = RESET(0x2c0, BIT23)
 *   CLK_USB_PHY0  = GATE (0x0cc, BIT8)    RST_USB_PHY0 = RESET(0x0cc, BIT0)
 * The old code gated 0x06c BIT24 — the WRONG register/bit entirely; it only
 * appeared to work because U-Boot had already enabled the real BUS_OTG gate.
 * clk_enable / reset_deassert both SET the bit (verified in the sunxi clk +
 * reset drivers), so all of these are "OR the bit in". */
#define CCU_BUS_GATING0      0x0060u
#define CCU_BUS_OTG_GATE     (1u << 23)
#define CCU_USB_CLK_CFG      0x00ccu
#define CCU_USBPHY0_CLK_GATE (1u << 8)
#define CCU_USBPHY0_RST      (1u << 0)
#define CCU_BUS_RESET0       0x02c0u
#define CCU_BUS_OTG_RST      (1u << 23)

/* ------------------------------------------------------------------ */
/* MUSB register offsets (ported from the reference)                   */
/* ------------------------------------------------------------------ */
/* !!! ALLWINNER LAYOUT, NOT standard Mentor !!!
 * The A64's MUSB uses Allwinner's own register map (U-Boot musb_regs.h,
 * "#else CONFIG_ARCH_SUNXI: SUNXI has different reg addresses"). The
 * reference driver used the standard Mentor offsets — correct for QEMU's
 * MUSB model, but on real silicon every write landed in the FIFO window
 * (0x00..0x3F) or padding: POWER is 0x40 here, not 0x01. That is why the
 * gadget never reacted: SOFTCONN, soft-reset, INTUSB reads — all no-ops.
 * Verified symptom: 111M-poll run with intr_seen=0, setups=0, and the host
 * never even saw the "disconnect". */
#define REG_FADDR     0x98
#define REG_POWER     0x40
#define REG_INTTX     0x44
#define REG_INTRX     0x46
#define REG_INTTXE    0x48
#define REG_INTRXE    0x4A
#define REG_INTUSB    0x4C
#define REG_INTUSBE   0x50
#define REG_EPINDEX   0x42
#define REG_CSR0      0x82
#define REG_TXMAXP    0x80
#define REG_TXCSRL    0x82
#define REG_TXCSRH    0x83
#define REG_RXMAXP    0x84
#define REG_RXCSRL    0x86
#define REG_RXCSRH    0x87
#define REG_RXCOUNT   0x88
#define REG_TXTYPE    0x8C
#define REG_RXTYPE    0x8E
#define REG_FSIZE     0x90
#define REG_EPFIFO(n) (0x00 + (4*(n)))   /* sunxi: FIFOs start at 0x00 */
#define REG_DEVCTL    0x41
#define REG_TXFIFOSZ  0x90
#define REG_RXFIFOSZ  0x94
#define REG_TXFIFOADD 0x92
#define REG_RXFIFOADD 0x96

#define REG_AWIN_VEND0 0x43

/* Allwinner vendor "Interface Status and Control" register (32-bit, at
 * MUSB base + 0x400). U-Boot's sunxi glue FORCES the D+/D- pull-up at the
 * PHY level via DPDM_PULLUP_EN — which is why merely dropping POWER.SOFTCONN
 * is invisible to the host (verified: 111M-poll run, host never saw a
 * disconnect, intr_seen=0, setups=0). A real disconnect/reconnect must
 * toggle this bit. Writes must keep the write-1-to-clear change-detect
 * bits (4..6) cleared or we'd ack pending change events by accident. */
#define REG_ISCR              0x0400
#define ISCR_DPDM_PULLUP_EN   (1u << 16)
#define ISCR_CHANGE_DETECT    ((1u << 4) | (1u << 5) | (1u << 6))

/* Power */
#define POWER_SOFTCONN  0x40
#define POWER_HSENAB    0x20
#define POWER_RESET     0x08

/* EP0 CSR0 */
#define CSR0_RXPKTRDY    0x01
#define CSR0_TXPKTRDY    0x02
#define CSR0_P_SENDSTALL 0x20
#define CSR0_P_SETUPEND  0x10
#define CSR0_P_SENTSTALL 0x04  /* HW set once a sent STALL is taken by host */
#define CSR0_P_DATAEND   0x08
#define CSR0_P_SVDRXPKTRDY 0x40  /* write-1: SETUP consumed, clears RXPKTRDY */
#define CSR0_P_SVDSETUPEND 0x80  /* write-1: clears SETUPEND */
#define CSR0_FLUSHFIFO   0x0100

/* TXCSR / RXCSR (EP1+) */
#define TXCSR_TXPKTRDY  0x01
#define TXCSR_FLUSHFIFO 0x08
#define RXCSR_RXPKTRDY  0x01

/* INTUSB — Mentor spec bit order (the reference had RESET at bit0, which is
 * actually SUSPEND: every bus suspend then re-armed EP0 and zeroed FADDR,
 * so the device kept losing its freshly-assigned address — the host's
 * "device not accepting address" in a nutshell). */
#define INTR_SUSPEND    0x01
#define INTR_RESUME     0x02
#define INTR_RESET      0x04
#define INTR_SOF        0x08
#define INTR_CONNECT    0x10
#define INTR_DISCONNECT 0x20

/* EP1 FIFO layout for the bulk IN/OUT pair. The reference wrote
 * TXFIFOSZ=0x09 while claiming "512 bytes"; per the Mentor MUSB FIFO-size
 * encoding (size = 8 << field) that value actually means 4096 bytes, and
 * the reference's own comment flags the discrepancy. We use field 6
 * (8 << 6 = 512 bytes) for both directions, which matches the 512-byte
 * TXMAXP/RXMAXP we configure, and place RX right after TX in FIFO RAM
 * (addresses are in 8-byte units) so the two never overlap. */
#define EP1_FIFO_SZ_CODE   6       /* 8 << 6 = 512 bytes */
#define EP1_TX_FIFO_ADDR   0x0080  /* byte offset 1024 */
#define EP1_RX_FIFO_ADDR   0x00c0  /* byte offset 1536 (right after TX)  */

/* ------------------------------------------------------------------ */
/* USB device state                                                    */
/* ------------------------------------------------------------------ */
enum { ST_RESET, ST_WAIT_SETUP, ST_CONNECTED };

static int usb_state = ST_RESET;
static int usb_ready = 0;

/* ------------------------------------------------------------------ */
/* Explicit EP0 control-endpoint state machine                          */
/* ------------------------------------------------------------------ */
/* Ported 1:1 from U-Boot musb_gadget_ep0.c (this exact silicon's
 * driver): the ep0_state enum (musb_core.h:128-134), the
 * musb_g_ep0_irq() stage switch (ep0.c:709-895), ep0_txstate()
 * (ep0.c:527-576), ep0_rxstate() (ep0.c:473-519) and musb_read_setup()
 * (ep0.c:584-630). Real MUSB hardware demands this: SETUP must be acked
 * with SVDRXPKTRDY *alone* first (then poll RXPKTRDY low) before the IN
 * FIFO is loaded, DATAEND must ride ONLY the final data packet, and the
 * OUT/IN status stage plus its return-to-IDLE (with coalesced-SETUP
 * detection) must run explicitly. The old ad-hoc ep0_tx() collapsed all
 * of that into one CSR0 write, which is why the host looped forever on
 * GET_DESCRIPTOR(device). Codes match U-Boot's enum values so the
 * breadcrumb state-history nibbles read the same. */
enum {
    EP0_IDLE = 0,       /* idle, waiting for SETUP */
    EP0_SETUP,          /* received SETUP */
    EP0_TX,             /* IN data stage (device -> host) */
    EP0_RX,             /* OUT data stage (host -> device) */
    EP0_STATUSIN,       /* IN status (follows an OUT data stage / no-data) */
    EP0_STATUSOUT,      /* OUT status (follows an IN data stage) */
    EP0_ACKWAIT         /* zero-data request, before status IN */
};

static int ep0_state = EP0_IDLE;

/* Current control-IN transfer (multi-packet: config_desc is 67 > 64). */
static const uint8_t *ep0_in_buf;
static int ep0_in_len;
static int ep0_in_actual;

/* Current control-OUT (data-stage) transfer sink (e.g. SET_LINE_CODING). */
static uint8_t ep0_out_buf[64];
static int ep0_out_len;
static int ep0_out_actual;

/* Deferred SET_ADDRESS: FADDR is written ONLY at the STATUSIN stage, per
 * USB spec and U-Boot ep0.c:735-738. */
static int ep0_set_address_pend;
static uint8_t ep0_address;

/* ackpend: CSR0 bits (SVDRXPKTRDY etc.) staged during SETUP parse and
 * committed with the arming write, exactly like musb->ackpend. */
static uint16_t ep0_ackpend;

/* CDC-ACM line coding: 115200 8N1 (dwDTERate LE, bCharFormat, bParityType,
 * bDataBits). Kept so GET/SET_LINE_CODING complete and ttyACM opens. */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };
static uint8_t ep0_in_scratch[2];
static uint8_t current_config;

/* DRAM breadcrumb — layout defined in main.c (word0 magic, word1 stage,
 * word2 poll_count, word3 usb_state, word4 usb_ready, word5 intr_seen,
 * word6 setup_count, word7 last_csr0). Read post-WDT-reset from U-Boot
 * via `md.l 0x50000000 8`. */
static inline void bc_write(int i, uint32_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)(0x50000000UL + (uint32_t)i * 4u);
    *p = v;
    /* clean+invalidate to PoC + barrier — see main.c: cached BC writes die
     * with the WDT reset and the post-reset readout shows stale DRAM. */
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}
#define BC(i, v) bc_write((i), (uint32_t)(v))
#define BC_STAGE_MUSB_INIT 3u   /* entered musb_init() */

/* musb_init()/musb_phy_init() progress marker in word34 (0x50000088). Written
 * (cache-flushed) at every phase so that if bring-up wedges on a register/FIFO
 * access, the post-WDT `md.l 0x50000088 1` shows EXACTLY the last phase reached
 * (0x1x = musb_phy_init phases, 0x2x = musb_init phases, 0x2F = fully done). */
#define MI(stage) BC(34, (uint32_t)(stage))

/* musb_poll() sub-phase marker in word35 (0x5000008c) + last INTRUSB read in
 * word36 (0x50000090). Same technique as MI() but for the poll path: written
 * (cache-flushed) at each register-touching step so a live-traffic hang is
 * localized to the exact stalling access via `md.l 0x5000008c 2`. The steady
 * no-interrupt path writes only a couple of markers per poll to stay cheap. */
#define PP(code) BC(35, (uint32_t)(code))
static uint32_t bc_poll_count, bc_intr_seen, bc_setup_count;

/* EP0 request trace: words[8..15] hold a ring of the last 8 control requests,
 * packed (bmReqType<<24)|(bReq<<16)|wValue, so a post-WDT `md.l 0x50000000 16`
 * shows EXACTLY which SETUP sequence the host sent and where it stalled. */
static uint32_t bc_req_idx;
static inline void bc_log_req(uint8_t bmreq, uint8_t breq, uint16_t wval)
{
    BC(8 + (bc_req_idx++ & 7u), ((uint32_t)bmreq << 24) | ((uint32_t)breq << 16) | wval);
}

/* EP0 state-machine telemetry, breadcrumb words 18..23 (read post-WDT via
 * `md.l 0x50000000 24`). Words 16/17 remain SET_ADDRESS telemetry.
 *
 *   word18 = ep0_state transition history — a shift register of 4-bit
 *            state codes (EP0_IDLE=0 .. EP0_ACKWAIT=6), newest in the low
 *            nibble, so the 8 most recent transitions read left->old.
 *   word19 = total EP0 state-transition count.
 *   word20 = last STATUS-stage outcome: (status_count<<8) | code, where
 *            code 1 = STATUSIN completed (no-data / OUT-data control xfer),
 *            code 2 = STATUSOUT completed (IN-data control xfer). A rising
 *            count with no forward enumeration means status is looping.
 *   word21 = last SETUP dispatched: (ep0_state_at_dispatch<<16) |
 *            (bmRequestType<<8) | bRequest.
 *   word22 = last control-IN progress: (ep0_in_actual<<16) | ep0_in_len.
 *            Device desc -> 0x0012_0012, config desc -> 0x0043_0043.
 *   word23 = IN-path proof counters, one byte each (saturating at 0xFF):
 *            (txstate_enter<<24)|(txpktrdy_set<<16)|(readsetup_enter<<8)|
 *            (in_branch_enter). Nonzero byte3/byte2 proves ep0_txstate ran
 *            and armed TXPKTRDY (i.e. a data packet was actually pushed).
 *            (Last CSR0 lives in word7.) */
static uint32_t ep0_state_hist;
static uint32_t ep0_trans_count;
static uint32_t ep0_status_count;
static uint32_t ep0_stall_count;
/* TX-path proof counters (byte-packed into word23) — prove the IN data
 * stage actually fires on the next hardware run. */
static uint8_t ep0_txstate_enter;   /* ep0_txstate() entered */
static uint8_t ep0_txpktrdy_set;    /* CSR0 write with TXPKTRDY set */
static uint8_t ep0_readsetup_enter; /* ep0_read_setup() entered */
static uint8_t ep0_in_branch_enter; /* IN data-stage branch taken */

static inline void bc_tx_counters(void)
{
    BC(23, ((uint32_t)ep0_txstate_enter << 24) |
           ((uint32_t)ep0_txpktrdy_set << 16) |
           ((uint32_t)ep0_readsetup_enter << 8) |
           (uint32_t)ep0_in_branch_enter);
}

/* Enumeration-progress counters, breadcrumb words 24..27 (extend the readout
 * to `md.l 0x50000000 28`). These answer WHERE the host stops:
 *
 *   word24 = (GET_DESC device count << 16) | GET_DESC config count.
 *            If the config half stays 0, the host NEVER asks for the config
 *            descriptor — it's dissatisfied with the device descriptor / the
 *            SET_ADDRESS handshake and keeps restarting. If it is nonzero, the
 *            host DOES request config (so the failure is in the config
 *            transfer itself, e.g. the wTotalLength/length bug fixed above).
 *   word25 = (SET_CONFIGURATION count << 16) | bus-reset count. Nonzero low
 *            half with SET_CONFIG staying 0 = host resets instead of
 *            configuring. SET_CONFIG going nonzero == enumeration COMPLETE.
 *   word26 = (GET_DESC string count << 16) | GET_DESC other count. "other" =
 *            descriptor types we don't provide (6=DEVICE_QUALIFIER,
 *            15=BOS, ...) which we STALL — a burst here can itself upset a
 *            picky host.
 *   word27 = (SET_ADDRESS count << 16) | last config-descriptor wLength.
 *            The wLength shows the two-step config read (9 to learn
 *            wTotalLength, then the full 75). */
static uint16_t ep0_cnt_getdesc_dev;
static uint16_t ep0_cnt_getdesc_cfg;
static uint16_t ep0_cnt_getdesc_str;
static uint16_t ep0_cnt_getdesc_other;
static uint16_t ep0_cnt_setconfig;
static uint16_t ep0_cnt_setaddr;
static uint16_t ep0_cnt_busreset;
static uint16_t ep0_last_cfg_wlen;

/* SET_ADDRESS / post-address handshake diagnostics, breadcrumb words 32..33
 * (extend readout to `md.l 0x50000000 34`) — pinpoint the -71 "device not
 * accepting address":
 *
 *   word32 = (count of SETUPs received while FADDR!=0 << 16) | last FADDR
 *            observed at a SETUP. If the high half stays 0, the device NEVER
 *            receives a SETUP at its assigned address — i.e. the host talks to
 *            the new address and gets no response (FADDR never took / took too
 *            late). If it's nonzero, the device IS answering at the new
 *            address and the failure is later.
 *   word33 = (INTR_RESET count << 16) | INTR_SUSPEND count. Distinguishes real
 *            host bus-resets from suspends; a suspend must NOT zero FADDR. If
 *            reset count balloons relative to SET_ADDRESS, the host is retrying
 *            because the address didn't take. (word16/17 still hold the last
 *            FADDR readback + set-address timeout flag.) */
static uint16_t ep0_cnt_setup_at_addr;
static uint8_t  ep0_last_setup_faddr;
static uint16_t ep0_cnt_intr_reset;
static uint16_t ep0_cnt_intr_suspend;

/* EP0 IN data-stage micro-telemetry, breadcrumb words 40..43 (extend readout
 * to `md.l 0x50000000 44`) — pinpoint why the first GET_DESCRIPTOR(device) IN
 * data never reaches the host (-110):
 *
 *   word40 = (GET_DESC(device) SETUP count << 16) | last device wLength.
 *   word41 = (bytes loaded into EP0 FIFO for the last IN packet << 16) |
 *            CSR0 read back IMMEDIATELY after we set TXPKTRDY. Bit1 (0x0002)
 *            of the low half MUST be set = TXPKTRDY latched (packet armed);
 *            bit3 (0x0008)=DATAEND, bit6 (0x0040)=SVDRXPKTRDY still pending.
 *   word42 = (times HW CLEARED TXPKTRDY, i.e. host ACKed/read the packet
 *            << 16) | times we polled EP0_TX and TXPKTRDY was still SET (host
 *            hasn't read yet). Cleared>0 == the host IS reading EP0 IN.
 *   word43 = (ep0_state at last EP0 IN activity << 8) | flags: bit0 = last IN
 *            packet carried DATAEND, bit1 = it was a short/terminating packet
 *            (len < 64). */
static uint16_t ep0_cnt_getdesc_dev_setup;
static uint16_t ep0_last_devdesc_wlen;
static uint16_t ep0_last_fifo_count;
static uint16_t ep0_csr0_after_tx;
static uint16_t ep0_txpktrdy_cleared;
static uint16_t ep0_txpktrdy_stuck;
static uint8_t  ep0_tx_flags;

/* word45 (0x500000b4) EP0 poll/transition trace: (marker<<24)|(ep0_state<<16)|
 * CSR0. marker 0xE0 = a SETUPEND was serviced this poll; 0x50 = throttled
 * periodic snapshot. On a freeze it shows the exact state+CSR0 last seen and
 * whether SETUPEND diverted us. Low telemetry: also counts SETUPENDs. */
static uint16_t ep0_setupend_count;

/* word46 (0x500000b8) FULL GET_DESCRIPTOR(device) tracking, kept separate from
 * the 8-byte probe (word40): (count of device GET_DESC with wLength>8 << 16) |
 * last such wLength. Nonzero == the host asked for the FULL descriptor after
 * the probe and we decoded it; if it stays 0 while the probe (word40) shows a
 * wLength=8 read, the 2nd/full GET_DESCRIPTOR is not reaching ep0_service_in
 * (host talking to an address we don't answer, or the SETUP being dropped). */
static uint16_t ep0_cnt_getdesc_full;
static uint16_t ep0_last_full_wlen;

/* word44 (0x500000b0) TXPKTRDY life-cycle for the current armed EP0 IN packet.
 * NOTE: with DATAEND folded onto the final packet, a SINGLE-packet control-IN
 * (device desc, 8-byte probe, GET_STATUS, ...) goes straight to EP0_STATUSOUT
 * and the controller auto-runs the status stage — so the EP0_TX poll never
 * re-observes it and word44 legitimately STAYS 0xA. That is NOT a freeze now;
 * real success is measured by enumeration progress (word40 device count, word24
 * config, word25 setconfig, usb_ready). 0xB/0xC only appear for MULTI-packet
 * config_desc (its non-final 64-byte packet):
 *   0xA000_00NN = just armed (NN = bytes). Normal terminal value for 1 packet.
 *   0xB000_00NN = a non-final packet is stuck (host not draining) — bad.
 *   0xC000_00NN = a non-final packet was drained (NN = polls) — healthy. */
static uint16_t ep0_tx_armed_polls;

static inline void bc_tx_detail(void)
{
    BC(40, ((uint32_t)ep0_cnt_getdesc_dev_setup << 16) | ep0_last_devdesc_wlen);
    BC(41, ((uint32_t)ep0_last_fifo_count << 16) | ep0_csr0_after_tx);
    BC(42, ((uint32_t)ep0_txpktrdy_cleared << 16) | ep0_txpktrdy_stuck);
    BC(43, ((uint32_t)ep0_state << 8) | ep0_tx_flags);
}

static inline void bc_enum_counters(void)
{
    BC(24, ((uint32_t)ep0_cnt_getdesc_dev << 16) | ep0_cnt_getdesc_cfg);
    BC(25, ((uint32_t)ep0_cnt_setconfig << 16) | ep0_cnt_busreset);
    BC(26, ((uint32_t)ep0_cnt_getdesc_str << 16) | ep0_cnt_getdesc_other);
    BC(27, ((uint32_t)ep0_cnt_setaddr << 16) | ep0_last_cfg_wlen);
    BC(32, ((uint32_t)ep0_cnt_setup_at_addr << 16) | ep0_last_setup_faddr);
    BC(33, ((uint32_t)ep0_cnt_intr_reset << 16) | ep0_cnt_intr_suspend);
}

static void ep0_set_state(int s)
{
    if (s != ep0_state) {
        ep0_state_hist = (ep0_state_hist << 4) | ((uint32_t)s & 0xFu);
        BC(18, ep0_state_hist);
        BC(19, ++ep0_trans_count);
    }
    ep0_state = s;
}

/* TX staging RING, drained to EP1 IN one max-packet at a time by
 * musb_service_ep1_tx() (called from musb_poll() on every service pass, and
 * once from musb_flush()). musb_putc() ONLY appends here and NEVER blocks:
 * the push to the host is entirely poll-driven, one bounded 64-byte packet
 * per musb_poll() call, so no producer ever spins waiting for the host to
 * drain the gadget.
 *
 * WHY A RING NOW, NOT THE OLD LINEAR tx_pos BUFFER + musb_tx_flush_now() SPIN:
 * musb_tx_flush_now() busy-waited up to 200000 musb_poll() iterations PER
 * FLUSH for the EP1-IN FIFO to drain (TXCSRL.TXPKTRDY to clear). On the CPU1
 * debug core that also services dbgmon/EMAC (smp.c), a host that stops reading
 * /dev/ttyACM would pin that spin and starve the lifeline channel — which is
 * exactly why usbacm_poll() had to be gated OFF (smp.c dbg_usbacm). With a
 * ring drained incrementally by musb_poll(), a stalled host merely means
 * TXPKTRDY stays set, every push returns immediately, bytes accumulate here
 * until full, and the NEWEST are dropped — fine for a debug console, and it
 * never costs CPU1 more than one TXCSRL read per poll.
 *
 * Single-producer / single-consumer on ONE core (CPU1's usbacm_poll(), or a
 * CPU0 main*.c loop when musb.c is used stand-alone) — no cross-core access,
 * so plain indices need no barriers. TX_BUF_SIZE is a power of two so the
 * head/tail wrap by mask; one slot is always left empty to tell full from
 * empty (usable capacity = TX_BUF_SIZE-1 bytes). */
#define TX_BUF_SIZE       512   /* power of two */
static uint8_t tx_buf[TX_BUF_SIZE];
static unsigned tx_head = 0;    /* producer: musb_putc() writes here        */
static unsigned tx_tail = 0;    /* consumer: musb_service_ep1_tx() reads    */

/* RX ring, filled by musb_poll() draining EP1 OUT, drained by musb_getc(). */
#define RX_RING_SIZE 256 /* power of two */
static uint8_t rx_ring[RX_RING_SIZE];
static unsigned rx_head = 0, rx_tail = 0;

/* ------------------------------------------------------------------ */
/* Low-level MMIO access — absolute physical addresses, volatile only. */
/* ------------------------------------------------------------------ */
static inline uint8_t
musb_read8(uint32_t off)
{
    return *(volatile uint8_t *)(MUSB_MMIO_BASE + off);
}

static inline void
musb_write8(uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(MUSB_MMIO_BASE + off) = val;
}

static inline uint32_t
musb_read32(uint32_t off)
{
    return *(volatile uint32_t *)(volatile void *)(MUSB_MMIO_BASE + off);
}

static inline void
musb_write32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(volatile void *)(MUSB_MMIO_BASE + off) = val;
}

static inline uint16_t
musb_read16(uint32_t off)
{
    return *(volatile uint16_t *)(volatile void *)(MUSB_MMIO_BASE + off);
}

static inline void
musb_write16(uint32_t off, uint16_t val)
{
    *(volatile uint16_t *)(volatile void *)(MUSB_MMIO_BASE + off) = val;
}

/* FIFO width matters: each access pushes/pulls EXACTLY its width in bytes into
 * the packet. The old code issued a full 32-bit write even for the last
 * partial word, so an 18-byte device descriptor went out as a 20-byte packet
 * (2 pad bytes), 75-byte config as 76, odd strings padded — the host saw a
 * wrong-length control-IN packet and never accepted the data stage (device
 * descriptor read timed out, -110).
 *
 * Access widths: 32-bit words for the aligned head, then INDIVIDUAL BYTES for
 * the 0..3-byte tail — NO 16-bit halfword accesses. Some sunxi MUSB cores are
 * documented word+byte only; byte access is definitely decoded (U-Boot's
 * writesb path uses it), so word-head + byte-tail is the safest sequence that
 * still emits exactly `len` bytes (never the old full-word padding that sent
 * an 18-byte descriptor as 20 -> host rejected the data stage, -110). All
 * callers pass word-aligned buffers (static descriptors, tx_buf, chunk[]). */
static void
musb_fifo_write(int ep, const uint8_t *data, int len)
{
    volatile uint32_t *fifo32 = (volatile uint32_t *)(MUSB_MMIO_BASE + REG_EPFIFO(ep));
    volatile uint8_t  *fifo8  = (volatile uint8_t  *)(MUSB_MMIO_BASE + REG_EPFIFO(ep));
    int i = 0;

    for (; i + 4 <= len; i += 4) {
        uint32_t word = (uint32_t)data[i]
                      | ((uint32_t)data[i + 1] << 8)
                      | ((uint32_t)data[i + 2] << 16)
                      | ((uint32_t)data[i + 3] << 24);
        *fifo32 = word;
    }
    for (; i < len; i++)                      /* 0..3-byte tail, byte-accurate */
        *fifo8 = data[i];
}

static void
musb_fifo_read(int ep, uint8_t *data, int len)
{
    volatile uint32_t *fifo32 = (volatile uint32_t *)(MUSB_MMIO_BASE + REG_EPFIFO(ep));
    volatile uint8_t  *fifo8  = (volatile uint8_t  *)(MUSB_MMIO_BASE + REG_EPFIFO(ep));
    int i = 0;

    for (; i + 4 <= len; i += 4) {
        uint32_t word = *fifo32;
        data[i]     = (uint8_t)(word);
        data[i + 1] = (uint8_t)(word >> 8);
        data[i + 2] = (uint8_t)(word >> 16);
        data[i + 3] = (uint8_t)(word >> 24);
    }
    for (; i < len; i++)                      /* 0..3-byte tail, byte-accurate */
        data[i] = *fifo8;
}

/* Defensive: re-assert the OTG bus clock gate. Cheap (one OR to one
 * register), safe to call every poll. */
static inline void
ensure_musb_clock(void)
{
    volatile uint32_t *reg = (volatile uint32_t *)(CCU_MMIO_BASE + CCU_BUS_GATING0);
    *reg |= CCU_BUS_OTG_GATE;   /* 0x060 BIT23 — CLK_BUS_OTG (bus clock to MUSB) */
}

/* ------------------------------------------------------------------ */
/* USB PHY0 bring-up — ported from U-Boot's working sunxi gadget path   */
/* (drivers/usb/musb-new/sunxi.c sunxi_musb_init + enable, and          */
/* drivers/phy/allwinner/phy-sun4i-usb.c sun4i_usb_phy_init with the    */
/* sun50i_a64_cfg profile). Our old musb_init only ungated the OTG bus  */
/* gate and toggled ISCR/POWER/DEVCTL — it NEVER powered/calibrated the  */
/* separate USB-PHY block. If U-Boot tore down its gadget before running */
/* us (sunxi_musb_exit disables the PHY0 clock + asserts its reset), the */
/* PHY was left powered-down/uncalibrated: the D+ pull-up alone made the */
/* host see "a device", but the analog PHY produced garbage signalling,  */
/* so the host reset the bus over and over and enumeration never got     */
/* past the device descriptor (24x GET_DESC(dev), 9x reset, 0 config).   */
/* ------------------------------------------------------------------ */

/* PHY calibration profile for sun50i-a64 (phy-sun4i-usb.c sun50i_a64_cfg:
 * phyctl_offset=REG_PHYCTL_A33(0x10), disc_thresh=3, hci_phy_ctl_clear=
 * PHY_CTL_H3_SIDDQ=BIT1, phy0_dual_route=true, siddq_in_base=false). */
#define PHY_CTL_H3_SIDDQ      (1u << 1)   /* PMU HCI_PHY_CTL: 1 = PHY powered down */
#define PHYCTL_DATA           (1u << 7)
#define OTGCTL_ROUTE_MUSB     (1u << 0)
#define PHY_RES45_CAL_EN      0x0c        /* 45-ohm resistor calibration enable  */
#define PHY_TX_AMPLITUDE_TUNE 0x20
#define PHY_DISCON_TH_SEL     0x2a
#define PHY_TX_RATE           (1u << 4)
#define PHY_TX_MAGNITUDE      (1u << 2)
#define A64_DISC_THRESH       3

/* Bit-banged PHY calibration write (phy-sun4i-usb.c sun4i_usb_phy_write, A33+
 * variant). usbc_bit = BIT(id*2) = BIT0 for PHY0. Address occupies PHYCTL
 * bits [15:8]; the data bit is PHYCTL[7]; usbc_bit is the strobe. */
static void
usbphy_calib_write(uint32_t addr, uint32_t data, int len)
{
    volatile uint8_t  *pc8  = (volatile uint8_t  *)USBPHY_PHYCTL;
    volatile uint32_t *pc32 = (volatile uint32_t *)USBPHY_PHYCTL;
    const uint8_t usbc_bit = 0x01;   /* BIT(0) — PHY0 */
    int i;

    *pc32 = 0;   /* A33+: must clear PHYCTL before the sequence */
    for (i = 0; i < len; i++) {
        uint32_t t = *pc32;
        uint8_t b;

        t &= ~(0xffu << 8);
        t |= ((addr + (uint32_t)i) & 0xffu) << 8;
        *pc32 = t;

        b = *pc8;
        if (data & 1u) b |= PHYCTL_DATA; else b &= (uint8_t)~PHYCTL_DATA;
        b &= (uint8_t)~usbc_bit;
        *pc8 = b;

        b = *pc8; b |= usbc_bit;  *pc8 = b;   /* pulse strobe high */
        b = *pc8; b &= (uint8_t)~usbc_bit; *pc8 = b;   /* pulse strobe low  */

        data >>= 1;
    }
}

static void
musb_phy_init(void)
{
    volatile uint32_t *gate0  = (volatile uint32_t *)(CCU_MMIO_BASE + CCU_BUS_GATING0);
    volatile uint32_t *usbcfg = (volatile uint32_t *)(CCU_MMIO_BASE + CCU_USB_CLK_CFG);
    volatile uint32_t *reset0 = (volatile uint32_t *)(CCU_MMIO_BASE + CCU_BUS_RESET0);
    volatile uint32_t *hcictl = (volatile uint32_t *)USBPHY_HCI_CTL;
    volatile uint32_t *otgctl = (volatile uint32_t *)USBPHY_OTGCTL;
    volatile uint32_t *sramc4 = (volatile uint32_t *)(SRAMC_MMIO_BASE + 0x04);
    int i;

    MI(0x10);
    /* 1. Ungate the MUSB bus clock + deassert its reset, and ungate/​deassert
     *    the USB-PHY0 clock+reset (sunxi_musb_init clk_enable/reset_deassert
     *    + sun4i_usb_phy_init clk_enable/reset_deassert). */
    *gate0  |= CCU_BUS_OTG_GATE;      /* 0x060 BIT23 CLK_BUS_OTG   */
    *reset0 |= CCU_BUS_OTG_RST;       /* 0x2c0 BIT23 RST_BUS_OTG   */
    *usbcfg |= CCU_USBPHY0_CLK_GATE;  /* 0x0cc BIT8  CLK_USB_PHY0  */
    *usbcfg |= CCU_USBPHY0_RST;       /* 0x0cc BIT0  RST_USB_PHY0  */
    for (i = 0; i < 1000; i++) __asm__ volatile("nop");
    MI(0x11);

    /* 2. Map the USB FIFO SRAM (USBC_ConfigFIFO_Base): SRAMC+0x04 [1:0]=01. */
    { uint32_t v = *sramc4; v &= ~0x3u; v |= 0x1u; *sramc4 = v; }
    MI(0x12);

    /* 3. Power the PHY analog block UP: clear SIDDQ in PMU HCI_PHY_CTL
     *    (sun4i_usb_phy_init, hci_phy_ctl_clear = PHY_CTL_H3_SIDDQ). This is
     *    the step most likely missing after a U-Boot gadget teardown. */
    *hcictl &= ~PHY_CTL_H3_SIDDQ;
    MI(0x13);

    /* 4. PHY calibration (sun4i_usb_phy_init, else-branch for !siddq_in_base):
     *    45-ohm cal enable (PHY0 only), TX amplitude/rate tune, disconnect
     *    threshold. Wrong/absent tuning = the signal-integrity failure that
     *    makes the host keep resetting. */
    usbphy_calib_write(PHY_RES45_CAL_EN, 1, 1);
    usbphy_calib_write(PHY_TX_AMPLITUDE_TUNE, PHY_TX_MAGNITUDE | PHY_TX_RATE, 5);
    usbphy_calib_write(PHY_DISCON_TH_SEL, A64_DISC_THRESH, 2);
    MI(0x14);

    /* 5. Route PHY0 to MUSB so the gadget (not EHCI/OHCI) owns the port
     *    (sun4i_usb_phy0_reroute(data, true) — phy0_dual_route). */
    *otgctl |= OTGCTL_ROUTE_MUSB;
    MI(0x15);

    /* Telemetry (words 28..31, read via `md.l 0x50000000 32`):
     *   word28 = CCU usb_clk_cfg (0x0cc): expect bit8 (PHY0 clk) & bit0
     *            (PHY0 rst deasserted) set -> low bits 0x101.
     *   word29 = PMU HCI_PHY_CTL: bit1 (SIDDQ) MUST read 0 (PHY powered up).
     *   word30 = bring-up status bits: b0=BUS_OTG gate, b1=BUS_OTG reset
     *            deasserted, b2=PHY0 routed to MUSB. Healthy = 0x7.
     *   word31 filled later with the final ISCR readback. */
    BC(28, *usbcfg);
    BC(29, *hcictl);
    BC(30, ((*gate0 & CCU_BUS_OTG_GATE) ? 1u : 0u) |
           ((*reset0 & CCU_BUS_OTG_RST) ? 2u : 0u) |
           ((*otgctl & OTGCTL_ROUTE_MUSB) ? 4u : 0u));
}

/* ISCR: enable the D+/D- and ID pull-ups and FORCE ID=high (peripheral/B) and
 * VBUS=valid (sunxi_musb_init USBC_EnableDpDmPullUp/EnableIdPullUp/
 * ForceIdToHigh + enable's ForceVbusValidToHigh). Forcing VBUS valid is what
 * makes the controller believe a host is attached and start a session — our
 * old code relied only on DEVCTL.Session and the DPDM pull-up. Every write
 * keeps the write-1-to-clear change-detect bits (4..6) cleared. */
static void
usbphy_iscr_setup(void)
{
    uint32_t v = musb_read32(REG_ISCR);

    v &= ~ISCR_CHANGE_DETECT;
    v |= (1u << 16);                       /* DPDM_PULLUP_EN               */
    v |= (1u << 17);                       /* ID_PULLUP_EN                 */
    v &= ~(0x3u << 14); v |= (0x3u << 14); /* FORCE_ID = 0b11 (high/B-dev) */
    v &= ~(0x3u << 12); v |= (0x3u << 12); /* FORCE_VBUS_VALID = 0b11      */
    musb_write32(REG_ISCR, v);
    BC(31, musb_read32(REG_ISCR));
}

/* ------------------------------------------------------------------ */
/* CDC-ACM descriptors (verbatim from the reference / PROJECT.md)      */
/* ------------------------------------------------------------------ */
/* 18-byte USB device descriptor. The reference we inherited was only 17
 * bytes — bcdDevice had been squished to 1 byte — while bLength still said
 * 18, so every GET_DESCRIPTOR(device) delivered a short packet the host
 * rejected, looping enumeration forever (root cause of the -71). Proven via
 * the live REPL: word22 breadcrumb read 17/17. Full correct layout below;
 * bDeviceClass/SubClass/Protocol = EF/02/01 (Misc, IAD) to match the IAD in
 * config_desc. */
static const uint8_t device_desc[] = {
    18, 1,            /* bLength=18, bDescriptorType=DEVICE               */
    0x00, 0x02,       /* bcdUSB   = 2.00                                  */
    0xEF, 0x02, 0x01, /* class/subclass/protocol = Misc / common / IAD   */
    64,               /* bMaxPacketSize0                                  */
    0x6b, 0x1d,       /* idVendor  = 0x1d6b                               */
    0x10, 0x00,       /* idProduct = 0x0010                               */
    0x00, 0x01,       /* bcdDevice = 1.00                                 */
    1, 2, 3,          /* iManufacturer / iProduct / iSerialNumber         */
    1                 /* bNumConfigurations                               */
};

/* wTotalLength MUST equal the full descriptor set: Config(9) + IAD(8) +
 * CommIF(9) + Header(5) + CallMgmt(5) + ACM(4) + Union(5) + NotifyEP(7) +
 * DataIF(9) + EP-IN(7) + EP-OUT(7) = 75 (0x4B). It previously read 67 — the
 * 8-byte IAD was added to the array without bumping wTotalLength, so a host
 * that read the 9-byte header (wTotalLength=67) then requested 67 bytes got a
 * config truncated by 8 bytes (the final EP1-OUT descriptor), rejected it and
 * re-enumerated from the device descriptor forever. sizeof(config_desc)==75
 * now matches this field; keep them in lock-step. */
static const uint8_t config_desc[] = {
    /* Config */
    9, 2, 75, 0, 2, 1, 0, 0xc0, 32,
    /* IAD */
    8, 11, 0, 2, 2, 2, 0, 0,
    /* Control IF */
    9, 4, 0, 0, 1, 2, 2, 1, 0,
    /* Header */
    5, 0x24, 0, 0x10, 0x01,
    /* Call Mgmt */
    5, 0x24, 1, 0, 1,
    /* ACM */
    4, 0x24, 2, 2,
    /* Union */
    5, 0x24, 6, 0, 1,
    /* EP2 IN (notify, unused but required) */
    7, 5, 0x82, 3, 0x40, 0x00, 32,
    /* Data IF */
    9, 4, 1, 0, 2, 0x0a, 0, 0, 0,
    /* EP1 IN (bulk console TX) — wMaxPacketSize=64 (full-speed bulk max) */
    7, 5, 0x81, 2, 0x40, 0x00, 0,
    /* EP1 OUT (bulk RX) — wMaxPacketSize=64 (full-speed bulk max) */
    7, 5, 0x01, 2, 0x40, 0x00, 0,
};

/* Guard the wTotalLength <-> array-size invariant at compile time so this
 * never silently drifts again (config_desc[2] is wTotalLength's low byte). */
_Static_assert(sizeof(config_desc) == 75, "config_desc size != wTotalLength");

static const uint8_t string0[] = { 4, 3, 0x09, 0x04 };
/* Proper USB string descriptors: [bLength, bDescriptorType=3, UTF-16LE...].
 * The reference returned raw ASCII (byte0 = 'b' = 0x62, read by the host as a
 * bogus 98-byte length), which corrupts EP0 the moment the host reads a
 * string. bLength = 2 + 2*nchars. */
static const uint8_t string1[] = { 12, 3, 'b',0,'z',0,'d',0,'O',0,'S',0 };
static const uint8_t string2[] = { 24, 3, 'U',0,'S',0,'B',0,' ',0,'C',0,'o',0,
                                   'n',0,'s',0,'o',0,'l',0,'e',0 };
static const uint8_t string3[] = { 8, 3, '0',0,'.',0,'1',0 };

/* Configure the EP1 bulk IN/OUT pair — the data-carrying half of the
 * SET_CONFIGURATION no-data request. Must leave EPINDEX == 0 on return, so
 * the caller's CSR0 (0x82) status-stage write lands on EP0, not EP1's
 * TXCSRL. */
static void
ep0_configure_endpoints(void)
{
    musb_write8(REG_EPINDEX, 1);

    /* EP1 IN: bulk, 64-byte max packet (full-speed bulk max — must match the
     * wMaxPacketSize=64 in config_desc), own FIFO slice. TXCSR is a 16-bit
     * register; MODE (0x2000) selects TX direction for the shared EP core,
     * FLUSHFIFO (0x0008) + CLRDATATOG (0x0040) start it clean. The FIFO RAM
     * slice stays generously sized (EP1_FIFO_SZ_CODE) — a FIFO larger than
     * the max packet is harmless; only TXMAXP must match the descriptor. */
    musb_write16(REG_TXMAXP, 64);
    musb_write8(REG_TXTYPE, 2); /* bulk */
    musb_write8(REG_TXFIFOSZ, EP1_FIFO_SZ_CODE);
    musb_write16(REG_TXFIFOADD, EP1_TX_FIFO_ADDR);
    musb_write16(REG_TXCSRL, 0x2000 | 0x0008 | 0x0040);

    /* EP1 OUT: bulk, 64-byte max packet, adjacent FIFO slice.
     * RXCSR: FLUSHFIFO (0x0010) + CLRDATATOG (0x0080). */
    musb_write16(REG_RXMAXP, 64);
    musb_write8(REG_RXTYPE, 2); /* bulk */
    musb_write8(REG_RXFIFOSZ, EP1_FIFO_SZ_CODE);
    musb_write16(REG_RXFIFOADD, EP1_RX_FIFO_ADDR);
    musb_write16(REG_RXCSRL, 0x0010 | 0x0080);

    musb_write8(REG_EPINDEX, 0);   /* MUST restore EP0 for the status write */
}

/* Populate ep0_in_{buf,len,actual} for a control-IN data request. Returns 1
 * if we have data to send, 0 to STALL. Mirrors service_in_request()
 * (U-Boot ep0.c:159-179) plus the GET_DESCRIPTOR the gadget would supply. */
static int
ep0_service_in(uint8_t bmReqType, uint8_t bReq, uint16_t wValue, uint16_t wLength)
{
    const uint8_t *buf = (const uint8_t *)0;
    int len = 0;

    if ((bmReqType & 0x60) == 0x00) {           /* standard */
        switch (bReq) {
        case 0x06: { /* GET_DESCRIPTOR */
            uint8_t type = (uint8_t)(wValue >> 8);
            uint8_t idx = (uint8_t)(wValue & 0xff);
            if (type == 1) { buf = device_desc; len = (int)sizeof(device_desc);
                ep0_cnt_getdesc_dev++;
                ep0_cnt_getdesc_dev_setup++; ep0_last_devdesc_wlen = wLength;
                if (wLength > 8) {   /* FULL read, not the 8-byte probe */
                    ep0_cnt_getdesc_full++; ep0_last_full_wlen = wLength;
                    BC(46, ((uint32_t)ep0_cnt_getdesc_full << 16) | ep0_last_full_wlen);
                }
                bc_tx_detail(); }
            else if (type == 2) { buf = config_desc; len = (int)sizeof(config_desc);
                ep0_cnt_getdesc_cfg++; ep0_last_cfg_wlen = wLength; }
            else if (type == 3) {
                ep0_cnt_getdesc_str++;
                if (idx == 0) { buf = string0; len = (int)sizeof(string0); }
                else if (idx == 1) { buf = string1; len = (int)sizeof(string1); }
                else if (idx == 2) { buf = string2; len = (int)sizeof(string2); }
                else if (idx == 3) { buf = string3; len = (int)sizeof(string3); }
            }
            else { ep0_cnt_getdesc_other++; } /* 6=QUALIFIER,15=BOS,... (STALL) */
            bc_enum_counters();
            break;
        }
        case 0x00: /* GET_STATUS */
            ep0_in_scratch[0] = 0; ep0_in_scratch[1] = 0;
            buf = ep0_in_scratch; len = 2;
            break;
        case 0x08: /* GET_CONFIGURATION */
            ep0_in_scratch[0] = current_config;
            buf = ep0_in_scratch; len = 1;
            break;
        case 0x0a: /* GET_INTERFACE */
            ep0_in_scratch[0] = 0;
            buf = ep0_in_scratch; len = 1;
            break;
        default:
            break;
        }
    } else if ((bmReqType & 0x60) == 0x20) {    /* class */
        if (bReq == 0x21) { buf = line_coding; len = 7; } /* GET_LINE_CODING */
    }

    if (buf == (const uint8_t *)0)
        return 0;
    if (len > (int)wLength)
        len = (int)wLength;
    ep0_in_buf = buf;
    ep0_in_len = len;
    ep0_in_actual = 0;
    return 1;
}

/* Handle a control request with NO data stage (SET_ADDRESS, SET_CONFIGURATION,
 * SET_INTERFACE, feature ops, CDC line-state). Mirrors
 * service_zero_data_request() (U-Boot ep0.c:212-468). Returns 1 = handled
 * (status IN), -1 = stall. SET_ADDRESS only *records* the address; FADDR is
 * written at the STATUSIN stage (U-Boot ep0.c:735-738). */
static int
ep0_service_zero(uint8_t bmReqType, uint8_t bReq, uint16_t wValue)
{
    if ((bmReqType & 0x60) == 0x00) {           /* standard */
        switch (bReq) {
        case 0x05: /* SET_ADDRESS: defer FADDR to status stage */
            ep0_set_address_pend = 1;
            ep0_address = (uint8_t)(wValue & 0x7f);
            ep0_cnt_setaddr++;
            bc_enum_counters();
            return 1;
        case 0x09: /* SET_CONFIGURATION */
            current_config = (uint8_t)(wValue & 0xff);
            ep0_configure_endpoints();          /* restores EPINDEX = 0 */
            usb_state = ST_CONNECTED;
            usb_ready = 1;
            tx_head = tx_tail = 0;
            rx_head = rx_tail = 0;
            ep0_cnt_setconfig++;
            bc_enum_counters();
            return 1;
        case 0x0b: /* SET_INTERFACE */
        case 0x01: /* CLEAR_FEATURE */
        case 0x03: /* SET_FEATURE */
            return 1;
        default:
            return -1;
        }
    } else if ((bmReqType & 0x60) == 0x20) {    /* class: SET_CONTROL_LINE_STATE,
                                                 * SEND_BREAK — accept */
        return 1;
    }
    return -1;
}

/* IN data-stage pump — U-Boot ep0_txstate (ep0.c:527-576) exactly. Loads up to
 * one 64-byte EP0 FIFO packet and arms it with TXPKTRDY, folding DATAEND into
 * the FINAL (short or length-satisfying) packet so the controller runs the OUT
 * status stage ATOMICALLY. This is correct now that the SETUP is acked
 * separately (SVDRXPKTRDY alone + spin in ep0_read_setup, giving DATA1): the
 * earlier "folded DATAEND is premature" symptom (word42=0) was really the
 * folded SVDRXPKTRDY forcing DATA0, which the host dropped.
 *
 * Folding DATAEND back in ALSO fixes the SETUPEND race that froze word44 at
 * 0xA: with a separated DATAEND, a host that knows the exact length (the
 * 8-byte bMaxPacketSize0 probe) completes the transfer before our poll could
 * issue DATAEND, so the core raised SETUPEND and diverted us out of EP0_TX,
 * orphaning the armed packet. Atomic DATAEND removes that window. SVDRXPKTRDY
 * is no longer folded here (ep0_ackpend is 0 — the SETUP was acked already). */
static void
ep0_txstate(void)
{
    uint16_t csr = CSR0_TXPKTRDY;
    int fifo_count = ep0_in_len - ep0_in_actual;

    ep0_txstate_enter++;

    if (fifo_count > 64)
        fifo_count = 64;

    musb_write8(REG_EPINDEX, 0);
    musb_fifo_write(0, ep0_in_buf + ep0_in_actual, fifo_count);
    ep0_in_actual += fifo_count;

    ep0_tx_flags = 0;
    if (fifo_count < 64 || ep0_in_actual == ep0_in_len) {
        ep0_set_state(EP0_STATUSOUT);        /* last packet: status next */
        csr |= CSR0_P_DATAEND;
        ep0_tx_flags |= 0x1;                 /* DATAEND folded on this packet */
        if (fifo_count < 64) ep0_tx_flags |= 0x2;  /* short/terminating       */
    }

    csr |= ep0_ackpend;     /* 0 here (SETUP already acked); kept for clarity */
    ep0_ackpend = 0;

    if (csr & CSR0_TXPKTRDY)
        ep0_txpktrdy_set++;
    ep0_tx_armed_polls = 0;   /* start counting polls-until-TXPKTRDY-clears */
    BC(22, ((uint32_t)ep0_in_actual << 16) | (uint32_t)(ep0_in_len & 0xffff));
    bc_tx_counters();
    musb_write16(REG_CSR0, csr);

    /* Read CSR0 back LIVE to confirm TXPKTRDY actually latched (packet armed)
     * — not a stale value. word41 = (bytes loaded << 16) | this CSR0. */
    ep0_last_fifo_count = (uint16_t)fifo_count;
    ep0_csr0_after_tx = musb_read16(REG_CSR0);
    bc_tx_detail();
    /* word44 state marker: 0xA=just armed. Nonzero-on-arm proves the write
     * path runs even if the EP0_TX poll never re-observes this packet. */
    BC(44, 0xA0000000u | (uint32_t)fifo_count);
}

/* OUT data-stage pump. Reads one packet into ep0_out_buf; DATAEND + STATUSIN
 * ride the final (short or length-satisfying) packet (U-Boot ep0_rxstate,
 * ep0.c:473-519). */
static void
ep0_rxstate(void)
{
    uint16_t csr;
    int count, room;

    musb_write8(REG_EPINDEX, 0);
    count = musb_read8(REG_RXCOUNT);
    if (count > ep0_out_len - ep0_out_actual)
        count = ep0_out_len - ep0_out_actual;

    room = (int)sizeof(ep0_out_buf) - ep0_out_actual;
    if (count > 0 && room > 0) {
        int n = (count < room) ? count : room;
        musb_fifo_read(0, ep0_out_buf + ep0_out_actual, n);
        /* drain any tail beyond our sink so the FIFO empties */
        if (count > n) {
            uint8_t junk[64];
            musb_fifo_read(0, junk, count - n);
        }
    } else if (count > 0) {
        uint8_t junk[64];
        musb_fifo_read(0, junk, count);
    }
    ep0_out_actual += count;

    csr = CSR0_P_SVDRXPKTRDY;
    if (count < 64 || ep0_out_actual >= ep0_out_len) {
        ep0_set_state(EP0_STATUSIN);
        csr |= CSR0_P_DATAEND;
    }
    musb_write16(REG_CSR0, csr);
}

/* Read + dispatch an 8-byte SETUP packet. Mirrors musb_read_setup()
 * (U-Boot ep0.c:584-630) merged with the SETUP switch in musb_g_ep0_irq()
 * (ep0.c:784-880): for IN data we ack the SETUP with SVDRXPKTRDY *alone*,
 * spin until RXPKTRDY clears, THEN load the FIFO — the deviation the old
 * one-shot ep0_tx() skipped, which stalled the descriptor status stage. */
static void
ep0_read_setup(void)
{
    uint8_t setup[8];
    uint8_t bmReqType, bReq;
    uint16_t wValue, wIndex, wLength;
    uint8_t count0;
    int handled;

    ep0_readsetup_enter++;
    bc_tx_counters();

    musb_write8(REG_EPINDEX, 0);
    count0 = musb_read8(REG_RXCOUNT);   /* should be 8 for a real SETUP */
    musb_fifo_read(0, setup, 8);
    BC(6, ++bc_setup_count);

    bmReqType = setup[0];
    bReq = setup[1];
    wValue = (uint16_t)(setup[2] | (setup[3] << 8));
    wIndex = (uint16_t)(setup[4] | (setup[5] << 8));
    wLength = (uint16_t)(setup[6] | (setup[7] << 8));
    bc_log_req(bmReqType, bReq, wValue);
    BC(21, ((uint32_t)ep0_state << 16) | ((uint32_t)bmReqType << 8) | bReq);

    /* Diagnostic: did we just receive a SETUP while addressed (FADDR!=0)?
     * Proves the device is answering at its assigned address, and latches the
     * RAW addressed request so we see EXACTLY what the host asked for at the
     * new address (the last unknown):
     *   word47 (0x500000bc) = (bmRequestType<<24)|(bRequest<<16)|wLength
     *   word48 (0x500000c0) = (COUNT0<<16)|wValue
     * A GET_DESCRIPTOR(device) full read would read word47=0x8006_00xx (xx =
     * wLength, e.g. 0x12 or 0x40), word48=0xNN01_00 (COUNT0=8, wValue=0x0100).
     * Anything else tells us the host is sending a different request, and a
     * COUNT0 != 8 tells us the 8-byte SETUP wasn't actually present (mis-read). */
    ep0_last_setup_faddr = musb_read8(REG_FADDR);
    if (ep0_last_setup_faddr != 0) {
        ep0_cnt_setup_at_addr++;
        BC(47, ((uint32_t)bmReqType << 24) | ((uint32_t)bReq << 16) | wLength);
        BC(48, ((uint32_t)count0 << 16) | wValue);
        bc_enum_counters();
    }
    (void)wIndex;
    (void)count0;

    ep0_set_address_pend = 0;
    ep0_ackpend = CSR0_P_SVDRXPKTRDY;

    if (wLength == 0) {
        /* Sequence #3: no data stage. Status is IN. */
        ep0_set_state(EP0_ACKWAIT);
        handled = ep0_service_zero(bmReqType, bReq, wValue);
        ep0_ackpend |= CSR0_P_DATAEND;
        if (handled > 0) {
            ep0_set_state(EP0_STATUSIN);
        } else {
            ep0_ackpend |= CSR0_P_SENDSTALL;
            ep0_set_state(EP0_IDLE);
            ep0_stall_count++;
        }
        musb_write8(REG_EPINDEX, 0);
        musb_write16(REG_CSR0, ep0_ackpend);   /* arm the IN status stage */

        /* SET_ADDRESS: complete the status stage SYNCHRONOUSLY and write FADDR
         * right here, not in a later poll's STATUSIN case. USB spec: FADDR
         * must change only AFTER the status stage completes; the hardware
         * CLEARS P_DATAEND when the status handshake finishes, so we bound-spin
         * on that, then write FADDR. Deferring the write to a subsequent
         * poll iteration (the task-2 async rewrite) added latency between
         * status-complete and the FADDR update; on this core the host's first
         * transaction at the new address then arrived before FADDR took, and
         * the device answered at address 0 -> "device not accepting address,
         * error -71". This synchronous form is the task-1 proven-good path. */
        if (ep0_set_address_pend) {
            int to = 300000;
            while (--to > 0 && (musb_read16(REG_CSR0) & CSR0_P_DATAEND))
                ; /* bounded: wait for the IN status handshake to finish */
            musb_write8(REG_FADDR, ep0_address);
            ep0_set_address_pend = 0;
            ep0_set_state(EP0_IDLE);   /* status done; ready for next SETUP */
            BC(16, musb_read8(REG_FADDR));                 /* FADDR read-back  */
            BC(17, ((uint32_t)(to <= 0) << 8) | ep0_address); /* timeout|addr  */
        }
        ep0_ackpend = 0;
        return;
    }

    if (bmReqType & 0x80) {
        /* Sequence #1: IN data stage (device -> host). Ack the SETUP with
         * SVDRXPKTRDY *ALONE* and spin until RXPKTRDY clears BEFORE arming the
         * data packet — exactly U-Boot musb_read_setup (ep0.c:621-627). This
         * clean two-step is what makes the controller set the EP0 IN data
         * toggle to DATA1 for the first data packet. Folding SVDRXPKTRDY into
         * the same write as TXPKTRDY (the old path) armed the packet but left
         * it going out as DATA0, which the host silently drops — TXPKTRDY then
         * never clears (word42=0) and the descriptor read times out (-110).
         * (The task-2 claim that separating this "breaks TX" was made before
         * the PHY/CCU/FIFO fixes; with those in place the U-Boot sequence is
         * correct. Bounded spin => cannot hang.) */
        int to;

        ep0_in_branch_enter++;
        ep0_set_state(EP0_TX);
        musb_write8(REG_EPINDEX, 0);
        musb_write16(REG_CSR0, CSR0_P_SVDRXPKTRDY);   /* ack SETUP, alone */
        to = 100000;
        while (--to > 0 && (musb_read16(REG_CSR0) & CSR0_RXPKTRDY))
            ; /* wait for the core to fully consume the SETUP (sets DATA1) */
        ep0_ackpend = 0;   /* SETUP already acked; ep0_txstate writes TXPKTRDY alone */

        handled = ep0_service_in(bmReqType, bReq, wValue, wLength);
        if (handled > 0) {
            ep0_txstate();      /* loads FIFO, then TXPKTRDY alone (DATA1) */
        } else {
            ep0_set_state(EP0_IDLE);
            musb_write16(REG_CSR0, CSR0_P_SENDSTALL);
            ep0_stall_count++;
        }
        return;
    }

    /* Sequence #2: OUT data stage (host -> device). Ack SETUP; the data
     * packet will re-raise RXPKTRDY and ep0_rxstate() collects it. */
    ep0_set_state(EP0_RX);
    ep0_out_len = (int)wLength;
    if (ep0_out_len > (int)sizeof(ep0_out_buf))
        ep0_out_len = (int)sizeof(ep0_out_buf);
    ep0_out_actual = 0;
    musb_write16(REG_CSR0, CSR0_P_SVDRXPKTRDY);
    ep0_ackpend = 0;
}

/* ------------------------------------------------------------------ */
/* EP0 control-endpoint poll — the explicit state machine, driven once  */
/* per musb_poll(). Structure mirrors musb_g_ep0_irq() (ep0.c:651-898). */
/* ------------------------------------------------------------------ */
static void
ep0_poll(void)
{
    uint16_t csr;
    uint8_t  count0;

    PP(0x32);
    musb_write8(REG_EPINDEX, 0);
    PP(0x33);
    csr = musb_read16(REG_CSR0);   /* suspect stall point on live reset traffic */
    count0 = musb_read8(REG_RXCOUNT);  /* COUNT0: 8 == a real SETUP is waiting */
    PP(0x34);
    BC(7, csr);   /* word7 already carries last CSR0 */

    /* Throttled state+CSR0 snapshot so a freeze is visible in word45. */
    if ((bc_poll_count & 0xFFFu) == 2u)
        BC(45, (0x50u << 24) | ((uint32_t)ep0_state << 16) | csr);

    /* DATAEND still set => the status handshake is in flight; wait for the
     * hardware to finish it before advancing (U-Boot ep0.c:668-674). */
    if (csr & CSR0_P_DATAEND)
        return;

    /* A stall we sent has been taken; ack SENTSTALL and go idle. */
    if (csr & CSR0_P_SENTSTALL) {
        musb_write16(REG_CSR0, (uint16_t)(csr & ~CSR0_P_SENTSTALL));
        ep0_set_state(EP0_IDLE);
        csr = musb_read16(REG_CSR0);
    }

    /* PRIORITY: a fresh 8-byte SETUP is waiting (RXPKTRDY + COUNT0==8) and we
     * are NOT in an active data stage. This is the host's next control request
     * — e.g. the FULL GET_DESCRIPTOR that arrives back-to-back with the 8-byte
     * probe's completion, coalesced with a SETUPEND. Decode it NOW, ahead of
     * the generic SETUPEND handler: on this core, servicing SETUPEND first and
     * relying on the re-read RXPKTRDY was dropping this second SETUP (device
     * counter stuck at 1). Clear any riding SETUPEND (SetupEnd bit only; the
     * SETUP data stays in the FIFO), then read it. */
    if ((csr & CSR0_RXPKTRDY) && count0 == 8 &&
        ep0_state != EP0_TX && ep0_state != EP0_RX) {
        if (csr & CSR0_P_SETUPEND) {
            ep0_setupend_count++;
            BC(45, (0xE0u << 24) | ((uint32_t)ep0_state << 16) | csr);
            musb_write16(REG_CSR0, CSR0_P_SVDSETUPEND);
        }
        ep0_set_state(EP0_SETUP);
        ep0_read_setup();
        return;
    }

    /* Host ended the control transfer early WITHOUT a fresh SETUP pending:
     * clear SETUPEND and roll into the appropriate status stage (U-Boot
     * ep0.c:686-703). Latched in word45 (0xE0 marker). */
    if (csr & CSR0_P_SETUPEND) {
        ep0_setupend_count++;
        BC(45, (0xE0u << 24) | ((uint32_t)ep0_state << 16) | csr);
        musb_write16(REG_CSR0, CSR0_P_SVDSETUPEND);
        if (ep0_state == EP0_TX)
            ep0_set_state(EP0_STATUSOUT);
        else if (ep0_state == EP0_RX)
            ep0_set_state(EP0_STATUSIN);
        csr = musb_read16(REG_CSR0);
    }

    switch (ep0_state) {
    case EP0_TX:
        /* Only reached for MULTI-packet transfers: the FINAL packet folds
         * DATAEND and moves straight to EP0_STATUSOUT in ep0_txstate, so this
         * case runs only for the non-final packets (config_desc's first 64).
         * TXPKTRDY clearing => host read the previous packet; arm the next. */
        if ((csr & CSR0_TXPKTRDY) == 0) {
            ep0_txpktrdy_cleared++;
            BC(44, 0xC0000000u | (uint32_t)ep0_tx_armed_polls);
            bc_tx_detail();
            if (ep0_in_actual < ep0_in_len)
                ep0_txstate();          /* arm the next packet */
            /* else: shouldn't happen (last packet already went STATUSOUT) */
        } else {
            ep0_txpktrdy_stuck++;
            if (ep0_tx_armed_polls != 0xffffu)
                ep0_tx_armed_polls++;
            /* word44 = 0xB=still stuck | polls-since-armed. Written on the
             * FIRST stuck poll (so it always latches) then throttled. */
            if (ep0_tx_armed_polls == 1u || (ep0_txpktrdy_stuck & 0x3ffu) == 1u) {
                BC(44, 0xB0000000u | (uint32_t)ep0_tx_armed_polls);
                bc_tx_detail();
            }
        }
        break;

    case EP0_RX:
        if (csr & CSR0_RXPKTRDY)
            ep0_rxstate();
        break;

    case EP0_STATUSIN:
    case EP0_STATUSOUT:
        /* Deferred SET_ADDRESS commits here, at the END of the status phase
         * only (U-Boot ep0.c:735-738): writing FADDR earlier makes the ACK
         * go out at the new address, the host misses it, re-enumerates. */
        if (ep0_state == EP0_STATUSIN && ep0_set_address_pend) {
            ep0_set_address_pend = 0;
            musb_write8(REG_FADDR, ep0_address);
            BC(16, musb_read8(REG_FADDR));   /* address read-back */
            BC(17, ep0_address);
        }
        BC(20, ((uint32_t)(++ep0_status_count) << 8) |
               (uint32_t)((ep0_state == EP0_STATUSIN) ? 1u : 2u));

        /* Coalesced SETUP: a fresh SETUP may already be waiting (U-Boot
         * ep0.c:766-767). Handle it now instead of waiting a poll. */
        if (csr & CSR0_RXPKTRDY) {
            ep0_set_state(EP0_SETUP);
            ep0_read_setup();
        } else {
            ep0_set_state(EP0_IDLE);
        }
        break;

    case EP0_IDLE:
        ep0_set_state(EP0_SETUP);
        /* FALLTHROUGH */
    case EP0_SETUP:
        if (csr & CSR0_RXPKTRDY)
            ep0_read_setup();
        break;

    case EP0_ACKWAIT:
    default:
        break;
    }
}

/* Drain one pending EP1-OUT packet (if any) into the RX ring. Bounded:
 * reads at most one already-arrived packet (<=512 bytes) and never loops
 * waiting for more — musb_poll() will pick up further packets on
 * subsequent calls. */
static void
musb_service_ep1_rx(void)
{
    uint8_t chunk[512];
    uint16_t count;
    int i;

    musb_write8(REG_EPINDEX, 1);
    if ((musb_read16(REG_RXCSRL) & RXCSR_RXPKTRDY) == 0)
        return;

    count = musb_read16(REG_RXCOUNT);
    if (count > sizeof(chunk))
        count = sizeof(chunk);

    musb_fifo_read(1, chunk, count);

    for (i = 0; i < (int)count; i++) {
        unsigned next = (rx_head + 1) & (RX_RING_SIZE - 1);
        if (next == rx_tail)
            break; /* ring full: drop the remainder rather than hang */
        rx_ring[rx_head] = chunk[i];
        rx_head = next;
    }

    /* Ack: tell hardware this packet buffer is free for the next OUT. */
    musb_write16(REG_RXCSRL, (uint16_t)(musb_read16(REG_RXCSRL) & ~RXCSR_RXPKTRDY));
}

/* Push at most ONE max-packet (64 bytes) of staged TX data to EP1 IN, and
 * only if the endpoint's FIFO is free right now. Strictly bounded and
 * NON-BLOCKING: it issues a single TXCSRL read; if the previous packet hasn't
 * been drained by the host yet (TXPKTRDY still set) it returns IMMEDIATELY and
 * the bytes stay in the ring for a later poll. This is the entire TX engine —
 * called once per musb_poll() service pass (below) and once from musb_flush()
 * — so guest console output trickles out incrementally without any producer
 * ever spinning on the host. (The old musb_tx_flush_now() spun up to 200000
 * poll iterations per flush: the CPU1-starvation bug that kept usbacm_poll()
 * gated off — see the tx_buf ring comment.) */
static void
musb_service_ep1_tx(void)
{
    uint8_t chunk[64];
    unsigned count, n, i;

    if (!usb_ready)
        return;                 /* EP1 not configured yet (no SET_CONFIG) */
    if (tx_head == tx_tail)
        return;                 /* ring empty — nothing staged */

    musb_write8(REG_EPINDEX, 1);
    if (musb_read16(REG_TXCSRL) & TXCSR_TXPKTRDY)
        return;                 /* previous packet still in flight — retry on
                                 * a later poll; do NOT wait for the host */

    /* Copy one bounded max-packet from the ring tail into a contiguous scratch
     * buffer (the ring may wrap mid-packet), then load+arm it. */
    count = (tx_head - tx_tail) & (TX_BUF_SIZE - 1);
    n = (count > 64u) ? 64u : count;
    for (i = 0; i < n; i++) {
        chunk[i] = tx_buf[tx_tail];
        tx_tail = (tx_tail + 1) & (TX_BUF_SIZE - 1);
    }
    musb_fifo_write(1, chunk, (int)n);
    musb_write16(REG_TXCSRL,
                 (uint16_t)(musb_read16(REG_TXCSRL) | TXCSR_TXPKTRDY));
}

/* ------------------------------------------------------------------ */
/* Public API (musb.h)                                                  */
/* ------------------------------------------------------------------ */
void
musb_init(void)
{
    int i;
    uint8_t power;

    /* Breadcrumb: prove we got INTO musb_init, and zero the live counters
     * so a warm rerun starts from a clean record. */
    BC(1, BC_STAGE_MUSB_INIT);
    bc_poll_count = bc_intr_seen = bc_setup_count = 0;
    BC(2, 0); BC(3, 0); BC(4, 0); BC(5, 0); BC(6, 0); BC(7, 0);

    /* Fine-grained init substages in word[7] (0xE1..0xE7). word[7] is only
     * reused for last_csr0 once the poll loop runs, so if we die inside
     * init the substage survives for the post-WDT `md.l` readout. word34
     * (MI) carries a parallel, dedicated progress marker that is NEVER
     * repurposed, so a hang is localized precisely. */
    BC(7, 0xE1);
    MI(0x20);

    /* Full clock/reset + USB-PHY0 bring-up (clocks, reset deassert, PHY power
     * up via SIDDQ clear, calibration, route to MUSB, FIFO SRAM). This is the
     * piece the old ensure_musb_clock()-only path was missing. */
    musb_phy_init();
    MI(0x21);

    /* Select PIO mode (Allwinner vendor register). */
    musb_write8(REG_AWIN_VEND0, 0);
    BC(7, 0xE2);

    /* Soft-reset the controller. */
    musb_write8(REG_POWER, POWER_RESET);
    for (i = 0; i < 1000; i++)
        __asm__ volatile("nop");
    power = musb_read8(REG_POWER);
    power = (uint8_t)(power & ~POWER_RESET);
    musb_write8(REG_POWER, power);
    for (i = 0; i < 10000; i++)
        __asm__ volatile("nop");
    BC(7, 0xE3);
    MI(0x22);

    /* Device mode. Force a CLEAN re-enumeration so the host queries OUR
     * descriptors (1d6b:0010 "bzdOS/USB Console") instead of keeping U-Boot's
     * already-enumerated gadget (1f3a:efe8 "USB download gadget"). U-Boot left
     * the device connected; a too-brief SOFTCONN drop is below the host's
     * disconnect-debounce and goes unnoticed, so the host never re-queries us,
     * we never see SET_CONFIGURATION, usb_ready stays 0, and every TX byte gets
     * dropped. Hold SOFTCONN low for ~tens of ms for a real disconnect, then
     * reconnect. */
    {
        uint32_t iscr = musb_read32(REG_ISCR) & ~ISCR_CHANGE_DETECT;

        /* Real disconnect: release the PHY-level D+ pull-up (SOFTCONN alone
         * is masked by it — see REG_ISCR comment). Host sees the device
         * leave the bus.
         *
         * FULL-SPEED ONLY: POWER.HSENAB is deliberately NOT set. Advertising
         * high speed made the core attempt the HS chirp handshake during each
         * USB reset; on this sunxi MUSB that handshake never completed, so the
         * host reset the bus over and over (breadcrumb signature: 24x
         * GET_DESC(device), 2x SET_ADDRESS, 9x bus-reset, 0 config requests)
         * and enumeration never reached GET_DESCRIPTOR(config). At full speed
         * (12 Mbps) there is no chirp — the handshake is trivial. The console
         * needs nowhere near HS bandwidth, so FS is the right trade. */
        musb_write8(REG_POWER, 0);
        musb_write32(REG_ISCR, iscr & ~ISCR_DPDM_PULLUP_EN);
        BC(7, 0xE4);
        MI(0x23);
        for (i = 0; i < 20000000; i++)
            __asm__ volatile("nop");
        BC(7, 0xE5);
        MI(0x24);

        /* B-device needs DEVCTL.Session=1 to (re)join the bus (writing 0
         * here is what used to zombify the port, error -71). Bring the core
         * fully up FIRST (session + SOFTCONN), then re-assert the pull-ups AND
         * force ID=high / VBUS=valid so the host's re-enumeration meets a
         * ready, session-active device (usbphy_iscr_setup mirrors U-Boot's
         * EnableDpDmPullUp + EnableIdPullUp + ForceIdToHigh +
         * ForceVbusValidToHigh). */
        musb_write8(REG_DEVCTL, (uint8_t)(musb_read8(REG_DEVCTL) | 0x01));
        musb_write8(REG_POWER, POWER_SOFTCONN);   /* SOFTCONN only, no HSENAB */
        usbphy_iscr_setup();
        BC(7, 0xE6);
        MI(0x25);
    }

    /* Enable RESET/CONNECT/DISCONNECT interrupts (polled, not IRQ-driven). */
    musb_write8(REG_INTUSBE, INTR_RESET | INTR_CONNECT | INTR_DISCONNECT);
    MI(0x26);

    /* EP0 max packet. */
    musb_write8(REG_EPINDEX, 0);
    musb_write16(REG_TXMAXP, 64);
    musb_write16(REG_RXMAXP, 64);

    usb_state = ST_RESET;
    usb_ready = 0;
    tx_head = tx_tail = 0;
    rx_head = rx_tail = 0;

    /* EP0 state machine starts idle. */
    ep0_state = EP0_IDLE;
    ep0_set_address_pend = 0;
    ep0_ackpend = 0;
    current_config = 0;
    ep0_state_hist = ep0_trans_count = ep0_status_count = ep0_stall_count = 0;
    ep0_txstate_enter = ep0_txpktrdy_set = 0;
    ep0_readsetup_enter = ep0_in_branch_enter = 0;
    ep0_cnt_getdesc_dev = ep0_cnt_getdesc_cfg = 0;
    ep0_cnt_getdesc_str = ep0_cnt_getdesc_other = 0;
    ep0_cnt_setconfig = ep0_cnt_setaddr = ep0_cnt_busreset = 0;
    ep0_last_cfg_wlen = 0;
    ep0_cnt_setup_at_addr = ep0_last_setup_faddr = 0;
    ep0_cnt_intr_reset = ep0_cnt_intr_suspend = 0;
    ep0_cnt_getdesc_dev_setup = ep0_last_devdesc_wlen = 0;
    ep0_last_fifo_count = ep0_csr0_after_tx = 0;
    ep0_txpktrdy_cleared = ep0_txpktrdy_stuck = 0;
    ep0_tx_flags = 0;
    ep0_tx_armed_polls = 0;
    ep0_setupend_count = 0;
    ep0_cnt_getdesc_full = ep0_last_full_wlen = 0;
    BC(18, 0); BC(19, 0); BC(20, 0); BC(21, 0); BC(22, 0); BC(23, 0);
    BC(24, 0); BC(25, 0); BC(26, 0); BC(27, 0);
    BC(32, 0); BC(33, 0);
    BC(35, 0); BC(36, 0);
    BC(40, 0); BC(41, 0); BC(42, 0); BC(43, 0); BC(44, 0); BC(45, 0); BC(46, 0);
    BC(47, 0); BC(48, 0);

    BC(7, 0xE7);
    MI(0x2F);   /* musb_init fully complete — if word34 != 0x2F, it wedged */
}

int
musb_poll(void)
{
    uint8_t intr;

    /* NB: deliberately NO wdt_pet() here. Petting belongs to main()'s loop
     * ONLY — if it lived here, main's enumeration-giveup spin (which keeps
     * calling musb_poll to service EP0) would pet forever and the guaranteed
     * WDT reset could never fire. The bounded spins that call musb_poll()
     * (tx flush, ep0_tx) finish in well under the 16 s window. */

    /* Defensive re-assert; cheap, and guards against anything else on the
     * SoC clearing the OTG gate behind our back. */
    PP(0x01);
    ensure_musb_clock();

    /* Throttled: BC writes now carry a dc civac + dsb each (must reach real
     * DRAM), which is far too heavy for every single poll. Every 4096th
     * call keeps the record fresh enough for the post-mortem readout. */
    if (((++bc_poll_count) & 0xFFFu) == 1u) {
        BC(2, bc_poll_count);
        BC(3, (uint32_t)usb_state);
        BC(4, (uint32_t)usb_ready);
    }

    PP(0x02);
    intr = musb_read8(REG_INTUSB);
    BC(36, intr);                 /* latch the INTRUSB bits seen this poll */
    if (intr != 0) {
        PP(0x03);
        bc_intr_seen |= intr;
        BC(5, bc_intr_seen);
        musb_write8(REG_INTUSB, intr); /* write-1-to-clear */
        PP(0x04);

        /* Count reset vs suspend separately — a SUSPEND (0x01) must never be
         * mistaken for a RESET (0x04) and zero FADDR mid-enumeration. */
        if (intr & INTR_SUSPEND) { ep0_cnt_intr_suspend++; bc_enum_counters(); }
        if (intr & INTR_RESET)   { ep0_cnt_intr_reset++;   bc_enum_counters(); }

        if (intr & INTR_RESET) {
            PP(0x20);
            musb_write8(REG_FADDR, 0);
            PP(0x21);
            musb_write8(REG_EPINDEX, 0);
            PP(0x22);
            /* NOTE: do NOT flush the EP0 FIFO here. A CSR0=FLUSHFIFO write
             * right after a real bus reset AHB-stalled this controller (the
             * poll-path hang). U-Boot never flushes EP0 on reset — the reset
             * itself clears the EP0 FIFO/state — so we don't either. */
            musb_write16(REG_TXMAXP, 64);
            PP(0x23);
            musb_write16(REG_RXMAXP, 64);
            musb_write8(REG_TXTYPE, 0);
            musb_write8(REG_RXTYPE, 0);
            PP(0x24);
            usb_state = ST_WAIT_SETUP;
            usb_ready = 0;
            tx_head = tx_tail = 0;
            rx_head = rx_tail = 0;
            /* Bus reset restarts enumeration from scratch — re-arm the EP0
             * state machine so a stale TX/STATUS stage can't swallow the
             * fresh SET_ADDRESS/GET_DESCRIPTOR sequence. */
            ep0_set_state(EP0_IDLE);
            ep0_set_address_pend = 0;
            ep0_ackpend = 0;
            current_config = 0;
            ep0_cnt_busreset++;
            bc_enum_counters();
            PP(0x25);
        }

        if (intr & INTR_DISCONNECT) {
            usb_state = ST_RESET;
            usb_ready = 0;
        }

        if ((intr & INTR_CONNECT) && usb_state == ST_RESET)
            usb_state = ST_WAIT_SETUP;
        PP(0x05);
    }

    /* Drive the EP0 control state machine unconditionally on every call —
     * enumeration progress never depends on whether musb_putc()/flush()
     * happen to be running. ep0_poll() advances SETUP -> DATA -> STATUS ->
     * IDLE explicitly, so multi-packet control-IN and the status stages
     * complete in a way the host accepts. */
    if (usb_state >= ST_WAIT_SETUP) {
        PP(0x30);
        ep0_poll();
        PP(0x31);
    }

    /* Service EP1 once the host has configured the device: drain one waiting
     * OUT packet into the RX ring, then push one staged TX packet to IN if the
     * FIFO is free. BOTH are bounded and non-blocking — this incremental TX
     * push is what lets the CPU1 debug core run usbacm_poll() without ever
     * starving dbgmon (smp.c dbg_usbacm), replacing the old per-flush spin. */
    if (usb_state == ST_CONNECTED) {
        musb_service_ep1_rx();
        musb_service_ep1_tx();
    }

    PP(0x3F);
    return usb_ready;
}

int
musb_ready(void)
{
    return usb_ready;
}

void
musb_putc(int c)
{
    unsigned next = (tx_head + 1) & (TX_BUF_SIZE - 1);

    /* Append ONLY — never block, never touch the hardware, never poll. If the
     * ring is full (host not draining the gadget) drop the newest byte: this
     * is a debug console, losing the tail of a flood is acceptable and,
     * crucially, keeps CPU1 free for dbgmon. The staged bytes are pushed to
     * the host incrementally by musb_poll()'s musb_service_ep1_tx(), so a
     * caller must keep calling musb_poll() for output to actually go out
     * (usbacm_poll() and the main*.c loops already do). */
    if (next == tx_tail)
        return;
    tx_buf[tx_head] = (uint8_t)c;
    tx_head = next;
}

void
musb_flush(void)
{
    /* Best-effort immediate push of one packet if the EP1-IN FIFO is free;
     * bounded and NON-BLOCKING (see musb_service_ep1_tx). Any remainder is
     * pushed by subsequent musb_poll() calls. Deliberately does NOT spin until
     * the host drains — that was the old CPU1-starving musb_tx_flush_now(). */
    musb_service_ep1_tx();
}

void
musb_puts(const char *s)
{
    while (*s)
        musb_putc((unsigned char)*s++);
}

int
musb_getc(void)
{
    int c;

    if (rx_head == rx_tail)
        return -1;

    c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) & (RX_RING_SIZE - 1);
    return c;
}
