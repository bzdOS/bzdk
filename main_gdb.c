/* SPDX-License-Identifier: BSD-2-Clause */

/* main_gdb.c — GDB-stub hypervisor build. Same live-debugger skeleton as
 * main_dbg.c (EMAC console up, real FreeBSD kernel booted as a preemptible
 * EL1 guest under stage-2), but the tick-path debugger is gdbstub.c/.h +
 * gdbstub_hw.c/.h INSTEAD OF dbgmon.c — see gdbstub.c's "PYTHON HOST BRIDGE
 * SPEC" header comment for the host-side bridge (raw Ethernet 0x88B5 <->
 * `target remote :1234`) and gdbstub.h's DESIGN section for the exact
 * caller contract this file implements.
 *
 * NON-INVASIVE WIRING TRICK: el2_exc.c (untouched, shared with the dbg
 * build) calls a single hardcoded symbol, `dbgmon_service(frame)`, on every
 * guest tick — declared `__attribute__((weak))` there so a build without
 * dbgmon.o still links. We do NOT link dbgmon.o in this build (GDB_OBJS
 * swaps it for gdbstub.o + gdbstub_hw.o); instead we give `dbgmon_service`
 * a STRONG definition right here that forwards straight to gdbstub_poll().
 * That overrides the weak stub for every caller of that symbol — both
 * el2_exc.c's own tick-path call AND smp.c's CPU1 debug-core loop — with
 * zero edits to either file, satisfying gdbstub.c's header requirement
 * ("gdbstub_poll() on the tick path INSTEAD OF dbgmon_service(), never
 * both") exactly: the real dbgmon.c is never linked, so there is only ever
 * one implementation live under that name, and it IS gdbstub.
 *
 * GAP (documented, not fixed here — see the task report): this trick only
 * wires the "poll for incoming GDB traffic / Ctrl-C while the guest runs"
 * half of gdbstub (gdbstub_poll). It does NOT wire gdbstub_on_debug_event()
 * for an actual breakpoint/single-step STOP — el2_exc.c's EC==0x30/0x32/0x34
 * cases still go straight to hwbp_handle()/el2_ss_handle() and EC==0x3C
 * (guest software BRK) has no case at all. That needs a small, surgical
 * edit to el2_exc.c's trap dispatch (out of scope here per the task's
 * el2_exc.c exclusion — see the report for the exact proposed diff).
 *
 * `dbgmon_call_active` is also normally defined (strong) in dbgmon.c and
 * referenced `extern` by el2_exc.c's cmd_call() fault-recovery path; since
 * dbgmon.o is not linked here we provide the same symbol, permanently 0
 * (gdbstub has no analogous "call a function" command, so it never sets it
 * — el2_exc.c's guard is simply always false in this build, a no-op).
 *
 * DEAD-MAN'S SWITCH: identical WDT arrangement to main_dbg.c — see that
 * file's header comment and SESSION-RULES.md.
 */
#include <stdint.h>
#include "emac.h"
#include "exceptions.h"
#include "kload.h"
#include "stage2.h"
#include "guest.h"
#include "vconsole.h"
#include "usbacm.h"
#include "gtrace.h"
#include "gic_timer.h"
#include "wdt.h"
#include "smp.h"
#include "gdbstub.h"
#include "reboot.h"
#include "onebp.h"
#include "vgic.h"
#include "vblk_emmc.h"
#include "el2_ncmap.h"
#include "dbgtools.h"    /* CPU1 heartbeat / build-id / entry-hold (2026-07-26) */
#include "soc_a64.h"   /* A64 peripheral addresses, consolidated — see that header */

#define K_ELF     0x44000000UL
#define K_PABASE  0x46000000UL
#define DTB_SRC   0x4a000000UL   /* DTB as TFTP'd by U-Boot (outside kernel map) */
#define DTB_DST   0x47200000UL   /* DTB copied here, just above _end, in-window  */
#define MODINFO   0x47400000UL   /* modinfo scratch, above the relocated DTB     */
#define SP_EL1    0x4c000000UL

/* Byte transport for gdbstub.c (see its header comment): rides the same raw
 * EMAC/0x88B5 channel as dbgmon/REPL. gdb_getc() MUST pump the EMAC RX ring
 * itself (gdbstub spins on it with IRQs otherwise masked while stopped). */
