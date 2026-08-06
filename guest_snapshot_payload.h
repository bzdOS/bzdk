/* SPDX-License-Identifier: BSD-2-Clause */

/* guest_snapshot_payload.h — minimal, OBSERVABLE EL1 guest for exercising
 * snapshot_save()/snapshot_restore() (snapshot.h/snapshot.c) under QEMU
 * `virt` (ROADMAP D1). See guest_snapshot_payload.c for the full rationale.
 */
#ifndef BZDOS_GUEST_SNAPSHOT_PAYLOAD_H
#define BZDOS_GUEST_SNAPSHOT_PAYLOAD_H

#include <stdint.h>

/* Breadcrumb window this payload writes, EL2-readable via a plain physical
 * load (same idiom as guest.c's GUEST_BC_BASE / main_qemu.c's
 * S2_AT_PAR_LO): word[0] magic "SNGP", word[1] the free-running loop
 * counter el2_exc_snapshot_qemu.c samples every tick, word[2] CurrentEL as
 * read by the guest itself (expected 1).
 *
 * Deliberately NOT guest.c's own GUEST_BC_BASE (0x50000b00): under this
 * target stage-2 IS enabled (main_snapshot_qemu.c mirrors main_qemu.c), and
 * stage2.c deliberately leaves the 2 MiB hv-scratch window starting at
 * 0x50000000 INVALID in the guest's stage-2 map (see vgic_qemu's own
 * VGST_BC_BASE comment in the Makefile for the identical constraint and the
 * identical fix) — a write there from EL1 would take a stage-2 fault
 * instead of landing in DRAM. 0x50200000 is the same "first valid address
 * after hv-scratch" the vgic-qemu target already established: inside
 * stage2.h's identity-mapped guest DRAM range (0x40000000..0x80000000),
 * outside the hv-image window (0x42000000 + 2 MiB) and outside hv-scratch
 * (0x50000000 + 2 MiB). Reused here for the same reason, not by accident.
 */
#define SNAPGP_BC_BASE   0x50200000UL
#define SNAPGP_BC_MAGIC  0x53474e50u   /* "SNGP" LE */

enum {
	SNAPGP_BC_MAGIC_IDX = 0,
	SNAPGP_BC_LOOP_IDX  = 1,
	SNAPGP_BC_EL_IDX    = 2,
};

/* guest_config() (guest.c, unmodified) + a private EL1 stack + guest_enter()
 * into the payload below. One-way trip for the CPU, same contract as
 * guest.c's guest_start_demo() / guest_qemu_payload.c's qemu_guest_start():
 * after this call, execution alternates between "running the EL1 guest" and
 * "servicing the EL2 tick" (el2_exc_snapshot_qemu.c) forever, by design,
 * until the tick handler's freeze/dump/restore sequence decides the run is
 * over. */
void snapshot_guest_start(void) __attribute__((noreturn));

#endif /* BZDOS_GUEST_SNAPSHOT_PAYLOAD_H */
