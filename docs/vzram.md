# vzram — compressed-RAM swap disk

Status 2026-09-30: hypervisor side done and hardware-verified (`08affdd`);
not swapped on at boot (see "Open" below). Code: `vblk_zram.[ch]`,
`vzram_pool.[ch]`, `lz4.[ch]`; hosted tests `test_vzram_pool`, `test_lz4`.

## What the guest sees

A third virtio-blk disk, `vtbd2` (virtio-mmio `0x0A005000`, SPI 109 /
INTID 141, device-id 2), 128 MiB at boot. FreeBSD attaches it with the
stock `virtio_blk`; nothing guest-side was changed.

## Where it lives

`[0xB0000000, 0xB8000000)` — the top 128 MiB of what `GUEST_DRAM_2G` used to
give the guest (`hv_addrmap.h` `HVMAP_VZRAM_*`). The `vzram` feature in
`board-config.xml` carries `dtb-memory-size="0x70000000"`, and
`gen_config.py` applies the **smallest** enabled `dtb-memory-size`, so the
guest's `/memory` ends exactly at `HVMAP_VZRAM_BASE` (1792 MiB). Stage-2
still maps all 2 GiB in 1 GiB blocks; the guest is simply never told about
the slice. Two compile-time guards: the slice must end at `0xB8000000`
(U-Boot's no-overwrite region starts above, see `stage2.h`), and `VZRAM`
without `GUEST_DRAM_2G` is an error (the slice is snapshot's mirror then).
Descriptors pointing into the slice are refused (`gpa_in_range`), and
scanout refuses it too.

Layout inside the slice: the first 2 MiB are the per-page slot table
(`VZRAM_PAGES_MAX` x 8 bytes), the remaining 126 MiB the LZ4 pool
(slab classes 256..4096, see `vzram_pool.h`).

## Geometry and growth

4 KiB logical pages. Exposed at boot: `VZRAM_PAGES_INITIAL` = 128 MiB —
about what the pool holds uncompressed, so incompressible data cannot
overcommit it. After a write, the disk grows by `VZRAM_PAGES_STEP`
(128 MiB) up to `VZRAM_PAGES_MAX` (1 GiB) when **both** hold:

- at least 3/4 of the exposed pages have been written, and
- the pool's bump pointer is at most half the pool.

Growth bumps `config_gen` and raises the config-change interrupt;
FreeBSD's `vtblk_config_change()` resizes the GEOM disk live (verified:
`diskinfo vtbd2` 128 -> 256 MiB without any guest action). The disk never
shrinks. A write that finds the pool full fails that request (`S_IOERR`,
`g_zr_errs`), leaving the page's previous content intact.

## Concurrency

One lock (`g_zr_lock`) over every MMIO access: request service,
completion, the ISR registers, reset. `lz4.c` and `vzram_pool.c` keep
file-static scratch state and four vCPUs can notify at once. Requests are
served synchronously in the notifying trap — there is no device to wait
for, only CPU work. `InterruptACK` carries vblk_emmc.c's lost-completion
re-notify (`g_zr_irq_rearms`).

## Measured (2026-09-30)

- 100 MiB of compressible text written at 72 MB/s, 32 MiB random at
  33 MB/s, both read back identical; resize 128 -> 256 MiB seen by GEOM.
- As the **only** swap: a 1700 MiB working set against 1523 MiB free RAM,
  every page verified twice, 0 bad, ~70k swap I/Os, `g_zr_errs` = 0.
- Counters, over the debug channel via `nm`: `g_zr_reads/writes/errs/
  grows/used_pages/irq_rearms/faults`.

A trap that cost an hour: a read-only pass over a working set larger than
RAM keeps every swapped-in page's swap slot, so it needs swap as large as
the whole set. The OOM that produced is FreeBSD behaving as designed; the
test program now stores to each page it checks, which frees the slot.

## Open — decisions, not code

1. **Does it pay for itself here?** The owner's framing (HANDOFF
   2026-09-27) treats swap as idle insurance on the eMMC with a usage target
   of 0. vzram costs the guest 128 MiB of RAM permanently to back a swap
   that, by that policy, should never be used. Turning it off is one
   attribute: `vzram enabled="false"` in `board-config.xml`, then
   `gen_config.py` (restores `/memory`) and rebuild.
2. **Coexisting with eMMC swap.** FreeBSD has no swap priorities: with
   both in `fstab` pages interleave across the two. "vzram first, eMMC as
   overflow" cannot be expressed.
3. **Adopting growth.** The swap pager keeps the size it had at `swapon`;
   using a grown disk needs `swapoff`/`swapon`, and `swapoff` under the
   memory pressure that caused the growth has to page everything back in.
   The old devd-rule plan is unsafe as stated; sizing once at boot is the
   likely answer.
