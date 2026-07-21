/* musb.h — MUSB CDC-ACM gadget console API for bzdOS microkernel (Allwinner A64).
 * Contract shared by main.c (caller) and musb.c (impl). See PROJECT.md. */
#ifndef BZDOS_MUSB_H
#define BZDOS_MUSB_H
#include <stdint.h>

/* Bring up MUSB in device (peripheral) mode: EP0 control + EP1 bulk IN/OUT.
 * Assumes U-Boot left the OTG clock enabled and PHY configured; re-asserts the
 * clock gate defensively. Does NOT touch the MMU. */
void musb_init(void);

/* Service USB: process EP0 SETUP/enumeration and EP1-OUT RX. MUST be called as
 * often as possible (tight loop) and MUST advance enumeration on every call,
 * independent of any TX activity — this is the fix for the FreeBSD console's
 * enumeration-only-during-printf bug. Returns musb_ready(). */
int musb_poll(void);

/* 1 once the host issued SET_CONFIGURATION (i.e. /dev/ttyACM* is up). */
int musb_ready(void);

/* Queue one byte for transmission on EP1 IN. NON-BLOCKING: appends to an
 * internal TX ring and returns immediately; if the ring is full (host not
 * draining) the newest byte is dropped. Staged bytes are pushed to the host
 * incrementally by musb_poll() (one bounded max-packet per call), so a caller
 * MUST keep calling musb_poll() for output to actually go out. Safe to call
 * before ready() (bytes are buffered, or dropped once the ring fills). */
void musb_putc(int c);

/* Best-effort: push ONE staged TX packet to the host now if the EP1-IN FIFO is
 * free. NON-BLOCKING and bounded — never spins waiting for the host to drain;
 * any remaining staged bytes go out on subsequent musb_poll() calls. */
void musb_flush(void);

/* Convenience: musb_putc over a NUL-terminated string. */
void musb_puts(const char *s);

/* Return one received byte (0..255) from EP1 OUT, or -1 if none available. */
int musb_getc(void);

#endif /* BZDOS_MUSB_H */
