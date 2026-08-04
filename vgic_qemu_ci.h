/* SPDX-License-Identifier: BSD-2-Clause */

/* vgic_qemu_ci.h — shared contract between main_vgic_qemu.c and
 * el2_exc_vgic_qemu.c (the QEMU-virt vGIC CI target). See
 * main_vgic_qemu.c's banner for what that target proves and what it does not.
 *
 * Everything here is READ-ONLY OBSERVATION of vgic.c's two existing
 * breadcrumb windows. It deliberately does not add instrumentation to vgic.c:
 * the whole point of this target is to run vgic.c unmodified, so the CI
 * verdict is derived from the same words a live board read would show
 * (0x50001c00 "VGIC" / 0x50001d00 "VGST"), which also means a failure here is
 * diagnosable with the same mental model as a failure on hardware.
 *
 * Both windows are plain DRAM on QEMU virt (RAM is 0x40000000..0x80000000 at
 * -m 1024), so EL2 can read them directly; the "VGST" ones are written by the
 * GUEST at EL1 and read here at EL2, which works because stage2.c
 * identity-maps guest DRAM.
 */
#ifndef BZDOS_VGIC_QEMU_CI_H
#define BZDOS_VGIC_QEMU_CI_H

#include <stdint.h>

/* ---- vgic.c's own breadcrumb window (VGIC_BC_BASE / VGIC_BC_MAGIC in
 * vgic.c:236-237, layout documented in vgic.c:203-229). Index names below
 * mirror that documented layout; only the ones this target checks are
 * named. ---- */
#define VGIC_BC_WINDOW      0x50001c00UL
#define VGIC_BC_MAGIC_VAL   0x56474943u   /* "VGIC", vgic.c:237 */
#define VGIC_BC_MAGIC_IDX   0            /* [0]  magic                      */
#define VGIC_BC_VTR         1            /* [1]  raw GICH_VTR               */
#define VGIC_BC_NR_LR       2            /* [2]  VTR[5:0]+1                 */
#define VGIC_BC_HCR         3            /* [3]  GICH_HCR readback after En */
#define VGIC_BC_INJECT_CNT  4            /* [4]  total inject calls         */
#define VGIC_BC_INJECT_OK   5            /* [5]  injects that found a free LR */
#define VGIC_BC_LAST_LR     7            /* [7]  last LR word written       */
#define VGIC_BC_LAST_ELRSR  8            /* [8]  last GICH_ELRSR0 sampled   */
#define VGIC_BC_CNTVOFF    12            /* [12] 1 once CNTVOFF_EL2=0 written */
#define VGIC_BC_VMCR       13            /* [13] GICH_VMCR readback         */
#define VGIC_BC_CNTV_INJ   20            /* [20] CNTV ticks placed in an LR */
#define VGIC_BC_CNTV_GATE  21            /* [21] CNTV ticks gated (live 27) */
#define VGIC_BC_CNTV_DROP  23            /* [23] CNTV ticks dropped (no LR) */

/* ---- vgic.c's self-test-guest breadcrumb window (VGST_BC_BASE /
 * VGST_BC_MAGIC in vgic.c:690-691, layout vgic.c:682-689). Written BY THE
 * GUEST at EL1. ---- */
/* Must match the vgic-qemu target's -DVGST_BC_BASE= in the Makefile, NOT
 * vgic.c's 0x50001d00 default — see that default's comment for why the default
 * cannot work (it is inside the stage-2-excluded hv-scratch window and the
 * GUEST is what writes it). */
#define VGST_BC_WINDOW      0x50200000UL
#define VGST_BC_MAGIC_VAL   0x56475354u   /* "VGST", vgic.c:691 */
#define VGST_MAGIC_IDX      0            /* [0] magic                      */
#define VGST_GUEST_EL       1            /* [1] CurrentEL sampled at EL1   */
#define VGST_ALIVE          2            /* [2] guest main-loop counter    */
#define VGST_VIRQ_COUNT     3            /* [3] vIRQs delivered <-- THE proof */
#define VGST_LAST_IAR       4            /* [4] last virtual IAR read      */
#define VGST_SPURIOUS       5            /* [5] spurious IARs (>=1020)     */
#define VGST_OTHER_EXC      6            /* [6] non-IRQ EL1 exceptions     */
#define VGST_VBAR_SET       7            /* [7] VBAR_EL1 the payload set   */

#define VGIC_BC_RD(i)  (*(volatile uint32_t *)(VGIC_BC_WINDOW + (uint32_t)(i) * 4u))
#define VGST_BC_RD(i)  (*(volatile uint32_t *)(VGST_BC_WINDOW + (uint32_t)(i) * 4u))

/* The single greppable verdict prefixes vgic-qemu-ci.sh looks for. Kept here
 * so the script, main_vgic_qemu.c and el2_exc_vgic_qemu.c cannot drift. */
#define VGIC_CI_PASS  "VGIC-QEMU-CI: PASS "
#define VGIC_CI_FAIL  "VGIC-QEMU-CI: FAIL "

/* PSCI SYSTEM_OFF, so QEMU exits on its own on BOTH verdicts (the script must
 * not have to rely on its timeout to distinguish pass from fail).
 * Implemented in el2_exc_vgic_qemu.c. */
void vgic_ci_poweroff(void) __attribute__((noreturn));

#endif /* BZDOS_VGIC_QEMU_CI_H */
