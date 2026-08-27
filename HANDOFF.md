# Handoff — 2026-08-27

Written at the end of a long session. Read this, then `SESSION-RULES.md`, then
`RELEASE-0.0.2.md`. Everything below is either measured or explicitly labelled as
a hypothesis.

---

## 1. The board, right now

**Known-good and running.** Build id `handoff-known-good`, three guest vCPUs,
1 GiB guest window, root read-only, ssh reachable at
`ssh -i ~/.ssh/chimp_ed25519 root@192.168.88.82`.

```
hw.physmem = 1033682944   (986 MB — the 1 GiB window)
hw.ncpu    = 3
```

Nothing is pushed to any remote. `v0.0.2-prealpha` is **not** tagged — that was
deferred to the owner and is still theirs to call.

**Before you believe anything you read off the board, run `python3 triage.py`
and read the BUILD IDENTITY block.** It is printed first and says "check this
FIRST, always" for a reason; see trap #1 below.

---

## 2. What is proven on hardware

| | |
|---|---|
| **Three guest vCPUs** | `hw.ncpu=3`, `kern.smp.cpus=3`, `cpu2:rendezvous` nonzero in `vmstat -i`, and three parallel spinners each completing identical work in the same wall clock (two cores cannot do that) |
| SoC address consolidation | `soc_a64.h`; proven by a byte-identical binary |
| 100 clean boots in a row | measured on the **2**-vCPU build — **not** re-run on the 3-vCPU default. Re-running it is a real outstanding item |
| Guest build environment | python3.12 and meson repaired; `meson setup` completes on the board with `Gallium drivers: lima` |

### The three fixes that produced the third vCPU

Worth reading in order, because the third is the interesting one:

1. `vcpu2.c` never called `vgic_init()` and never unmasked EL2 IRQ/FIQ — the fix
   `vcpu1` already had, never carried across. Commit `3a5e5e2`.
2. CPU2 had no periodic tick of its own, so `vtimer_mask_watchdog()` could not
   run there; that is what left virtio-blk's SPI stuck `act=1`. Commit `fe9add8`.
3. `own_cpu_mask()` in `vgicd.c` hardcoded the CPU0/CPU1 pair when it was taught
   about `vcpu1`, and CPU2 became a vCPU two days later without it being told —
   so **every cross-core SGI aimed at CPU2 had that bit stripped before reaching
   the real distributor.** CPU2 sat outside `smp_rendezvous()` entirely while
   CPU0/CPU1 blocked waiting for an acknowledgement from a core nobody could
   ask. Commit `7b9603b`.

---

## 3. Open, with the next step named

### 3a. Four vCPUs — DONE on hardware (2026-08-27)

Root cause of the freeze found and fixed. `el2_exc.c`'s data-abort dispatch
tested `smp_cpu_id() == 3u` and routed CPU3's EC-0x24 faults into the `dual`
build's Zephyr path (`vconsole(1)`/`vgicd`/`mmio_absorb_fault`). With `vcpu3`
armed (FreeBSD vCPU on CPU3) those faults fell through unhandled, ELR never
advanced, and CPU3 sat in an infinite EL2 fault storm — the whole-guest
freeze. Fix: the branch now also requires `!dbg_vcpu3`, so an armed CPU3
takes the FreeBSD handler set exactly like CPU1/CPU2.

Verified on hardware (fixed build, `board-config.xml` vcpu3 enabled, DTB with
cpu@3): guest boots to multiuser, `sysctl hw.ncpu` → **4** over ssh, `hw.ncpu=4`
also confirmed over the serial console, guest network end-to-end (ping 0 %
loss, ssh) green.

One operational gotcha, measured: on the cold boot right after a physical
power cycle the PHY did not train (`link=0` forever → EMAC dbg channel dark,
guest `vtnet0` up-but-deaf, host ARP INCOMPLETE). It is NOT the "channel dies
when guest active" regression — a WDOG warm reset re-runs `emac_init()`, the
link trains (EMAC bc: `stage=11 link=1 speed=100`), and a 300 s one-read-per-
second EMAC watch with the guest fully active showed the channel healthy
throughout. Recovery lever that needs no EMAC and no user action: `reboot`
inside the guest (PSCI SYSTEM_RESET → `wdt_debug_hold` → WDOG → U-Boot →
TFTP reload of the current tftpboot image).

