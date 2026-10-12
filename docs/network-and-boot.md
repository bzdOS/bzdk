# Board network config, routed debug channel, eMMC boot

Since 2026-10-12 the board does not have to share a segment with the host,
and it boots without the host's TFTP.

## One config place

| Where | What | Changed by |
|---|---|---|
| host `~/.config/bzdk/board.conf` | transport (`l2`/`udp`), wire iface, udplink tap, board/guest/host/gateway IPs (board/guest may be `auto`), U-Boot's static IP, DNS, UDP port | `tools/subnet/cutover.py`, by hand |
| U-Boot env (`uboot.env` on the ESP) | `ipaddr netmask gatewayip serverip` -- U-Boot's TFTP fallback and, exported by `bzdk_boot`, the HV's own address; `bzdk_net=dhcp` makes the HV use DHCP instead (ipaddr etc. become its fallback) | `uboot_env.py`, `emmc_boot.py netmode`, `cutover.py` |
| guest `/etc/rc.conf`, `/etc/resolv.conf` | `ifconfig_vtnet0` (static or `"DHCP"`), `defaultrouter`, nameserver | `cutover.py` |
| host `~/.config/bzdk/board.ip`, `guest.ip` | learned addresses for `auto` (below) | `bzd_udplink.py`, `bzd_board.guest_ip()` |

`bzd_board.py` reads board.conf (defaults = the 192.168.88.0/24 setup);
every host tool takes the debug interface (`bzd_board.IFACE`), the guest
address (`bzd_board.guest_ip()`, `GUEST`, `guest_ssh_argv()`), the board
address (`board_ip()`) and U-Boot's own static address (`UBOOT_IP`: the
U-Boot tools -- uboot_nc, uboot_setup, chimpd -- talk to U-Boot, which
never uses DHCP) from there. `tools/subnet/board.conf.example` documents
the keys.

## Debug channel over UDP/IPv4 (`dbgip.c`, `bzd_udplink.py`)

The raw-Ethernet debug plane (0x88B5..0x88B9) does not cross a router. The
HV therefore also owns one IPv4 address -- U-Boot's `ipaddr`, it shares
U-Boot's MAC anyway -- answers ARP and ICMP echo for it, and takes UDP
datagrams to port 34997 whose payload is one whole debug Ethernet frame,
exactly as the wire carries it. That inner frame goes through `dbgauth_rx()`
like any other (only the authenticated envelope; no legacy frames over UDP).
Replies to the UDP peer are wrapped back and sent to the Ethernet address
the peer's datagram came from (host or router): no routing table. The peer
is committed only by an authenticated DATA frame. Before any host spoke in
a boot, broadcast output (boot log, panic text) also goes to
`serverip`:34997 through the gateway. With an L2 host and a UDP host both
active (120 s), "to the peer" output goes to both.

Config reaches the HV without a rebuild: `bzdk_boot` runs
`env export -t 0x4a0f0000 gatewayip ipaddr netmask serverip bzdk_net`, `dbgip_init()`
parses and zeroes it. No block (old bootcmd) = UDP off, L2 unchanged.
dbgmon `ip` shows address, peer and counters; `ip set IP MASK GW HOST
[PORT]` (hex) changes it until the next boot, `ip off` stops it.

Host side: `bzd_udplink.py` (`bzd-udplink.service`) owns a persistent tap
`bzd0` and moves every debug-ethertype frame between it and
board_ip:34997. With `transport = udp` the tools bind `bzd0` instead of
`br0`; their code is unchanged. `bzd_udplink.py --check` does one
handshake through it. firewalld: udp/34997 accepted from 192.168.88.0/24
and 192.0.2.0/24 (the board's unsolicited boot output).

Watchdog reachability (wdt.c): authenticated UDP frames are dispatched
through `hv_dispatch()`, which calls `note_rx_frame()` like the L2 path.

## DHCP (`dbgdhcp.c`, `bzdk_net=dhcp`)

U-Boot's `bzdk_net` (exported with the other four; absent or anything but
`dhcp` = static, the behaviour above) puts the HV's address on DHCP:

