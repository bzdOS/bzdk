/* guest_qemu_payload.c — minimal "hello from EL1" guest for the QEMU
 * `virt` CI target (ROADMAP.md T3).
 *
 * This is the QEMU-target analogue of guest.c's guest_demo_el1()/
 * guest_start_demo(), reusing guest_config()/guest_enter() from guest.c
 * UNCHANGED (both are fully generic AArch64 EL1 setup — no board-specific
 * addresses in either). The only thing genuinely new here is the payload
 * itself: instead of a silent spin loop whose progress can only be read
 * back from a breadcrumb word (guest.c's approach, needed on the real board
 * because there is no direct guest console), this guest prints straight to
 * the PL011 UART via pl011_qemu.c — safe and simple under QEMU because
 * stage2.c's existing MMIO identity map (unmodified) already covers the
 * PL011's real address, so the guest's writes reach an ACTUAL device
 * instead of needing trap-and-emulate (vconsole.c's whole reason for
 * existing on the real board, which has no such passthrough).
 *
 * Proves, end to end, exactly what this milestone needs to prove:
 *   - the EL1 guest actually executes (CurrentEL prints as 1, loop counter
 *     climbs across repeated prints);
 *   - it is running under stage-2 translation (HCR_EL2.VM=1, programmed by
 *     stage2_init()/stage2_enable() before guest_enter() — the guest could
 *     not reach the UART otherwise, since IPA==PA identity mapping is what
 *     makes the direct MMIO write land on the real PL011);
 *   - it is preemptible by the EL2 GICv2 timer tick (el2_exc_qemu.c prints
 *     its own progress lines interleaved with this guest's, from the same
 *     serial output, while this loop keeps running afterward).
 */
#include <stdint.h>
#include "guest.h"
#include "guest_qemu_payload.h"
#include "pl011_qemu.h"

static inline uint64_t
read_currentel(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
	return v >> 2;   /* CurrentEL.EL is bits [3:2] */
}

/* Private EL1 stack, owned entirely by this file — same 16 KiB sizing as
 * guest.c's own guest_el1_stack (a payload that only loops and prints needs
 * nothing more). */
#define GUEST_STACK_WORDS (16384 / 8)
static uint64_t qemu_guest_stack[GUEST_STACK_WORDS] __attribute__((aligned(16)));

/* Print a progress line roughly every this many loop iterations — pure
 * busy-loop count, not time (this build has no guest-visible timebase set
 * up), just needs to be "occasionally", not "every iteration" (which would
 * make the UART, not the loop, the bottleneck). */
#define GUEST_PRINT_PERIOD 2000000u

static void qemu_guest_entry(void) __attribute__((noreturn));

static void
qemu_guest_entry(void)
{
	uint32_t n = 0;
	uint32_t loops = 0;

	pl011_puts("GUEST EL1: hello from under stage-2 translation, CurrentEL=");
	pl011_put_udec((uint32_t)read_currentel());
	pl011_puts("\n");

	for (;;) {
		if (++n == GUEST_PRINT_PERIOD) {
			n = 0;
			loops++;
			pl011_puts("GUEST EL1: still alive, loop=");
			pl011_put_udec(loops);
			pl011_puts("\n");
		}
	}
}

void
qemu_guest_start(void)
{
	uint64_t sp_top = (uint64_t)&qemu_guest_stack[GUEST_STACK_WORDS];

	guest_config();
	guest_enter((uint64_t)qemu_guest_entry, sp_top);
	__builtin_unreachable();
}
