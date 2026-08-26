/* SPDX-License-Identifier: BSD-2-Clause */

/* emac.c — sun8i-emac (Allwinner A64) driver + raw-Ethernet console for the
 * bzdOS microkernel. Implements emac.h. Freestanding, bare-metal AArch64,
 * MMIO via volatile pointers built from ABSOLUTE physical addresses (U-Boot
 * leaves the MMU on with a flat device mapping — same contract as musb.c).
 *
 * EVERYTHING here (register offsets, bit definitions, syscon value, CCU
 * gate/reset bits, the bring-up SEQUENCE) is ported VERBATIM from the
 * known-good U-Boot driver for THIS silicon:
 *     /opt/bzdos/build/u-boot/drivers/net/sun8i_emac.c
 * and the board DTS
 *     .../arch/arm/dts/sun50i-a64.dtsi  (+ ...-bananapi-m64.dts)
 * NOT guessed from generic datasheets (that trap cost the USB lane weeks).
 * File:line citations are in the comments next to each constant.
 *
 * DMA coherency: the D-cache is ON, so DMA descriptor rings and packet
 * buffers (in a fixed scratch DRAM region) are kept coherent by hand — clean
 * (dc civac) before handing memory to the EMAC, invalidate (dc ivac) before
 * reading what the EMAC wrote, each bracketed by a dsb. Generalized from
 * musb.c's bc_write "dc civac + dsb sy" pattern.
 *
 * ============================================================================
 * TX CROSS-CORE MUTUAL EXCLUSION (added for ROADMAP C1 / vnet_emac.c).
 * ============================================================================
 * As of vnet_emac.c (virtio-net multiplexed onto this EMAC), tx_frame_raw()
 * gets a SECOND caller on a SECOND core:
 *   - CPU1 (the dedicated debug core, see smp.c's "CPU1 = dedicated EMAC/
 *     dbgmon DEBUG CORE" block): the console/repl/status-line TX path
 *     (emac_putc/emac_puts/emac_flush -> tx_frame() -> tx_frame_raw()),
 *     driven from dbgmon_service() inside CPU1's free-running poll loop.
 *   - CPU0 (the guest's core): vnet_emac.c's TX-kick path
 *     (vnet_kick() -> emac_send_frame() -> tx_frame_raw()), driven
 *     synchronously inside the guest's QueueNotify trap.
 * Both share g_tx_slot, the TX descriptor ring (tx_desc()), and the
 * EMAC_TX_CTL1 DMA-kick register with NO prior locking (this driver was
 * written and hardware-verified for single-core (CPU1) use only). Two cores
 * racing tx_frame_raw() concurrently can hand out the SAME g_tx_slot to both
 * (one frame silently clobbers the other's descriptor mid-fill) or kick TX
 * DMA while a descriptor is only half-written.
 *
 * Note RX (emac_poll()'s ring, g_rx_slot, rx_push()) does NOT need the same
 * treatment: emac_poll() is only ever invoked from ONE core at a time by
 * construction — dbg_core_active gates it (CPU1 exclusively when the debug
 * core is enabled; CPU0 inline inside its own trap, serially, when it is
 * not) — see el2_exc.c's `if (!dbg_core_active) dbgmon_service(frame)` call
 * sites and smp.c's secondary-core loop. There is never a second core also
 * calling emac_poll() concurrently, so g_rx_slot/rx_ring are single-owner at
 * all times and vnet_emac_rx_frame()'s queue-0 (RX) virtio ring likewise
 * stays single-owner (only ever reached via emac_poll()).
 *
 * Fix: a single test-and-set spinlock in a fixed DRAM word (EMAC_TX_LOCK_PA,
 * below), acquired around the WHOLE of tx_frame_raw() — mirrors
 * vblk_emmc.c's vblk_emmc_trylock()/vblk_emmc_unlock() precedent exactly
 * (ldaxr/stlxr exclusive test-and-set; no LSE on the A53). Bounded acquire
 * (never an unbounded spin): CPU0 must never hang the guest trap past the
 * HW watchdog waiting for CPU1's console output to finish, and CPU1 must
 * never stall its RX/dbgmon service loop waiting for a guest-driven TX
 * burst. A failed acquire is treated exactly like the pre-existing "TX
 * descriptor still busy" case a few lines below: the frame is dropped,
 * counted, and the caller's existing 0-return contract (retry later) is
 * unchanged — no new failure mode is introduced, just a new REASON for the
 * same, already-handled outcome.
 */
#include <stdint.h>
#include "emac.h"
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */
#if defined(DBG_AUTH)
#include "hmac_sha256.h"   /* ROADMAP T5 keyed-auth gate, see dbg_auth_check()
                            * below and docs/security-notes.md. Only pulled in
                            * when -DDBG_AUTH is set (EXTRA_CFLAGS); the
                            * default build never sees this header. */
#endif
#include "wdt.h"   /* pet the 16 s WDT during the multi-second autoneg wait —
                    * emac_init() runs BEFORE main_net.c's pet loop, so a
                    * naked seconds-long wait here would trip the watchdog. */
#include "timer.h" /* timer_now()/timer_freq() — emac_link_watchdog()'s own
                    * rate limit; same timebase the CPU1 loop already uses. */

/* ------------------------------------------------------------------ */
/* Physical bases (DTS-verified)                                       */
/* ------------------------------------------------------------------ */
/* EMAC MMIO: sun50i-a64.dtsi ethernet@1c30000 "reg = <0x01c30000 0x10000>"
 * (dtsi line ~1121-1124). */
#define EMAC_BASE      SOC_A64_EMAC_BASE
/* SYS_CON EMAC clock register: syscon@1c00000 (dtsi ~393) + a64 variant
 * syscon_offset 0x30 (sun8i_emac.c emac_variant_a64, line ~893-896). */
#define SYSCON_EMAC    SOC_A64_SYSCON_EMAC
/* CCU (clock/reset). CLK_BUS_EMAC = GATE(0x060, BIT(17)); RST_BUS_EMAC =
 * RESET(0x2c0, BIT(17)) — u-boot drivers/clk/sunxi/clk_a64.c lines 23 & 79. */
#define CCU_BASE       SOC_A64_CCU_BASE
#define CCU_BUS_GATE0  0x060u
#define CCU_BUS_RST0   0x2C0u
#define CCU_EMAC_BIT   (1u << 17)
/* PIO (GPIO/pinmux) controller — the PD bank carries the RGMII pins. */
#define PIO_BASE       SOC_A64_PIO_BASE

/* ------------------------------------------------------------------ */
/* EMAC register offsets (sun8i_emac.c lines 81-125, verbatim)          */
/* ------------------------------------------------------------------ */
#define EMAC_CTL0            0x00
#define  EMAC_CTL0_FULL_DUPLEX   (1u << 0)
#define  EMAC_CTL0_SPEED_MASK    (3u << 2)
#define  EMAC_CTL0_SPEED_10      (0x2u << 2)
#define  EMAC_CTL0_SPEED_100     (0x3u << 2)
#define  EMAC_CTL0_SPEED_1000    (0x0u << 2)
#define EMAC_CTL1            0x04
#define  EMAC_CTL1_SOFT_RST      (1u << 0)
#define  EMAC_CTL1_BURST_LEN_SHIFT 24
#define EMAC_INT_STA         0x08
#define EMAC_INT_EN          0x0c
#define EMAC_TX_CTL0         0x10
#define  EMAC_TX_CTL0_TX_EN      (1u << 31)
#define EMAC_TX_CTL1         0x14
#define  EMAC_TX_CTL1_TX_MD        (1u << 1)
#define  EMAC_TX_CTL1_TX_DMA_EN    (1u << 30)
#define  EMAC_TX_CTL1_TX_DMA_START (1u << 31)
#define EMAC_TX_FLOW_CTL     0x1c
#define EMAC_TX_DMA_DESC     0x20
#define EMAC_RX_CTL0         0x24
#define  EMAC_RX_CTL0_RX_EN      (1u << 31)
#define EMAC_RX_CTL1         0x28
#define  EMAC_RX_CTL1_RX_MD        (1u << 1)
#define  EMAC_RX_CTL1_RX_RUNT_FRM  (1u << 2)
#define  EMAC_RX_CTL1_RX_ERR_FRM   (1u << 3)
#define  EMAC_RX_CTL1_RX_DMA_EN    (1u << 30)
#define  EMAC_RX_CTL1_RX_DMA_START (1u << 31)
#define EMAC_RX_DMA_DESC     0x34
/* Receive Frame Filter (offset+bit confirmed against Linux mainline
 * drivers/net/ethernet/stmicro/stmmac/dwmac-sun8i.c, NOT in U-Boot's
 * minimal sun8i_emac.c driver, which never touches this register at all
 * -- explaining why the hardware's default (address-filtered, RXALL=0)
 * behavior was never noticed before ROADMAP C1: every prior use of this
 * EMAC (debug protocol, netcon, snapshot-net) only ever needed frames
 * addressed to OUR_MAC, which the default filter already passes). RXALL
 * (bit0) disables destination-address filtering entirely -- needed so a
 * unicast reply addressed to the GUEST's own (not OUR_MAC) virtio MAC
 * reaches the RX descriptor ring at all; see emac_init()'s own comment at
 * the RX_FRM_FLT write site for the live-hardware story (2026-07-23/24). */
#define EMAC_RX_FRM_FLT      0x38
#define  EMAC_FRM_FLT_RXALL      (1u << 0)
#define EMAC_MII_CMD         0x48
#define EMAC_MII_DATA        0x4c
#define EMAC_ADDR0_HIGH      0x50
#define EMAC_ADDR0_LOW       0x54

/* Descriptor status/ctl bits (sun8i_emac.c lines 120-125). */
#define EMAC_DESC_OWN_DMA      (1u << 31)
#define EMAC_DESC_LAST_DESC    (1u << 30)
#define EMAC_DESC_FIRST_DESC   (1u << 29)
#define EMAC_DESC_CHAIN_SECOND (1u << 24)
#define EMAC_DESC_RX_ERROR_MASK 0x400068dbu

/* MDIO command bits (sun8i_emac.c lines 33-44). */
#define MDIO_CMD_MII_BUSY           (1u << 0)
#define MDIO_CMD_MII_WRITE          (1u << 1)
#define MDIO_CMD_MII_PHY_REG_SHIFT  4
#define MDIO_CMD_MII_PHY_ADDR_SHIFT 12
#define MDIO_CMD_MII_CLK_CSR_DIV_128 0x3u
#define MDIO_CMD_MII_CLK_CSR_SHIFT  20

