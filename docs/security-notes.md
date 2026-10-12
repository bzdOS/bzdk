# Security notes: the hypervisor's LAN-facing debug surfaces

Since 2026-10-12 the board image (`make dbg` / `make hv-uimage`) only acts on
debug frames that carry a valid HMAC-SHA256 session envelope. Before that the
whole debug plane was open to anyone on the board's L2 segment; that history
and the threat it describes are kept at the end of this file because they
still describe an image built with `DBG_NO_AUTH=1`.

## What is exposed, and to whom

The hypervisor owns four ethertypes on the EMAC:

| ethertype | channel | what a sender can do |
|---|---|---|
| `0x88B5` | dbgmon console / gdbstub RSP (`dbgmon.c`, `gdbstub.c`) | read/write any physical address (`r`/`w`), call any HV function (`call`), patch code, set breakpoints, reset the board |
| `0x88B6` | netcon (`netcon.c`) | land bytes at an operator-chosen address during `netcon_recv()` |
| `0x88B7` | dbgtools (`dbgtools.c`) | read CPU1 heartbeat, hold/release flags and the build id |
| `0x88B8` | snapnet (`snapshot_net.c`) | write guest RAM during a snapshot restore |

Everything on all four is now authenticated (below). The HV also transmits
on `0x88B7` (coredump stream, `coredump.c`) and `0x88B9` (fbdump,
`fbdump.c`); those are output only.

## The envelope (`dbgauth.c`, `dbgauth.h`, host side `hvauth.py`)

All host-to-HV debug traffic now travels on `0x88B5`, inside one envelope
that names the channel in its authenticated header; after verification
`emac.c`'s `hv_rx_frame()` dispatches it to the console ring, netcon,
dbgtools or snapnet exactly as the bare ethertypes used to. Payload layout
(after the 14-byte Ethernet header, integers big-endian):

    HELLO host->HV  00 'B' 'Z' 'K' | 01 | 00   | Nh[16]
    CHAL  HV->host  00 'B' 'Z' 'K' | 02 | 00   | sess[4] | Sb[16] | Nh[16] | proof[16]
    DATA  host->HV  00 'B' 'Z' 'K' | 03 | chan | sess[4] | ctr[8] | len[2] | tag[16] | inner[len]
    NAK   HV->host  00 'B' 'Z' 'K' | 04 | chan | sess[4]

    Ks    = HMAC-SHA256(K, "BZK1/sess" | Sb | sess | Nh)
    proof = HMAC-SHA256(K, "BZK1/chal" | Sb | sess | Nh)[:16]
    tag   = HMAC-SHA256(Ks, payload[0..19] | inner)[:16]

- `K` is the shared key (see "The key" below). `Nh` is a fresh random host
  nonce per handshake.
- `Sb` is a **per-boot board nonce**. The HV derives it, keyed with `K`, on
  the first HELLO of a boot from an entropy pool that has absorbed the
  arrival time (CNTPCT, 41.7 ns resolution) of every frame received since
  boot plus the duration of the PHY autoneg at init. There is no hardware RNG
  in use; what matters here is that `Sb` does not repeat across boots, and
  the arrival time of the first HELLO alone varies by far more than the
  tick. Session numbers also start at a per-boot random value.
- `ctr` must strictly increase within a session. Up to 16 sessions are live
  at once (several host tools run concurrently: triage, the console
  followers, gdb-bridge); a 17th HELLO evicts the least recently used.
- **Replay across reboots** fails because a frame from an earlier boot names
  a session (and so a key derived from an earlier `Sb`) that this boot never
  issued. **Replay within a boot** fails on `ctr`. A replayed HELLO creates a
  new session with a new key, so it does not reopen an old one. Nothing is
  persisted, and nothing resets to a predictable state at boot -- the old
  `DBG_AUTH` counter that restarted at 0 on every boot is gone.
- A DATA frame naming an unknown session or carrying a bad tag is answered
  with a NAK to its source MAC (at most one per 20 ms); `hvauth.py` then
  re-handshakes on its next frame. That is how long-lived host tools (the
  `bzdctl.py console --follow` followers, gdb-bridge) survive an HV reload.
- The leading `0x00` keeps every envelope invisible to the host's text
  console readers, which cut each frame at the first NUL.
