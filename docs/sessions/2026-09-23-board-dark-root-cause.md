# 2026-09-23 — the board went dark, and why

The board stopped answering on every channel at 21:22:55 and was still silent
hours later: no USB gadget on any bus, no Ethernet frames, no ARP. It took a
power-cycle. This is what actually happened, in the order it was established.

## Symptom

Host `dmesg`, the last three events on `usb 1-4` and then nothing:

```
21:21:02  new device 56  1f3a:efe8  bzdOS "USB download gadget"   <- U-Boot
21:21:09  disconnect 56                                            <- HV takeover
21:21:14  new device 57 ... device descriptor read/64, error -71
21:21:14  new device 57 ... device descriptor read/64, error -71
21:21:14  new device 58  1d6b:0010  bzdOS "USB Console"            <- hypervisor
21:22:55  disconnect 58
          (nothing, ever)
```

`bzdctl status` unreachable, 40 s of `tcpdump` on the board's MAC and both
debug ethertypes: zero frames. ARP sweep: nothing. The host's own port was
fine — `usb1-port4` reported `state=not attached`, `disable=0`,
`over_current_count=0`, so the board was not pulling up D+.

## Cause, part 1 — the board reboots itself when nobody is talking to it

`emac_rx_recent()` returns 0 when `g_rx_count == 0`. "No frame has ever
arrived" is therefore indistinguishable from "the wire went dark". With
`LINK_WD_CHECK_PERIOD_S = 8` and `LINK_WD_MAX_ATTEMPTS = 6` the watchdog gives
up ~48 s after `emac_init()`; `smp.c` sets `wdt_debug_hold`
(`dbg_emac_watchdog_reboot` defaults to 1), both pet paths stop, and the 16 s
HW watchdog reboots the board.

**~64 s after boot, every time.** The guest cannot prevent it: `vtnet0` needs
two to three minutes to pass its first packet, four times the budget. So a
board nobody polls reboots before its guest can produce the traffic that would
prove the link, and does it again, forever.

It never showed because chimpd polls every 10 s, and every boot-reliability
measurement goes through `bzdctl boot-watch`, which also polls. The act of
measuring prevented the failure. chimpd was killed at 20:57; the measurement
kept the board alive until it finished at 21:19:57; the board rebooted itself
65 s later, at 21:21:02. 48 + 16 is exactly that 65 s.

Fixed in `fe2ecd6`: while no frame has ever arrived and we are within 300 s of
`emac_init()`, keep re-kicking the PHY but withhold the reboot. Rebooting
cannot make traffic appear. After any frame the 2026-08-27 recency behaviour
is unchanged, so a link that drops after traffic still escalates at once.

## Cause, part 2 — that reboot was unclean, which is why it vanished

The escalation only set `wdt_debug_hold` and went quiet, letting the HW
watchdog fire **with the MUSB pull-up still up**. The host-initiated reset
(`bmc.c` `reset`) has always gone through `reboot_clean()`, which disconnects
first. So every manual reset was clean and every autonomous one was not, and
nothing said so.

An unclean WDOG reset wedges U-Boot's gadget on the way back — `device
descriptor read/64, error -71`, which is why `reboot_clean()` exists in that
shape. It costs more here than anywhere else because `preboot` makes U-Boot's
console the usbacm gadget: a wedged gadget costs U-Boot its console, so it
never reaches `bootcmd`, never TFTPs, never brings EMAC up. Powered,
executing, silent everywhere.

The log shows both stages. The first autonomous reset enumerated only after
two -71 errors. The second never enumerated again.

Fixed in `4084408`: set the hold first (CPU0's `wdt_pet()` re-arms the counter
whenever the guest is progressing, which would defeat the timer), then
`reboot_clean()` — clean disconnect, 2 s watchdog, deterministic.

## Cause, part 3 — nothing was watching during U-Boot

```
# CONFIG_WATCHDOG_AUTOSTART is not set
# CONFIG_CMD_WDT is not set
CONFIG_WATCHDOG=y
CONFIG_WDT=y
```

U-Boot has watchdog support compiled in, never starts it, and has no `wdt`
command to start it with. On this board the SoC watchdog is armed by exactly
one thing: the hypervisor's `wdt_init()`.

So the window from SoC reset through BROM, SPL, U-Boot, `bootcmd`, TFTP and
`bootm` until the hypervisor starts — normally 12–20 s — is covered by
nothing. "The watchdog didn't work" is the wrong reading: it worked, it is
what reset the board at 21:22:55. The reset then cleared it, and the next
generation hung before reaching the only code that re-arms it. A hardware
watchdog cannot recover a hang that happens before it is armed.

`ea9cf38` narrows the same class from the other side: `reboot_clean()` no
longer spins forever trusting a reset that may not come. After a bounded wait
it puts the gadget back and keeps re-arming, so a failed reset leaves a
reachable board instead of a silent one.

## Still open

Whether the last generation hung inside U-Boot's console switch specifically
is inference, not evidence — there was no channel left to ask. The three fixes
above address mechanisms each independently established from the source and
the log; none of them is asserted as "the" proven final step.

## First things to do on the next power-up

1. ~~Read the WDEP record at `0x50022000` — it carries the episode counters
   from the boots that died.~~ **Wrong, and tried: it does not survive a
   POWER-CYCLE.** WDEP is written only by emac.c's watchdog paths and never
   by `emac_init()`, which makes it survive a warm reset — that is all it was
   ever designed for. After the plug was pulled the magic was re-stamped by
   the fresh boot but every counter read `0xFFFFFFFF`, i.e. uninitialised
   DRAM. The evidence from the boots that died is gone. Any post-mortem that
   needs to cross a power-cycle has to land somewhere other than DRAM.
2. Arm the watchdog from `preboot`, ahead of the console switch, so a U-Boot
   hang self-recovers:
   `mw.l 0x1c20cb4 1 ; mw.l 0x1c20cb8 0xb1 ; mw.l 0x1c20cb0 0x14af ; setenv stdout usbacm,serial ; ...`
   This is an environment change, so `uboot_env.py` can do it from the guest —
   no flashing. Cost: the rest of U-Boot's boot must fit in 16 s, the A64
   ceiling. Measured 12 s. If it ever does not fit, the board reset-loops
   visibly, which is strictly better than hanging silently.
3. The TFTP image is deliberately the proven `d80c9e2` build, not HEAD. Put
   the three fixes on the board one at a time, with a poller running.