/* SYS_CON EMAC-clock register fields (sun8i_emac.c lines 60-75). */
#define H3_EPHY_SHUTDOWN   (1u << 16)  /* 1 = internal PHY off (we use ext) */
#define SC_RMII_EN         (1u << 13)
#define SC_EPIT            (1u << 2)   /* 1 = RGMII, 0 = MII */
#define SC_ETCS_INT_GMII   0x2u        /* clock source = internal GMII */

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */
#define PHY_ADDR        1              /* .config CONFIG_PHY_ADDR=1; DTS
                                        * ext_rgmii_phy reg = <1> (RTL8211E) */
#define N_TX_DESC       8
/* RX ring depth. WAS 8, which is 8 frames = ~12 KiB of buffering: at the
 * ~170 KB/s this link actually achieves inbound, that is about 70 ms of slack
 * before the DMA has nowhere to put a frame. Anything that keeps CPU1 out of
 * emac_poll() for longer than that loses packets at the HARDWARE level -- and
 * those losses are invisible to vnet_emac.c's rx_dropped counter, which only
 * counts frames the HV received and then had to discard. That blind spot is why
 * a paced sender could show rx_dropped pinned at 201 while a 68 MB inbound
 * transfer still collapsed.
 *
 * Linux's sun8i-emac uses 256. 64 is 8x the buffering for 132 KiB of the 1 MiB
 * non-cacheable scratch window, which was 96% unused. The ring is a linked list
 * (each descriptor carries `next`), so depth is purely a memory question. */
#define N_RX_DESC       64
#define ETH_BUFSIZE     2048           /* per-descriptor buffer */
#define ETH_RXSIZE      2044           /* sun8i_emac.c CFG_ETH_RXSIZE */

/* Our locally-administered MAC and console EtherType. */
static const uint8_t OUR_MAC[6] = { 0x02, 0xbd, 0x05, 0x00, 0x00, 0x01 };
#define ETHERTYPE_CONSOLE 0x88B5u
/* Reliable-datagram (netcon) EtherType — a SECOND, independent channel from
 * the console. Frames on this EtherType are demuxed away from the console
 * byte ring in emac_poll() and handed to netcon_rx_frame() instead (see
 * "RX demux hook" below). Kept as a plain broadcast dst byte to reuse the
 * exact same TX descriptor path as the console (see tx_frame_raw()). */
#define ETHERTYPE_NETCON  0x88B6u
/* Bulk snapshot-stream channel (snapshot_net.h, ROADMAP D1) — a THIRD,
 * independent channel from the console and netcon. Fire-and-forget bulk
 * DATA frames only (no ACK on this channel; reliability is a small
 * netcon-carried manifest/missing-list handshake — see snapshot_net.h).
 * Demuxed away from both the console ring and netcon_rx_frame() exactly
 * like netcon's own EtherType is demuxed away from the console below. */
#define ETHERTYPE_SNAPNET 0x88B8u
/* Raw dbgtools peek/reply (dbgtools.h, added 2026-07-26) — a FOURTH,
 * independent channel, demuxed away from the console/netcon/snapnet exactly
 * like the others, straight to dbgtools_rx_frame() (see that file's header
 * comment for why: it must be servicable from inside emac_poll() itself,
 * independent of whether dbgmon_service() or gdbstub_poll() is CPU1's
 * current branch). 0x88B7 sits between netcon (0x88B6) and snapnet (0x88B8)
 * — confirmed unused by a full-tree grep before picking it. */
