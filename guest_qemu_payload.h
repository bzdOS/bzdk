/* guest_qemu_payload.h — minimal "hello from EL1" guest for the QEMU
 * `virt` CI target. See guest_qemu_payload.c for the full rationale.
 */
#ifndef BZDOS_GUEST_QEMU_PAYLOAD_H
#define BZDOS_GUEST_QEMU_PAYLOAD_H

/* guest_config(); pick a private EL1 stack; guest_enter() into the payload.
 * One-way trip for the CPU, same contract as guest.c's guest_start_demo():
 * after this call, execution alternates between "running the EL1 guest" and
 * "servicing the EL2 tick" (el2_exc_qemu.c), forever, by design — until the
 * tick handler decides enough preemptions have been proven and shuts QEMU
 * down. */
void qemu_guest_start(void) __attribute__((noreturn));

#endif /* BZDOS_GUEST_QEMU_PAYLOAD_H */