int  gdb_getc(void)  { emac_poll(); return emac_getc(); }
void gdb_putc(int c) { emac_putc(c); }
void gdb_flush(void) { emac_flush(); }

/* See the file header: overrides el2_exc.c's weak dbgmon_service() so every
 * existing call site (tick path + the CPU1 debug core in smp.c) drives
 * gdbstub instead, with no edits to either file. gdbstub_poll() applies its
 * resume decision directly to `f`; the GDB_RUN_* return value is not needed
 * by either caller. */
void dbgmon_service(struct el2_frame *f) { (void)gdbstub_poll(f); }

/* Stand-in for dbgmon.c's global of the same name (see file header). Always
 * 0 here: gdbstub has no "call a guest function" command that would set it,
 * so el2_exc.c's cmd_call() fault-recovery branch is simply dead code in
 * this build. */
volatile int dbgmon_call_active = 0;

#define DBG_BC(i, v) do { \
	volatile uint32_t *p = (volatile uint32_t *)(0x50000e00UL + (uint32_t)(i) * 4u); \
	*p = (uint32_t)(v); \
	__asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(p) : "memory"); \
} while (0)

extern volatile uint32_t gdb_channel;   /* gdbstub.c -- see below for why this
                                          * build must force it to 1 itself. */

int main(void)
{
	uint64_t mi, entry, i;

	DBG_BC(0, 0x47444231);   /* "GDB1" */
	DBG_BC(1, 1);
	usb_gadget_disconnect();   /* clean-disconnect U-Boot gadget on takeover */

	/* Network debug console up first. */
	emac_init();
	for (i = 0; i < 200000000UL && !emac_link_up(); i++)
		__asm__ volatile("nop");
	DBG_BC(1, emac_link_up() ? 2 : 0x1177);   /* 0x1177 = link never came up */

	el2_install();
	vconsole_init();

	/* CPU1 heartbeat / build-id / entry-hold breadcrumb lane (dbgtools.h,
	 * dbgtools.c) — see main_dbg.c's fuller comment. Same ordering
	 * constraint: before smp_init() brings CPU1 up. */
	dbgtools_init();

	/* eMMC-backed virtio-blk (see main_dbg.c's fuller comment on why this
	 * runs before stage2_init()/stage2_enable() below). */
	vblk_init();

	/* USB-OTG CDC-ACM interactive console bridge (must run before smp_init(),
	 * same ordering constraint as main_dbg.c). */
	usbacm_init();
	DBG_BC(1, 0x05B0AC30);   /* 'USB-ACM3' truncated: usbacm_init() done   */

#ifndef DBG_NO_TVM
	gtrace_init();
#endif
	gdbstub_init();          /* replaces dbgmon_init(): emits NOTHING on the
	                          * wire (RSP channel, no human banner). */
	DBG_BC(1, 3);

	/* This build has no dbgmon.o (see this file's own GDB_OBJS) — the ONLY
	 * mode is RSP debugging. gdb_channel (gdbstub.c) normally flips from 0
	 * to 1 via dbgmon's `gdb` text command; that command doesn't exist
	 * here, so it would otherwise stay 0 forever. smp.c's CPU1 loop gates
	 * the ENTIRE gdb_stop_pending/gdb_resume_act stop-reply handshake on
	 * `if (gdb_channel)` -- with it stuck at 0, CPU1 never checks
	 * gdb_stop_pending and never sets gdb_resume_act, so a real BRK divert
	 * (el2_exc.c) parks CPU0 in `while (gdb_resume_act == 0xffffffff) wfe;`
	 * FOREVER. CONFIRMED this session (2026-07-26/27, see memory
	 * hold-gate-plus-breakpoint-crashes-board.md): once gdbstub.c's
	 * resolve()/bp_insert() bug was fixed and a software breakpoint could
	 * finally fire for real in THIS build, the board hung exactly this way
	 * -- independent of the (separately added, unrelated) pause-before-
	 * entry gate, which merely made a real BRK hit reachable for the first
	 * time. Must be set before smp_init() (right below) so CPU1 sees the
	 * correct value from its very first loop iteration. */
	gdb_channel = 1;

	/* Arm the dead-man's-switch watchdog EARLY (see main_dbg.c). */
	wdt_arm();

	if (!kload_parse_elf(K_ELF)) { DBG_BC(1, 0xBAD1); for (;;) { } }
	kload_place_segments(K_ELF, K_PABASE);
	mi = kload_build_modinfo(DTB_SRC, DTB_DST, MODINFO);   /* returns modulep KVA */
	entry = kload_entry_pa();
	DBG_BC(1, 4);
	DBG_BC(6, (uint32_t)entry);

	guest_config();
#ifndef DBG_NO_TVM
	{ uint64_t vb = gtrace_vbar_el1();
	  __asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(vb)); }