* **Client.** `dbgdhcp.c` is plain RFC 2131 (DISCOVER/OFFER/REQUEST/ACK,
  RENEW at T1 unicast to the server, REBIND at T2 broadcast, back to INIT on
  NAK or expiry; retransmissions 2, 4, .. 64 s with jitter), driven from
  `dbgip_tick()` (CPU1's EMAC poll) with no blocking. Client identity is the
  HV's MAC (`02:ba:fb:3b:f5:8c` here; chaddr and option 61), hostname
  `bzdk`. It asks for mask, router, DNS, lease, T1, T2; the address, mask
  and router replace U-Boot's. No ARP conflict probe, no DECLINE.
* **The guest's DHCP is untouched.** emac.c now also offers broadcast IPv4
  frames to `dbgip_rx()`, which takes a frame only if it is a BOOTP reply
  (UDP 67 -> 68) whose chaddr is the HV's MAC. The guest's own exchange
  (chaddr `12:..`), broadcast or unicast, goes on to vnet exactly as before;
  so does every other broadcast.
* **Fallback.** No lease 10 s (`DBGIP_FALLBACK_S`) after boot: U-Boot's
  static `ipaddr/netmask/gatewayip` (src `env-fallback`); DHCP keeps going
  and a lease replaces the address when it comes. Losing the lease goes
  back to the fallback. `serverip` (the host) always comes from the env:
  it is the one fixed address.
* **Announce.** On every address change, and every 30 s while no UDP peer
  is active, the HV sends `serverip`:34997 one inner 0x88B5 frame of
  envelope type 5 (layout in `dbgip.h`: its address, mode, the guest's
  address). It is not authenticated and nothing trusts it: with
  `board_ip = auto`, `bzd_udplink.py` treats any datagram from an address
  that is not the current board's as a candidate, sends a HELLO there, and
  adopts the address only if the CHAL carries a valid proof of the shared
  key (one try per candidate per 10 s). Behind the MikroTik the announce
  never arrives (it forwards from 192.0.2.0/24 to the host's LAN only
  replies to connections the host opened, and source-NATs the host), so
  `board_ip = auto` also sweeps the board's subnet with HELLOs (last
  known address first) until one answers with a valid proof, and again
  when frames go up and nothing comes back; the CHAL is a reply and
  passes. The verified address goes to
  `~/.config/bzdk/board.ip`; a new lease is followed the same way. An
  address change drops the HV's UDP peer (its next hop may be different);
  sessions stay valid, the next DATA frame re-commits the peer.
* **Guest address.** vnet TX hands every guest frame's first bytes to
  `dbgip_guest_tx()`, which records the address the guest uses (sender of
  its ARP frames, else IPv4 source, guest MAC as Ethernet source; never
  0.0.0.0, 127/8, 169.254/16, multicast). With `guest_ip = auto`,
  `bzd_board.guest_ip()` asks the HV once per process (dbgmon `mac`) and
  caches the answer in `~/.config/bzdk/guest.ip`, used when the HV cannot
  be asked. All ssh-using tools get it there.
* **dbgmon.** `ip` prints the hex counters (now with `mode dhcp_state
  dhcp_server lease_left dhcp_rx announce guest_ip`) and a second line for
  people: `net mode=dhcp addr=192.0.2.23/24 gw=192.0.2.1 src=dhcp
  host=192.168.88.2:34997 dhcp=bound lease=.. server=.. left=1700s/3600s
  fallback=192.0.2.7 tx= offer= ack= nak= stale= lost= guest=192.0.2.50`.
  `ip dhcp` starts DHCP live (the current address stays until a lease
  arrives), `ip static` releases the lease and goes back to the static
  address, `ip set` is static too. `mac` ends with `guest_ip=`.
* **Setting it.** `emmc_boot.py netmode dhcp|static` writes `bzdk_net` and
  the `bzdk_boot` that exports it (needed once: older `bzdk_boot` does not
  export `bzdk_net`); `cutover.py` does the same for a whole move.

