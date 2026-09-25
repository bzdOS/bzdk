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


## 2026-09-24 — the second disappearance, and what it ruled out

A deliberate test reproduced it. With chimpd stopped, the board was reset onto
the fixed image, came up (HV gadget at host-uptime 1043061), sat with a dark
EMAC for 370 s, escalated, reset — and never enumerated again.

**That reset went through the new clean-disconnect path.** So the
"unclean reset wedges U-Boot's gadget" mechanism from 4084408 is *not* what
makes the board vanish. 4084408 is still correct on its own terms, and the
-71 errors in the earlier log are still real, but it does not explain this.

Ruled out along the way:

- **internal-note's break-glass wiring.** `usbacm_force_breakglass()` sets
  `wdt_debug_hold = 1`; the line above it already did. A no-op.
- **A cross-core `wdt_disarm()` race.** No caller in the dbg build.
- **The 32 kHz fanout gate.** Nothing in this tree touches it, and a causal
  claim about that bit has already been retracted once.
- **MUSB not being reset by the watchdog.** The host-initiated path drops the
  same pull-up and the board comes back from it routinely.

**One real bug found, and it was mine.** ea9cf38's retry loop called
`arm_watchdog()` repeatedly; `WDOG_CTRL = 0x14AF` is the RESTART key, so that
loop pets the watchdog it is waiting for. A merely-slow watchdog would have
been held off forever by the code waiting for it. Fixed in 3682eaf: arm once,
wait, then reconnect and sit still.

It is not the cause of either disappearance — the watchdog is armed for 2 s
and the wait is 5 s, so on both occasions it should have fired long before
that loop ran — but it is the same failure shape, and it would have bitten
eventually.

**What is still unknown, precisely.** After the reset, the host logged *zero*
USB events: not a successful enumeration, not a failed one. U-Boot brings its
gadget up early, before `bootcmd`, so "nothing at all" means the SoC is not
reaching that code. Whether it reset and then hung, or never reset, cannot be
told apart from outside — and DRAM, where all our breadcrumbs live, is wiped
by the power-cycle that is the only way back.

**So the next step is not another theory, it is evidence that outlives the
board.** A boot record written to a raw SD sector (LBA 64 is inside the
1004 K gap before p1, below the partition table and above the GPT array)
would answer the one question that matters: after an escalation, does the
board reboot repeatedly and silently, or does it stop once? Nothing else
distinguishes those, and they point at completely different faults.

**Meanwhile the path is disarmed.** `dbg_emac_watchdog_reboot` is 0 as of
1c3d6db, so the board no longer resets itself on a dark EMAC. The failure
cannot recur unattended; it can only be provoked deliberately, which is the
right way round for something this expensive to observe.

## 2026-09-25 — Correction: the bootcmd edit was not the killer

On the morning of the 25th, with the board dark since 01:46 on the 24th, I
said the cause was my 01:46 environment edit (arming the watchdog at the
front of `bootcmd`), and "fixed" it by restoring the previous `bootcmd`
byte-for-byte via `rescue_env.py`. The board came back. Re-reading the
host kernel log and chimpd's log against the transcript shows the claim
does not hold:

- **01:46:04** environment written. **01:46:15** `bzdctl.py power reset`
  (`reboot_clean()` via BMC). **01:46:18** the HV gadget disconnected —
  and *no U-Boot gadget ever appeared*. U-Boot's gadget comes up in
  `preboot`, before `bootdelay`, before `bootcmd`. So `bootcmd` never ran.
  This is the same shape as 21:22:55 on the 23rd: a warm reset after which
  nothing enumerates, until the power is cut.
- **10:55:09 (25th)** power-cycle; U-Boot gadget up; **10:55:11** chimpd
  caught the prompt, inside the 3 s `bootdelay` — `bootcmd` did not run;
  **10:55:13.8** USB disconnect, then a ghost reconnect that never
  answered a SETUP. **11:00:09** second power-cycle, prompt caught at
  11:00:10, dead at 11:00:42 with `Cannot enable. Maybe the USB cable is
  bad?` on the host, while the cable was being re-plugged. **11:05:54**
  third power-cycle: prompt caught, env restored, `boot` typed, TFTP at
  10.8 MiB/s, guest up. Same U-Boot, same eMMC.

So: the edit was reverted (correctly — an unverifiable boot-chain edit
should not stay), but reverting it fixed nothing. What is actually
established is narrower and worse: **a warm (WDOG) reset issued by the
hypervisor sometimes never returns to U-Boot, and a cold start always
does.** Both losses had the guest busy on the eMMC (root mount on the
23rd; `uboot.env` written 11 s earlier on the 24th). The WDOG resets the
SoC but not the PMIC, the eMMC, the SD card or the RTC domain — whatever
state they were in survives into the BROM. `warm_reset_soak.py` measures
this: N resets idle, N under eMMC reads + SD writes.

The morning's two dead prompts are a separate, unexplained thing; the host
errors were physical-layer, and they coincided with the cable being
handled. Not pursued.

## 2026-09-25 — Resolved: why the warm reset "never returned"

`warm_reset_soak.py` lost the board on its very first cycle, idle, 25 min
after a cold start. Reading `reboot_clean()` against its callers gave the
mechanism, and the USB monitor confirmed the state:

- `bmc_reset()` → `reboot_clean()` runs on CPU1: drop the USB pull-up, arm
  a **2 s** WDOG, park. CPU0 keeps running the guest, and `el2_exc.c`
  calls `wdt_pet()` on **every** EL2 exception while the guest's last
  "progress" is under 180 s old — and `vblk_emmc.c` counts eMMC traffic as
  progress. Any ssh command in the previous three minutes therefore kept
  the WDOG reloaded hundreds of times a second. **No reset ever happened.**
  Gadget gone, CPU1 parked, guest alive at `login:`, board dark on every
  channel.
- Confirmed live: with the board "lost", `~BZDBG` on ttyACM still answered
  and the guest console showed `login:`. Break-glass (`\x00~BZRST\x00`, which
  sets `wdt_debug_hold`) brought it back in 16 s.
- Fixed in 6c01475: `wdt_debug_hold = 1` inside `reboot_clean()` itself.
  The PSCI and EMAC-dark callers had set it; `bmc_reset()` and `repl.c`
  had not.

The 01:39 reset that worked did so because the guest had been idle for
nine minutes. The "eMMC state survives the warm reset" hypothesis from
earlier today is withdrawn: there was no warm reset to survive.

## 2026-09-25 — Second, independent: the guest switches the PHY's rail off

Two consecutive boots after that came up with the EMAC dead once the guest
finished booting. WDEP (read over USB) showed the BMSR ring all `0xffff`;
`rsb_read(0x2d, 0x12)` via `~BZDBG call` returned **0x58: DC1SW (vcc-phy)
off.** FreeBSD disables every regulator nobody references at the end of
boot, and the guest DTB's emac node is `status="disabled"` because this
hypervisor drives the EMAC. The boots that survived did so because hdmi.c's
relock fallback happens to write 0x88 (DC1SW+DLDO1) when its RSB read
fails under contention. Fixed in 228623b: `phy_rail_ensure()` re-enables
DC1SW whenever the PHY reads as absent, before re-kicking.
