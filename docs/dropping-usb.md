# Getting off the microUSB cable

## The premise

**There is no serial adapter, and there is not going to be one.** That is not
an inconvenience to work around; it is why this project exists in the shape it
does. The hypervisor implements its own USB CDC-ACM gadget precisely so the
board can be brought up and debugged with nothing but a plain microUSB cable.
Any plan that answers "put a USB-TTL on the uart0 header" has missed the
point.

So: no adapter, and eventually no cable either. What is left is Ethernet, the
SD/eMMC, and the board's own power.

## What is already done

**Booting without the cable is solved and hardware-verified** — see
`autoboot-no-cable.md` (2026-08-20, including a cold power-on with the host
not touching the console). U-Boot's `bootcmd` TFTPs the hypervisor, DTB and
kernel itself, retries ten times, and arms the SoC watchdog if it never gets
there.

The hypervisor's debug monitor and the guest console both already run over
EMAC. `bzdctl status`, `console`, `power reset` never touch the tty.

## The one real gap, and it is one config option wide

Everything above works without the cable except **reaching U-Boot**. U-Boot
talks on uart0's physical pins, which we cannot read. That matters because
changing `bootcmd` — the thing that makes autoboot work at all — currently
needs the prompt.

U-Boot has a network console built in. We build our own U-Boot, networking is
already on, and the option is simply off:

```
/opt/bzdos/build/u-boot/.config
  CONFIG_NET=y
  CONFIG_CMD_NET=y
  # CONFIG_NETCONSOLE is not set      <- this
```

Turning it on gives the U-Boot prompt over UDP, reachable from the host with
no adapter and nothing plugged into the board's USB. That is the single change
that closes the gap.

A second, independent route to the same end: the U-Boot environment lives on
storage, so the guest can rewrite it directly and never need the prompt for a
config change at all. Worth having both — netconsole for interactive work, the
storage route as the path that survives a broken netconsole.

## What software cannot fix

Being straight about this matters more than the rest of the plan.

- **Hypervisor never starts:** handled. U-Boot retries, then the watchdog.
- **Hypervisor wedges, EMAC alive:** handled. `bzdctl power reset`.
- **Hypervisor wedges, EMAC dark:** the SoC watchdog still recovers it to
  U-Boot on its own. What is lost is forcing a reset *on demand*.

  **That sentence was wrong, and 2026-09-23 showed how.** The SoC watchdog is
  armed by exactly one thing on this board -- the hypervisor's `wdt_init()`.
  U-Boot arms none of its own (`CONFIG_WATCHDOG_AUTOSTART` unset, and
  `CONFIG_CMD_WDT` unset so there is nothing to arm it with), so the window
  from the reset through BROM, SPL, U-Boot, bootcmd, TFTP and bootm is
  covered by nothing. A hang in that window is permanent.

  Worse, the autonomous EMAC-dark escalation was resetting *uncleanly* --
  letting the watchdog fire with the MUSB pull-up still up, which wedges
  U-Boot's gadget coming back. Since `preboot` makes that gadget U-Boot's
  console, the board loses its console, never reaches bootcmd, and goes
  silent on every channel at once. Fixed in 4084408 (clean disconnect first)
  and fe2ecd6 (do not escalate on a link that has never carried a frame).
  Full account in `sessions/2026-09-23-board-dark-root-cause.md`.

  The remaining hole is the U-Boot window itself, and it closes with an
  environment change rather than a flash: arm the watchdog at the front of
  `preboot`, ahead of the console switch. `uboot_env.py` can do that from the
  guest.

  Measured once, 2026-09-23, and it is not as clean as that sentence reads.
  The WDEP record (`0x50022000`) after the episode: 7 link-watchdog attempts,
  6 PHY re-kicks, `rekick_result = 0` on every one, and one give-up -- the
  re-kick ladder never retrained the link, while BMSR reported the link UP on
  all three samples (0x796d). Recovery came from the watchdog resetting to
  U-Boot, and chimpd then reloaded over USB. Whether the board would have
  recovered on its own is not known from that run, because chimpd catches the
  prompt first and masks it. Any future measurement of cable-free recovery has
  to account for that.
- **U-Boot itself hangs:** nothing helps. Note `reset` at the prompt calls
  `hang()` on this platform and makes it worse.

The last two want hardware: a relay or a network-switched socket on the
board's power. One cheap part, and it is the last reason to keep a cable
attached.

## Order of work

1. `CONFIG_NETCONSOLE=y`, rebuild U-Boot, verify the prompt over Ethernet and
   that `bootcmd` can still be changed. **Demoted, and no longer urgent** --
   step 2 below turned out to deliver the thing this was wanted for, without
   flashing anything. Netconsole is still worth having for interactive work,
   but testing it means putting an untested bootloader where the BROM will
   find it, and doing that cost a dark board on 2026-09-23. When it is
   attempted, chain-load the candidate from the working U-Boot first; never
   write it to boot media to find out whether it runs.
2. **DONE, 2026-09-23.** Rewrite the U-Boot environment on storage from the
   guest -- `uboot_env.py`. `bootcmd` and everything else U-Boot reads can now
   be changed with no prompt, no netconsole and no cable. Verified end to end
   on hardware: set a variable, read it back, remove it, and the resulting
   file is byte-identical to the backup taken beforehand.

   This did not work until the same day, and for a reason worth recording:
   `VBLK_BOOT_GUARD_LBA` was 18432, which reached 2014 sectors past the U-Boot
   reserve and into the start of the ESP, so the guest could not mount the ESP
   read/write at all. The guard exists to stop the guest destroying SPL; it was
   also stopping it from editing boot configuration. Floor is now the ESP's
   first LBA (16418) and SPL remains unwritable.
   **2026-09-25, the other half of this step is now proven too.** The
   autoboot path itself -- `bootcmd` → `boothv` → TFTP of DTB, kernel and
   `microkernel-dbg.uimg` → `bootm` -- brought the board from a reset to a
   running guest with chimpd *stopped*, repeatedly, in ~45 s per cycle
   (`warm_reset_soak.py`). chimpd is a backstop for that path now, not the
   path.

   And the reset that feeds it is fixed: `bmc reset` used to arm a 2 s WDOG
   on CPU1 while CPU0 kept petting it on the guest's behalf, so the reset
   never happened and the board went dark until someone cut the power
   (6c01475, see docs/sessions/2026-09-23-board-dark-root-cause.md).
   That, not anything in U-Boot, was the "warm reset sometimes never
   returns" of the last two days.
3. Switch `board_ctl.wait_for_power_cycle()` off "does /dev/ttyACM0 exist" and
   onto EMAC liveness.
4. Triage the 21 python files under `microkernel/` that reference the ACM tty
   into "port to EMAC", "port to netconsole", and "delete" — most are bench
   tooling the board itself does not need.
5. Add switched power for the two cases above that software cannot reach.
6. Later, once the board stops being reflashed dozens of times a day, move the
   boot payload from TFTP to local storage and the host disappears entirely.
