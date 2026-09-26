# Link reachability through the HAL — spec

**Status: design only (2026-09-26). Nothing here is implemented.** Owner's
direction: WiFi (and later BLE, cellular) must count as ways to reach the
board, and must reach the hypervisor through the future bsdOS HAL, not
through ad-hoc code.

## Why

Since 2247d7f the hypervisor's watchdog is petted only while the board is
*reachable*: an EMAC frame, or USB SOFs from a host (the MUSB frame
counter), within `WDT_UNREACH_S` (900 s). Both sources are
hypervisor-native. A guest that is reachable over WiFi is a real lever too:
over it one can `reboot` the guest, and the guest's PSCI reset becomes a
hypervisor reset. Today that lever is invisible to the gate, so a board with
EMAC dark and no USB cable is reset after 15 min even while ssh over WiFi
works.

## Principle

The hypervisor does not learn what WiFi is. It learns one fact per channel
— "someone reached the board over channel N, just now" — and keeps its own
policy (the 900 s window, the reset). What a channel is, whether it is up
and how to prove it belongs to the HAL. So one truth about connectivity
feeds both the UI's status bar and the watchdog.

## Layers

```
 HAL connectivity module (Zig, guest userland)      bsdos/link/<name> on Zenoh -> UI
        |  proof of reach, per channel
        v
 bzhv.ko (guest kernel)   sysctl hw.bzhv.*   (root only)
        |  SMC, SMCCC vendor-hypervisor call (trapped, never reaches EL3)
        v
 hypervisor wdt.c         per-source last-reach stamps -> gate, BMC health
```

### 1. Hypervisor ABI (SMCCC, vendor-specific hypervisor service, owner 6)

Fast calls, SMC64 convention, **conduit SMC** -- the same one the guest's
PSCI already uses: `HCR_EL2.TSC=1` traps every guest `smc` to EL2
(`el2_exc.c`, EC 0x17), where these IDs are dispatched *before*
`psci_guest_filter()` and are never forwarded to EL3. Not HVC: every guest
EC 0x16 is already claimed -- `gtrace_handle_hvc()` treats any HVC as its
EL1-vector trampoline, `onebp` uses its own immediates. EL0 cannot issue
SMC. Function IDs:

| ID | Name | In | Out |
|---|---|---|---|
| `0x8600FF01` | Call UID (SMCCC-mandated) | — | x0..x3 = bzdOS UID (fixed, published here once chosen) |
| `0xC6000000` | `BZHV_VERSION` | — | x0 = `major<<16 \| minor` (1.0) |
| `0xC6000001` | `BZHV_REACH` | x1 = source id, x2 = proof kind | x0 = 0, or `NOT_SUPPORTED` (-1) / `INVALID_PARAMETERS` (-3) |
| `0xC6000002` | `BZHV_LINKS` | — | x1 = seconds since last EMAC reach, x2 = USB, x3 = the oldest guest-reported source still counted; `UINT64_MAX` = never |

Source ids: 0 = EMAC, 1 = USB (both hypervisor-native, the guest may NOT
report them: `INVALID_PARAMETERS`); 16 = WiFi, 17 = BLE, 18 = cellular,
19..31 reserved. Proof kinds: 1 = round trip with the gateway on that
interface, 2 = inbound session from a trusted peer (Zenoh mesh key / ssh),
3 = bonded BLE connection. The hypervisor records the kind only for health
output; it does not grade them.

