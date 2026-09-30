/* SPDX-License-Identifier: BSD-2-Clause */

/* hdmi_stub.c — the `hdmi` bring-up target links smp.o for its small helpers
 * but never calls smp_init(), so the CPU1 debug-core block in
 * smp_secondary_main() never runs here. The linker still wants every symbol
 * that block names; these are inert stand-ins, the subset of
 * smp_qemu_stub.c this target lacks (it keeps the real wdt.o/reboot.o, which
 * that file also stubs and would collide with). */
#include <stdint.h>
#include "exceptions.h"
#include "musb.h"
#include "emac.h"
#include "dbgmon.h"
#include "usbacm.h"

struct el2_frame g_last_guest_frame;
volatile uint32_t dbg_core_active;
volatile uint32_t gdb_stop_pending;
volatile uint32_t gdb_stop_signal;
volatile uint32_t gdb_resume_act;

void el2_snapshot_guest_frame(struct el2_frame *out)
{
	const uint64_t *src = (const uint64_t *)&g_last_guest_frame;
	uint64_t *dst = (uint64_t *)out;
	unsigned i;

	for (i = 0; i < sizeof(*out) / sizeof(uint64_t); i++)
		dst[i] = src[i];
}

void dbgmon_service(struct el2_frame *guest_frame) { (void)guest_frame; }
void musb_putc(int c) { (void)c; }
void musb_puts(const char *s) { (void)s; }
void musb_flush(void) { }
int emac_link_watchdog(void) { return 0; }
void usbacm_poll(void) { }
void usbacm_force_breakglass(void) { }
