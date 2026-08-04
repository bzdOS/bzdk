.. zephyr:board:: bpi_m64_hv

Overview
********

Zephyr running as an alternate EL1 guest under the bzdOS EL2 hypervisor
(from-scratch, freestanding, on a Banana Pi M64 / Allwinner A64,
Cortex-A53), in place of bzdOS's usual FreeBSD guest. Not real hardware in
the sense Zephyr usually means it (there is no bare-metal boot path here at
all) -- this board describes the hypervisor's real, trap-and-emulated MMIO
map as seen from EL1, not the physical SoC's raw addresses at EL3/EL2.

* GICv2 (GIC-400 w/ virtualization extensions) at the SoC's real addresses
* One ns16550-compatible UART (Allwinner UART0), trap-and-emulated
* No architected timer wired up yet (tickless first bring-up -- see
  Kconfig.defconfig)
* No flash / no XIP -- single RAM region, guest ELF loaded and relocated at
  runtime by the hypervisor (see ../../../LOADING.md)

Hardware
********

Not applicable in the usual sense -- see the file header comment in
``bpi_m64_hv.dts`` for exactly which real Allwinner A64 addresses are used
and why, and ``../../../LOADING.md`` for how an image actually gets onto
the board.

Programming and Debugging
**************************

See ``../../../LOADING.md``.

Running without the board
*************************

This image boots unmodified under the bzdOS hypervisor's QEMU-virt target --
not via ``west build -t run`` (see ``board.cmake``), but::

    ZEPHYR_BASE=<zephyr-4.4.x> ../../../../zephyr-qemu-ci.sh

UART0 works there because the hypervisor trap-emulates it, so its address
being the A64's is irrelevant. What such a run does *not* prove (the GIC,
above all) is spelled out in ``../../../../docs/zephyr-guest.md``.
