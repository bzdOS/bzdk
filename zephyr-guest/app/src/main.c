/*
 * Copyright (c) 2026 bzdOS project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal proof-of-life app for Zephyr as an alternate EL1 guest under the
 * bzdOS EL2 hypervisor (board: bpi_m64_hv). Deliberately does NOT depend on
 * any interrupt reaching this guest (no system tick, no shell, no k_sleep):
 * see boards/bzdos/bpi_m64_hv/Kconfig.defconfig for exactly why (the
 * hypervisor's virtual-timer/vgic plumbing is not wired into any
 * guest-boot path yet). Only two things are exercised here, both of which
 * work with zero guest-visible interrupts:
 *   - printk() over the polled ns16550 UART0 driver (bzdOS's vconsole.c
 *     trap-and-emulates every MMIO access synchronously, so "polling" from
 *     the guest's point of view is just normal trapped stores/loads).
 *   - k_busy_wait(), which on this board config compiles down to a plain
 *     calibrated spin loop (CONFIG_SYS_CLOCK_EXISTS=n --
 *     CONFIG_BUSYWAIT_CPU_LOOPS_PER_USEC-driven), not a timer peripheral.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	printk("\r\nbzdOS/Zephyr: hello from EL1 (board: bpi_m64_hv)\r\n");

	uint32_t beat = 0;

	for (;;) {
		printk("heartbeat %u\r\n", beat++);
		k_busy_wait(1000000); /* ~1s, per the board's (uncalibrated --
					* see Kconfig.defconfig) busy-loop
					* constant */
	}

	return 0;
}