#define ETHERTYPE_DBGRAW  0x88B7u
static const uint8_t BCAST_MAC[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* netcon.c provides the real implementation when linked in; the weak no-op
 * fallback here lets emac.c (and anything built without netcon.c, e.g. the
 * plain console-only images) link cleanly. */
extern void netcon_rx_frame(const uint8_t *payload, uint16_t len);
__attribute__((weak)) void netcon_rx_frame(const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;   /* no netcon linked in: drop silently */
}

/* snapshot_net.c provides the real implementation when linked in; same
 * weak-fallback pattern as netcon_rx_frame above so any build without
 * snapshot_net.c links cleanly. Same "payload after the 14-byte Ethernet
 * header" convention as netcon_rx_frame. */
extern void snapshot_net_rx_frame(const uint8_t *payload, uint16_t len);
__attribute__((weak)) void snapshot_net_rx_frame(const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;   /* no snapshot_net linked in: drop silently */
}

/* vnet_emac.c (ROADMAP C1, virtio-net-over-EMAC) provides the real
 * implementation when linked in; same weak-fallback pattern as
 * netcon_rx_frame above so any build without vnet_emac.c (every image today)
 * links cleanly. Unlike netcon_rx_frame's "payload after ethertype"
 * convention, this hook receives the FULL Ethernet frame (dst MAC first
 * byte) — see vnet_emac.h's vnet_emac_rx_frame() doc comment. Called for
 * any accepted frame whose ethertype is neither the console's nor
 * netcon's — i.e. every ethertype this file's own RX dispatch does not
 * already claim. */
extern void vnet_emac_rx_frame(const uint8_t *frame, uint16_t len);
__attribute__((weak)) void vnet_emac_rx_frame(const uint8_t *frame, uint16_t len)
{
    (void)frame; (void)len;   /* no vnet_emac linked in: drop silently */
}

/* dbgtools.c provides the real implementation (dbg/gdb builds only); same
 * weak-fallback pattern as netcon_rx_frame above so any build without
 * dbgtools.o (repl/fbsd/zephyr/hdmi/net/stage0) links cleanly. Same
 * "payload after the 14-byte Ethernet header" convention as netcon_rx_frame. */
extern void dbgtools_rx_frame(const uint8_t *payload, uint16_t len);
__attribute__((weak)) void dbgtools_rx_frame(const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;   /* no dbgtools linked in: drop silently */
}

/* ------------------------------------------------------------------ */
/* Scratch DRAM layout for DMA (rings + buffers). 0x50100000 onward is    */
/* clear of the image (0x42000000..0x42800000), of musb.c's breadcrumb    */
/* (0x50000000) and of our own breadcrumb window (0x50000100). All under  */
/* 4 GiB, as the EMAC DMA engine is 32-bit. Each descriptor gets its own   */
/* 64-byte cache line so per-descriptor clean/invalidate never disturbs a  */
/* neighbour.                                                              */
/* ------------------------------------------------------------------ */
#define SCRATCH_BASE    0x50100000UL
/* The non-cacheable window el2_ncmap.c maps for DMA is 0x50100000..0x50200000
 * (its own comment: "idx256..511"). Named here so the asserts below can prove
 * the layout fits instead of assuming it. */
#define SCRATCH_SIZE    0x00100000UL
#define DESC_STRIDE     64u                          /* one cache line each */

/* Re-laid out when N_RX_DESC went 8 -> 64: the RX ring alone is now 4 KiB, which
 * would have run straight into the old TX_BUF_BASE at +0x1000. Each region gets
 * room to grow again, and the asserts below make a future overlap a build
 * failure rather than a DMA engine quietly writing over a neighbour. */
#define TX_DESC_BASE    (SCRATCH_BASE + 0x00000UL)   /* 8 * 64   =   512 B */
#define RX_DESC_BASE    (SCRATCH_BASE + 0x01000UL)   /* 64 * 64  =  4 KiB  */
#define TX_BUF_BASE     (SCRATCH_BASE + 0x02000UL)   /* 8 * 2048 = 16 KiB  */
#define RX_BUF_BASE     (SCRATCH_BASE + 0x08000UL)   /* 64 * 2048 = 128 KiB */
#define SCRATCH_END     (RX_BUF_BASE + (unsigned long)N_RX_DESC * ETH_BUFSIZE)

_Static_assert(TX_DESC_BASE + (unsigned long)N_TX_DESC * DESC_STRIDE <= RX_DESC_BASE,
               "EMAC TX descriptor ring overlaps the RX descriptor ring");
_Static_assert(RX_DESC_BASE + (unsigned long)N_RX_DESC * DESC_STRIDE <= TX_BUF_BASE,
               "EMAC RX descriptor ring overlaps the TX buffers");
_Static_assert(TX_BUF_BASE + (unsigned long)N_TX_DESC * ETH_BUFSIZE <= RX_BUF_BASE,
               "EMAC TX buffers overlap the RX buffers");
_Static_assert(SCRATCH_END <= SCRATCH_BASE + SCRATCH_SIZE,
               "EMAC DMA scratch layout overflows the non-cacheable window");

/* A single hardware DMA descriptor. Only the first 16 bytes are meaningful
 * to the EMAC; we access them at their absolute physical addresses. */
struct emac_desc {
    uint32_t status;    /* +0  OWN(31); RX: length[29:16] */
    uint32_t ctl_size;  /* +4  buffer size / TX flags+len  */
    uint32_t buf_addr;  /* +8  physical buffer address (32-bit) */
    uint32_t next;      /* +12 physical addr of next descriptor */
};

static inline volatile struct emac_desc *tx_desc(int i)
{ return (volatile struct emac_desc *)(TX_DESC_BASE + (uint32_t)i * DESC_STRIDE); }
static inline volatile struct emac_desc *rx_desc(int i)
{ return (volatile struct emac_desc *)(RX_DESC_BASE + (uint32_t)i * DESC_STRIDE); }
static inline uint32_t tx_buf(int i) { return (uint32_t)(TX_BUF_BASE + (uint32_t)i * ETH_BUFSIZE); }
static inline uint32_t rx_buf(int i) { return (uint32_t)(RX_BUF_BASE + (uint32_t)i * ETH_BUFSIZE); }

/* ------------------------------------------------------------------ */
/* Low-level MMIO                                                       */
/* ------------------------------------------------------------------ */
static inline uint32_t rd(uint32_t off)
{ return *(volatile uint32_t *)(EMAC_BASE + off); }
static inline void wr(uint32_t off, uint32_t v)
{ *(volatile uint32_t *)(EMAC_BASE + off) = v; }
static inline void setbits(uint32_t off, uint32_t m) { wr(off, rd(off) | m); }

/* ------------------------------------------------------------------ */
/* Cache maintenance (D-cache is ON — generalized from musb.c bc_write)  */
/* ------------------------------------------------------------------ */
static inline void cache_clean(uintptr_t addr, uint32_t size)
{
    uintptr_t p = addr & ~63UL, end = addr + size;
    for (; p < end; p += 64)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline void cache_inval(uintptr_t addr, uint32_t size)
{
    uintptr_t p = addr & ~63UL, end = addr + size;
    for (; p < end; p += 64)
        __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void udelay_spin(uint32_t n)
{
    /* No timer; crude bounded busy-spin (~n loop iterations). */
    for (volatile uint32_t i = 0; i < n; i++)
        __asm__ volatile("nop");
}

/* ------------------------------------------------------------------ */
/* Breadcrumb telemetry — SECOND window at 0x50000100 (musb.c owns       */
/* 0x50000000). Same cache-coherent store pattern as musb.c: a plain      */
/* store dies in cache across a WDT reset. Read after a run with          */
/*     md.l 0x50000100 16                                                 */
/* Layout (word index -> 0x50000100 + i*4):                               */
/*   [0] 0xE3AC0DE1  magic (proves emac.c wrote this)                      */
/*   [1] stage       max checkpoint reached (BC_STAGE_* below)            */
/*   [2] clk_ungated 1 once CCU gate set + reset deasserted               */
/*   [3] phy_reset   1 once PHY soft-reset completed                       */
/*   [4] link_up     1 once PHY reported link                              */
/*   [5] link_speed  10 / 100 / 1000 (0 if unknown)                        */
/*   [6] tx_count    frames handed to TX DMA                               */
/*   [7] rx_count    frames accepted from RX DMA                           */
/*   [8] last_int    last EMAC_INT_STA value observed in emac_poll()       */
/*   [9] phy_id      (PHYID1<<16)|PHYID2 read over MDIO (diagnostic)        */
/*  [10] bmcr_rb     PHY BMCR (reg 0) read back after enabling autoneg      */
/*  [11] bmsr_rb     PHY BMSR (reg 1) — last value seen in the link wait    */
/*  [12] physr_rb    RTL8211E PHYSR (reg 0x11) — resolved speed/duplex/link
 *  [13] link_down_events  count of up->down transitions seen by emac_poll()
 *  [14] link_up_events    count of down->up transitions (initial link plus
 *                         every flap recovery) seen by emac_poll()
 *  [15] tx_drops     frames dropped by tx_frame() (link down, or a TX
 *                     descriptor stayed busy past the bounded wait)
 *  [16] force_link   1 if built with EMAC_FORCE_LINK (fixed speed/duplex,
 *                     autoneg disabled), 0 if running autoneg
 *  [17] mdio_valid   1 if the PHY answered a sane ID over MDIO before we
 *                     committed to resetting/programming it (0 = retry gave
 *                     up and we pressed on against a maybe-phantom PHY)
 *  [18] wd_reinits   emac_link_watchdog() bounded self-heal attempt count —
 *                     nonzero means EMAC never saw an RX frame and CPU1 is
 *                     retrying the PHY/rings bring-up
 *  [20] anar_rb      Auto-Negotiation Advertisement read back after we
 *                     restrict it (expect EMAC_AN_ADVERT: full-duplex only,
 *                     no gigabit). 0 in a forced-link build.
 *  [21] gbcr_rb      1000BASE-T Control read back (expect 0 = gigabit NOT
 *                     advertised). 0 in a forced-link build.
 *  [22] an_fallback  1 if auto-negotiation failed to bring the link up within
 *                     the bounded wait and we fell back to forced
 *                     EMAC_FORCE_SPEED/full so the debug channel survives.
 *                     Nonzero here means the far end would not negotiate the
 *                     restricted advertisement -- read [20] and the switch.
 *  [19] first_rx     latches to 1 the FIRST time a frame is ever accepted
 *  [32] tx_lock_contended  sticky 1 if the cross-core TX lock (added for
 *                     vnet_emac.c, EMAC_TX_LOCK_PA) was ever seen already
 *                     held by tx_frame_raw()'s bounded acquire — expected
 *                     occasionally under real CPU0(vnet)/CPU1(console) TX
 *                     contention, NOT expected to ever hit the bounded
 *                     acquire's give-up case (see the "TX CROSS-CORE MUTUAL
 *                     EXCLUSION" block comment at the top of this file)
 * [5] link_speed is kept LIVE (rewritten on every debounced link check in
 * emac_poll()), not just set once at init.
 * Read after a run with  md.l 0x50000100 24 ; md.l 0x50000180 1               */
/* ------------------------------------------------------------------ */
#define BC_BASE  0x50000100UL
#define BC_MAGIC 0xE3AC0DE1u
enum {
    BC_STAGE_ENTER   = 1,   /* entered emac_init() */
    BC_STAGE_CLK     = 2,   /* CCU gate on + reset deasserted */
    BC_STAGE_SYSCON  = 3,   /* SYS_CON EMAC clk reg programmed */
    BC_STAGE_PINMUX  = 4,   /* PD RGMII pins muxed */
    BC_STAGE_RESET   = 5,   /* EMAC soft reset complete */
    BC_STAGE_MDIO    = 6,   /* PHY ID read over MDIO */
    BC_STAGE_PHYRST  = 7,   /* PHY soft-reset complete */
    BC_STAGE_LINK    = 8,   /* link up, speed/duplex resolved */
    BC_STAGE_RINGS   = 9,   /* DMA rings initialised */
    BC_STAGE_ENABLED = 10,  /* RX/TX DMA + MAC enabled (init done) */
    BC_STAGE_LOOP    = 11,  /* emac_poll() serviced at least once */
    BC_STAGE_WD_GAVEUP = 98, /* emac_link_watchdog() exhausted its retries */
    BC_STAGE_NOLINK  = 99,  /* gave up: no link within bounded wait */
};

static inline void bc(int i, uint32_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)(BC_BASE + (uint32_t)i * 4u);
    *p = v;
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

/* ------------------------------------------------------------------ */
/* TX cross-core lock — see the "TX CROSS-CORE MUTUAL EXCLUSION" block at   */
/* the top of this file. Fixed DRAM word (cross-core coherent under         */
/* SMPEN), zeroed in emac_init() so a stale "1" can never survive a warm     */
/* WDT reset into a deadlock — same discipline as VBLK_EMMC_LOCK_PA.        */
/* Lives inside EMAC's own breadcrumb window (0x50000100..0x500001ff), well */
/* clear of the highest breadcrumb index actually used (19 -> 0x1a4c) and   */
/* of every other lane's documented window.                                 */
/* ------------------------------------------------------------------ */
#define EMAC_TX_LOCK_PA        0x50000180UL
#define EMAC_TX_LOCK_CONTEND_BC 32   /* bc() index: contended-acquire count  */

static inline volatile uint32_t *emac_tx_lock_word(void)
{
    return (volatile uint32_t *)EMAC_TX_LOCK_PA;
}

/* Non-blocking test-and-set. Returns 1 if acquired (caller MUST call
 * emac_tx_unlock()), 0 if already held. ARMv8.0 A53 has no LSE atomics, so
 * this is the same ldaxr/stlxr exclusive loop as vblk_emmc_trylock() and
 * smp.c's g_online set. */
static int emac_tx_trylock(void)
{
    volatile uint32_t *p = emac_tx_lock_word();
    uint32_t prev, status, one = 1u;
    __asm__ volatile(
        "	ldaxr	%w0, [%3]\n"
        "	cbnz	%w0, 1f\n"          /* already held -> fail */
        "	stlxr	%w1, %w2, [%3]\n"   /* try to store 1 */
        "	b	2f\n"
        "1:	mov	%w1, #1\n"          /* prev!=0: report failure */
        "2:\n"
        : "=&r"(prev), "=&r"(status)
        : "r"(one), "r"(p)
        : "memory");
    if (prev == 0u && status == 0u) {
        __asm__ volatile("dsb sy" ::: "memory");
        return 1;
    }
    return 0;
}

static void emac_tx_unlock(void)
{
    volatile uint32_t *p = emac_tx_lock_word();
    __asm__ volatile("dsb sy" ::: "memory");
    *p = 0u;
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* Bounded acquire: a few hundred spins is generous (a single tx_frame_raw()
 * call, even including its own bounded descriptor-busy wait, is a bounded,
 * short critical section — nothing in it blocks on link state or an
 * external event). Never risk hanging CPU0's guest trap or CPU1's RX/dbgmon
 * loop waiting on the other side's console/vnet TX burst; a failed acquire
 * just means "drop this frame, exactly like a busy TX descriptor" (see the
 * call site below). */
#define EMAC_TX_LOCK_SPINS  20000u
static int emac_tx_lock_acquire_bounded(void)
{
    uint32_t contended = 0;
    for (uint32_t i = 0; i < EMAC_TX_LOCK_SPINS; i++) {
        if (emac_tx_trylock())
            return 1;
        contended = 1;
        __asm__ volatile("yield" ::: "memory");
    }
    if (contended)
        bc(EMAC_TX_LOCK_CONTEND_BC, 1u);   /* sticky "we saw contention" flag */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Driver state                                                         */
/* ------------------------------------------------------------------ */
static int      g_link_up;       /* LIVE, debounced — refreshed in emac_poll() */
static uint32_t g_speed;         /* 10/100/1000 */
static int      g_tx_slot;       /* next TX descriptor to use */
static int      g_rx_slot;       /* next RX descriptor to inspect */
static uint32_t g_tx_count, g_rx_count;
static uint32_t g_poll_calls;         /* throttles MDIO link-status polling */
static int      g_link_down_streak;   /* consecutive "not up" reads while up,
                                       * OR consecutive polls while down
                                       * (reused for the autoneg re-kick) */
static uint32_t g_link_up_events;     /* down->up transition count */
static uint32_t g_link_down_events;   /* up->down transition count */
static uint32_t g_tx_drops;           /* frames dropped by tx_frame() */
static uint32_t g_first_rx_latched;   /* one-shot: has ANY frame ever arrived */

/* emac_poll() throttling/debounce constants for live link monitoring. */
#define LINK_CHECK_INTERVAL     256   /* polls between MDIO BMSR reads (power
                                       * of two — used as a mask) */
#define LINK_DOWN_DEBOUNCE      3     /* consecutive down-reads (each
                                       * LINK_CHECK_INTERVAL polls apart)
                                       * before declaring the link down —
                                       * absorbs a brief autoneg re-handshake
                                       * blip instead of reading it as loss */
#define LINK_RENEG_KICK_STREAK  40    /* consecutive down-reads while already
                                       * down before nudging autoneg restart
                                       * (a register write only — never a
                                       * full MAC/DMA reinit); autoneg-only */

/* TX line staging (flushed as one frame on '\n' or when full). */
#define TX_LINE_MAX 512
static uint8_t  tx_line[TX_LINE_MAX];
static int      tx_line_len;

/* RX byte ring feeding emac_getc(). */
#define RX_RING_SIZE 1024            /* power of two */
static uint8_t  rx_ring[RX_RING_SIZE];
static unsigned rx_head, rx_tail;

static void rx_push(uint8_t b)
{
    unsigned next = (rx_head + 1) & (RX_RING_SIZE - 1);
    if (next == rx_tail)
        return;                      /* full: drop rather than hang */
    rx_ring[rx_head] = b;
    rx_head = next;
}

/* Count an accepted RX frame + latch the one-shot "link proven alive" marker
 * (word [19]) the first time any frame ever arrives. */
static inline void note_rx_frame(void)
{
    bc(7, ++g_rx_count);
    if (!g_first_rx_latched) {
        g_first_rx_latched = 1;
        bc(19, 1);
    }
}

/* ------------------------------------------------------------------ */
/* MDIO (sun8i_emac.c sun8i_mdio_read/write, external-PHY path)          */
/* ------------------------------------------------------------------ */
static int mdio_read(int phy, int reg)
{
    uint32_t cmd = ((uint32_t)reg << MDIO_CMD_MII_PHY_REG_SHIFT) & 0x000001f0u;
    cmd |= ((uint32_t)phy << MDIO_CMD_MII_PHY_ADDR_SHIFT) & 0x0001f000u;
    cmd |= MDIO_CMD_MII_CLK_CSR_DIV_128 << MDIO_CMD_MII_CLK_CSR_SHIFT;
    cmd |= MDIO_CMD_MII_BUSY;

    wr(EMAC_MII_CMD, cmd);
    for (int t = 0; t < 100000; t++) {
        if (!(rd(EMAC_MII_CMD) & MDIO_CMD_MII_BUSY))
            return (int)(rd(EMAC_MII_DATA) & 0xffffu);
    }
    return -1;                       /* bounded timeout */
}

static void mdio_write(int phy, int reg, uint16_t val)
{
    uint32_t cmd = ((uint32_t)reg << MDIO_CMD_MII_PHY_REG_SHIFT) & 0x000001f0u;
    cmd |= ((uint32_t)phy << MDIO_CMD_MII_PHY_ADDR_SHIFT) & 0x0001f000u;
    cmd |= MDIO_CMD_MII_CLK_CSR_DIV_128 << MDIO_CMD_MII_CLK_CSR_SHIFT;
    cmd |= MDIO_CMD_MII_WRITE | MDIO_CMD_MII_BUSY;

    wr(EMAC_MII_DATA, val);
    wr(EMAC_MII_CMD, cmd);
    for (int t = 0; t < 100000; t++)
        if (!(rd(EMAC_MII_CMD) & MDIO_CMD_MII_BUSY))
            return;
    /* bounded timeout — best effort */
}

/* Standard IEEE 802.3 clause-22 PHY registers. */
#define MII_BMCR   0x00
#define  BMCR_RESET      0x8000
#define  BMCR_ANENABLE   0x1000
#define  BMCR_ANRESTART  0x0200
#define MII_BMSR   0x01
#define  BMSR_LSTATUS    0x0004
#define  BMSR_ANEGDONE   0x0020
#define MII_PHYID1 0x02
#define MII_PHYID2 0x03
/* RTL8211E PHY-Specific Status Register (vendor reg 0x11): resolved link. */
#define RTL_PHYSR  0x11
#define  PHYSR_SPEED_SHIFT 14
#define  PHYSR_SPEED_MASK  0x3
#define  PHYSR_DUPLEX      (1u << 13)
#define  PHYSR_LINK        (1u << 11)
/* Clause-22 BMCR speed/duplex fields, used only for the forced-link path
 * below (autoneg path never touches these directly). */
#define  BMCR_DUPLEX_FULL  0x0100
#define  BMCR_SPEED_LSB    0x2000   /* bit13 */
#define  BMCR_SPEED_MSB    0x0040   /* bit6  */

/* Auto-negotiation advertisement, clause 22 registers 4 and 9. Needed because
 * the reset default advertises 10/100/1000 and we specifically do NOT want
 * gigabit on this board (see the RGMII delay-strap discussion below). */
#define MII_ANAR   0x04             /* Auto-Negotiation Advertisement */
#define  ANAR_CSMA        0x0001    /* selector: IEEE 802.3 */
#define  ANAR_10_HALF     0x0020
#define  ANAR_10_FULL     0x0040
#define  ANAR_100_HALF    0x0080
#define  ANAR_100_FULL    0x0100
#define  ANAR_PAUSE       0x0400    /* symmetric PAUSE capable */
#define MII_GBCR   0x09             /* 1000BASE-T Control */
#define  GBCR_1000_HALF   0x0100
#define  GBCR_1000_FULL   0x0200

/* What we advertise when autoneg is on.
 *
 * FULL DUPLEX ONLY, and no gigabit. Both halves matter:
 *
 *  - No gigabit, because this board's RGMII clock-to-data timing has no delay
 *    configured on the SoC side and depends entirely on the PHY's strap pins
 *    (the discussion below). Gigabit's skew budget is what made the link flap.
 *    Negotiating 100 gets the tolerant rate WITHOUT giving up negotiation.
 *
 *  - Full duplex only, because advertising half as a fallback is how a silent
 *    duplex mismatch happens, and a mismatch is far worse than no link: it
 *    passes traffic while losing a large fraction of it, with no error counter
 *    anywhere. A link that refuses to come up is loud and diagnosable.
 *
 *  - 10FULL is kept as a safety net purely so that SOMETHING links if the far
 *    end cannot do 100: 10 Mbit is miserable but it keeps the EMAC debug
 *    channel alive, and losing that channel is the expensive failure here. */
#define EMAC_AN_ADVERT   (ANAR_CSMA | ANAR_100_FULL | ANAR_10_FULL | ANAR_PAUSE)

/* ------------------------------------------------------------------ *
 * Forced link mode vs. autoneg — see the "flap" root-cause discussion
 * in the report. Root cause under investigation: the SYS_CON EMAC clock
 * register here has BOTH TX/RX delay fields at 0 (CONFIG_GMAC_TX_DELAY=0,
 * board DTS has no allwinner,tx/rx-delay-ps), meaning correct RGMII
 * clock-to-data timing depends ENTIRELY on the RTL8211E's internal
 * "rgmii-id" delay being enabled by its RXD1/RXD0 strap pins at PHY
 * power-on/reset. If those straps are marginal (weak/wrong pull, PD-bank
 * drive strength too low for the trace load), gigabit's tight skew budget
 * is the first thing to suffer — signal integrity errors show up to
 * software as spurious link renegotiation (the "flap": link comes up,
 * a few frames get through before a burst of bit errors triggers the link
 * partner or the PHY to drop and retrain). 100 Mbit's slower edge rate has
 * roughly an order of magnitude more setup/hold margin, so it tends to
 * "just work" even with marginal delay strapping.
 *
 * Trade-off: forcing speed/duplex does NOT fix a genuine strap/skew fault
 * — it sidesteps it by using a slower, more tolerant rate, and it gives up
 * autoneg's ability to adapt to whatever the far end actually supports
 * (a forced PHY on an autoneg-only partner may fail to link at all, or
 * link in the wrong duplex under parallel detection — this board's far
 * end is a fixed switch port we control, so that risk is low here).
 * Given this REPL is now the primary debug channel and the reported
 * failure mode is "link flaps then drops to 0" rather than "never links",
 * we default to FORCED 100/full as the more conservative choice until the
 * hardware lane can confirm the RTL8211E's delay straps directly. Flip
 * EMAC_FORCE_LINK to 0 to go back to full autoneg (with the new debounce
 * logic below, occasional autoneg blips no longer read as permanent loss
 * either way).
 *
 * UPDATE 2026-08-18 -- THE ACCEPTED RISK CAME TRUE, so the default is now 0.
 *
 * The paragraph above accepts exactly one risk: "a forced PHY on an autoneg-only
 * partner may fail to link at all, or link in the wrong duplex under parallel
 * detection -- this board's far end is a fixed switch port we control, so that
 * risk is low here." That assumption about the far end appears to be wrong.
 *
 * Measured: inbound bulk transfers retransmit ~17% and collapse (cwnd 2, rto
 * grown to 83 s) while the board reports ZERO loss at every layer -- RX ring
 * idle with all 64 descriptors DMA-owned, no RX_BUF_UA/RX_OVERFLOW/RX_DMA_STOP,
 * rx_dropped +4 over a whole transfer, tx_drops 0, guest window wide open,
 * vtnet0 0 errors -- at 1.4% utilisation of a 100 Mbit link, where nothing can
 * overflow. Outbound is byte-exact at ~900 KB/s.
 *
 * Heavy loss that neither endpoint counts, on a quiet link, in one direction
 * more than the other, is the textbook signature of a duplex mismatch: with
 * autoneg disabled on this side, the partner parallel-detects the SPEED but
 * cannot negotiate duplex and falls back to HALF, while we drive FULL.
 * Collisions then eat a large fraction of the traffic and are counted by the
 * switch, which is the one place we cannot read.
 *
 * The fix is not "go back to autoneg" -- that would negotiate gigabit and bring
 * the flap back. It is autoneg with the advertisement restricted to full-duplex
 * and no gigabit (EMAC_AN_ADVERT above): duplex becomes AGREED rather than
 * guessed, and gigabit's skew budget is never engaged. Set EMAC_FORCE_LINK back
 * to 1 to return to the old forced behaviour. */
#define EMAC_FORCE_LINK         0
#define EMAC_FORCE_SPEED        100   /* 10, 100, or 1000 — only used if
                                       * EMAC_FORCE_LINK is 1 */
#define EMAC_FORCE_FULL_DUPLEX  1

/* Bring the PHY up: read ID, soft-reset, enable+restart autoneg, then wait
 * (bounded) for link. Resolves speed/duplex from the RTL8211E PHYSR. Returns
 * 1 if link came up, 0 on bounded timeout. Sets g_speed / duplex-in-*dup. */
/* How many BMSR poll passes to wait for link. Each pass performs ~2 MDIO
 * transactions (real, MDIO-clock-bounded time, ~30-60 us each) plus a short
 * spin, so a pass is ~O(100 us) of WALL time largely independent of CPU
 * speed. ~120000 passes therefore covers well over the 2-5 s a gigabit
 * switch needs to auto-negotiate, while still finishing in a handful of
 * seconds. We wdt_pet() every pass so this seconds-long wait cannot trip the
 * 16 s watchdog (emac_init runs before main_net.c's pet loop); the loop is
 * still hard-bounded, so on genuine no-link it exits and the caller lets the
 * WDT reset us. */
#define LINK_WAIT_PASSES 120000

static int phy_startup(int *duplex_full)
{
    int id1, id2, bmsr = 0, bmcr_rb;

    id1 = mdio_read(PHY_ADDR, MII_PHYID1);
    id2 = mdio_read(PHY_ADDR, MII_PHYID2);
    /* Bounded retry: on some cold boots the external PHY hasn't finished its
     * own power-on-reset by the time we first poke MDIO, and a premature
     * transaction reads back garbage (all-1s / all-0s / timed-out -1).
     * Soft-resetting and programming BMCR against a PHY that never really
     * answered leaves the link untrained for the rest of the boot. Retry for
     * a bounded ~1 s before pressing on anyway (a genuinely absent PHY can't
     * be fixed from software; a merely-slow one now gets a real chance). */
    {
        int tries, valid = 0;
        for (tries = 0; tries < 500; tries++) {
            valid = (id1 >= 0 && id2 >= 0) &&
                    !((id1 & 0xffff) == 0xffff && (id2 & 0xffff) == 0xffff) &&
                    !(id1 == 0 && id2 == 0);
            if (valid)
                break;
            wdt_pet();
            udelay_spin(2000);
            id1 = mdio_read(PHY_ADDR, MII_PHYID1);
            id2 = mdio_read(PHY_ADDR, MII_PHYID2);
        }
        bc(17, (uint32_t)valid);
    }
    bc(9, ((uint32_t)(id1 & 0xffff) << 16) | (uint32_t)(id2 & 0xffff));
    bc(1, BC_STAGE_MDIO);

    /* Soft-reset the PHY and wait (bounded) for BMCR.RESET to self-clear.
     * After the reset the PHY needs a short settle before it responds
     * reliably to further MDIO writes. */
    mdio_write(PHY_ADDR, MII_BMCR, BMCR_RESET);
    for (int t = 0; t < 100000; t++) {
        int v = mdio_read(PHY_ADDR, MII_BMCR);
        wdt_pet();
        if (v >= 0 && !(v & BMCR_RESET))
            break;
        udelay_spin(2000);
    }
    udelay_spin(200000);   /* post-reset settle */
    bc(3, 1);
    bc(1, BC_STAGE_PHYRST);

#if EMAC_FORCE_LINK
    /* Forced speed/duplex: autoneg (BMCR_ANENABLE) is left OFF entirely. Set
     * the clause-22 speed-select bits + duplex directly. See the trade-off
     * discussion above EMAC_FORCE_LINK for why this is the current default. */
    {
        uint16_t bmcr = 0;
        if (EMAC_FORCE_FULL_DUPLEX) bmcr |= BMCR_DUPLEX_FULL;
        if (EMAC_FORCE_SPEED == 1000) bmcr |= BMCR_SPEED_MSB;
        else if (EMAC_FORCE_SPEED == 100) bmcr |= BMCR_SPEED_LSB;
        /* 10 Mbps: both speed bits stay clear. */
        mdio_write(PHY_ADDR, MII_BMCR, bmcr);
    }
#else
    /* Restrict what we advertise BEFORE restarting negotiation. The BMCR_RESET
     * above returns the advertisement registers to their defaults, which
     * advertise 10/100/1000 half and full -- both of which we specifically do
     * not want here (see EMAC_AN_ADVERT). Writing them after the reset and
     * before ANRESTART is the whole point; a plain restart, which is what this
     * used to do, negotiates gigabit. */
    mdio_write(PHY_ADDR, MII_ANAR, EMAC_AN_ADVERT);
    mdio_write(PHY_ADDR, MII_GBCR, 0);        /* advertise NO 1000BASE-T */
    bc(20, (uint32_t)(mdio_read(PHY_ADDR, MII_ANAR) & 0xffff));
    bc(21, (uint32_t)(mdio_read(PHY_ADDR, MII_GBCR) & 0xffff));

    /* Now enable + restart auto-negotiation (BMCR bit12 ANENABLE + bit9
     * ANRESTART). */
    mdio_write(PHY_ADDR, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
#endif
    bmcr_rb = mdio_read(PHY_ADDR, MII_BMCR);
    bc(10, (uint32_t)(bmcr_rb & 0xffff));

    /* Bounded wait for link. BMSR.LSTATUS (bit2) is latched-low, so read
     * twice per pass and use the second read. We key on LINK (not ANEGDONE)
     * as the primary condition — some switches raise link slightly before
     * the PHY latches aneg-complete; speed/duplex are then read from the
     * RTL8211E PHYSR. */
    g_link_up = 0;
    for (uint32_t t = 0; t < LINK_WAIT_PASSES; t++) {
        wdt_pet();                                   /* keep the WDT at bay */
        (void)mdio_read(PHY_ADDR, MII_BMSR);         /* clear the latch */
        bmsr = mdio_read(PHY_ADDR, MII_BMSR);
        if ((t & 0x3ffu) == 0)                       /* throttle the BC write */
            bc(11, (uint32_t)(bmsr & 0xffff));
        if (bmsr >= 0 && (bmsr & BMSR_LSTATUS)) {
            g_link_up = 1;
            break;
        }
        udelay_spin(2000);
    }
    bc(11, (uint32_t)(bmsr & 0xffff));               /* final BMSR */

#if !EMAC_FORCE_LINK
    /* SAFETY NET. Autoneg with a restricted advertisement is the right thing
     * (see EMAC_AN_ADVERT), but if the far end will not negotiate any of what we
     * offer, the result is no link at all -- and this link IS the debug channel.
     * Losing it costs a full serial-console reload cycle to recover from.
     *
     * So: if negotiation produced nothing, fall back to the old forced
     * speed/duplex rather than returning failure. That reinstates the duplex
     * mismatch this change exists to avoid, which is exactly why [22] records
     * that it happened -- a degraded-but-reachable board that says so is better
     * than an unreachable one, and better than a silently degraded one. */
    if (!g_link_up) {
        uint16_t bmcr_f = BMCR_DUPLEX_FULL;
        if (EMAC_FORCE_SPEED == 1000)     bmcr_f |= BMCR_SPEED_MSB;
        else if (EMAC_FORCE_SPEED == 100) bmcr_f |= BMCR_SPEED_LSB;
        bc(22, 1);
        mdio_write(PHY_ADDR, MII_BMCR, bmcr_f);
        udelay_spin(200000);
        for (uint32_t t = 0; t < LINK_WAIT_PASSES; t++) {
            wdt_pet();
            (void)mdio_read(PHY_ADDR, MII_BMSR);
            bmsr = mdio_read(PHY_ADDR, MII_BMSR);
            if (bmsr >= 0 && (bmsr & BMSR_LSTATUS)) {
                g_link_up = 1;
                break;
            }
            udelay_spin(2000);
        }
        bc(11, (uint32_t)(bmsr & 0xffff));
        if (g_link_up) {
            g_speed = EMAC_FORCE_SPEED;
            if (duplex_full) *duplex_full = 1;
            bc(5, g_speed);
            return 1;
        }
    }
#endif

    if (!g_link_up)
        return 0;

#if EMAC_FORCE_LINK
    /* Speed/duplex are whatever we forced — no PHYSR resolution needed, but
     * still read it for the breadcrumb (diagnostic only). */
    g_speed = EMAC_FORCE_SPEED;
    *duplex_full = EMAC_FORCE_FULL_DUPLEX;
    bc(12, (uint32_t)(mdio_read(PHY_ADDR, RTL_PHYSR) & 0xffff));
#else
    /* Give aneg a brief bounded moment to finish resolving after link, then
     * read speed/duplex from the RTL8211E PHYSR (vendor reg 0x11). */
    for (int t = 0; t < 20000; t++) {
        wdt_pet();
        if (mdio_read(PHY_ADDR, MII_BMSR) & BMSR_ANEGDONE)
            break;
        udelay_spin(2000);
    }
    {
        int physr = mdio_read(PHY_ADDR, RTL_PHYSR);
        uint32_t sp = ((uint32_t)physr >> PHYSR_SPEED_SHIFT) & PHYSR_SPEED_MASK;
        bc(12, (uint32_t)(physr & 0xffff));
        /* PHYSR speed field: 2 = 1000, 1 = 100, 0 = 10. Handle all. */
        g_speed = (sp == 2) ? 1000u : (sp == 1) ? 100u : 10u;
        *duplex_full = (physr & PHYSR_DUPLEX) ? 1 : 0;
    }
#endif
    return 1;
}

/* ------------------------------------------------------------------ */
/* MAC address + link programming                                       */
/* ------------------------------------------------------------------ */
static void write_hwaddr(void)
{
    uint32_t lo = OUR_MAC[0] | (OUR_MAC[1] << 8) | (OUR_MAC[2] << 16) | ((uint32_t)OUR_MAC[3] << 24);
    uint32_t hi = OUR_MAC[4] | (OUR_MAC[5] << 8);
    wr(EMAC_ADDR0_HIGH, hi);
    wr(EMAC_ADDR0_LOW, lo);
}

static void adjust_link(int duplex_full)
{
    uint32_t v = rd(EMAC_CTL0);
    if (duplex_full) v |= EMAC_CTL0_FULL_DUPLEX;
    else             v &= ~EMAC_CTL0_FULL_DUPLEX;
    v &= ~EMAC_CTL0_SPEED_MASK;
    switch (g_speed) {
    case 1000: v |= EMAC_CTL0_SPEED_1000; break;
    case 100:  v |= EMAC_CTL0_SPEED_100;  break;
    default:   v |= EMAC_CTL0_SPEED_10;   break;
    }
    wr(EMAC_CTL0, v);
}

/* ------------------------------------------------------------------ */
/* Live link monitoring — called (throttled) from emac_poll(). Ride out a
 * flap WITHOUT touching MAC/DMA state: only EMAC_CTL0 (speed/duplex) is ever
 * reprogrammed here, and only on a confirmed down->up recovery. A momentary
 * BMSR blip (e.g. a fast autoneg re-handshake) is debounced and must NOT
 * read as a permanent drop — see LINK_DOWN_DEBOUNCE. */
/* ------------------------------------------------------------------ */
static void link_recheck(void)
{
    int bmsr, up;

    (void)mdio_read(PHY_ADDR, MII_BMSR);      /* BMSR.LSTATUS is latched-low:
                                               * throw away the stale read */
    bmsr = mdio_read(PHY_ADDR, MII_BMSR);
    bc(11, (uint32_t)(bmsr & 0xffff));
    up = (bmsr >= 0) && (bmsr & BMSR_LSTATUS);

    if (up) {
        g_link_down_streak = 0;
        if (!g_link_up) {
            /* Recovering from a drop. Re-resolve speed/duplex and reprogram
             * EMAC_CTL0 ONLY — no soft reset, no ring reinit, no RX/TX
             * DMA disable. In-flight rings and counters are untouched. */
            int duplex_full;
#if EMAC_FORCE_LINK
            g_speed = EMAC_FORCE_SPEED;
            duplex_full = EMAC_FORCE_FULL_DUPLEX;
            bc(12, (uint32_t)(mdio_read(PHY_ADDR, RTL_PHYSR) & 0xffff));
#else
            {
                int physr = mdio_read(PHY_ADDR, RTL_PHYSR);
                uint32_t sp = ((uint32_t)physr >> PHYSR_SPEED_SHIFT) & PHYSR_SPEED_MASK;
                bc(12, (uint32_t)(physr & 0xffff));
                g_speed = (sp == 2) ? 1000u : (sp == 1) ? 100u : 10u;
                duplex_full = (physr & PHYSR_DUPLEX) ? 1 : 0;
            }
#endif
            adjust_link(duplex_full);
            g_link_up = 1;
            bc(4, 1);
            bc(14, ++g_link_up_events);
        }
        bc(5, g_speed);                       /* keep the live-speed word fresh */
    } else if (g_link_up) {
        /* Was up, this sample says down: debounce before believing it. */
        if (++g_link_down_streak >= LINK_DOWN_DEBOUNCE) {
            g_link_up = 0;
            bc(4, 0);
            bc(13, ++g_link_down_events);
            g_link_down_streak = 0;
        }
    } else {
#if !EMAC_FORCE_LINK
        /* Already down (autoneg mode only): if it's been down a long while,
         * nudge autoneg to restart. Cheap register write, not a reinit —
         * just gives the PHY another shot at training. */
        if (++g_link_down_streak >= LINK_RENEG_KICK_STREAK) {
            mdio_write(PHY_ADDR, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
            g_link_down_streak = 0;
        }
#endif
    }
}

/* ------------------------------------------------------------------ */
/* DMA ring init (sun8i_emac.c rx_descs_init/tx_descs_init)              */
/* ------------------------------------------------------------------ */
static void rings_init(void)
{
    int i;

    for (i = 0; i < N_RX_DESC; i++) {
        volatile struct emac_desc *d = rx_desc(i);
        d->buf_addr = rx_buf(i);
        d->next     = (uint32_t)(RX_DESC_BASE + (uint32_t)((i + 1) % N_RX_DESC) * DESC_STRIDE);
        d->ctl_size = ETH_RXSIZE;
        d->status   = EMAC_DESC_OWN_DMA;   /* owned by DMA, ready to receive */
        cache_clean((uintptr_t)d, sizeof(*d));
        cache_inval((uintptr_t)d->buf_addr, ETH_BUFSIZE);
    }

    for (i = 0; i < N_TX_DESC; i++) {
        volatile struct emac_desc *d = tx_desc(i);
        d->buf_addr = tx_buf(i);
        d->next     = (uint32_t)(TX_DESC_BASE + (uint32_t)((i + 1) % N_TX_DESC) * DESC_STRIDE);
        d->ctl_size = 0;
        d->status   = 0;                   /* owned by CPU */
        cache_clean((uintptr_t)d, sizeof(*d));
    }

    wr(EMAC_RX_DMA_DESC, (uint32_t)RX_DESC_BASE);
    wr(EMAC_TX_DMA_DESC, (uint32_t)TX_DESC_BASE);
    g_rx_slot = 0;
    g_tx_slot = 0;
}

/* ------------------------------------------------------------------ */
/* Bring-up (sequence ported from sun8i_emac.c probe + eth_start)        */
/* ------------------------------------------------------------------ */
int emac_init(void)
{
    int duplex_full = 1;
    int linked;
    volatile uint32_t *reg;

    /* Reset live state / lay down the breadcrumb magic. */
    bc(0, BC_MAGIC);
    bc(1, BC_STAGE_ENTER);
    bc(2, 0); bc(3, 0); bc(4, 0); bc(5, 0); bc(6, 0); bc(7, 0); bc(8, 0); bc(9, 0);
    bc(10, 0); bc(11, 0); bc(12, 0);
    bc(13, 0); bc(14, 0); bc(15, 0); bc(16, EMAC_FORCE_LINK);
    bc(17, 0); bc(18, 0); bc(19, 0);
    g_link_up = 0; g_speed = 0;
    g_tx_count = g_rx_count = 0;
    g_poll_calls = 0; g_link_down_streak = 0;
    g_link_up_events = 0; g_link_down_events = 0; g_tx_drops = 0;
    g_first_rx_latched = 0;
    tx_line_len = 0; rx_head = rx_tail = 0;

    /* Zero the TX cross-core lock so a stale "1" left over from a prior warm
     * WDT reset can never deadlock a fresh boot — same discipline as
     * VBLK_EMMC_LOCK_PA in vblk_init(). Plain store (not the bc() cache-
     * maintenance helper) is fine here: the lock's own trylock/unlock
     * already bracket every access with dsb, and this runs before either
     * core could possibly contend for it. */
    *(volatile uint32_t *)EMAC_TX_LOCK_PA = 0u;
    bc(EMAC_TX_LOCK_CONTEND_BC, 0u);

    /* 1. CCU: ungate EMAC bus clock + deassert EMAC bus reset. */
    reg = (volatile uint32_t *)(CCU_BASE + CCU_BUS_GATE0);
    *reg |= CCU_EMAC_BIT;
    reg = (volatile uint32_t *)(CCU_BASE + CCU_BUS_RST0);
    *reg |= CCU_EMAC_BIT;
    __asm__ volatile("dsb sy" ::: "memory");
    bc(2, 1);
    bc(1, BC_STAGE_CLK);

    /* 2. SYS_CON EMAC clock register: external PHY, RGMII, internal-GMII clk.
     * Value derived exactly as sun8i_emac_set_syscon() does for the A64 +
     * rgmii-id, tx/rx delay 0 (bananapi-m64.dts sets no allwinner,*-delay-ps;
     * .config CONFIG_GMAC_TX_DELAY=0):
     *   H3_EPHY_SHUTDOWN | SC_EPIT | SC_ETCS_INT_GMII = 0x10000|0x4|0x2 = 0x10006 */
    *(volatile uint32_t *)SYSCON_EMAC =
        H3_EPHY_SHUTDOWN | SC_EPIT | SC_ETCS_INT_GMII;
    __asm__ volatile("dsb sy" ::: "memory");
    bc(1, BC_STAGE_SYSCON);

    /* 3. Pinmux: PD RGMII pins -> function 4 ("emac"). Pin set from
     * sun50i-a64.dtsi rgmii_pins (~815-820): PD8-PD13, PD15-PD23 (PD14
     * skipped). Mux value 4 from pinctrl-sunxi.c sun50i_a64 table (line
     * ~629: {"emac", 4} PD8-PD23). PIO PD bank = index 3, CFG regs at
     * PIO_BASE + 3*0x24; 4 bits/pin. Drive strength set to level 3 (DTS
     * drive-strength=<40>). */
    {
        static const uint8_t pd_pins[] = { 8, 9, 10, 11, 12, 13, 15, 16,
                                           17, 18, 19, 20, 21, 22, 23 };
        const uint32_t bank_off = 3u * 0x24u;           /* PD bank */
        for (unsigned k = 0; k < sizeof(pd_pins); k++) {
            unsigned pin = pd_pins[k];
            volatile uint32_t *cfg =
                (volatile uint32_t *)(PIO_BASE + bank_off + (pin / 8u) * 4u);
            unsigned sh = (pin % 8u) * 4u;
            uint32_t v = *cfg;
            v = (v & ~(0xFu << sh)) | (0x4u << sh);      /* function 4 */
            *cfg = v;
            /* Drive strength (2 bits/pin), DRV0=+0x14 pins0-15, DRV1=+0x18 16-31 */
            volatile uint32_t *drv =
                (volatile uint32_t *)(PIO_BASE + bank_off + 0x14u + (pin / 16u) * 4u);
            unsigned dsh = (pin % 16u) * 2u;
            uint32_t dv = *drv;
            dv = (dv & ~(0x3u << dsh)) | (0x3u << dsh);  /* level 3 */
            *drv = dv;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    bc(1, BC_STAGE_PINMUX);

    /* 4. EMAC soft reset (EMAC_CTL1 SOFT_RST, poll for self-clear, bounded). */
    wr(EMAC_CTL1, EMAC_CTL1_SOFT_RST);
    {
        int ok = 0;
        for (int t = 0; t < 1000000; t++) {
            if (!(rd(EMAC_CTL1) & EMAC_CTL1_SOFT_RST)) { ok = 1; break; }
        }
        (void)ok;   /* even on timeout we press on; breadcrumb shows stage */
    }
    bc(1, BC_STAGE_RESET);

    /* MAC config: store-and-forward for TX and RX, DMA burst length 8. */
    setbits(EMAC_TX_CTL1, EMAC_TX_CTL1_TX_MD);
    setbits(EMAC_RX_CTL1, EMAC_RX_CTL1_RX_MD);
    wr(EMAC_CTL1, 8u << EMAC_CTL1_BURST_LEN_SHIFT);

    /* Set our MAC address. */
    write_hwaddr();

    /* 5. MDIO / PHY: reset, autoneg, resolve speed.
     *
     * ROOT-CAUSE FIX for the "dbgmon never comes up, everything else alive"
     * boots: this used to `return -1` HERE on a failed phy_startup(), before
     * rings_init() and the RX/TX/MAC enables below ever ran — and nothing in
     * the tree ever revisited that omission: link_recheck() (the only other
     * place a late link is noticed) assumes rings/DMA/MAC are already live
     * and only reprograms EMAC_CTL0 on a down->up transition. A PHY that
     * trained slower than the bounded LINK_WAIT_PASSES window left EMAC
     * permanently dark for that whole boot even once the wire linked fine.
     * Fix: ALWAYS arm rings + DMA + MAC below (enabling with no line signal
     * is harmless — nothing moves until the PHY links), so a late-training
     * link is picked up for free by link_recheck()/emac_link_watchdog(). */
    linked = phy_startup(&duplex_full);
    bc(4, linked ? 1 : 0);
    if (linked) {
        bc(5, g_speed);
        bc(14, ++g_link_up_events);      /* count the initial link too */
        bc(1, BC_STAGE_LINK);
        adjust_link(duplex_full);        /* program EMAC_CTL0 speed/duplex */
    } else {
        bc(1, BC_STAGE_NOLINK);          /* diagnostic only — press on below */
    }

    /* 6/7. DMA rings + buffers — UNCONDITIONAL now, see the fix note above. */
    rings_init();
    bc(1, BC_STAGE_RINGS);

    /* 8. Enable RX/TX DMA, then MAC RX/TX — UNCONDITIONAL, same reason. */
    setbits(EMAC_RX_CTL1, EMAC_RX_CTL1_RX_DMA_EN | EMAC_RX_CTL1_RX_ERR_FRM |
                          EMAC_RX_CTL1_RX_RUNT_FRM);
    setbits(EMAC_TX_CTL1, EMAC_TX_CTL1_TX_DMA_EN);
    /* Disable the hardware unicast destination-address filter (RXALL) --
     * found live 2026-07-23/24 while chasing why an ARP REPLY (unicast,
     * addressed to the GUEST's own virtio MAC) never reached vnet_emac.c
     * despite ARP REQUESTS (broadcast) working fine and despite the
     * software-side (to_us||bcast) gate already being removed from
     * emac.c's RX demux for the vnet path. Direct experiment on real
     * hardware (send a raw unicast test frame to a non-OUR_MAC
     * destination, watch vnet's rx_ethertype breadcrumb) proved the
     * default hardware filter (RXALL=0, i.e. only frames matching
     * EMAC_ADDR0/OUR_MAC or broadcast reach the RX ring at all) was
     * silently dropping it BEFORE any of our software ever saw it -- no
     * prior use of this EMAC (debug protocol, netcon, snapshot-net) ever
     * needed a different destination than OUR_MAC, so this was never hit
     * before. Written here (config-before-enable, at init time) rather
     * than as a live runtime toggle -- toggling EMAC_RX_CTL0's RX_EN bit
     * on a RUNNING board to test this disables ALL reception including
     * the debug protocol itself, self-inflicting an "EMAC dark" episode
     * (recovered via the USB-ACM break-glass, see project memory) before
     * this fix was written correctly here. */
    wr(EMAC_RX_FRM_FLT, EMAC_FRM_FLT_RXALL);
    setbits(EMAC_RX_CTL0, EMAC_RX_CTL0_RX_EN);
    setbits(EMAC_TX_CTL0, EMAC_TX_CTL0_TX_EN);
    __asm__ volatile("dsb sy" ::: "memory");
    bc(1, BC_STAGE_ENABLED);

    /* Callers still get the same "no link yet" signal as before; EMAC itself
     * is now always armed regardless. */
    return linked ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* CPU1 link watchdog — self-heal for "EMAC never delivered a single RX  */
/* frame". Called every CPU1 debug-loop iteration; internally rate-      */
/* limited (one real check per LINK_WD_CHECK_PERIOD_S), so the healthy   */
/* path costs ~one branch. Returns 1 exactly once — on the iteration     */
/* where it gives up — so the caller may escalate (e.g. opt-in reboot);  */
/* 0 on every other call.                                                */
/* ------------------------------------------------------------------ */
#define LINK_WD_CHECK_PERIOD_S   8u   /* how often we re-check             */
#define LINK_WD_MAX_ATTEMPTS     6u   /* ~48 s of retries before giving up */

static uint64_t g_wd_last_check_ticks;
static uint32_t g_wd_attempts;
static uint32_t g_wd_gave_up;

int emac_link_watchdog(void)
{
    uint64_t freq = timer_freq();
    uint64_t now  = timer_now();
    uint64_t period_ticks;

    if (freq == 0)
        freq = 24000000ull;
    period_ticks = freq * (uint64_t)LINK_WD_CHECK_PERIOD_S;

    if (g_wd_gave_up)
        return 0;
    if (g_wd_last_check_ticks == 0)
        g_wd_last_check_ticks = now;      /* first call: establish baseline */
    if (now - g_wd_last_check_ticks < period_ticks)
        return 0;                         /* not due yet — cheap common path */
    g_wd_last_check_ticks = now;

    if (g_rx_count != 0)
        return 0;           /* healthy: at least one frame has ever arrived */

    g_wd_attempts++;
    bc(18, g_wd_attempts);
    if (g_wd_attempts > LINK_WD_MAX_ATTEMPTS) {
        g_wd_gave_up = 1;
        bc(1, BC_STAGE_WD_GAVEUP);
        return 1;           /* one-shot: tell the caller to consider escalating */
    }

    /* Bounded re-kick: re-run PHY bring-up, then unconditionally re-arm the
     * rings and RX/TX/MAC enables — safe by construction, since this only
     * ever runs while g_rx_count==0 (no in-flight traffic to clobber).
     * phy_startup() pets the WDT throughout and is itself bounded. */
    {
        int duplex_full = 1;
        int linked = phy_startup(&duplex_full);
        bc(4, linked ? 1 : 0);
        if (linked) {
            bc(5, g_speed);
            adjust_link(duplex_full);
        }
        rings_init();
        setbits(EMAC_RX_CTL1, EMAC_RX_CTL1_RX_DMA_EN | EMAC_RX_CTL1_RX_ERR_FRM |
                              EMAC_RX_CTL1_RX_RUNT_FRM);
        setbits(EMAC_TX_CTL1, EMAC_TX_CTL1_TX_DMA_EN);
        setbits(EMAC_RX_CTL0, EMAC_RX_CTL0_RX_EN);
        setbits(EMAC_TX_CTL0, EMAC_TX_CTL0_TX_EN);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* TX — build and send one raw Ethernet frame (shared by the console line  */
/* flusher and emac_send_frame()). Returns 1 if the frame was queued to    */
/* TX DMA, 0 if it was dropped (link down, TX ring stayed busy past the    */
/* bounded wait, or the cross-core TX lock could not be acquired) — the    */
/* caller (netcon, vnet_emac.c, in the send-frame case) decides whether/   */
/* how to retry; this function itself never blocks unboundedly.           */
/* ------------------------------------------------------------------ */
static int tx_frame_raw(const uint8_t dst[6], uint16_t ethertype,
                         const uint8_t *payload, int plen)
{
    volatile struct emac_desc *d;
    uint8_t *buf;
    int total, i, to;

    if (!emac_tx_lock_acquire_bounded()) {
        bc(15, ++g_tx_drops);
        return 0;
    }

    if (!g_link_up) {
        /* No point queuing onto a dead link — count the drop instead of
         * silently swallowing it, so the host lane can see loss happening
         * instead of just a gap in the log. */
        bc(15, ++g_tx_drops);
        emac_tx_unlock();
        return 0;
    }

    if (plen > (ETH_BUFSIZE - 14 - 4))
        plen = ETH_BUFSIZE - 14 - 4;

    d = tx_desc(g_tx_slot);

    /* Make sure this descriptor is free (OWN clear) — bounded wait. Nudge
     * TX DMA periodically in case it stalled rather than being legitimately
     * busy with a prior frame still in flight. */
    cache_inval((uintptr_t)d, sizeof(*d));
    to = 200000;
    while (--to > 0 && (d->status & EMAC_DESC_OWN_DMA)) {
        if ((to & 0x3fffu) == 0)
            setbits(EMAC_TX_CTL1, EMAC_TX_CTL1_TX_DMA_START);
        cache_inval((uintptr_t)d, sizeof(*d));
    }
    if (to <= 0) {
        /* Previous TX still in flight after the bounded wait: drop this
         * frame, count it, but STILL advance g_tx_slot so a single wedged
         * descriptor doesn't retry the same slot forever and wedge the
         * whole channel — the next call gets a fresh descriptor to try. */
        bc(15, ++g_tx_drops);
        if (++g_tx_slot >= N_TX_DESC)
            g_tx_slot = 0;
        emac_tx_unlock();
        return 0;
    }

    buf = (uint8_t *)(uintptr_t)d->buf_addr;

    for (i = 0; i < 6; i++) buf[i] = dst[i];           /* dst */
    for (i = 0; i < 6; i++) buf[6 + i] = OUR_MAC[i];   /* src */
    buf[12] = (uint8_t)(ethertype >> 8);
    buf[13] = (uint8_t)(ethertype & 0xff);
    for (i = 0; i < plen; i++) buf[14 + i] = payload[i];

    total = 14 + plen;
    if (total < 60) {                 /* pad to the 60-byte min (pre-CRC) */
        for (i = total; i < 60; i++) buf[i] = 0;
        total = 60;
    }

    cache_clean((uintptr_t)buf, (uint32_t)total);

    d->ctl_size = (uint32_t)total | EMAC_DESC_CHAIN_SECOND |
                  EMAC_DESC_FIRST_DESC | EMAC_DESC_LAST_DESC;
    d->status = EMAC_DESC_OWN_DMA;
    cache_clean((uintptr_t)d, sizeof(*d));

    /* Kick the TX DMA. */
    setbits(EMAC_TX_CTL1, EMAC_TX_CTL1_TX_DMA_START);

    if (++g_tx_slot >= N_TX_DESC)
        g_tx_slot = 0;
    bc(6, ++g_tx_count);
    emac_tx_unlock();
    return 1;
}

/* Console TX kept byte-identical in behavior: broadcast dst, ETHERTYPE_CONSOLE. */
static void tx_frame(const uint8_t *payload, int plen)
{
    (void)tx_frame_raw(BCAST_MAC, ETHERTYPE_CONSOLE, payload, plen);
}

void emac_flush(void)
{
    if (tx_line_len == 0)
        return;
    tx_frame(tx_line, tx_line_len);
    tx_line_len = 0;
}

/* Public: send ONE raw frame on an arbitrary EtherType (used by netcon.c).
 * Broadcast dst — same as the console, and simplest/most robust for a
 * point-to-point debug link with exactly one host peer. Zero-padded to the
 * 60-byte minimum by tx_frame_raw(). Returns 1 if queued, 0 if dropped. */
int emac_send_frame(uint16_t ethertype, const uint8_t *payload, uint16_t len)
{
    return tx_frame_raw(BCAST_MAC, ethertype, payload, (int)len);
}

int emac_send_frame_to(const uint8_t dst[6], uint16_t ethertype,
                       const uint8_t *payload, uint16_t len)
{
    return tx_frame_raw(dst, ethertype, payload, (int)len);
}

void emac_putc(int c)
{
    if (tx_line_len < TX_LINE_MAX)
        tx_line[tx_line_len++] = (uint8_t)c;
    if (c == '\n' || tx_line_len >= TX_LINE_MAX)
        emac_flush();
}

void emac_puts(const char *s)
{
    while (*s)
        emac_putc((unsigned char)*s++);
}

#if defined(DBG_AUTH)
/* ------------------------------------------------------------------ *
 * ROADMAP T5 keyed auth (docs/security-notes.md option 1, -DDBG_AUTH).
 * ------------------------------------------------------------------ *
 * Wire envelope for a DBG_AUTH console frame's PAYLOAD (the bytes starting
 * right after the 14-byte Ethernet header, i.e. buf+14 in emac_poll()):
 *
 *   [0..7]   nonce   8 bytes, big-endian uint64, STRICTLY INCREASING
 *   [8..39]  mac     32 bytes, HMAC-SHA256(DBG_AUTH_KEY, nonce || cmd)
 *   [40..]   cmd     the command line bytes — same zero-padded-ASCII
 *                    convention the unauthenticated protocol already uses
 *                    from here on (dbgmon.c / gdbstub.c neither know nor
 *                    care that the bytes they're handed passed a MAC check
 *                    first).
 *
 * See hvdbg.py's HV(key=...) / _send() and gdb-bridge.py's --key for the
 * host-side encoder that produces this envelope; when no key is passed on
 * the host, those tools send the plain unauthenticated frame exactly as
 * before (this board-side gate simply isn't compiled in unless -DDBG_AUTH
 * is set, so there is no behavior to keep in sync in that case).
 */
#ifndef DBG_AUTH_KEY
/* No key was supplied via -DDBG_AUTH_KEY="..." (EXTRA_CFLAGS) — fall back to
 * an obviously-not-secret placeholder so the ROADMAP T5 build-verify matrix
 * (`make dbg EXTRA_CFLAGS=-DDBG_AUTH`, no key) still compiles clean. NEVER
 * ship this default key to a real deployment — see docs/security-notes.md. */
#define DBG_AUTH_KEY "CHANGE-ME-bzdOS-dbg-auth-default-key"
#endif

#define DBG_AUTH_NONCE_LEN 8u
#define DBG_AUTH_MAC_LEN   32u
#define DBG_AUTH_HDR_LEN   (DBG_AUTH_NONCE_LEN + DBG_AUTH_MAC_LEN)
#define DBG_AUTH_MAX_CMD   300u   /* CHUNK in gdb-bridge.py is 256; dbgmon.c's
                                   * own DBGMON_LINE_MAX is 128 — 300 covers
                                   * both with headroom, staged on the stack
                                   * below (bounded, never overflowed). */

/* Monotonic replay guard: only nonces STRICTLY GREATER than the last one
 * accepted are honored. Resets to 0 on every HV boot — a replayed frame
 * from a previous power cycle is exactly the threat this stops. Single-
 * owner: RX is always serviced from one core at a time (see this file's own
 * "RX demux hook" reasoning above emac_poll()'s TX cross-core comment). */
static uint64_t g_dbg_auth_last_nonce;

/* Returns 1 and advances g_dbg_auth_last_nonce iff `payload` (the frame
 * bytes after the Ethernet header, `paylen` of them) carries a valid
 * DBG_AUTH envelope. Returns 0 for anything else (short frame, stale/
 * replayed nonce, bad MAC) — caller drops the frame silently, same as an
 * unrecognized ethertype. */
static int dbg_auth_check(const uint8_t *payload, uint32_t paylen)
{
    static const uint8_t key[] = DBG_AUTH_KEY;
    uint64_t nonce;
    uint8_t  mac[DBG_AUTH_MAC_LEN];
    uint8_t  expect[DBG_AUTH_MAC_LEN];
    uint8_t  staged[DBG_AUTH_NONCE_LEN + DBG_AUTH_MAX_CMD];
    uint32_t cmd_len, msg_len;
    uint32_t i;

    if (paylen < DBG_AUTH_HDR_LEN)
        return 0;

    nonce = 0;
    for (i = 0; i < DBG_AUTH_NONCE_LEN; i++)
        nonce = (nonce << 8) | payload[i];
    if (nonce <= g_dbg_auth_last_nonce)
        return 0;                      /* stale or replayed */

    for (i = 0; i < DBG_AUTH_MAC_LEN; i++)
        mac[i] = payload[DBG_AUTH_NONCE_LEN + i];

    cmd_len = paylen - DBG_AUTH_HDR_LEN;
    if (cmd_len > DBG_AUTH_MAX_CMD)
        cmd_len = DBG_AUTH_MAX_CMD;    /* never read/hash past staged[] */

    for (i = 0; i < DBG_AUTH_NONCE_LEN; i++)
        staged[i] = payload[i];
    for (i = 0; i < cmd_len; i++)
        staged[DBG_AUTH_NONCE_LEN + i] = payload[DBG_AUTH_HDR_LEN + i];
    msg_len = DBG_AUTH_NONCE_LEN + cmd_len;

    hmac_sha256(key, (uint32_t)(sizeof(key) - 1), staged, msg_len, expect);

    if (!hmac_sha256_equal(mac, expect))
        return 0;

    g_dbg_auth_last_nonce = nonce;
    return 1;
}
#endif /* DBG_AUTH */

/* ------------------------------------------------------------------ */
/* RX — drain the ring into the getc byte buffer                        */
/* ------------------------------------------------------------------ */
void emac_poll(void)
{
    bc(1, BC_STAGE_LOOP);
    bc(8, rd(EMAC_INT_STA));

    /* Throttled live link check: MDIO transactions are relatively slow
     * (tens of us each), so we don't want one on every single poll — but we
     * do want emac_link_up() to reflect reality within a fraction of a
     * second, not just at emac_init() time. */
    if ((++g_poll_calls & (LINK_CHECK_INTERVAL - 1u)) == 0)
        link_recheck();

    for (int guard = 0; guard < N_RX_DESC; guard++) {
        volatile struct emac_desc *d = rx_desc(g_rx_slot);
        uint32_t status;
        int length;
        const uint8_t *buf;

        cache_inval((uintptr_t)d, sizeof(*d));
        status = d->status;
        if (status & EMAC_DESC_OWN_DMA)
            break;                    /* still owned by DMA — nothing new */

        length = (int)((status >> 16) & 0x3fff);
        buf = (const uint8_t *)(uintptr_t)d->buf_addr;

        if (!(status & EMAC_DESC_RX_ERROR_MASK) && length >= 14 &&
            length <= ETH_RXSIZE) {
            cache_inval((uintptr_t)buf, (uint32_t)length);

            /* Filter: our EtherType, addressed to our MAC or broadcast. */
            uint16_t et = (uint16_t)((buf[12] << 8) | buf[13]);
            int to_us = 1, bcast = 1;
            for (int i = 0; i < 6; i++) {
                if (buf[i] != OUR_MAC[i]) to_us = 0;
                if (buf[i] != 0xff)       bcast = 0;
            }
            if (et == ETHERTYPE_CONSOLE && (to_us || bcast)) {
#if defined(PROD_NO_DBG)
                /* ROADMAP T5 prod lockout (docs/security-notes.md option 2,
                 * -DPROD_NO_DBG): the debug console's COMMAND DISPATCH is
                 * compiled out of this build entirely. Reject every 0x88B5
                 * console frame right here at the RX-accept edge — not one
                 * byte of it reaches the byte ring dbgmon_service()'s/
                 * gdb_getc()'s callers read from, so there is no dispatcher
                 * left to drive even if bytes somehow got in some other
                 * way. The board runs everything else (guest, HDMI,
                 * virtio, netcon, snapnet, ...) completely unchanged; only
                 * this one LAN-facing peek/poke/call/gdb surface is gone. */
#elif defined(DBG_AUTH)
                /* ROADMAP T5 keyed auth (docs/security-notes.md option 1,
                 * -DDBG_AUTH): only accept the frame onto the console byte
                 * ring if dbg_auth_check() verifies its HMAC-SHA256 envelope
                 * (see the function's doc comment above emac_poll()). A
                 * frame that fails (bad MAC, replayed/stale nonce, too
                 * short) is dropped silently — same as a frame that failed
                 * the to_us/bcast/ethertype filter above. */
                if (dbg_auth_check(buf + 14, (uint32_t)(length - 14))) {
                    const uint8_t *cmd = buf + 14 + DBG_AUTH_HDR_LEN;
                    int cmd_len = length - 14 - (int)DBG_AUTH_HDR_LEN;
                    for (int i = 0; i < cmd_len; i++) {
                        uint8_t b = cmd[i];
                        if (b == 0)       /* stop at zero padding */
                            break;
                        rx_push(b);
                    }
                    note_rx_frame();
                }
#else
                for (int i = 14; i < length; i++) {
                    uint8_t b = buf[i];
                    if (b == 0)           /* stop at zero padding */
                        break;
                    rx_push(b);
                }
                note_rx_frame();
#endif
            } else if (et == ETHERTYPE_NETCON && (to_us || bcast)) {
                /* Reliable-datagram channel: NEVER feed these bytes into the
                 * console ring (they are binary, not zero-terminated ASCII).
                 * Hand the whole payload to netcon.c's parser instead. The
                 * netcon frame header carries its own chunk_len, so — unlike
                 * the console path — we pass the full Ethernet payload
                 * (length - 14) and let netcon_rx_frame() figure out how much
                 * of it (including the zero-padding tail) is real. */
                netcon_rx_frame(buf + 14, (uint16_t)(length - 14));
                note_rx_frame();
            } else if (et == ETHERTYPE_SNAPNET && (to_us || bcast)) {
                /* Bulk snapshot-stream channel: same "strip the 14-byte
                 * Ethernet header, hand the rest to the module's own parser"
                 * convention as netcon above. NEVER falls through to the
                 * vnet_emac muxer below (that hook is for the ROADMAP C1
                 * virtio-net device and expects IP-ish traffic, not our raw
                 * framing). */
                snapshot_net_rx_frame(buf + 14, (uint16_t)(length - 14));
                note_rx_frame();
            } else if (et == ETHERTYPE_DBGRAW && (to_us || bcast)) {
                /* Raw dbgtools peek (see dbgtools.h): serviced HERE, directly
                 * inside emac_poll(), so it answers regardless of whether
                 * CPU1's for(;;) loop (smp.c) is currently inside
                 * dbgmon_service() or gdbstub_poll()/command_loop() — both
                 * call emac_poll() (via console_poll()/gdb_getc()) on every
                 * pass, so this branch is reachable from either. */
                dbgtools_rx_frame(buf + 14, (uint16_t)(length - 14));
                note_rx_frame();
            } else if (et != ETHERTYPE_CONSOLE && et != ETHERTYPE_NETCON &&
                       et != ETHERTYPE_SNAPNET && et != ETHERTYPE_DBGRAW) {
                /* Not our debug protocol, not netcon: ROADMAP C1 hook —
                 * hand the whole frame to the virtio-net-over-EMAC
                 * multiplexer (vnet_emac.c) instead of silently dropping
                 * it. Weak no-op above when vnet_emac.c isn't linked in, so
                 * every existing image's behavior is unchanged.
                 *
                 * DELIBERATELY no (to_us || bcast) gate here (2026-07-23,
                 * found live via tcpdump while chasing why ping never got
                 * an ARP reply back to the guest): emac_send_frame()'s TX
                 * path always sources frames from OUR_MAC (see its own
                 * "MAC IDENTITY NOTE"), but the guest's virtio-net driver's
                 * OWN protocol payloads (e.g. an ARP request's sender-HA
                 * field) correctly carry the guest's REAL virtio MAC. Peers
                 * that learn that MAC from the payload (not from the
                 * Ethernet source, which is a separate mechanism) address
                 * their unicast REPLIES to it directly -- a destination our
                 * hardware never learns to recognize as "us". Since this
                 * link is a dedicated point-to-point board<->host cable
                 * (not a busy shared segment), accepting every non-debug-
                 * protocol frame regardless of destination MAC is safe and
                 * correct here -- vnet_emac.c's own ethertype/queue checks
                 * are still the real gate on what it does with a frame.
                 * This also matches what a future in-guest packet sniffer
                 * (promiscuous-style capture) needs: frames not addressed
                 * to the guest's own MAC must still reach it to be seen. */
                vnet_emac_rx_frame(buf, (uint16_t)length);
                note_rx_frame();
            }
        }

        /* Hand the descriptor back to the DMA. */
        d->status = EMAC_DESC_OWN_DMA;
        cache_clean((uintptr_t)d, sizeof(*d));

        if (++g_rx_slot >= N_RX_DESC)
            g_rx_slot = 0;
    }

    /* Keep RX DMA pumping in case it stalled at the ring tail. */
    setbits(EMAC_RX_CTL1, EMAC_RX_CTL1_RX_DMA_START);
}

int emac_getc(void)
{
    int c;
    if (rx_head == rx_tail)
        return -1;
    c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) & (RX_RING_SIZE - 1);
    return c;
}

int emac_link_up(void)
{
    return g_link_up;
}
