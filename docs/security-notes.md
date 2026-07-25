# Security notes: the debug protocol has no authentication (by default)

This started as a clear-eyed writeup of a known, accepted risk, per
ROADMAP.md T5 ("Debug-протокол без аутентификации — любой в LAN может
peek/poke память"). As of 2026-07-25 **both options named below are now
implemented, each behind a compile flag that defaults OFF** — see
"Implementation status" below. Everything above that heading describes the
risk as it stands with NO flag set, which remains this project's default,
unchanged build.

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

Absent either flag (i.e. the project's default build), treat any board
running this hypervisor as fully open to anyone on its LAN segment — the
same trust level as an unlocked physical debug console, just reachable
without a cable.

## Implementation status (2026-07-25)

Both options above are now implemented, board-side, each behind a
DEFAULT-OFF compile flag. **Neither has been exercised on live hardware
yet** — board testing was explicitly deferred for this pass (the board was
in concurrent use for HDMI work); everything below is build-verified only
(`make clean && make dbg [EXTRA_CFLAGS=...]`, `make gdb`, `make qemu`,
`make test` all green, and the default no-flag `make dbg`/`make gdb`
binaries are confirmed **byte-for-byte identical** to before this change —
see the git history for the commit that landed this).

### Option 2 — prod compile-out: `-DPROD_NO_DBG`

The simplest, highest-assurance lockout. Two gates, belt-and-suspenders:

- **`emac.c`, RX-accept edge** (`emac_poll()`'s `ETHERTYPE_CONSOLE` branch):
  under `-DPROD_NO_DBG` the branch drops the frame outright — not one byte
  of it is pushed into the byte ring `dbgmon.c`'s `console_getc()`/
  `gdbstub.c`'s `gdb_getc()` read from. This alone makes both `dbgmon.c`'s
  command dispatch AND `gdbstub.c` (which shares the exact same RX byte
  ring — confirmed by reading `gdbstub.c`'s `gdb_getc()` contract) fully
  unreachable: no bytes ever arrive for either to act on.
- **`dbgmon.c`'s `exec_line()`**: also gated directly (early-return before
  tokenizing/dispatch) as defense in depth, in case some other path ever
  feeds the console ring. This includes the `gdb` hand-off command itself
  (the one dbgmon.c command that would otherwise switch the channel over to
  `gdbstub.c`), so under this flag the RSP hand-off cannot happen either.

Net effect: the board still runs everything else (guest, HDMI, virtio,
netcon, snapshot-net, ...) completely unchanged; only the `dbgmon`/`gdb`
peek/poke/call/step/breakpoint surface on ethertype `0x88B5` is gone. Build:

    make clean && make dbg EXTRA_CFLAGS=-DPROD_NO_DBG

### Option 1 — keyed auth: `-DDBG_AUTH` (+ optional `-DDBG_AUTH_KEY="..."`)

A shared-secret HMAC-SHA256 gate on the same `emac.c` RX-accept edge, so a
console frame is only pushed onto the byte ring (for `dbgmon.c` OR
`gdbstub.c` — same single choke point as above) if it carries a valid MAC
over a strictly-increasing nonce.

**Wire envelope** (the frame payload, i.e. bytes right after the 14-byte
Ethernet header — see `emac.c`'s `dbg_auth_check()` doc comment):

    [0..7]   nonce   8 bytes, big-endian uint64, STRICTLY INCREASING
    [8..39]  mac     32 bytes, HMAC-SHA256(key, nonce || cmd)
    [40..]   cmd     command bytes — same zero-padded-ASCII convention the
                     unauthenticated protocol already used

- **Replay protection**: a single monotonic `g_dbg_auth_last_nonce` in
  `emac.c`, reset to 0 at boot; only nonces strictly greater than the last
  *accepted* one are honored. This is single-client-shaped (matches the
  existing "one operator on the link at a time" model of this protocol) —
  it does not attempt to arbitrate between two host tools signing frames
  concurrently with independently-seeded nonces.
- **HMAC-SHA256 implementation**: `hmac_sha256.c`/`.h`, a compact freestanding
  FIPS 180-4 SHA-256 core + RFC 2104 HMAC wrapper — no libc, no malloc, no
  float, plain fixed-size-buffer arithmetic (same constraints as the rest of
  the tree, `-ffreestanding -mgeneral-regs-only`). Verified against the RFC
  4231 HMAC-SHA256 test vectors on the host (gcc, x86_64) before board-side
  integration — both vectors matched exactly. The entire translation unit is
  wrapped in `#if defined(DBG_AUTH)` so linking `hmac_sha256.o`
  unconditionally into `DBG_OBJS` (Makefile requires new dbg-build objects be
  appended there) is a true no-op — zero bytes added — when the flag isn't set.
- **Key**: `-DDBG_AUTH_KEY="your-shared-secret"` (falls back to an
  obviously-fake default, `CHANGE-ME-bzdOS-dbg-auth-default-key`, only so the
  build-verify matrix compiles without a key — **never ship that default**).

Build (note the escaped inner quotes — `DBG_AUTH_KEY` expands to a C string
literal, so the value itself needs to arrive at the compiler already
quoted):

    make clean && make dbg EXTRA_CFLAGS='-DDBG_AUTH -DDBG_AUTH_KEY=\"your-shared-secret\"'

Host side: `hvdbg.py`'s `HV(key="<hex>")` and `gdb-bridge.py`'s `--key <hex>`
sign every outgoing frame with the matching envelope when a key is passed;
omitting `key`/`--key` (the default) sends byte-for-byte the same
unauthenticated frames as always — neither tool's default code path changed.

**Known limitations / follow-ups** (not board-tested, listed honestly):

- Scoped to the `dbg` (dbgmon) Makefile target's object list (`DBG_OBJS`),
  per this pass's explicit constraint ("add to `DBG_OBJS` only"). The
  standalone `gdb` (gdbstub-only) build target's own object list
  (`GDB_OBJS`) does NOT yet list `hmac_sha256.o`, so
  `make gdb EXTRA_CFLAGS=-DDBG_AUTH` currently fails to link (undefined
  reference to `hmac_sha256`/`hmac_sha256_equal`) — plain `make gdb` (no
  flags, the build-verify requirement for this pass) is unaffected and
  green. Extending `DBG_AUTH` to the `gdb` target only needs appending
  `hmac_sha256.o` to `GDB_OBJS` — left as a follow-up.
- Replay protection is a single global counter, not per-peer — fine for the
  existing single-operator link model, not a general multi-client design.
- Framing/timing has NOT been validated live: the console is serviced from
  a non-blocking tick handler (see `dbgmon.c`'s header comment), and this
  gate now does an HMAC computation on that same path for every accepted
  frame. It's a small, fixed-size computation (one to a few SHA-256 blocks
  per command line) so it's expected to be cheap, but "expected" is not
  "measured on hardware" — that measurement is the deferred board-testing
  step.
- The staged per-frame command buffer in `dbg_auth_check()` is capped at 300
  bytes (covers `gdb-bridge.py`'s 256-byte RSP chunk size and `dbgmon.c`'s
  128-byte line cap with headroom); a command frame carrying more payload
  than that is truncated before hashing, which would only ever cause a
  legitimate oversized frame to fail its own MAC check (fails closed, not
  open) — but it means DBG_AUTH does not currently support arbitrarily large
  single frames.
