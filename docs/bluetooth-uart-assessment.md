# Bluetooth over UART (BCM43430A1 / AP6212) under FreeBSD: feasibility assessment

Read-only research, done off the board. No hardware touched. Scope: whether
to bring up Bluetooth on the AP6212's UART side (uart1, `serial@1c28400`,
PG6/PG7 + RTS/CTS PG8/PG9) for the FreeBSD 15.1 guest, given the chip's
WiFi (SDIO) side already works.

## 1. The gap, confirmed independently

FreeBSD once had a UART HCI transport: `ng_h4`, implementing H4 (Bluetooth
Spec chapter H4) as a netgraph node tied to a tty line discipline
(`H4DISC`). Gone from every branch checked, deliberately:

- Removal: commit `79a100e28e3c` ("bluetooth: complete removal of ng_h4"),
  author `imp`, landed 2021-11-10, reviewed as
  https://reviews.freebsd.org/D31846. Quoted rationale: the node "was
  disconnected 13 years ago when the tty layer was locked by Ed" (the
  MPSAFE TTY rewrite, ~2008), it "completely fails to compile, and has a
  number of false positives for Giant use," and "Bluetooth has largely
  (completely?) moved on from bluetooth over UART transport."
- Before removal: `sys/netgraph/bluetooth/drivers/h4/ng_h4.c` (~1019 lines,
  13.5-era tree), built on the pre-2008 `linesw` ldisc model, not the
  modern `ttyhook_register()` API `ng_tty(4)` uses today. A rewrite, not a
  resurrection, even as a starting point.
- Confirms the prior finding: no `ng_h4` anywhere in this tree or upstream,
  no third-party FreeBSD port/out-of-tree driver providing UART HCI found.
  `usr.sbin/bluetooth/` (`bcmfw`, `ath3kfw`, `iwmbtfw`, `rtlbtfw`) is
  entirely USB tooling — `rtlbtfw/main.c` calls `libusb_control_transfer`,
  confirming USB, not tty, I/O. `sys/netgraph/bluetooth/drivers/` here
  holds only `ubt`/`ubtbcmfw` (USB). DragonFly/NetBSD/OpenBSD imports not
  checked — could not determine.

FreeBSD 15.1 has no UART Bluetooth transport, full stop.

## 2. What the BCM43430A1 needs (Linux as reference)

Sources: `drivers/bluetooth/hci_bcm.c` and `btbcm.c`, https://github.com/torvalds/linux.

- **Power-up**: `bcm_gpio_set_power()` asserts the shutdown/BT_REG_ON GPIO,
  enables `vbat`/`vddio` regulators and the LPO 32 kHz clock, sleeps
  100–120 ms before touching the UART. This board's DT already has those
  properties — see §3.
- **Initial baud**: link comes up at a fixed default (commonly 115200) via
  `hu->init_speed`/`dev->init_speed`, before firmware loads — a stock UART
  open, not chip-specific.
- **Firmware download (patchram)**: `btbcm_patchram()` sends vendor opcode
  `0xfc2e` ("Download Minidriver"), waits 50 ms, walks the `.hcd` file as
  `hci_command_hdr` records (opcode + `plen` + params), issuing each as an
  HCI command. After "Launch Ram" (250 ms wait), `btbcm_finalize()` re-runs
  `btbcm_initialize()` (reads local version, "verbose config" opcode
  `0xfc79` for chip ID), then `btbcm_check_bdaddr()` rejects a
  factory-default BD_ADDR.
- **Baud switch**: `bcm_set_baudrate()` issues vendor opcode `0xfc18`
  ("Update UART Baud Rate") to reach operational speed, then the host
  matches its own UART. Speed is board-specific — `hci_bcm.c`'s ACPI path
  shows 4 Mbps, AP6212/BCM43430 boards in the wild (Raspberry Pi 3/Zero W)
  commonly run 3 Mbps instead; this module's figure not independently
  verified.
- **Firmware identity**: `btbcm_initialize()` reads local name/version to
  pick the file; for this chip it's `BCM43430A1.hcd` (§4).

Net: GPIO sequencing + fixed init baud + vendor-command firmware push +
baud switch. No crypto handshake — a vendor protocol layered on H4
framing, not H4 itself.

## 3. What would have to be written for FreeBSD

**Device-tree binding — already done, unclaimed.**
`sys/contrib/device-tree/src/arm64/allwinner/sun50i-a64-bananapi-m64.dts`
already has a complete upstream binding under `&uart1` (~line 356): a
`bluetooth` child node, `compatible = "brcm,bcm43438-bt"`, LPO clock
(`&rtc CLK_OSC32K_FANOUT`), `vbat-supply`/`vddio-supply`
(`&reg_dldo2`/`&reg_dldo4`), `device-wakeup-gpios` (PL6),
`host-wakeup-gpios` (PL5), and `shutdown-gpios` (PL4, the BT_REG_ON
equivalent). `grep -rl "bcm43438-bt\|brcm,bcm4343" sys/ --include=*.c`
finds nothing — no FreeBSD driver claims this compatible string. Binding
costs zero; a small new OF/GPIO driver to consume it is under a day of code.