Tested off the board: `test_dbgdhcp` (the real `dbgdhcp.c`, natively:
full lease cycle, backoff, NAK, release, and guest replies with chaddr
`12:..` -- broadcast, unicast, even with the HV's xid -- never taken),
`bzd_udplink.py --selftest` (adoption only after a valid CHAL, wrong key
refused, rate limit, explicit mode never learns; once over real loopback
sockets).

On the board, 2026-10-12, in 192.0.2.0/24 behind the MikroTik: the HV took
a lease (192.0.2.121, 24 h), the guest got its own by dhclient, the host
found the HV by the sweep within seconds and the guest through the HV;
hvauth, hvdbg, triage, hv_deploy and guest ssh work, the guest reaches the
internet and resolves names. After a warm reboot both came back with the
two addresses swapped (HV .122, guest .121), and the host followed both
without help. Give the two MACs static leases on the router if stable
addresses matter.

Not understood yet: the first boot in DHCP mode, still on the old segment
(192.168.88.0/24) right after `cutover.py apply`, sent no DHCP request at
all, fell back to 192.0.2.7, kept ARPing for 192.0.2.1 and answered ping,
but answered no debug frame (neither L2 nor UDP) and the guest never came
on the wire. Moving the cable to 192.0.2.0/24 brought the board back
through a reset whose cause was not captured. A live `ip dhcp` on the
same segment worked earlier the same day.

## Boot from the eMMC (`emmc_boot.py`, `hv_deploy.py`)

Slots on the ESP (mmc `${mmc_bootdev}:2`, guest `vtbd0p2`):
`bzdk/cur/{hv.uimg,kernel,dtb}` and `bzdk/lkg/...` (last known good).
`bootcmd = run bzdk_boot; <the old TFTP retry loop>`. `bzdk_boot`:

1. exports the network config for the HV (above);
2. slot record at 0x50021100 (`HVMAP_BOOTSLOT`, survives a watchdog reset,
   a cold boot's garbage fails the magic): `tries += 1`;
3. `tries <= 3`: load `cur` (dtb, kernel, HV; `iminfo` checks the uImage
   CRC), write slot code 1, `bootm`; on any failure, or tries > 3: `lkg`
   (code 2); then the TFTP loop (code 3, routed through `gatewayip`), then
   the watchdog.

The HV marks the record healthy after 90 s up with the guest not held
(`dbgtools_tick`, same point as the safe-mode counter) and, when it came
from `cur`, zeroes `tries`. A bad `cur` is tried three times (three warm
resets), then the board runs `lkg` and stays there until the next deploy.
dbgmon `bootslot` prints the record, `bootslot tries 0` resets it.

    python3 hv_deploy.py status            # slots, md5s, record, tftpboot match
    python3 hv_deploy.py deploy [--all]    # make hv-uimage output -> cur
    python3 hv_deploy.py reboot            # clean guest shutdown -r
    python3 hv_deploy.py promote           # cur -> lkg once cur proved itself
    python3 hv_deploy.py rollback          # lkg -> cur

Writes go guest /tmp -> md5 -> ESP `<name>.new` -> md5 -> rename -> md5
through a fresh read-only mount. U-Boot's binary is never touched.

Testing a boot path without touching the saved env: `uboot_maint.py open`
(bootdelay 3), then `uboot_nc.py run <script>` catches the prompt over
netconsole (UDP 6666) and runs `emmc_boot.py nc-script`. Netconsole loses
characters from long input lines, so the variables are not typed: put
`emmc_boot.py env-file` on the ESP as `bzdk/test.env` first (through the
guest), and the script `env import`s it in RAM and runs `bzdk_boot`; preset
the tries with `--slot lkg` to test the fallback. Verified 2026-10-12: cur
boot, lkg fallback after 3 tries, then three reboots from the saved env, all
on slot cur with the tries cleared after 90 s.

## Moving the board to another subnet

    python3 tools/subnet/cutover.py plan  tools/subnet/target-10.0.0.conf
    python3 tools/subnet/cutover.py apply tools/subnet/target-10.0.0.conf
    # move the cable, then:
    python3 tools/subnet/cutover.py verify
    python3 tools/subnet/cutover.py rollback <backup dir> [--console]

`apply` backs up U-Boot env, guest rc.conf/resolv.conf, board.conf, the
learned board.ip/guest.ip and the host's kernel netconsole script, writes
all of them (target 10.0.0: `net = dhcp`, `board_ip = guest_ip = auto`,
`uboot_ip = 192.0.2.7` as U-Boot's address and the HV's fallback), restarts
`bzd-udplink.service`, and reboots the board cleanly; on the old segment
the L2 channel (`BZDK_TRANSPORT=l2`) and the guest console still work, so
the new config is checked before the cable moves -- and DHCP there hands
out 192.168.88.x to the HV and the guest, which exercises the whole
discovery path on the old segment. `rollback` restores all of it (env keys
incl. `bzdk_net`/`bzdk_boot`, restarts the udplink too); `rollback
--console` works through the guest console on L2 when the board is back on
the host's segment but unreachable by IP.