Residual — CLOSED same day: the link watchdog now also runs from CPU1's
10 ms tick (gic_timer.c's `dbg_vcpu1` block), the only place it is reachable
now that the old SMP_DEBUG_CPU tight loop is dead code whenever vcpu1 is
armed. Verified on hardware by reload: no regression (channel green from
first second, 150 s watch, 4-vCPU guest + ssh end-to-end); the heal path
itself (failed train → re-kick at ~8 s intervals, ≤6 attempts) could not be
reproduced at will — the PHY has trained on every warm reset so far.

Not yet committed: `el2_exc.c` (fix) + `board-config.xml` (vcpu3 on).

### 3b. A bigger guest window — DONE on hardware (2026-08-27)

`guest_dram_2g` is **on** (`board-config.xml`, `dtb-memory-size=0x78000000`):
the guest sees `hw.realmem = 0x78000000` (1920 MiB), boots to multiuser with
4 vCPUs, network end-to-end green. The window that made it work, all measured:

- The board really has 2 GiB (`bdinfo memory[0] [0x40000000-0xbfffffff]`), but
  the guest must not be told about the top: U-Boot reserves
  `[0xb8f18770-0xbfffffff]` no-overwrite (self at 0xbdf44000, TLB at
  0xbfff0000) and FreeBSD allocates downward — hence map 2 GiB, tell
  `0xB8000000`.
- The old early-userland stop ("after the regulator shutdowns") is ROOT-CAUSED
  and fixed: it was NOT regulators and NOT the block path. FreeBSD allocates
  userland from the TOP of RAM, i.e. the second GiB — which stage2_init left
  as a flat XN 1 GiB block (needs_split gated tables to the first GiB only),
  and `stage2_wx_flip()` rejected any IPA with `l2_idx >= 512`. The first
  userland exec fault (ESR `0x8200000d`, permission fault L2; ELR `0x274618`,
  a user VA — read straight off the per-core window from commit 4931822) had
  no owner, and the guest span that one instruction at ~45 kHz on CPU3
  forever. Fix: every DRAM 1 GiB block now gets its own `stage2_l2_dram[]`
  row (`STAGE2_DRAM_L1_BLOCKS`), `needs_split` is gone, and `stage2_wx_flip()`
  indexes block-then-entry. The five hardcod window copies were already
  unified by `45ac193`.
- The stale `hv_addrmap.h` claim that `0xC0000000` was "confirmed live" on the
  board is corrected: the confirming fault was under QEMU; on the board the
  top ~7 MiB is U-Boot's.
- Ops hardening from the same night: `emac_link_watchdog()` now also heals
  "link dropped AFTER boot traffic flowed" (the old "one RX frame ever =
  healthy" test went permanently blind to that), re-arms itself after giving
  up, and resets its attempt budget on a successful re-kick; and the per-core
  fault window is zeroed at boot (it lives in DRAM and survived WDOG resets,
  showing a phantom 33M-count storm on a healthy boot). One WDOG-reset boot
  did still land EMAC-dark and needed the USB-ACM break-glass
  (`\x00~BZRST\x00`); with the watchdog fixes such a boot should now
  self-heal within ~a minute.

Residual, pre-existing, unchanged: W^X flips race if two guest vCPUs fault
into the same 2 MiB block concurrently (stage2.c's CONCURRENCY NOTE still
says "only CPU0" — stale since vcpu1). Not observed; worth a real
interlocked path if W^X ever becomes load-bearing.

**Why anyone cares**: the board is its own build host, and a single Mesa NIR
generator peaks at **648 MB** (measured, `time -l`, `max RSS 663020 KB`, 740 s,
exit 0 alone) against ~850 MB of usermem. So Mesa cannot be built at any `-j`,
and giving the guest a third vCPU made it *worse* — `ninja -j3` tripled peak
memory on a machine with under a gigabyte. There is a two-phase build script at
`/opt/mesa-2phase.sh` on the guest that carries these numbers in its own header.

### 3c. Passing dark hardware to the guest

The reframing that matters: **`bananapi-min.dtb` already carries the full
upstream node set** with correct clocks, resets, regulators and pinctrl.
"Minimal" describes which nodes say `status=okay`, not which nodes exist.
Verified — `mmc@1c10000` (the AP6212's SDIO controller), `usb@1c1a000`,
`usb@1c1a400`, `usb@1c1b000`, `usb@1c1b400`, `codec@1c22e00`,
`codec-analog@1f015c0` and `ir@1f02000` are all present, just disabled. So
enabling a device is **one status flip**, not a node to author.

`gen_config.py` gained `<soc-nodes>`: a dtb-only feature flips node status in
both directions, self-healing. `board-config.xml` has one feature per device,
all **off**. Design and per-device findings: `docs/guest-hw-enablement.md`.

| Device | Reality | Verdict |
|---|---|---|
| **USB host port 1** | `generic_ehci_fdt`/`generic_ohci`/`aw_usbphy` all compiled in; `awusbphy0` already attaches live | **DONE 2026-08-27.** One DTB flip (`guest_usb_host1`): `ehci0` attaches (irq 24), `usbus0`/`usbus1` up, and a real plugged **Terminus Technology hub enumerates at 480 Mbps** (`ugen0.2`). The feared INTID-106 storm did NOT appear (storm-BC zero, no kHz-rate counter in the g_gt sweep, MUSB word 37 stays `0x2`) — with the driver attached, the guest services the line and the old 145 kHz no-driver storm cannot form. Port 0 correctly stayed disabled (`no driver attached`) |
| **USB host port 0** | shares PHY0 with MUSB | **no switch exists.** MUSB carries the CDC-ACM console and the break-glass reset; a hard `validate()` error in `FORBIDDEN_DTB_NODES` refuses it |
| **Audio** | real drivers, already compiled (`pcm0` attaches today) | **DTB problem, not a driver problem.** Attach-only landed; the third (DAI) node is the live path to an unguarded DMA engine and is deliberately excluded |
| **IR** | `aw_cir` compiled in, binds via a *fallback* compat string | cheapest to verify |
| **WiFi** | source present but shipped `BRCMFMAC_SDIO=0`, `BRCMFMAC_OF=0`; firmware absent from FreeBSD, though the exact board-matched blob exists in the host's Linux tree | needs a **guest kernel rebuild**, not a switch |
| **Bluetooth / MIPI-DSI / MIPI-CSI** | no driver anywhere in FreeBSD (`sys/netgraph/bluetooth/drivers` has only USB transports; no DSI or camera driver for this SoC) | **closed as absent**, with the absence checks cited. Stop planning for them |

### 3d. Merged, gate-green, never validated on hardware

Each of these needs one board cycle. None is armed by default except where noted.

- **MUSB storm fix** (`745b58d`, in the default build): the storm was **our own
  regression** from `6cf4215` — endpoint interrupt enables were added and their
  status registers `REG_INTTX`/`REG_INTRX` were never read, on a level-high
  aggregate line. **VALIDATED live 2026-08-27** on the 4-vCPU 2G board:
  breadcrumb word 37 (`0x50000094`) latches only `0x2` (the benign EP0 TX bit),
  the storm-BC window reads all zeros, no counter in a full `g_gt[]` sweep
  moves faster than the 100 Hz tick, and the CDC-ACM console ran flawlessly
  through an entire night of interactive use. The **break-glass channel** was
  exercised for real twice (two `\x00~BZRST\x00` WDOG resets, both recovered
  the board) — validated.
- **HDMI vblank counters** (`97b16e1`): `hdmi_wrong_core` and `hdmi_throttles`
  now published in `BC_HDMI_BASE` words 13/14 — they existed but had no live
  export, so the numbers previously used to rule those causes out were a
  snapshot nobody could re-take. **VALIDATED live 2026-08-27**: vblank rate
  measured **60.8 Hz** with `hdmi_wrong_core=0` and `hdmi_throttles=0` —
  exactly the "if the MUSB fix works, the vblank rate should move toward
  60 Hz on its own" prediction (same-GIC-priority contention on CPU1
  exonerated as resolved).
- **GDB**: the "channel goes dark" root cause is fixed — `command_loop_ex()`
  kicked a **16-second** hardware watchdog once per complete RSP packet, so any
  human pause between commands reset the board. Same cause as
  "pause-gate + armed Z0 kills the board ~15 s after release". Both closed.
  `el2_exc.c`'s GDB divert is now gated on CPU0. Validation: stop at a
  breakpoint, then **sit at the gdb prompt doing nothing for 30-40 s** —
  pre-fix the board reset within ~16-20 s. Use the `make gdb` build.
  **Still open, deliberately**: the `gdb` runtime command is a no-op in the
  default build because the vcpu1 tick block never wires `gdb_channel`. Wiring
  it needs a real rework first — `gdbstub.c` reads and writes `sp_el1`,
  `mdcr_el2` and `mdscr_el1` raw (lines 378/393, 132/134, 140/142), all per-PE
  banked, safe today only because they run on a core that never executes guest
  code. Wire the tick in and a GDB read of "the guest's SP" silently returns
  CPU1's own guest's `SP_EL1`: a plausible wrong answer, worse than obvious
  garbage. Needs the queue-and-let-CPU0-apply-it pattern `gdb_hw_op_pending`
  already uses.
  Note also `Z0` and `Z1` breakpoints were **already fixed** on 2026-08-21
  (`3c28d5e`) — older notes calling them broken are stale.
- **`wdogtrap.c`**: policy for the page holding CCU, PIO and the watchdog
  registers (all three share one 4 KiB page, L2 block 14 / L3 index 32, pinned
  by a `_Static_assert`). WDOG writes refused, PC5 forced to func3
  byte-partial-correctly, rest passed through. **The stage-2 table edit itself is
  behind a default-off flag** because that page is on the critical path for eMMC
  clocking and its trap volume is unknown. The guest's watchdog node is disabled
  in the DTB *deliberately* (`PROGRESS.md`, 2026-07-15: FreeBSD's `aw_wdog(4)`
  was attaching and disabling our only automatic recovery path) — **leave it
  disabled**. Note that this closes the driver-attach path only; `/dev/mem` and
  a custom module are still open, on a board with its own root shell.

### 3e. Deliberately parked

- **Mediating the shared controllers.** GICD is **already half-mediated** and I
  was wrong to say otherwise: `stage2.c:315` leaves its page invalid, so guest
  accesses trap, and `vgicd.c` polices `GICD_ITARGETSR` and `GICD_SGIR`. Reads
  and all other writes pass through. Full per-guest GICD virtualization (enable,
  priority, group state) is **multi-week** and is what a second guest with real
  devices needs. The CCU is the hard one — machine-wide, no narrow fix found,
  its own design pass. PIO is the one cheap win (one field). RSB's
  "one bit for dldo1" is deceptive: it is a serial bus protocol, so a stage-2
  trap can only see "the guest touched the controller", not "the guest is about
  to clear DLDO1" — that hack stays regardless. See `docs/wdog-ccu-pio-stage2.md`.
- **Copy-on-write snapshots.** The 2× memory cost is an artefact of
  `snapshot.c` copying guest DRAM eagerly, not of snapshots. COW would cost the
  pages written since the snapshot plus ~2 MiB of tables per GiB, and the
  fault-and-fixup machinery already exists and is hardware-proven as
  `STAGE2_WX_DYNAMIC`. Blocker: guest DRAM is mapped in 1 GiB blocks and COW
  needs page granularity — which is also why the dynamic-W^X pool is a fixed 64
  tables rather than a full shatter. This would retire the memory-vs-snapshots
  trade entirely.
- **PinePhone.** Same A64 die, so no new SoC — but no Ethernet at all, so the
  whole EMAC-based debug plane must move to USB, and MIPI-DSI off TCON0 replaces
  HDMI off TCON1. Note "the whole Banana Pi family" is not one family: only the
  M64 is 64-bit among the Allwinner ones (H3, H2+, A83T, V40 are all Cortex-A7,
  32-bit — an AArch64 EL2 hypervisor does not port there at all).
- **`pkg`'s database.** Root-caused: genuine b-tree corruption from an
  interrupted install of mine (`PRAGMA integrity_check` → rowid out of order,
  rows missing from an index), combined with a real missing-error-check bug in
  pkg's own `pkgdb_load_files()` (`pkgdb_iterator.c:414` loops
  `while (sqlite3_step(...) == SQLITE_ROW)` with no exit-status check, unlike the
  block right after it). Decision: **leave it alone.** pkg works for the other
  121 packages; python312 and meson are already repaired at the file level. Just
  do not run pkg operations on those two and do not run `pkg check -s -a`. A
  verified `.recover` procedure is written down if it is ever wanted.
- Base-system file integrity is **unknown**: the corruption sweep covered only
  pkg-managed packages with an exact-version cached reference. `/lib/libc.so.7`,
  `ld-elf.so.1` and pkg's own binary have no reference checksum at all because
  this is a custom buildworld. Neither confirmed nor excluded.

---

## 4. Traps that cost real time today — read these

1. **`make dbg` does NOT refresh the uImage.** `hv-uimage` is a separate
   `.PHONY` target and has to be (it depends on the `dbg` target so `HV_HDMI`'s
   target-specific CFLAGS propagate). `uboot_setup.py` programs the board's own
   saved bootcmd as `tftpboot microkernel-dbg.uimg && bootm`, so every reset the
   board performs by itself fetches that file. A warm reset also preserves DRAM,
   so a hypervisor already resident at `0x42000000` keeps running **and keeps
   publishing its own build-id**. Net effect: a board quietly executing an older
   hypervisor while the tree says otherwise. I diagnosed a stale image's stage-2
   tables for ten minutes as though they were code I had just written.
   `reliable_load.py` now refuses to load when the uImage is older than the
   binary — it fired for real within the hour. **Use `make hv-uimage`.**
2. **`triage.py`'s BUILD IDENTITY block is first for a reason.** Read it before
   believing any memory, table or counter you read off the board.
3. **A gate that flakes trains you to ignore it.** The dual-guest stages bounded
   each QEMU run with a flat timeout and went red under host load — the same
   commit gave red and green on consecutive runs. They now wait on *progress*
   (the firmware emits a workload-independent heartbeat) with a hard ceiling.
   And some stage scripts print an internal `FAIL` line that is **not** their
   verdict: `dual-qemu-ci.sh` prints `DUAL-QEMU-CI2: FAIL (cpu3 did not
   advance)` and then `PASS`, because in that case CPU3 *not* advancing is
   correct. **Judge by exit codes and the `^ci: ` verdict lines**; there is now a
   `ci: SUMMARY` line naming any failed stages.
4. **`ktrace` names the last syscall before a SIGSEGV, not its cause.** A
   userland process faults between syscalls, so adjacency proves nothing. This
   sent me hunting our own W^X mechanism for a corrupt-file problem.
   `guest_memtest.c` now settles that question in ten seconds — six escalating
   steps ending in write-bytes / `mprotect R|X` / *call it*, which is the only
   one that forces a promotion.
5. **Never give `reliable_load.py` a short timeout.** It needs minutes, and
   killing it mid-flight leaves the board unreachable. I did that too.
6. **Do not run `fsck` on a mounted filesystem via `timeout`.** A killed
   `fsck_ffs -n` left a filesystem suspension in the kernel (`DN suspfs`) and
   three subsequent `mount` attempts piled up in `D` state behind it.
7. **`/opt` took real damage** (`panic: ffs_valloc: dup alloc`, `fsck` recovered
   1555 orphaned files) most likely because the board was reset four times with
   it mounted read-write under an active build. Cannot be attributed cleanly —
   some damaged inodes carry mtimes from *during* a three-core run, so
   three-way concurrency on the `vblk_sd` path is an unexcluded second suspect.
   **The experiment that separates them**: build with no resets at all, then
   check `/opt` afterwards.
8. **Guest network latency is architectural, not a fault.** 40/40 packets, zero
   loss, but RTT 0.47–9.4 ms (mean 4.2) against a 0.17–1.6 ms gateway, because
   virtio-net RX is drained from CPU1's 10 ms tick.
9. **Watch the shell when writing commit messages.** Backticks and apostrophes
   inside a double-quoted `-m` get eaten or break the quoting; `--` inside an XML
   comment is illegal. All three bit me. Write the message to a file and use
   `-F`.

---

## 5. Where things live

| | |
|---|---|
| Board lifecycle | `bzdctl.py` (status / console / power / crash / ledger) |
| First diagnostic, always | `python3 triage.py` |
| Load a build | `make hv-uimage` then `reliable_load.py --cycles 1` (after `board_ctl.force_to_uboot()`) |
| Boot-streak gate | `boot_streak.py` — uses the recovery ladder that works, not `reliable_load --cycles N` |
| Board-free gate | `./ci.sh` — read `^ci: ` lines and `ci: SUMMARY` |
| Guest shell | ssh, or `guest_sh.py` over the console |
| Feature switches | `board-config.xml`, then **always** `python3 gen_config.py` |
| Mesa build on the guest | `/opt/mesa-2phase.sh`; option set in `../bsdOS/hal/lima/mesa/FEASIBILITY.md` — do not re-derive it |
| Crash evidence | `crash-<timestamp>/` (gitignored; `finding.md` inside is tracked) |

`gen_config.py` is the single reconciler for build flag ↔ DTB. It now handles
`cpu@N` nodes, virtio nodes, `/memory` size and SoC node status — **in both
directions**. It learned that the hard way twice in one day: a one-directional
handler that only ever adds is how you get a DTB promising a core or a gigabyte
the build will not honour.
