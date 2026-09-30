/* scrub.c -- CRC scrubber for the hypervisor's .text/.rodata. See scrub.h. */
#include "scrub.h"

extern char _text_start[], _rodata_end[];

struct scrub_table g_scrub_table
	__attribute__((section(".data.scrub_table"), used)) = { 0 };

static uint8_t g_scrub_golden[SCRUB_MAX_BYTES] __attribute__((aligned(64)));
/* Chunks whose golden copy did not match the build table at boot. */
static uint8_t g_scrub_golden_bad[SCRUB_CHUNKS_MAX];

volatile uint32_t g_scrub_on = 1;          /* scan at all                    */
volatile uint32_t g_scrub_repair_on = 1;   /* repair, or only count          */
volatile uint32_t g_scrub_state;           /* SCRUB_ST_*                     */
volatile uint32_t g_scrub_hw_crc;          /* 1: crc32x, 0: bitwise fallback */
volatile uint32_t g_scrub_boot_bad;        /* chunks wrong as loaded         */
volatile uint32_t g_scrub_passes;          /* full passes completed          */
volatile uint32_t g_scrub_mismatch;        /* chunk checks that failed       */
volatile uint32_t g_scrub_repairs;         /* chunks repaired and re-verified*/
volatile uint32_t g_scrub_words_fixed;     /* words rewritten by repairs     */
volatile uint32_t g_scrub_unrepairable;    /* failed chunk, no good golden   */
volatile uint32_t g_scrub_accepts;         /* scrub_accept() rebases         */
volatile uint64_t g_scrub_last_bad;        /* PA of the last differing word  */

static uint32_t scrub_next;

static uint32_t crc32_soft(uint32_t crc, const uint8_t *p, uint32_t len)
{
	uint32_t i, k;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc;
}

uint32_t scrub_crc32(const void *p, uint32_t len)
{
	const uint8_t *b = (const uint8_t *)p;
	uint32_t crc = 0xFFFFFFFFu;

	if (g_scrub_hw_crc) {
		while (len >= 8u) {
			uint64_t v = *(const uint64_t *)(const void *)b;
			__asm__ (".arch_extension crc\n\tcrc32x %w0, %w0, %x1"
			         : "+r"(crc) : "r"(v));
			b += 8;
			len -= 8u;
		}
	}
	return ~crc32_soft(crc, b, len);
}

static uint32_t chunk_len(uint32_t i)
{
	uint32_t off = i * SCRUB_CHUNK, len = g_scrub_table.len;

	return len - off < SCRUB_CHUNK ? len - off : SCRUB_CHUNK;
}

void scrub_init(void)
{
	uint32_t len = (uint32_t)(_rodata_end - _text_start), i, n;
	uint64_t isar0;

	__asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
	g_scrub_hw_crc = ((isar0 >> 16) & 0xFu) != 0u;

	if (len > SCRUB_MAX_BYTES) {
		g_scrub_state = SCRUB_ST_TOO_BIG;
		return;
	}
	for (i = 0; i < len; i++)
		g_scrub_golden[i] = (uint8_t)_text_start[i];
	n = (len + SCRUB_CHUNK - 1u) / SCRUB_CHUNK;

	if (g_scrub_table.magic == SCRUB_TABLE_MAGIC &&
	    g_scrub_table.base == (uint32_t)(uintptr_t)_text_start &&
	    g_scrub_table.len == len && g_scrub_table.chunks == n) {
		for (i = 0; i < n; i++) {
			if (scrub_crc32(g_scrub_golden + i * SCRUB_CHUNK,
			                chunk_len(i)) != g_scrub_table.crc[i]) {
				g_scrub_golden_bad[i] = 1;
				g_scrub_boot_bad++;
			}
		}
		g_scrub_state = SCRUB_ST_BUILD;
	} else {
		/* No build table (not patched, or a different link): the image as
		 * loaded is the reference. Still catches corruption after boot. */
		g_scrub_table.base = (uint32_t)(uintptr_t)_text_start;
		g_scrub_table.len = len;
		g_scrub_table.chunks = n;
		for (i = 0; i < n; i++)
			g_scrub_table.crc[i] = scrub_crc32(
			    g_scrub_golden + i * SCRUB_CHUNK, chunk_len(i));
		g_scrub_state = SCRUB_ST_BOOT;
	}
	__asm__ volatile("dsb ish" ::: "memory");
}

