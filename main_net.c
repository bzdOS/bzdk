/* main_net.c — bzdOS microkernel standalone network test main.
 *
 * A SECOND, INDEPENDENT bring-up channel to the MUSB USB console: brings up
 * the sun8i-emac (emac.h/emac.c) and runs a pure polling loop that emits a
 * raw-Ethernet heartbeat and echoes injected bytes. Sniff with:
 *     tcpdump -i <if> -e ether proto 0x88b5 -A
 * and inject commands with a raw AF_PACKET socket (dst = our MAC
 * 02:bd:05:00:00:01 or broadcast, ethertype 0x88B5, ASCII payload).
 *
 * Deliberately shares NOTHING with musb.c: this is the analogue of main.c but
 * over Ethernet. Freestanding: only <stdint.h>, "emac.h", "wdt.h".
 *
 * Watchdog contract (identical to main.c): arm the WDT BEFORE any risky
 * bring-up, pet it in the healthy loop, and if we can't get a link, STOP
 * petting so the ~16 s WDT returns us to a fresh U-Boot (never bricks).
 */
#include <stdint.h>
#include "emac.h"
#include "wdt.h"

/* Loop-counter throttle for the heartbeat (no wall clock available). */
#define HEARTBEAT_PERIOD 3000000u

static void put_udec(uint32_t v)
{
    char buf[10];
    int i = 0;

    if (v == 0) {
        emac_putc('0');
        return;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0)
        emac_putc(buf[--i]);
}

int main(void)
{
    uint32_t loop_counter = 0;
    uint32_t heartbeat_n = 0;
    int rc;

    /* Arm the hardware watchdog BEFORE any risky bring-up — exactly like
     * main.c. From here a hang/fault self-resets the board in ~16 s back to
     * U-Boot '=>'. The poll loop below pets it. */
    wdt_arm();

    rc = emac_init();

    /* No link (bounded autoneg wait inside emac_init timed out)? Give up:
     * stop petting the WDT and spin, so the proven ~16 s reset path returns
     * us to a FRESH U-Boot. Mirrors main.c's ENUM_GIVEUP behaviour. The EMAC
     * breadcrumb at 0x50000100 records stage 99 (BC_STAGE_NOLINK). */
    if (rc != 0 || !emac_link_up()) {
        for (;;) {
            /* intentionally NO wdt_pet() — let the WDT reset us */
        }
    }

    /* Announce ourselves once link is up. */
    emac_puts("BZDOS-NET: emac_init done, link up\r\n");
    emac_flush();

    for (;;) {
        wdt_pet();                   /* healthy loop keeps the reset at bay */
        emac_poll();                 /* service RX + TX completion */

        if (++loop_counter >= HEARTBEAT_PERIOD) {
            loop_counter = 0;
            emac_puts("BZDOS-NET-ALIVE ");
            put_udec(heartbeat_n++);
            emac_puts("\r\n");
            emac_flush();
        }

        /* Echo injected bytes back to the host, non-blocking. */
        int c = emac_getc();
        if (c >= 0) {
            emac_putc(c);
            if (c == 'q' || c == 'Q') {
                emac_puts("BZDOS-NET: returning to U-Boot\r\n");
                emac_flush();
                wdt_disarm();        /* don't reset U-Boot after handing back */
                return 0;
            }
        }
    }
}
