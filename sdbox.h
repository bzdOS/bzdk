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
extern volatile uint32_t reboot_reason;
void sdbox_record(uint32_t reason);   /* weak no-op in builds without SD */
#endif