Hypervisor side: one stamp per source id (the current `wdt_last_reach`
becomes `stamp[src]`), `wdt_reachable()` = any stamp fresh. At most one
accepted report per source per second (cheap either way; it bounds a
misbehaving guest's trap rate). `BZHV_LINKS` and the BMC health record both
show per-source ages, so `bzdctl status` says which channel keeps the board
alive. `wdt_unreach_test` becomes a bitmask of sources to ignore, so each
source can be tested alone.

**Accepted risk:** a guest that lies keeps the watchdog fed. It can already
keep the board busy in many ways; the gate exists to end "alive but nobody
can reach it", not to police the guest. A `board-config.xml` feature can
turn guest-reported sources off entirely.

### 2. Guest kernel: `bzhv.ko`

- Probe: SMCCC version ≥ 1.1, then Call UID must match; otherwise the
  module attaches nothing (so the same guest image runs on bare metal or
  another hypervisor).
- `sysctl hw.bzhv.reach.<wifi|ble|cellular>` — write-only, root, value =
  proof kind; each write is one `BZHV_REACH`.
- `sysctl hw.bzhv.link.{emac,usb}_age` — read-only, from `BZHV_LINKS`, so
  the HAL can show the hypervisor's own channels in the UI too.
- The only place FreeBSD knows about the hypervisor. Uses the kernel's
  existing SMCCC helpers (`dev/psci/smccc.h`: `arm_smccc_invoke_smc`,
  owner constant `SMCCC_VENDOR_HYP_SERVICE_CALLS` = 6), no new conduit.
  Nothing in FreeBSD's SMCCC code probes owner 6 today (checked in the
  guest's kernel tree, 2026-09-26).

### 3. HAL: `connectivity` module

Owns every link: `wlan0` (through wpa_supplicant's control socket), BLE
(through ng_hci once the H4 transport has its bring-up tool, see
[ng-h4-notes](ng-h4-notes.md)), later the modem. Per link it:

- publishes `bsdos/link/<name>` on Zenoh: up/down, SSID/RSSI or peer,
  address, age of the last proof, proof kind — for the UI;
- **reports to `hw.bzhv.reach.<name>` only after a proof**, at most every
  60 s while the proof stays fresh.

What counts as a proof (and what does not):

| Link | Proof | Not a proof |
|---|---|---|
| WiFi | ICMP/ARP round trip with the gateway, **sourced from wlan0's address** (routing may otherwise send it out vtnet0, i.e. over the EMAC) | association, carrier, DHCP lease |
| WiFi / any IP | inbound authenticated session (ssh, Zenoh peer) arriving on that interface | a listening socket |
| BLE | active connection with a bonded peer | advertising, scanning |
| vtnet0 | never reported: its traffic is the EMAC, which the hypervisor already sees first-hand | — |

The rule this table enforces: a "link up" alone must never feed the
watchdog. Otherwise a live guest cut off from the world keeps the board
resident for ever — the CPU1 hole again, one level up.

## Plan

| Step | What | Depends on | Done when |
|---|---|---|---|
| P0 | This spec | — | agreed |
| P1 | Hypervisor: owner-6 dispatch in the EC 0x17 path ahead of the PSCI filter, per-source stamps, `BZHV_LINKS`, health + `bzdctl status` columns, `wdt_unreach_test` as a mask | — | on hardware: with EMAC+USB masked and a test kld reporting WiFi every 60 s the board stays up 30 min; stop reporting → reset at 900 s + ≤16 s; a report of source 0/1 from the guest is refused |
| P2 | `bzhv.ko` + an interim `rc.d/bzreach` shell script (gateway ping sourced from wlan0, then the sysctl) | P1; WiFi association at boot (works today, see memory `wifi-brcmfmac-port-state`) | same test with the real WiFi link instead of the test kld; unplugging the AP → reset after the window |
| P3 | HAL `connectivity` module replaces the script; Zenoh `bsdos/link/*` | P2; the HAL/Zenoh pipeline on the device | UI shows every link with its proof age; the script is deleted |
| P4 | BLE as a source | P3; ng_h4 bring-up tool | bonded phone connected = reachable |
| P5 | Cellular | modem driver | — |

P1 and P2 do not wait for the HAL; the interface below the HAL is fixed from
P1 on, so P3 swaps only the reporter.

## Open questions

- The UID value (four words, fixed forever once a guest ships with it).
- One window for all sources (current plan) or a per-source window (e.g. a
  shorter one for BLE, whose "connected" state is cheaper to fake by accident).
- Whether P1 should also let the guest *read* the reset reason (black box)
  through the same device, so the HAL can show "last reboot: EMAC dark".
