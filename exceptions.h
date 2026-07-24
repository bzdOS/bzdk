/* exceptions.h — EL2 exception vectors / trap frame for the bzdOS microkernel.
 * See exceptions.S for the vector table and the frame-layout contract; the
 * offsets in struct el2_frame MUST match the stores in el2_common. */
#ifndef BZDOS_EXCEPTIONS_H
#define BZDOS_EXCEPTIONS_H

#include <stdint.h>

/* Vector index (which of the 16 architectural entries fired). Low 2 bits are
 * the type; the group (current-SP0 / current-SPx / lower-64 / lower-32) is
 * (kind >> 2). */
enum {
	EL2_KIND_SYNC = 0,   /* synchronous (undef, data/instr abort, HVC, ...) */
	EL2_KIND_IRQ  = 1,
	EL2_KIND_FIQ  = 2,
	EL2_KIND_SERR = 3,
};

/* Must match the byte offsets stored by el2_common in exceptions.S. */
struct el2_frame {
	uint64_t x[31];      /* 0x000: x0..x30 (ends at 0x0f8)      */
	uint64_t kind;       /* 0x0f8: EL2_KIND_* + group<<2        */
	uint64_t elr;        /* 0x100: ELR_EL2  (faulting/return PC) */
	uint64_t spsr;       /* 0x108: SPSR_EL2                      */
	uint64_t esr;        /* 0x110: ESR_EL2  (syndrome)          */
	uint64_t far;        /* 0x118: FAR_EL2  (fault address)     */
	uint64_t sp_at_entry;/* 0x120                               */
};

/* Install / restore our EL2 vector table (implemented in exceptions.S).
 * el2_install() saves U-Boot's VBAR_EL2; el2_uninstall() restores it so a
 * clean `ret` to the U-Boot prompt still works. */
void el2_install(void);
void el2_uninstall(void);

/* C trap handler, called from el2_common with the saved frame. Advancing
 * frame->elr by 4 before returning skips the faulting instruction (used to
 * survive a deliberate fault during self-test). */
void el2_trap(struct el2_frame *frame, unsigned long kind);

#endif /* BZDOS_EXCEPTIONS_H */
