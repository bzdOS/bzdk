# Board network config, routed debug channel, eMMC boot

Since 2026-10-12 the board does not have to share a segment with the host,
and it boots without the host's TFTP.

## One config place

| Where | What | Changed by |
|---|---|---|
| host `~/.config/bzdk/board.conf` | transport (`l2`/`udp`), wire iface, udplink tap, board/guest/host/gateway IPs, DNS, UDP port | `tools/subnet/cutover.py`, by hand |
| U-Boot env (`uboot.env` on the ESP) | `ipaddr netmask gatewayip serverip` -- U-Boot's TFTP fallback and, exported by `bzdk_boot`, the HV's own address | `uboot_env.py`, `cutover.py` |
| guest `/etc/rc.conf`, `/etc/resolv.conf` | `ifconfig_vtnet0`, `defaultrouter`, nameserver | `cutover.py` |

`bzd_board.py` reads board.conf (defaults = the 192.168.88.0/24 setup);
every host tool takes the debug interface (`bzd_board.IFACE`), the guest
address (`bzd_board.GUEST`, `guest_ssh_argv()`) and the board address from
there. `tools/subnet/board.conf.example` documents the keys.

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
`env export -t 0x4a0f0000 gatewayip ipaddr netmask serverip`, `dbgip_init()`
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
netconsole (UDP 6666) and runs `emmc_boot.py nc-script` (setenv in RAM,
`run bzdk_boot`).

## Moving the board to another subnet

    python3 tools/subnet/cutover.py plan  tools/subnet/target-10.0.0.conf
    python3 tools/subnet/cutover.py apply tools/subnet/target-10.0.0.conf
    # move the cable, then:
    python3 tools/subnet/cutover.py verify
    python3 tools/subnet/cutover.py rollback <backup dir> [--console]

`apply` backs up U-Boot env, guest rc.conf/resolv.conf, board.conf and the
host's kernel netconsole script, writes all of them, and reboots the board
cleanly; on the old segment the L2 channel (`BZDK_TRANSPORT=l2`) and the
guest console still work, so the new config is checked before the cable
moves. `rollback --console` works through the guest console on L2 when the
board is back on the host's segment but unreachable by IP.
