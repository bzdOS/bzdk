# BOARD RECOVERY BRIEF — BPI-M64 hard-offline since 2026-08-31 22:38

You are the SMARTER-MODEL escalation per the fleet ladder (free hv session failed →
GLM-5.3-Flash orchestrator scoped it → you are the next rung). Task: exhaust every
SOFTWARE recovery path for the Banana Pi M64 board. Escalate to OWNER (via hub queue
role orchestrator, from=boardfix) ONLY if you prove physics (power/cable/hub dead,
nothing host-side left). Do NOT touch the git tree except docs/sessions/.

## Timeline and established facts (verified 2026-09-01, host build-host)

- 2026-08-31 ~22:38: board stopped answering on EMAC (guest ssh 192.168.88.82) during
  WiFi bring-up SSH operations by hv worker. Board has been offline ~16h.
- EMAC: ping 192.168.88.82 = 100% loss; `ip neigh` has NO entry at all (no carrier/
  no L2). BMC channel (bzdctl power reset etc.) rides EMAC → also dead. "no BMC1
  record, board down, or magic mismatch" per bzdctl status.
- USB: `lsusb` shows ONLY root hubs (1d6b:0002/0003). dmesg USB events stop at host
  boot (xHCI 0000:00:14.0) — NO enumeration of any external device since boot, no
  disconnect events in buffer. /dev/ttyACM0 absent. This is suspicious in two
  directions: (a) board (and possibly its upstream hub) lost power; (b) OR host
  booted after the board had already been plugged and buffer rotated — check
  `journalctl -k --since "-3 days" | grep -iE 'ttyACM|usb 1-'` for history.
- Watchdog self-heal: hv waited >90 min — no recovery. The EMAC cold-boot PHY
  lottery (RTL8211E) was hardened 2026-08-30 (commits 1f3f1ee, 9a695bf, 821bd1e,
  586882e: PHYSR cross-check, one full phy_startup retry) — that path is proven,
  but it needs the board to be BOOTING at all.
- hv (free-model worker, session 155918) is DOWN; its restart is fleet's business.
  You are not hv; you are the escalation.

## What hv already tried (before dying)

- Repeated SSH/ping retries; U-Boot watchdog recovery wait (no help).
- uhubctl: NOT INSTALLED on this host (`command not found`) — hv's earlier claim
  "uhubctl не переключает эту плату" came from a DIFFERENT host/session; re-check
  here: apt install uhubctl OR check /sys/bus/usb/devices/*/power/control,
  /sys/bus/usb/devices/X-Y/authorized (hub port power cycling via sysfs is possible
  without uhubctl IF the board hangs off a root-hub port or an external hub that
  is itself enumerated — NOTE: today nothing external is enumerated at all).

## Hypotheses to work through (in order)

1. POWER: board unpowered (single-point failure: bus-powered upstream hub or its
   cable died — would kill USB serial AND the PHY simultaneously? NO — EMAC is a
   separate wire; but if the BOARD has no power, EMAC is dead too. USB hub dying
   alone would NOT kill EMAC. Both dead together ⇒ board power or board itself).
   ⇒ Strongest hypothesis: board has no power / PSU fault / barrel jack issue.
2. USB-only local failure (hub or cable on host side) while board actually RUNS
   and EMAC should work — but EMAC is ALSO dead, so this requires two independent
   faults. Still: verify the host xHCI is healthy (lspci, xhci events in
   journalctl), try USB enumeration refresh: echo 0/1 > authorized on ports,
   usbreset via driver unbind/rebind.
3. BMC channel exists but on a different interface/address than we probe. Read
   bzdctl.py `_bmc()` and bmc_client.py for the actual transport (UDP magic over
   EMAC? which MAC/IP?). Maybe a directed L2 frame could wake the PHY or the BMC
   (WOL-style): check EMAC MAC of the board in known_hosts/arp caches of OTHER
   mesh hosts (workstation node also reaches the board).
4. Mesh relays: the workstation node has its own USB/EMAC view? hv worked "через
   workstation" — if workstation sees the board's USB, a power-cycle command might exist
   THERE. (workstation is reachable from build-host? check ~/.ssh/config.)
5. If everything confirms power-dead ⇒ physics ⇒ owner escalation with a
   precise one-paragraph ask (re-plug PSU/barrel, check hub power), nothing less.

## Ground rules (from SESSION-RULES.md / DEBUG_RULES.md — read them)

- R0: never ask the OWNER to press reset / re-plug until physics is PROVEN —
  and then only through the fleet orchestrator queue, never directly.
- One process on /dev/ttyACM0 at a time (moot while it does not exist).
- Do not trust a guest-written DRAM word read from another core as liveness
  (cache/Device-type trap — see CLAUDE.md rule 5).
- Do NOT edit docs or code to "fix" anything; your deliverable is (a) the board
  back online via any software path, or (b) a proven-physics escalation note.
- triage.py first, never `gr` (stale frame) — once the board answers again.

## Tool map (all in /opt/bzdos/microkernel unless noted)

- python3 orient.py — one-screen live orientation (safe, no tty).
- python3 bzdctl.py status / power reset|hold|release|arm|disarm / boot-watch /
  console / crash / ledger. `power uboot` is the only tty-touching subcommand.
- chimpd.py — supervisor (TFTP + bootelf) used for reloads.
- docs/brick-recovery.md, docs/war-stories.md, docs/emac-*, SESSION-RULES.md.

## Reporting

- hub_report project=bzdos agent=boardfix at each milestone (CLI: `hub report -p bzdos`).
- Owner escalation (ONLY if physics proven): hub queue send role=orchestrator
  from=boardfix, one paragraph, exact physical action requested.