/* Rewrite only the words that differ, then make them visible to every
 * core's instruction fetch: clean to PoU, invalidate the I-cache line (IC
 * IVAU is broadcast across the inner-shareable domain), barrier. A core
 * executing this chunk meanwhile sees either the corrupt or the good word,
 * never a third value; the corrupt one is what it had anyway. */
static uint32_t repair_chunk(uint32_t i)
{
	volatile uint32_t *live = (volatile uint32_t *)
	    (void *)(_text_start + i * SCRUB_CHUNK);
	const uint32_t *gold = (const uint32_t *)(const void *)
	    (g_scrub_golden + i * SCRUB_CHUNK);
	uint32_t w, fixed = 0, nw = chunk_len(i) / 4u;

	for (w = 0; w < nw; w++) {
		if (live[w] != gold[w]) {
			g_scrub_last_bad = (uint64_t)(uintptr_t)&live[w];
			live[w] = gold[w];
			__asm__ volatile("dc cvau, %0\n\tdsb ish\n\tic ivau, %0"
			                 :: "r"(&live[w]) : "memory");
			fixed++;
		}
	}
	__asm__ volatile("dsb ish\n\tisb" ::: "memory");
	return fixed;
}

void scrub_tick(void)
{
	uint32_t i;

	if (!g_scrub_on || (g_scrub_state != SCRUB_ST_BUILD &&
	                    g_scrub_state != SCRUB_ST_BOOT))
		return;
	i = scrub_next;
	if (scrub_crc32(_text_start + i * SCRUB_CHUNK, chunk_len(i)) !=
	    g_scrub_table.crc[i]) {
		g_scrub_mismatch++;
		if (g_scrub_golden_bad[i]) {
			g_scrub_unrepairable++;
		} else if (g_scrub_repair_on) {
			g_scrub_words_fixed += repair_chunk(i);
			if (scrub_crc32(_text_start + i * SCRUB_CHUNK, chunk_len(i)) ==
			    g_scrub_table.crc[i])
				g_scrub_repairs++;
			else
				g_scrub_unrepairable++;
		}
	}
	if (++i >= g_scrub_table.chunks) {
		i = 0;
		g_scrub_passes++;
	}
	scrub_next = i;
}

void scrub_accept(uint64_t pa)
{
	uint64_t base = (uint64_t)(uintptr_t)_text_start;
	uint32_t off, i;

	if (g_scrub_state != SCRUB_ST_BUILD && g_scrub_state != SCRUB_ST_BOOT)
		return;
	if (pa < base || pa + 4u > base + g_scrub_table.len)
		return;
	off = (uint32_t)(pa - base) & ~3u;
	i = off / SCRUB_CHUNK;
	*(uint32_t *)(void *)(g_scrub_golden + off) =
	    *(volatile uint32_t *)(void *)(_text_start + off);
	g_scrub_table.crc[i] = scrub_crc32(_text_start + i * SCRUB_CHUNK,
	                                   chunk_len(i));
	g_scrub_golden_bad[i] = 0;
	g_scrub_accepts++;
}

uint32_t scrub_image_id(void)
{
	if (g_scrub_table.magic != SCRUB_TABLE_MAGIC ||
	    g_scrub_table.chunks > SCRUB_CHUNKS_MAX)
		return 0;
	return scrub_crc32(g_scrub_table.crc, g_scrub_table.chunks * 4u);
}
