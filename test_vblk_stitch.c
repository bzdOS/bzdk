/* SPDX-License-Identifier: BSD-2-Clause */

/* test_vblk_stitch.c — host-side proof of the partial-sector stitch that
 * serve_data() (vblk_emmc.c) still refuses to do.
 *
 * WHY THIS FILE EXISTS BEFORE THE FEATURE DOES.
 * serve_data()'s guard
 *
 *     if (*sector_fill != 0 || (len % VBLK_SECTOR_BYTES) != 0)
 *             return VBLK_RC_UNALIGNED;
 *
 * is a deliberate stub whose comment claims "FreeBSD's virtio_blk hands
 * whole-sector, 512-aligned segments, so this branch should not be exercised".
 * A soak on 2026-07-30 proved otherwise: cycle 2 failed a real guest request
 * with rc=-100 at LBA 922338 (ioerr_unaligned=1), and UFS turns a failed
 * metadata I/O into a dead boot.
 *
 * That branch is also the ONE place in the file that can corrupt the guest's
 * filesystem silently: a wrong offset writes real data to the wrong place and
 * fsck cannot tell. So the arithmetic gets proven HERE, on a fake device where
 * a mistake costs a failed assert instead of a filesystem — and only then gets
 * transcribed into vblk_emmc.c. That inverts the risk of writing it directly
 * into a live, hardware-verified path.
 *
 * WHAT IS MIRRORED, AND WHY BY HAND.
 * Same constraint test_vblk_ring.c documents at length: vblk_emmc.c is threaded
 * with raw ARMv8 asm (dc civac, ldaxr/stlxr, mrs hpfar_el2) that an x86_64
 * assembler rejects outright, so the file cannot be #included. stitch_serve()
 * below is therefore the REFERENCE implementation of the missing behaviour,
 * written to be transcribed verbatim into serve_data()'s aligned+unaligned path
 * (vblk_emmc.c ~line 690-760). It omits only cache maintenance (gmem_cmo:
 * changes cache state, never the bytes) and breadcrumbs (vblk_bc: telemetry).
 *
 * Build: gcc -o test_vblk_stitch test_vblk_stitch.c && ./test_vblk_stitch
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define VBLK_SECTOR_BYTES 512u
#define DEV_SECTORS       64u

/* ------------------------------------------------------------------ *
 * Fake block device + fake guest memory.
 * ------------------------------------------------------------------ */
static uint8_t dev[DEV_SECTORS * VBLK_SECTOR_BYTES];
static uint8_t gmem[64 * 1024];
static uint8_t bounce[VBLK_SECTOR_BYTES];

static unsigned dev_reads, dev_writes;

static int emmc_bio_read_stub(uint32_t lba, uint8_t *dst)
{
	if (lba >= DEV_SECTORS)
		return -1;
	memcpy(dst, dev + (size_t)lba * VBLK_SECTOR_BYTES, VBLK_SECTOR_BYTES);
	dev_reads++;
	return 0;
}

static int emmc_bio_write_stub(uint32_t lba, const uint8_t *src)
{
	if (lba >= DEV_SECTORS)
		return -1;
	memcpy(dev + (size_t)lba * VBLK_SECTOR_BYTES, src, VBLK_SECTOR_BYTES);
	dev_writes++;
	return 0;
}

/* ------------------------------------------------------------------ *
 * REFERENCE stitch — this is what serve_data() should become.
 *
 * Contract (unchanged from the current signature):
 *   gpa/len          one descriptor's guest buffer
 *   *sector          eMMC LBA the descriptor's FIRST byte belongs to; advanced
 *                    by every sector this call fully consumes
 *   *sector_fill     bytes of the CURRENT sector already consumed by previous
 *                    descriptors (0 on a sector boundary); left pointing at the
 *                    partial tail this call ends on
 *
 * A virtio-blk request's data region is a whole number of sectors IN TOTAL, but
 * any single descriptor may start and/or end mid-sector. So each touched sector
 * is read-modify-written through the bounce buffer, and only the overlapping
 * bytes move. A write that does not cover a whole sector MUST read it first, or
 * the untouched bytes are destroyed -- that is the corruption this file exists
 * to rule out.
 * ------------------------------------------------------------------ */
