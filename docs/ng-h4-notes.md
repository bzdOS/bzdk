# ng_h4: notes for whoever builds and tests this

Written by the agent that wrote the code, without a compiler and without
touching the board. Read `docs/bluetooth-uart-assessment.md` first for the
overall picture (and its bottom line: it recommends a USB Bluetooth dongle
over this path). This file assumes you're proceeding with the UART path
anyway and covers only the new code.

New files, all under `/opt/bzdos/freebsd-src-earlyboot-wt`:

- `sys/netgraph/bluetooth/include/ng_h4.h` -- node type name, hook name,
  control-message cookie/commands (public, mirrors `ng_ubt.h`'s shape).
- `sys/netgraph/bluetooth/drivers/h4/ng_h4_var.h` -- private softc/state.
- `sys/netgraph/bluetooth/drivers/h4/ng_h4.c` -- the node itself (~780
  lines with comments).
- `sys/modules/netgraph/bluetooth/h4/Makefile` -- module Makefile.

Nothing existing was modified. `board-config.xml` and `gen_config.py` were
explicitly off-limits (concurrent edits) and were not touched.

## What it does

A Netgraph node, type `"h4"`, one hook (`"hook"`), meant to sit between an
open UART tty and `ng_hci`'s `"drv"` hook. It attaches to the tty via
`ttyhook_register(9)` -- the modern MPSAFE-tty API `ng_tty.c` uses, not the
old linesw/`TIOCSETD` mechanism the pre-2021-removal `ng_h4` used. On
receive it reassembles the H4 byte stream (indicator byte + type-specific
header + length-prefixed payload) into HCI frames and hands them to its
peer; on transmit it takes frames from its peer (already carrying the H4
indicator byte, exactly as `ng_hci_ulpi.c` already builds them before
sending to a driver's `drv` peer) and queues them for the tty verbatim.

It does **not** do anything BCM43430/AP6212-specific. No GPIO power
sequencing (`shutdown-gpios`/BT_REG_ON, `device-wakeup-gpios`), no LPO
32 kHz clock enable, no firmware (`.hcd` patchram) download, no baud-rate
switch. Per the task, the patchram upload is explicitly out of scope here
-- it's a separate userland piece, described in
`docs/bluetooth-uart-assessment.md` §2-3, that still needs to be written.
The devicetree binding for the chip already exists
(`sun50i-a64-bananapi-m64.dts`, `brcm,bcm43438-bt` under `&uart1`) but no
driver claims it -- this node doesn't either; it's purely a byte-stream
netgraph node that expects the tty (and whatever powers the chip) to
already be usable.

## How to load and attach it (once built)

1. Add `h4` to `sys/modules/netgraph/bluetooth/Makefile`'s `SUBDIR` list
   (not done here -- that file wasn't in the file list and I didn't want
   to touch anything outside the four new files without being asked).
2. `kldload ng_h4` (plus `ng_hci`, `ng_socket`, etc. as usual).
3. Bind the node to an open UART fd:
   - Create the node: `ngctl mkpeer h4: hook whatever` or `ngctl mknod h4
     h4bt0` (h4 has no `newhook`-time requirement beyond the hook being
     named `"hook"`).
   - **This is the part with no existing userland tool.** Binding needs
     an `NGM_H4_NODE_SET_TTY` control message carrying `{int32_t pid;
     int32_t fd;}` (see `ng_h4.h`). `ngctl`'s text `msg` command can't
     build this without a registered `ng_parse_type_t` for the message
     (I did not write one -- see "Known gaps" below). The working
     precedent is `usr.sbin/ppp/tty.c`, which does the equivalent for
     `ng_tty`'s `NGM_TTY_SET_TTY` by calling `NgSendMsg()` directly from
     C with the struct filled in by hand, after opening the tty itself
     (so the fd and the sending process's pid line up). A similar ~30
     line helper (open the UART with the right termios, call
     `NgMkSockNode`/`NgSendMsg` with `NGM_H4_COOKIE`/`NGM_H4_NODE_SET_TTY`
     and `{getpid(), fd}`) is required and does not exist yet.
   - Connect the graph: `ngctl connect h4: hci: hook drv` (creating the
     `hci` node first via `ngctl mkpeer hci: ... ` or similar, per
     `ng_hci(4)`'s usual setup).
4. Whatever opens the UART fd in step 3 must itself have already put the
   tty into the right termios state (baud rate, `CRTSCTS` for hardware
   flow control if the board's RTS/CTS pins are wired, as the PG8/PG9
   binding in the assessment doc suggests they are). ng_h4 does not touch
   termios at all.

## Known-unfinished / out of scope (confirmed, not guessed)

- **Firmware (patchram) upload.** Explicitly excluded per the task. The
  chip will not do anything useful without it -- HCI reset may or may not
  even respond before the vendor `.hcd` is pushed, depending on ROM
  bootloader behavior, which I did not research further.
- **GPIO/clock power sequencing.** No driver claims the DT's
  `brcm,bcm43438-bt` node. Without asserting `shutdown-gpios` (BT_REG_ON)
  and enabling the LPO clock, the radio is almost certainly powered off
  or unclocked and nothing on the UART will happen at all.
- **Baud-rate switch.** The real chip starts at a fixed init baud and
  later needs a vendor HCI command (`0xfc18`) plus a matching termios
  change on the same fd. ng_h4 has no support for "renegotiate my own
  baud mid-flight"; that's userland's job (re-`ioctl(TIOCSETA)` on the
  fd it already handed to the node) and I did not verify that changing
  termios on an fd already inside a live `ttyhook_register()` even works
  cleanly -- plausible, not checked.
- **No `ngctl` text parse-type registration** for any of the four new
  control messages (`SET_TTY`, `SET_DEBUG`, `GET_DEBUG`, `GET_STAT`,
  `RESET_STAT`). They can only be sent as raw binary messages or from a
  small C program using libnetgraph, mirroring `ppp/tty.c`.
- **No man page** (`ng_h4.4`).
- **Single FIFO output queue**, not the three (cmd/ACL/SCO) priority
  queues `ng_ubt.c` keeps. Under load this means a large ACL burst can
  delay a time-sensitive SCO (voice) or command frame behind it. Fine for
  bring-up/HCI-reset-level testing, likely wrong for anything with audio.
- **`th_close` hook not implemented.** Neither is it in `ng_tty.c`. My
  reading of `kern/tty.c`/`kern/tty_ttydisc.c` is that this is safe
  because `tty_gone(tp)` already gates every place `ng_h4` would otherwise
  touch a revoked tty (mirrors `ng_tty.c`'s `ngt_rcvdata`'s
  `!tty_gone(tp)` check) -- but this reasoning was not tested against a
  real "unplug the USB-serial adapter" or "kill -9 the attaching process"
  race on hardware, and `sc->tp` itself is read without a lock in
  `ngh4_rcvdata`/`ngh4_shutdown`, exactly as `ng_tty.c`'s `sc->tp` is.
  Same accepted gap, not newly introduced.
- **`NGM_H4_COOKIE`'s numeric value** (1474837201) was picked to look
  distinct from the handful of other Bluetooth-subsystem cookies in this
  tree; it was not checked against any upstream registry of Netgraph
  message cookies, since none of this is destined for upstream.

## What I'm least sure of -- review these first

1. **The RX chaining logic in `ngh4_rx_putc()`/`ngh4_rx_newframe()`**
   (grows the reassembly mbuf into a chain via `MGET`/`MCLGET` when a
   2048-byte cluster isn't enough, e.g. a large ACL frame). This is the
   most novel part of the file -- there's no existing analogue doing
   *stream* reassembly into a chain byte-by-byte in this tree (`ng_tty.c`
   only ever fills one mbuf per `rint()` call and hands it off unparsed;
   `ng_ubt.c` gets whole USB transfers, no reassembly needed at all). I'm
   fairly confident the mbuf bookkeeping (`m_pkthdr.len` on the head,
   `m_len` per segment, `sc->rx_last` tracking the tail) is right, but
   this is the one piece I'd most want someone to trace through by hand
   or test with an oversized synthetic ACL frame before trusting it.
2. **Locking.** I followed `ng_tty.c`'s pattern as closely as I could
   (single `ifq_mtx` on the outq, `NG_HOOK_FORCE_QUEUE()` on our own hook
   in `connect()`, RX state touched only from tty-locked context) rather
   than inventing anything new. Medium-high confidence this is
   structurally sound, but it is copying an existing pattern's
   assumptions, including its `sc->tp`/`sc->hook` unlocked-read gaps
   noted above -- I did not independently re-derive that those are safe,
   I trusted that `ng_tty.c` already gets away with it.
3. **Resync-on-garbage behavior.** On an unexpected indicator byte, or a
   line error mid-frame, the code drops the byte/aborts the in-progress
   frame and waits for the next byte that looks like a valid indicator.
   I don't know what garbage (if any) actually appears on this UART
   before firmware load, at the wrong baud rate, or during power-up --
   this behavior is a reasonable-looking guess, not something I could
   check against real line noise.
4. **`le16toh()` on the ACL length field.** High confidence (matches
   `ng_ubt.c`'s identical handling of the same field type over USB, and
   HCI's wire format is little-endian throughout), but not independently
   verified against the Core Spec text itself.
5. **Never compiled.** No compiler was run against any of this. Include
   ordering, macro/struct name spellings (`ng_hci_acldata_pkt_t` and
   friends), and `bsd.kmod.mk` mechanics were all checked by reading the
   existing tree, not by building. Expect at least a small syntax/typo
   fix pass on first build attempt.

## What I'm fairly confident about

The overall shape (node type, single hook, `ttyhook_register()` attach,
H4 header parsing/sizes, TX pass-through with the indicator byte already
in place) is a direct, careful application of what
`docs/bluetooth-uart-assessment.md` already established and what
`ng_tty.c`/`ng_ubt.c`/`ng_hci.h` already show. I did not invent a new
packet model or guess at wire formats not already confirmed in that doc.