- Counters: dbgmon `auth` prints the mode (`strict`/`compat`/`off`) and
  `ok hello badtag replay nosess malformed legacy_ok legacy_drop naks evict
  verify_ticks verify_len`; `hvauth.board_stats(hv)` parses it.

What the envelope does **not** do: it does not encrypt (commands and replies
are readable on the wire) and the HV's replies are not authenticated (a LAN
host can forge console output or NAKs -- a nuisance, not a way in). It does
not rate-limit HELLOs; a LAN host can churn the session table and force the
real tools to re-handshake (denial of service only).

### Build modes (Makefile)

    make hv-uimage                      # DEFAULT: auth only ("strict")
    make hv-uimage DBG_AUTH_COMPAT=1    # transition: auth + legacy, legacy counted
    make dbg DBG_NO_AUTH=1              # explicit opt-out (QEMU/CI): legacy, open
    make dbg EXTRA_CFLAGS=-DPROD_NO_DBG # nothing accepted on any HV ethertype

In strict mode an unauthenticated frame on `0x88B5`..`0x88B8` is counted
(`legacy_drop`) and dropped; `0x88B6`..`0x88B8` stop being HV channels on
receive and are handed to the guest like any other ethertype (`0x88B7` and
`0x88B8` are public IEEE assignments -- OUI-extended and IEC 61850 GOOSE --
that the HV used to swallow). Only the `dbg` target gets auth by default;
`gdb`, `repl`, `dual` and the other images still speak the legacy protocol.

### The key

`$(DBG_AUTH_KEY_FILE)`, default `~/.config/bzdk/dbg.key`: 16..64 random bytes
as hex, mode 600, never committed. Create it with

    mkdir -p ~/.config/bzdk && umask 077 && \
      python3 -c "import secrets; print(secrets.token_hex(32))" > ~/.config/bzdk/dbg.key

The build renders it into the untracked, gitignored `dbg_auth_key.h` (never
onto a compiler command line, so it is not in `.flagstamp` or `ps`); a
missing key file stops the build with an error naming the opt-out. Host
tools read the same file through `hvauth.py` (`$BZDK_DBG_KEY_FILE`
overrides the path, `HV(key=...)` / `gdb-bridge.py --key` override the
file). Rotating the key = new file, rebuild, redeploy; the host tools pick
it up on their next start.

**The key is in the HV image.** `microkernel-dbg.uimg` is served by
`in.tftpd -s /opt/bzdos/tftpboot` to anyone who can reach the host's TFTP
port, and the binary contains the key, so anyone on the LAN who fetches the
image can extract it. The envelope protects against LAN peers only once the
TFTP server is restricted to the board (host firewall rule for udp/69 from
the board's U-Boot address only) or the image is delivered another way.
This is the most important open item.

### Host tools

Every host tool that sends to the HV goes through `hvauth.Channel`:
`hvdbg.py` (and through it `triage.py`, `orient.py`, `bzdctl.py`,
`bmc_client.py`, `safemode.py`, `screenshot.py`, `reliable_load.py`,
`vetrace.py`), `gdb-bridge.py` (and `gdbstub_client.py`), `rsp_bridge.py`,
`chimpd.py`'s `NetCon`, `netcon.py`, `snapshot_net.py`. With the key file
present a tool authenticates; if the board does not answer the handshake at
all (an older image) the tool falls back to legacy frames with a warning,
which an auth-only image ignores, so a rollback to an older image stays
debuggable. `BZDK_DBG_AUTH=require` turns the fallback off, and
`BZDK_DBG_NOAUTH=1` forces legacy frames (for a `DBG_NO_AUTH=1` image).
`push.py`, `hotload.py`, `repl-client.py` and `repl-cmd.py` talk to the
`repl` image, which is not authenticated, and are unchanged.
`gdb-bridge.py` now listens on 127.0.0.1 by default (it used to listen on
0.0.0.0, which would have handed the authenticated channel to the LAN).

### Addresses: the HV and the guest no longer share a MAC

Until 2026-10-12 the HV and the guest both used `02:bd:05:00:00:01`, so a
host could tell a guest frame from an HV frame only by ethertype. Now the
HV's MAC is derived from the SoC SID (the same rule U-Boot uses for eth0's
`ethaddr`) and the guest gets a different one (octet 0 | 0x10) through
`VIRTIO_NET_F_MAC`; dbgmon `mac` prints both. vnet drops a guest frame whose
source is the HV's MAC (like the HV ethertypes), and a non-debug frame sent
to the HV's MAC no longer reaches the guest. Host tools take the HV's MAC
from the source address of the handshake answer that carried a valid proof
(`hvauth.Channel.handshake()`), record it in `~/.config/bzdk/board.mac` for
listeners that start before any handshake, and accept the legacy address
too so an older image stays debuggable (`bzd_board.board_mac()` /
`is_board_mac()`; `$BZDK_BOARD_MAC` overrides).

