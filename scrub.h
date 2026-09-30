/* scrub.h -- CRC scrubber for the hypervisor's own .text/.rodata.
 *
 * The image's code and constants, [_text_start, _rodata_end), are checked in
 * 4 KiB chunks against a CRC-32 table computed at BUILD time (scrub_crc.py
 * patches g_scrub_table into the linked ELF), one chunk per CPU1 tick. A
 * chunk that no longer matches is repaired, word by word, from a golden copy
 * taken at boot -- but only if that copy's chunk still matches the build
 * table itself. The same table therefore also catches an image that was
 * already wrong when it was loaded (reliable_load's stale-symbol sweep wrote
 * into .text on every reload until 7a1f302): those chunks are counted in
 * g_scrub_boot_bad and cannot be repaired, since nothing on the board holds
 * a good copy of them.
 *
 * Everything is readable over the debug channel with nm + hvdbg (see
 * scrub.py): the counters below, and g_scrub_on / g_scrub_repair_on to
 * switch scanning or repair off live. dbgmon's `patch` command is an
 * intentional write into code, so it calls scrub_accept() to move the golden
 * copy and the table along with it instead of having the patch undone.
 */
#ifndef BZDOS_SCRUB_H
#define BZDOS_SCRUB_H

#include <stdint.h>

#define SCRUB_CHUNK        4096u
#define SCRUB_MAX_BYTES    0x40000u                /* 256 KiB golden copy */
#define SCRUB_CHUNKS_MAX   (SCRUB_MAX_BYTES / SCRUB_CHUNK)
#define SCRUB_TABLE_MAGIC  0x42435243u             /* "CRCB" */

/* Patched post-link by scrub_crc.py. Lives in .data, which is not scanned. */
struct scrub_table {
	uint32_t magic;          /* SCRUB_TABLE_MAGIC once patched          */
	uint32_t base;           /* _text_start                              */
	uint32_t len;            /* _rodata_end - _text_start                */
	uint32_t chunks;         /* ceil(len / SCRUB_CHUNK)                  */
	uint32_t crc[SCRUB_CHUNKS_MAX];   /* zlib.crc32 of each chunk        */
};

/* g_scrub_state */
#define SCRUB_ST_OFF        0u   /* scrub_init() not run                   */
#define SCRUB_ST_BUILD      1u   /* running against the build-time table   */
#define SCRUB_ST_BOOT       2u   /* no build table: baseline taken at boot */
#define SCRUB_ST_TOO_BIG    3u   /* image larger than SCRUB_MAX_BYTES      */

void scrub_init(void);            /* CPU0, before the guest starts          */
void scrub_tick(void);            /* CPU1 tick: one chunk                   */
void scrub_accept(uint64_t pa);   /* an intentional write at pa: rebase it  */

/* CRC-32 (IEEE, reflected; same value as zlib.crc32). */
uint32_t scrub_crc32(const void *p, uint32_t len);

#endif /* BZDOS_SCRUB_H */
