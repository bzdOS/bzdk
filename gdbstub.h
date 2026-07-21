/* gdbstub.h — GDB Remote Serial Protocol stub for the bzdOS EL2 hypervisor.
 *
 * Lets a real `gdb`/`lldb` attach to the running system (both our EL2 kernel
 * and the EL1 FreeBSD guest) over the same raw-Ethernet console channel the
 * dbgmon/REPL use (ethertype 0x88B5, board MAC 02:bd:05:00:00:01). A tiny
 * Python TCP<->raw-Ethernet bridge on the host turns `target remote :1234`
 * into 0x88B5 frames (see the spec at the bottom of gdbstub.c).
 *
 * DESIGN (fits the existing tick-driven debugger, exceptions.h frame model):
 *
 *   - The guest runs preempted every CNTP tick; on each tick el2_trap already
 *     has a full `struct el2_frame *` snapshot. Call gdbstub_poll(frame) on
 *     that path (in place of dbgmon_service) to service GDB. While the guest
 *     is *running* this only watches for a Ctrl-C (0x03) interrupt or the
 *     initial GDB handshake; the moment GDB talks to us we halt the guest and
 *     enter the RSP command loop (GDB's model: an attached target is stopped).
 *
 *   - When a software breakpoint (guest BRK, ESR_EL2.EC==0x3C) or a completed
 *     single-step (EC==0x32, while we own stepping) traps into el2_trap, call
 *     gdbstub_on_debug_event(frame, 5 == SIGTRAP). It sends the stop reply and
 *     runs the RSP command loop until GDB resumes (c/s/D/k).
 *
 *   - Single-step reuses the EL2 mechanism from el2_exc.c: MDSCR_EL1.SS=1 +
 *     MDCR_EL2.TDE=1 + PSTATE.SS on eret (set in frame->spsr). Breakpoints
 *     patch a "BRK 0x7d0" into (translated) guest/physical memory, saving the
 *     original instruction, with a dc cvau + ic ivau flush like dbgmon/gtrace.
 *
 * Byte transport is provided by the caller (wired to emac in main_dbg.c):
 *     int  gdb_getc(void);   // one RX byte, or -1 if none (MUST pump emac RX)
 *     void gdb_putc(int c);  // queue one TX byte
 *     void gdb_flush(void);  // force queued TX bytes out as a frame now
 *
 * Freestanding: <stdint.h> + "exceptions.h" only, no libc, no FP/SIMD.
 */
#ifndef BZDOS_GDBSTUB_H
#define BZDOS_GDBSTUB_H

#include "exceptions.h"

/* gdbstub_poll() return codes. The stub ALSO applies the decision directly to
 * `frame` (arms/disarms PSTATE.SS in frame->spsr and the debug sysregs), so a
 * caller that just erets the frame gets the right behaviour and may ignore the
 * return value. The codes are exposed for callers that want to log/branch. */
enum {
	GDB_RUN_NONE     = 0,   /* nothing pending — keep running the guest */
	GDB_RUN_CONTINUE = 1,   /* resume the guest (single-step disarmed)  */
	GDB_RUN_STEP     = 2,   /* resume for exactly one instruction       */
	GDB_RUN_DETACH   = 3,   /* GDB detached/killed — resume, stub idle  */
};

/* Reset stub state. Call once after the console hooks are wired, before the
 * guest starts ticking. Emits NOTHING on the wire (the 0x88B5 channel now
 * carries binary RSP, so no human banner may be printed on it). */
void gdbstub_init(void);

/* Tick-path service. Non-blocking while the guest is running (watches for the
 * GDB handshake / Ctrl-C); once GDB is interacting it runs the RSP command
 * loop until GDB resumes. Returns a GDB_RUN_* code and arms `guest` to match. */
int  gdbstub_poll(struct el2_frame *guest);

/* Called from el2_trap when a breakpoint / single-step / fault stops the guest
 * (signal = target signal number, 5==SIGTRAP). Sends the stop reply, runs the
 * RSP command loop until GDB resumes, and arms `guest` for the chosen action. */
void gdbstub_on_debug_event(struct el2_frame *guest, int signal);

/* 1 if the stub currently owns single-stepping (so el2_trap should route a
 * lower-EL Software-Step, EC==0x32, to gdbstub_on_debug_event instead of the
 * el2_ss ring logger). */
int  gdbstub_step_active(void);

/* 1 once GDB has connected (any RSP packet seen). el2_trap should only divert
 * BRK (EC==0x3C) / step traps to the stub while attached; otherwise a guest's
 * own BRK (e.g. a FreeBSD panic) must fall through to the normal fault path
 * instead of blocking in the RSP loop for a GDB that isn't there. */
int  gdbstub_attached(void);

#endif /* BZDOS_GDBSTUB_H */
