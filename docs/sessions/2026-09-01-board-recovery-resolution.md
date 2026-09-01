# Board recovery 2026-09-01 — RESOLVED, no physical intervention

Outcome of `2026-09-01-board-recovery-brief.md`. Board back online via software-only
path (its own watchdog + hardened EMAC cold boot). No owner escalation — the
power-physics hypothesis (brief H1) was **refuted**.

## Resolution in one paragraph

The board was never unpowered and never fully dead. Its HV USB gadget
(`1d6b:0010`, musb.c:704) stayed enumerated on the **workstation node** from the
22:00:12 watchdog reboot until 2026-09-01 15:59:35 — 17 h with zero USB
disconnects — which also proves the WDOG was being petted the whole time (a hung
HV resets in ~16 s). Only EMAC was dark. At 15:59:35 the petting stopped for
reasons unknown (see open questions); the WDOG fired; the 2026-08-30-hardened
EMAC cold-boot path won the PHY lottery on the fresh boot; the FreeBSD guest came
up with networking. Verified: workstation `ip neigh` 192.168.88.82 → 02:bd:05:00:00:01
REACHABLE, ping 0% loss; guest `uname` = 15.1-RC3 BPI64 arm64, `hw.ncpu=4`,
vtnet0 UP, gateway RTT 5.7 ms, /opt (vtbd1p2) mounted, `kern.boottime` 15:59:56;
BMC health: 4/4 cores moving, 44–45 °C; boot-watch recorded (ledger total 391).

## The brief's premise error (cost most of the confusion)

**build-host has never had the board's control plane.** Its kernel journal (retained
back to 08-17) shows zero USB enumeration in every boot, `/root/.ssh/chimp_ed25519`
does not exist there, and build-host is not L2-adjacent to 192.168.88.0/24. The USB
serial (/dev/ttyACM0), the raw-Ethernet EMAC segment (br0 = 192.168.88.2/24) and
the guest SSH key all live on **workstation** (reached from build-host via the
`Host workstation` tunnel, 127.0.0.1:<port>). "USB dead" in the brief was an artifact
of probing from the wrong host. `bzdctl status`/`bmc_client` only work where the
L2 segment is — on workstation.

## Verified timeline (workstation kernel journal + board breadcrumbs)

- 2026-08-31 21:59:58 — gadget dev 21 disconnects; 22:00:00 U-Boot gadget
  `1f3a:efe8` (dev 22) — a watchdog reset landed the board in U-Boot.
- 22:00:11 — dev 23 full-speed with two `descriptor read/64, error -71` (HV
  taking MUSB over), 22:00:12 — HV gadget `1d6b:0010` (dev 24) → ttyACM0,
  stable for the next 17 h.
- Somewhere between 22:00:12 and ~22:38 the EMAC went dark during hv's guest
  WiFi (SDIO) bring-up SSH ops. Board stayed in that state, alive but dark.
- 2026-09-01 15:59:35 — WDOG fired (petting stopped ~16 s earlier, cause
  unknown); 15:59:42 U-Boot gadget (dev 25, ttyACM1); 15:59:46 HV gadget
  (dev 27) → ttyACM0. EMAC came up on this boot; guest network up by ~16:02.

## Forensic leads preserved for the root-cause task (internal-note)

- BZDBG pipeline breadcrumbs @0x5009E4C0 (warm-reset survivors): **7 lines
  posted / 7 executed / 7 replied during the dark boot** — someone drove dbgmon
  over USB (`~BZDBG<line>`, usbacm.c) while EMAC was dark, yet no reset
  followed. Who, and why not?
- Dead boot's last dbgmon command: `r 0x...` (word1 @0x50000a00 = "r 0x"),
  phase=3 — consistent with a monitor poll being the last activity, not a
  mid-command wedge.
- BMC flags were/are 0x27 incl. **block_reset** (`dbg_block_reset`,
  el2_exc.c) — the reset-verb gate was SET. May explain why any attempted
  remote reset was refused for 17 h.
- Historical (pre-this-boot) exception latch: count=107422, last_kind=0x8,
  last_esr=0xf20007d0 — stage-2 fault traffic; volume alone is not proof of a
  storm (normal guest MMIO traps land there).

## Rules honored

R0 kept (no user reset requested; escalation never sent — physics disproven).
`triage.py`-first is moot for the dark window (no channel); once EMAC returned,
only health/boot-watch/ledger were used. One-process-on-tty respected (nobody
held it; my single BZDBG probe attempt raced the 15:59 re-enumeration and got
EIO — the board had already reset itself by then; the write never reached it).
Git tree untouched except this file. Board left running as-is — no chimpd, no
reload; version reconciliation (build-host @5a50489 vs workstation @28cd3e1 vs the
eMMC image) is hv/fleet business.

## Handoff

Root cause + recovery hardening tracked as task **internal-note**. hv's supervision
loop should learn: EMAC-dark + USB-gadget-alive ⇒ drive BZDBG over workstation's
ttyACM0 (or send the break-glass `0x00 ~ B Z R S T 0x00`) instead of waiting.
