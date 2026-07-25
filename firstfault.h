/* SPDX-License-Identifier: BSD-2-Clause */

/* firstfault.h — catch the FreeBSD guest's ORIGINAL first EL1 fault.
 *
 * Companion to stage2_unmap_guest_vector() (stage2.c): with the guest's EL1
 * vector page unmapped at stage-2, the guest's very first vector fetch takes
 * a stage-2 abort to EL2. el2_trap calls firstfault_handle() on any lower-EL
 * instruction/data abort (ESR_EL2 EC 0x20/0x21/0x24/0x25); this function
 * checks whether the faulting IPA is the vector page and, if so, latches the
 * guest's still-pristine EL1 fault state (ELR_EL1/ESR_EL1/FAR_EL1/SPSR_EL1/
 * SP_EL1 + all GPRs) into the cache-coherent FF1V breadcrumb at 0x50005800,
 * then re-maps the page and returns 1 so the guest can proceed.
 *
 * Freestanding: <stdint.h> only. See firstfault.c for the FF1V word layout
 * and the re-entry policy.
 */
#ifndef BZDOS_FIRSTFAULT_H
#define BZDOS_FIRSTFAULT_H

#include "exceptions.h"

/* Returns 1 if this abort was the guest's vector-page fault and was latched
 * (el2_trap should then return without recording it as a generic fault);
 * 0 if the faulting IPA is not the vector page (el2_trap handles it as
 * usual). */
int firstfault_handle(struct el2_frame *frame);

#endif /* BZDOS_FIRSTFAULT_H */
