/* SPDX-License-Identifier: BSD-2-Clause -- see sdbox.c */
#ifndef SDBOX_H
#define SDBOX_H
#include <stdint.h>
/* reboot_reason values, set by the caller just before reboot_clean() */
#define RB_REASON_UNKNOWN   0u
#define RB_REASON_BMC       1u   /* bmc.c `reset`                          */
#define RB_REASON_PSCI_OFF  2u   /* guest PSCI SYSTEM_OFF                  */
#define RB_REASON_PSCI_RST  3u   /* guest PSCI SYSTEM_RESET                */
#define RB_REASON_EMACDARK  4u   /* smp.c EMAC-dark escalation             */
#define RB_REASON_REPL      5u   /* repl.c `reset`                         */
/* Not reboot_clean() callers: recorded from CPU1's tick on the way to a
 * watchdog reset that nothing else would leave a trace of. */
#define RB_REASON_EMACGIVEUP 6u  /* link ladder gave up, auto-reboot off   */
#define RB_REASON_UNREACH   7u   /* wdt.c: no reach within WDT_UNREACH_S   */
extern volatile uint32_t reboot_reason;
void sdbox_record(uint32_t reason);   /* weak no-op in builds without SD */
#endif