**tty attach — has a working modern analogue.** `sys/netgraph/ng_tty.c`
(506 lines) already attaches a netgraph node to an open tty via
`ttyhook_register()` (the current MPSAFE hook API — not the old `linesw`
table `ng_h4` used), driven from userland via `NGM_TTY_SET_TTY` (consumer:
`usr.sbin/ppp/tty.c`). Right pattern to copy; no `TIOCSETD`/classic-ldisc
plumbing needed.

**H4 framing node — the real new work.** `ng_tty` only moves raw bytes, no
HCI framing. Framing must find command/event/ACL/SCO boundaries in an
arbitrary byte stream — USB needs none of this since each transfer is
already one packet. Useful finding: `sys/netgraph/bluetooth/include/ng_hci.h`
defines `NG_HCI_CMD_PKT`=0x01, `NG_HCI_ACL_DATA_PKT`=0x02,
`NG_HCI_SCO_DATA_PKT`=0x03, `NG_HCI_EVENT_PKT`=0x04 — the same values H4
uses on the wire — and `ng_ubt.c` already prepends them onto mbufs (e.g.
`*mtod(m, uint8_t *) = NG_HCI_ACL_DATA_PKT;`, line 1109) before the HCI
socket layer. The internal ng_hci representation is already H4-shaped: a
new node mostly needs stream reassembly plus flow control, not a new
packet model. `ng_ubt.c` (2059 lines) is the closest HCI-logic sibling but
oversized with USB transfer/endpoint management a UART node doesn't need
— realistic size closer to `ng_tty.c`'s scale.

**Userland firmware loader — right shape, wrong bus.**
`usr.sbin/bluetooth/rtlbtfw/` (~1698 lines total) pushes firmware via
vendor HCI commands but is built entirely on `libusb_control_transfer()`/
`libusb_interrupt_transfer()`. A BCM `.hcd` loader swaps those for
`read()`/`write()` on the HCI socket or raw tty, speaking `0xfc2e`/`0xfc18`
from §2.

**Rough sizing:** DT binding 0 (present, unclaimed) + OF/GPIO driver
~100–200 lines (no analogue exists, but the pattern is generic) + tty
attach ~200 lines (partial analogue: `ng_tty.c`) + H4 framing node
~500–800 lines (no analogue; `ng_ubt.c`/`ng_hci.h` inform it) + firmware
loader ~400–600 lines (wrong-bus analogue: `rtlbtfw`). None of the last
three exist today in any form.

## 4. Firmware provenance and licence

`BCM43430A1.hcd` is proprietary Broadcom/Cypress firmware, distributed via
`linux-firmware` at `firmware/brcm/BCM43430A1.hcd` (e.g.
https://github.com/armbian/firmware/blob/master/brcm/BCM43430A1.hcd).
Marked "Redistributable" in `WHENCE`, deferring to a
`LICENCE.broadcom_bcm43xx` terms file, but with public, unresolved dispute
over what those terms actually permit (see
https://github.com/raspberrypi/linux/issues/1325) — not clean licensing.
Identical open question already exists in this project for this chip's
WiFi side: `docs/guest-hw-enablement.md` notes the SDIO firmware "exists
only in this HOST's own Linux firmware tree (`/usr/lib/firmware/brcm/`) —
copying it into the guest would need its own licensing check, not
evaluated here." Bluetooth firmware lands in the same bucket, same path.

## 5. Effort estimate and recommendation

**Effort**: not a bug fix or config flip. A new kernel netgraph node, a new
small OF/GPIO driver, and a new userland firmware loader, built from a
Linux reference with no FreeBSD starting point (`ng_h4` was obsolete even
before removal) — roughly 1500–2500 lines of new/adapted C, plus real
board time to debug UART framing, vendor-command sequencing, and baud
switching live, none verifiable without hardware. Multi-week effort even
with the analogues above, before "HCI reset succeeds." **Alternative**:
the board has a working EHCI/OHCI USB host port
(`sys/dev/usb/controller/generic_ehci.c`, the OF-attached glue FreeBSD
already uses on Allwinner arm64), and `ubt(4)`
(`sys/netgraph/bluetooth/drivers/ubt/ng_ubt.c`) already supports USB
Bluetooth adapters — no new code, no firmware licence question, no UART
framing to get right blind. A USB Bluetooth dongle is a five-dollar part.

**Recommendation: use a USB dongle. Do not write this.** The UART path
means rebuilding a transport layer FreeBSD deliberately let die, against a
firmware blob whose redistribution terms are already an open question for
this chip's WiFi side, to reach a radio fully replaceable by a part that
already works with zero new code. Revisit only if something specifically
forbids the USB port or demands this exact onboard radio.
