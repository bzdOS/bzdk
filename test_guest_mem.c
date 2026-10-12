/* SPDX-License-Identifier: BSD-2-Clause */

/* test_guest_mem.c — hosted unit test for the two guards the emulated devices
 * apply to guest-supplied input, compiled from the REAL headers (no mirror):
 *
 *   guest_mem.h  guest_gpa_ok() / guest_vring_ok() / hv_dram_block_is_hole():
 *                a guest PA range is usable only if it is wholly guest RAM and
 *                touches no HV-owned window; the stage-2 carve uses the same
 *                table.
 *   vnet_emac.h  vnet_tx_is_hv_ethertype(): guest TX frames carrying the HV's
 *                0x88B5..0x88B8, bare or behind one/two VLAN tags, are dropped.
 *
 * Built with the board's feature flags (GUEST_DRAM_2G, VZRAM, HV_HDMI) so the
 * table under test is the one the dbg image carries.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "guest_mem.h"
#include "vnet_emac.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_gpa(void)
{
	/* Ordinary guest RAM, both GiBs. */
	CHECK(guest_gpa_ok(0x40000000UL, 4096));
	CHECK(guest_gpa_ok(0x60000000UL, 65536));
	CHECK(guest_gpa_ok(0x9FFFF000UL, 4096));
	CHECK(guest_gpa_ok(0x41000000UL, 0));
	CHECK(guest_gpa_ok(0x0UL, 0));               /* len 0 touches nothing */

	/* Every HV window the audit named, plus their edges. */
	CHECK(!guest_gpa_ok(0x42000000UL, 1));       /* hv-image */
	CHECK(!guest_gpa_ok(0x421FFFFFUL, 1));
	CHECK(guest_gpa_ok(0x42200000UL, 16));       /* first byte after it */
	CHECK(!guest_gpa_ok(0x41FFFFF0UL, 0x20));    /* straddles its start */
	CHECK(!guest_gpa_ok(0x50000180UL, 4));       /* EMAC TX lock word */
	CHECK(!guest_gpa_ok(0x50022000UL, 0x40));    /* WDEP */
	CHECK(!guest_gpa_ok(0x50021000UL, 0x40));    /* dbgtools */
	CHECK(!guest_gpa_ok(0x50030000UL, 0x50));    /* vnet breadcrumbs */
	CHECK(!guest_gpa_ok(0x50100000UL, 0x9000));  /* EMAC DMA rings/buffers */
	CHECK(!guest_gpa_ok(0x4FFFFFFFUL, 2));       /* straddles hv-scratch start */
	CHECK(guest_gpa_ok(0x50200000UL, 64));
	CHECK(!guest_gpa_ok(0x4D000000UL, 16));      /* HV framebuffer */
	CHECK(!guest_gpa_ok(0x4D7FFFF0UL, 16));
	CHECK(guest_gpa_ok(0x4D800000UL, 16));

	/* Above the guest's /memory: vzram pool and U-Boot (EL2's page tables). */
	CHECK(GUEST_RAM_END == 0xB0000000UL);
	CHECK(!guest_gpa_ok(0xB0000000UL, 1));
	CHECK(!guest_gpa_ok(0xAFFFFFF0UL, 0x20));
	CHECK(guest_gpa_ok(0xAFFFFFF0UL, 0x10));
	CHECK(!guest_gpa_ok(0xBFFF0000UL, 8));

	/* Outside DRAM, MMIO, and wrap-around lengths. */
	CHECK(!guest_gpa_ok(0x01C30000UL, 4));       /* EMAC registers */
	CHECK(!guest_gpa_ok(0x3FFFFFFFUL, 2));
	CHECK(!guest_gpa_ok(0xC0000000UL, 1));
	CHECK(!guest_gpa_ok(0x60000000UL, 0xFFFFFFFFFFFFFFF0ULL));
	CHECK(!guest_gpa_ok(0xFFFFFFFFFFFFF000ULL, 0x2000));
}

static void test_vring(void)
{
	CHECK(guest_vring_ok(0x60000000UL, 0x60001000UL, 0x60002000UL, 256));
	CHECK(!guest_vring_ok(0x60000000UL, 0x60001000UL, 0x60002000UL, 0));
	/* used ring placed on the EMAC DMA ring */
	CHECK(!guest_vring_ok(0x60000000UL, 0x60001000UL, 0x50100000UL, 256));
	/* desc table that runs into hv-image: 256*16 bytes from 0x41FFF800 */
	CHECK(!guest_vring_ok(0x41FFF800UL, 0x60001000UL, 0x60002000UL, 256));
	CHECK(guest_vring_ok(0x41FFF000UL, 0x60001000UL, 0x60002000UL, 256));
}