#endif
	stage2_init();
	stage2_enable();
	stage2_unmap_guest_vector();   /* arm first-fault trap: unmap guest EL1
	                                * vector page so the guest's first fault
	                                * takes a stage-2 abort to EL2, exposing
	                                * the ORIGINAL ELR_EL1/ESR_EL1/FAR_EL1. */
	DBG_BC(1, 5);

	/* Zero CNTVOFF_EL2 so the guest's virtual counter CNTVCT_EL0 matches CNTPCT_EL0
	 * and DELAY() doesn't hang. */
	__asm__ volatile("msr cntvoff_el2, xzr\n\tisb" ::: "memory");

	/* IRQ POLICY: identical to main_dbg.c (route every physical IRQ straight
	 * to the guest's EL1, HCR_EL2.IMO=0/FMO=0; TSC=1 traps guest SMC/PSCI so
	 * SYSTEM_RESET/OFF can be intercepted). See main_dbg.c's fuller comment
	 * (ehci-intid106-storm, internal task) for why. */
	{
		uint64_t hcr;
		__asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
		hcr &= ~((1ull << 4) | (1ull << 5));   /* clear IMO(4) and FMO(5) */
		hcr |= (1ull << 19);                   /* TSC: trap guest SMC/PSCI */
		__asm__ volatile("msr hcr_el2, %0\n\tisb" :: "r"(hcr) : "memory");
	}
	DBG_BC(1, 6);

	DBG_BC(1, 0x53591417);   /* 'SY..' about to call smp_init */
	smp_init();
	DBG_BC(1, 0x53590417);   /* smp_init returned cleanly */
	DBG_BC(1, 7);

	/* eMMC pinmux fix: mux PC5 (SMHC2/MMC2 CLK) to function 3 (mmc2). See
	 * main_dbg.c's fuller comment; identical fix, needed regardless of which
	 * tick-path debugger is linked. */
	{
		volatile uint32_t *pc_cfg0 = (volatile uint32_t *)SOC_A64_PIO_PC_CFG0;
		uint32_t v = *pc_cfg0;
		v = (v & ~(0xFu << 20)) | (3u << 20);
		*pc_cfg0 = v;
		__asm__ volatile("dsb sy" ::: "memory");
	}

	/* PAUSE-BEFORE-ENTRY GATE — see main_dbg.c's fuller comment. Default
	 * OFF (both words start 0 on a cold boot); only takes effect across a
	 * WARM hv.wdt_reset()-style reload where a host tool armed HOLD=1
	 * beforehand.
	 *
	 * NOTE this build has neither dbgmon.o's text console nor its `w`
	 * word-write command (GDB_OBJS swaps dbgmon.o out for gdbstub.o) — but
	 * gdbstub_poll() (smp.c) already answers standard GDB RSP 'M' (write
	 * memory) packets on every CPU1 loop pass once smp_init() has run,
	 * WITHOUT requiring the guest to have trapped/stopped first. So the
	 * realistic way to arm this in the gdb build is: connect
	 * (gdb-bridge.py / `target remote`) after triggering a warm
	 * hv.wdt_reset()-style reload, and send a plain 'M' packet (or
	 * `set *(int*)HVMAP_DBGTOOLS_HOLD = 1` from a real gdb prompt) for
	 * HVMAP_DBGTOOLS_HOLD BEFORE this point runs. This is exactly the
	 * "plant a breakpoint before the guest's first instruction" use case
	 * that motivated this feature -- RSP debugging is where it matters
	 * most, since gdbstub (unlike dbgmon) has no other way to stop the
	 * guest before it has already run. */
	if (dbgtools_hold_get()) {
		DBG_BC(1, 0x60008);
		for (;;) {
			wdt_pet();
			if (dbgtools_release_get())
				break;
			__asm__ volatile("wfe" ::: "memory");
		}
		DBG_BC(1, 0x60009);
	}

	kload_enter(entry, mi, SP_EL1);   /* noreturn -> FreeBSD at EL1, gdb attaches live */
	for (;;) { }
}