### Replies: unicast instead of broadcast

Everything the HV originates -- console text, netcon/dbgtools/snapnet
replies, coredump and fbdump streams -- goes to the source MAC of the last
accepted debug frame (`emac.c` `reply_dst()`), so a session's output is no
longer shown to the whole segment. HELLO answers and NAKs go to the
requester. Broadcast remains only until the first accepted frame of a boot:
the boot log, and a panic or coredump before any host has spoken, still
have to reach the operator's console follower, which cannot have announced
itself yet. The dbgtools probe no longer answers unauthenticated requests,
so the build id is not handed to anyone who asks.

### Cost (measured on the board, 2026-10-12)

dbgauth records the CNTPCT ticks its last tag check took (`verify_ticks`,
24 MHz). Minimum of six samples per size:

| inner bytes | 5 | 64 | 128 | 256 | 512 | 1005 |
|---|---|---|---|---|---|---|
| verify, us | 18.6 | 23.6 | 27.0 | 34.2 | 47.5 | 76.0 |

About 18 us fixed plus 0.057 us per byte, so a full snapnet frame (1416
bytes) costs about 100 us on CPU1. At the link's measured inbound ceiling
(~170 KB/s, ~120 such frames per second) that is about 1.2% of CPU1; a
dbgmon command adds ~20 us to a round trip of ~70 ms. A session handshake
is one extra round trip per tool start. Not optimised further: the per-
session HMAC pad states could be cached to save about 3 us per frame.

### Verified on hardware (2026-10-12)

Staged deploy: a `DBG_AUTH_COMPAT=1` image first, all host tools switched to
the envelope while `legacy_ok` was watched until it stopped moving (it found
two long-running `bzdctl.py console --follow` processes with the old code,
restarted), then the strict image. On the strict image: triage, `bzdctl.py status`,
orient's HV section, hvdbg read/write, dbgtools peek, gdb-bridge (`$?` ->
`T05`, `$g`, `$D`) all work (`bzdctl.py status` and orient only while the
two console followers are paused: every host tool shares one console, and a
follower's `dbg>` prompt ends another tool's reply drain early -- this
predates the envelope); an unauthenticated frame on each of `0x88B5`..`0x88B8`, broadcast
and unicast, is dropped (`legacy_drop` +1, a `w` it carried did not execute,
no DBGT reply); an authenticated frame on each channel replayed byte for byte
is rejected (`replay` +1, no effect); a forged tag is rejected (`badtag` +1,
NAK); authenticated frames saved on one boot and sent after an HV reload are
rejected (`nosess`); replies carry the host's MAC as destination.
`test_dbgauth` (in `make test`) and `hvauth.py --selftest` check the same
DATA vector from both sides.

## Before 2026-10-12, and any `DBG_NO_AUTH=1` image

The legacy protocol has no authentication, no encryption and no source
checking. Any frame on `0x88B5` reaching the EMAC is serviced as a dbgmon
command, which means arbitrary physical memory read/write (`r`/`rb`/`w`/`wb`
dereference the address with no checks), calling any function in the HV's
`.text` with chosen arguments, breakpoints, single-step, register edits,
console injection into the guest and a watchdog reset -- the whole
hypervisor and the whole guest. `0x88B6` lets a sender land bytes during a
`netcon_recv()`, `0x88B8` writes guest RAM during a snapshot restore, and a
broadcast `0x88B7` probe returned the build id to anyone. All HV output was
broadcast. Treat such a board as an unlocked physical debug console that is
reachable from the whole L2 segment.

The first attempt at option 1 (`-DDBG_AUTH`, 2026-07-25) only gated
`0x88B5`, used a single replay counter that restarted at 0 on every boot (a
frame captured before a reboot replayed after it), compiled in a placeholder
key when none was given, and was never enabled or tested on hardware. It is
replaced by the envelope above.