static int stitch_serve(uint32_t is_read, uint32_t gpa, uint32_t len,
                        uint64_t *sector, uint32_t *sector_fill)
{
	uint32_t done = 0;

	while (done < len) {
		uint32_t off = *sector_fill;                 /* offset into sector */
		uint32_t chunk = VBLK_SECTOR_BYTES - off;    /* room left in it    */
		uint32_t lba = (uint32_t)*sector;
		int rc;

		if (chunk > len - done)
			chunk = len - done;

		if (is_read) {
			rc = emmc_bio_read_stub(lba, bounce);
			if (rc != 0)
				return rc;
			memcpy(gmem + gpa + done, bounce + off, chunk);
		} else {
			/* Partial sector => read-modify-write, or the bytes outside
			 * [off, off+chunk) are lost. A full-sector write skips the read. */
			if (off != 0 || chunk != VBLK_SECTOR_BYTES) {
				rc = emmc_bio_read_stub(lba, bounce);
				if (rc != 0)
					return rc;
			}
			memcpy(bounce + off, gmem + gpa + done, chunk);
			rc = emmc_bio_write_stub(lba, bounce);
			if (rc != 0)
				return rc;
		}

		done += chunk;
		if (off + chunk == VBLK_SECTOR_BYTES) {
			*sector += 1;             /* sector fully consumed */
			*sector_fill = 0;
		} else {
			*sector_fill = off + chunk;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * Harness
 * ------------------------------------------------------------------ */
static int failures;

static void check(int cond, const char *what)
{
	if (cond) {
		printf("  ok    %s\n", what);
	} else {
		printf("  FAIL  %s\n", what);
		failures++;
	}
}

static void fill_dev_pattern(void)
{
	for (size_t i = 0; i < sizeof dev; i++)
		dev[i] = (uint8_t)(i * 31u + 7u);
}

static void fill_gmem_pattern(void)
{
	for (size_t i = 0; i < sizeof gmem; i++)
		gmem[i] = (uint8_t)(i * 17u + 3u);
}

/* Whole-sector case: must behave exactly like the code already in the tree, so
 * the stitch cannot regress the path that works today. */
static void t_aligned_read(void)
{
	uint64_t sec = 4; uint32_t fill = 0;
	printf("aligned read, 3 sectors\n");
	fill_dev_pattern(); memset(gmem, 0, sizeof gmem);
	dev_reads = 0;
	int rc = stitch_serve(1, 1024, 3 * VBLK_SECTOR_BYTES, &sec, &fill);
	check(rc == 0, "rc == 0");
	check(sec == 7, "sector advanced by 3");
	check(fill == 0, "no partial tail");
	check(dev_reads == 3, "exactly 3 device reads");
	check(memcmp(gmem + 1024, dev + 4 * VBLK_SECTOR_BYTES,
	             3 * VBLK_SECTOR_BYTES) == 0, "bytes match the device");
}

/* The case that fails on hardware today (rc=-100): a descriptor whose length is
 * not a multiple of the sector size. */
static void t_unaligned_read_tail(void)
{
	uint64_t sec = 2; uint32_t fill = 0;
	printf("unaligned read: 700 bytes (1 sector + 188)\n");
	fill_dev_pattern(); memset(gmem, 0, sizeof gmem);
	int rc = stitch_serve(1, 0, 700, &sec, &fill);
	check(rc == 0, "rc == 0 (today's code returns -100 here)");
	check(sec == 3, "one whole sector consumed");
	check(fill == 188, "188-byte tail carried");
	check(memcmp(gmem, dev + 2 * VBLK_SECTOR_BYTES, 700) == 0,
	      "700 bytes match the device");
}

/* The other half: a descriptor that STARTS mid-sector because a previous one
 * left a carry (*sector_fill != 0), which today's guard also refuses. */
static void t_carry_across_descriptors(void)
{
	uint64_t sec = 10; uint32_t fill = 0;
	printf("two descriptors stitched across one sector boundary\n");
	fill_dev_pattern(); memset(gmem, 0, sizeof gmem);
	int rc = stitch_serve(1, 0, 300, &sec, &fill);
	check(rc == 0 && fill == 300 && sec == 10, "first 300 bytes, no advance");
	rc = stitch_serve(1, 300, 212 + 512, &sec, &fill);
	check(rc == 0, "second descriptor rc == 0");
	check(sec == 12 && fill == 0, "two sectors consumed, no tail");
	check(memcmp(gmem, dev + 10 * VBLK_SECTOR_BYTES, 1024) == 0,
	      "reassembled 1024 bytes are contiguous and correct");
}

/* The corruption case. A partial write MUST preserve the bytes it does not
 * cover; getting this wrong is silent and fsck cannot see it. */
static void t_partial_write_preserves(void)
{
	uint64_t sec = 20; uint32_t fill = 100;
	uint8_t before[VBLK_SECTOR_BYTES];
	printf("partial write preserves the untouched bytes\n");
	fill_dev_pattern(); fill_gmem_pattern();
	memcpy(before, dev + 20 * VBLK_SECTOR_BYTES, VBLK_SECTOR_BYTES);
	int rc = stitch_serve(0, 2048, 50, &sec, &fill);   /* bytes 100..149 */
	check(rc == 0, "rc == 0");
	check(sec == 20 && fill == 150, "still inside the same sector");
	check(memcmp(dev + 20 * VBLK_SECTOR_BYTES + 100, gmem + 2048, 50) == 0,
	      "the 50 written bytes landed");
	check(memcmp(dev + 20 * VBLK_SECTOR_BYTES, before, 100) == 0,
	      "bytes BEFORE the window untouched");
	check(memcmp(dev + 20 * VBLK_SECTOR_BYTES + 150, before + 150,
	             VBLK_SECTOR_BYTES - 150) == 0,
	      "bytes AFTER the window untouched");
}

/* A full-sector write must NOT pay for a read-modify-write. */
static void t_full_write_skips_read(void)
{
	uint64_t sec = 30; uint32_t fill = 0;
	printf("full-sector write does no read-modify-write\n");
	fill_dev_pattern(); fill_gmem_pattern();
	dev_reads = 0; dev_writes = 0;
	int rc = stitch_serve(0, 4096, 2 * VBLK_SECTOR_BYTES, &sec, &fill);
	check(rc == 0 && sec == 32 && fill == 0, "two sectors written");
	check(dev_reads == 0, "no reads issued");
	check(dev_writes == 2, "exactly 2 writes");
	check(memcmp(dev + 30 * VBLK_SECTOR_BYTES, gmem + 4096,
	             2 * VBLK_SECTOR_BYTES) == 0, "device matches the source");
}

/* A device error must propagate unchanged, not be swallowed into a short
 * transfer -- the guest has to see the failure. */
static void t_error_propagates(void)
{
	uint64_t sec = DEV_SECTORS - 1; uint32_t fill = 0;
	printf("device error propagates\n");
	int rc = stitch_serve(1, 0, 2 * VBLK_SECTOR_BYTES, &sec, &fill);
	check(rc == -1, "rc == -1 from the stub's out-of-range sector");
}

int main(void)
{
	printf("test_vblk_stitch: reference partial-sector stitch\n\n");
	t_aligned_read();
	t_unaligned_read_tail();
	t_carry_across_descriptors();
	t_partial_write_preserves();
	t_full_write_skips_read();
	t_error_propagates();
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
	       failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