/* The stage-2 carve: which 2 MiB blocks stage2_build_dram_table() leaves
 * invalid. hv-image 1 + hv-scratch 1 + HV framebuffer 4 + tail
 * (0xB0000000..0xC0000000) 128 = 134, and nothing else. */
static void test_carve(void)
{
	unsigned n = 0;
	for (uint64_t pa = STAGE2_DRAM_BASE; pa < GUEST_STAGE2_END;
	     pa += GUEST_MEM_BLOCK) {
		int hole = hv_dram_block_is_hole(pa);
		n += (unsigned)hole;
		/* A block is carved iff no byte of it is usable guest RAM. */
		int any_ok = 0;
		for (uint64_t p = pa; p < pa + GUEST_MEM_BLOCK; p += 0x1000)
			any_ok |= guest_gpa_ok(p, 0x1000);
		CHECK(hole == !any_ok);
	}
	CHECK(n == 134u);
	CHECK(hv_dram_block_is_hole(0x42000000UL));
	CHECK(hv_dram_block_is_hole(0x50000000UL));
	CHECK(!hv_dram_block_is_hole(0x50200000UL));
	CHECK(hv_dram_block_is_hole(0xB0000000UL));
	CHECK(!hv_dram_block_is_hole(0xAFE00000UL));
}

static uint32_t frame(uint8_t *f, const uint16_t *types, int ntypes)
{
	uint32_t off = 12;
	memset(f, 0xAA, 12);
	for (int i = 0; i < ntypes; i++) {
		f[off] = (uint8_t)(types[i] >> 8);
		f[off + 1] = (uint8_t)types[i];
		off += 2;
		if (i + 1 < ntypes) {           /* TCI after a TPID */
			f[off] = 0x00; f[off + 1] = 0x05;
			off += 2;
		}
	}
	memset(f + off, 0x55, 46);
	return off + 46;
}

static void test_ethertype(void)
{
	uint8_t f[128];
	uint32_t n;

	for (uint16_t t = 0x88B5; t <= 0x88B8; t++) {
		uint16_t bare[] = { t };
		n = frame(f, bare, 1);
		CHECK(vnet_tx_is_hv_ethertype(f, n) == 1);
		uint16_t q[] = { 0x8100, t };
		n = frame(f, q, 2);
		CHECK(vnet_tx_is_hv_ethertype(f, n) == 2);
		uint16_t qinq[] = { 0x88A8, 0x8100, t };
		n = frame(f, qinq, 3);
		CHECK(vnet_tx_is_hv_ethertype(f, n) == 2);
	}
	uint16_t ip[] = { 0x0800 }, arp[] = { 0x0806 }, v6[] = { 0x86DD };
	uint16_t qip[] = { 0x8100, 0x0800 }, b4[] = { 0x88B4 }, b9[] = { 0x88B9 };
	n = frame(f, ip, 1);  CHECK(vnet_tx_is_hv_ethertype(f, n) == 0);
	n = frame(f, arp, 1); CHECK(vnet_tx_is_hv_ethertype(f, n) == 0);
	n = frame(f, v6, 1);  CHECK(vnet_tx_is_hv_ethertype(f, n) == 0);
	n = frame(f, qip, 2); CHECK(vnet_tx_is_hv_ethertype(f, n) == 0);
	n = frame(f, b4, 1);  CHECK(vnet_tx_is_hv_ethertype(f, n) == 0);
	n = frame(f, b9, 1);  CHECK(vnet_tx_is_hv_ethertype(f, n) == 0);

	/* Three tags: dropped, whatever is inside. */
	uint16_t q3[] = { 0x8100, 0x8100, 0x8100, 0x0800 };
	n = frame(f, q3, 4);  CHECK(vnet_tx_is_hv_ethertype(f, n) == 2);
	/* Tagged but truncated before the inner type: dropped. */
	uint16_t q1[] = { 0x8100, 0x0800 };
	frame(f, q1, 2);
	CHECK(vnet_tx_is_hv_ethertype(f, 17) == 2);
	CHECK(vnet_tx_is_hv_ethertype(f, 18) == 0);
	/* Too short for even the outer type: passes to the caller's own check. */
	CHECK(vnet_tx_is_hv_ethertype(f, 13) == 0);
}

int main(void)
{
	test_gpa();
	test_vring();
	test_carve();
	test_ethertype();
	if (fails) {
		printf("test_guest_mem: %d FAILED\n", fails);
		return 1;
	}
	printf("test_guest_mem: all passed\n");
	return 0;
}
