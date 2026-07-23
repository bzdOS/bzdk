# Security notes: the debug protocol has no authentication

This is a clear-eyed writeup of a known, accepted risk — not a fix. It exists
to make the risk precise and discoverable, per ROADMAP.md T5 ("Debug-протокол
без аутентификации — любой в LAN может peek/poke память"). No code changes
were made as part of writing this file.

## What the protocol actually is

`emac.c`'s header comment describes it plainly: a from-scratch raw-Ethernet
console, no IP stack, at **ethertype `0x88B5`** (`ETHERTYPE_CONSOLE` in
`emac.c`). `dbgmon.c` services it as a tiny non-blocking, line-oriented
command console running in the EL2 tick handler. The host-side client is
`hvdbg.py`: it opens a raw `AF_PACKET` socket bound to that ethertype on a
given interface (`br0` by default) and sends broadcast-destination frames —
see `hvdbg.py`'s own module docstring and its `_send()` (broadcast dst MAC,
plaintext command line, no envelope beyond the Ethernet header).

There is:

- **No authentication.** Any frame with ethertype `0x88B5` reaching the
  hypervisor's EMAC RX path is serviced as a command, from whoever sent it.
- **No encryption.** Commands and responses are plaintext ASCII on the wire.
- **No source-address checking.** The protocol is framed as broadcast
  (`BCAST_MAC`) by convention, not restricted to a specific host — anything
  on the same L2 broadcast domain that sends a well-formed frame is treated
  identically to the legitimate operator.
- **A second sibling channel** at ethertype `0x88B6` (`ETHERTYPE_NETCON`,
  the `netcon.c` reliable-datagram channel) shares the same "any L2 peer can
  send" trust model.

## What an attacker on the same LAN segment could actually do

This is not a narrow information leak — the command set gives near-total
control of the hypervisor:

- **Arbitrary physical memory read/write** — `dbgmon.c`'s `r`/`rb` (read
  words/bytes) and `w`/`wb` (write word/byte) commands take a raw physical
  address with **no bounds or permission checking at all**
  (`cmd_read_words`/`cmd_write_word` in `dbgmon.c` dereference the address
  directly). This includes the guest's entire memory (stage-2 today is an
  identity map covering all of guest DRAM — see `stage2.h`'s own isolation
  caveat) and the hypervisor's own code, data, and breadcrumb/ring buffers.
- **Arbitrary hypervisor function invocation** — the `call` command
  (`cmd_call` in `dbgmon.c`) invokes any 4-argument function whose address
  falls inside the HV's own `.text` section (validated against
  `_text_start`/`_text_end` from `link.ld` — this one check *is* present).
  In practice that still means calling any hypervisor-internal routine —
  `musb_init`, `vconsole_dump`, `gic_timer_init`, register pokes via `sw`,
  etc. — with attacker-chosen arguments. Combined with unrestricted memory
  write, an attacker can stage arbitrary data in DRAM and then call a
  function that acts on it.
- **Guest control and DoS** — commands exist to set/clear hardware
  breakpoints and watchpoints, single-step the guest, dump/alter guest
  registers, inject bytes into the guest's virtual console RX (`vconsole_rx_push`,
  used by the built-in `poweroff` command), and force a watchdog reset. An
  attacker can freeze, corrupt, or repeatedly reboot the guest at will.
- **No board-presence proof needed.** Because this is a raw L2 broadcast
  protocol, anyone on the same LAN segment/VLAN/switch as the board — not
  just someone with physical access — has all of the above. A compromised
  or malicious host on the same network segment is equivalent to an
  operator standing at the board's own debug console.

## Scope of the exposure

- **Blast radius:** the whole hypervisor and the whole guest VM. There is no
  privilege separation inside the debug protocol itself — every command is
  available to every sender.
- **Network scope:** limited to whatever L2 broadcast domain the board's
  EMAC is bridged into (`br0` on the host side, per `hvdbg.py`/`chimpd.py`).
  Not routable/reachable from outside that segment without a bridging
  misconfiguration elsewhere (e.g. a switch mirroring the broadcast domain,
  or the host's `br0` including an untrusted uplink) — this document does
  not evaluate any particular deployment's network topology, only the
  protocol itself.
- **Persistence:** none of this requires the attacker to have ever touched
  the board; it's purely a network-reachability condition.

## Future work (named in ROADMAP.md, not decided or implemented here)

ROADMAP.md T5 names exactly two options; both remain open project-owner
decisions, not something this pass implements:

1. **Add authentication to the protocol** — e.g. an HMAC over each frame
   with a pre-shared key, replay protection (sequence numbers/nonces), and
   probably scoping down the unrestricted `r`/`w`/`call` primitives with
   some notion of allowed ranges. This is a real protocol-design change
   that needs live board testing to get the framing/timing right (the
   console is serviced from a tick handler with no blocking I/O) — not
   something to bolt on without hardware validation.
2. **Compile it out entirely for a "prod" build** — i.e. a build target
   that omits `dbgmon.c`/`emac.c`'s console path (or the whole EMAC debug
   plane) so the LAN-facing surface doesn't exist at all outside a
   debug/dev image. This trades away the project's headline feature (debug
   a fallen guest over the network without physical access) for the
   hardened build, so it's a product-shape decision, not a pure
   engineering one.

Until one of these lands, treat any board running this hypervisor as fully
open to anyone on its LAN segment — the same trust level as an unlocked
physical debug console, just reachable without a cable.
