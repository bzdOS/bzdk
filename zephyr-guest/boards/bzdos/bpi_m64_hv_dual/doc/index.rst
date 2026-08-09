.. zephyr:board:: bpi_m64_hv_dual

Overview
********

Zephyr running as a SECOND, genuinely concurrent EL1 guest on CPU3 of the
bzdOS EL2 hypervisor, alongside FreeBSD on CPU0 (the "dual-guest"
milestone) -- see ``../../bpi_m64_hv``'s own ``doc/index.rst`` for the
single-guest version of this board, which this one is derived from.

* Same GICv2 and UART0 as ``bpi_m64_hv`` (trap-and-emulated/absorbed by the
  hypervisor either way)
* No architected timer wired up yet (tickless, same as ``bpi_m64_hv``)
* Single RAM region, but at a DIFFERENT physical address: 0xBE000000, a
  32 MiB slice inside the hypervisor's high-GiB "Zephyr private slice"
  (see ``stage2_zephyr.h`` in the hypervisor tree) rather than
  ``bpi_m64_hv``'s 0x51000000 (inside FreeBSD's own low-DRAM gigabyte,
  which CPU3's stage-2 table deliberately leaves entirely unmapped).

Why a separate board instead of reusing bpi_m64_hv
***************************************************

See ``bpi_m64_hv_dual.dts``'s file header for the full account: a naive
byte-for-byte relocation of the ``bpi_m64_hv`` image to 0xBE000000 at
hypervisor load time was tried first (cheaper, no rebuild) and found to
silently wedge the guest before it ever printed anything -- ``CONFIG_ARM_MMU=y``
means Zephyr builds its own stage-1 page tables against this devicetree's
addresses at build time, and those do not move when the image's bytes are
relocated afterward. This board fixes that at the source.

Running without the board
*************************

Board-free only -- this board has no real-hardware load path at all (unlike
``bpi_m64_hv``, which real hardware does run). See
``../../../../dual-zephyr-qemu-ci.sh``::

    ZEPHYR_BASE=<zephyr-4.4.x> ../../../../dual-zephyr-qemu-ci.sh
