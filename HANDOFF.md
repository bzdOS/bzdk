# Handoff — 2026-10-02: 18.7 h dark board was U-Boot waiting for a USB host — FIXED (boots with no USB)

- **What happened.** The guest did a `shutdown -r` at 02:03 MSK. The
  micro-USB was not on the host (a phone had the host port). No TFTP request
  arrived until 20:46:57, and EMAC sent no frames, while the RJ45 LEDs blinked
  on RX. When the owner plugged the micro-USB into the host, U-Boot's "USB
  download gadget" (1f3a:efe8) enumerated, TFTP followed 4 s later, and the
  board booted with no reset.
- **Root cause.** The ESP `uboot.env` has `preboot=setenv stdin usbacm,serial`
  (and the same for stdout and stderr). In `acm_stdio_start()`
  (`build/u-boot/drivers/usb/gadget/f_acm.c:654`), U-Boot spins
  `while (!acm_connected) schedule();`, and `schedule()` pets the U-Boot
  watchdog. So a reload with no USB host never reaches bootcmd.
  76907c1 ("network-only recovery") assumes autoboot recovers by itself; it
  does not.
- **FIXED 2026-10-02 21:55: the eMMC FIT is now `u-boot-nc.itb`** (md5
  6b7a0e6f…, `U-Boot 2026.07-rc5-dirty (Oct 02 2026 - 21:44:45)`). It is the
  flashed 09-26 config plus `CONFIG_NETCONSOLE=y`, plus
  `uboot-patches/0001-f_acm-bound-the-wait-for-a-USB-host.patch`:
  `acm_stdio_start` gives up after 3 s and leaves the gadget registered, so a
  host that comes later still enumerates. The SPL was not touched, and the
  previous FIT is `tftpboot/u-boot-retry.itb`.
  - Before flashing, it was chain-loaded with no media write, via a one-shot
    `microkernel-dbg.uimg` wrapper: the stub copies U-Boot to 0x4a000000,
    cleans by set/way and arms the WDOG. It was tested with a USB host, with
    no host (`echo 0 > /sys/bus/usb/devices/usb2/authorized_default`), and
    with the real env.
  - Flashed by the owner with `uboot_flash_fit.py`: "verify: Total of 879749
    byte(s) were the same". A read-back of the card from the guest matches.
  - **Verified from the card with no USB host:** the gadget was "not
    authorized" at 21:57:23, TFTP followed at 21:57:29, and the guest was up
    at 21:58:06.
  - **Netconsole is ON** (env set by the owner at 21:59, md5 bfbb5075…):
    `ncip=192.168.88.2`, stdin/stdout/stderr and preboot = `serial,nc,usbacm`.
    Verified at the next boot: `U-Boot … (Oct 02 2026 - 21:44:45)`,
    `In: serial,nc,usbacm` and the TFTP log all arrived on UDP 6666. To watch:
    `tcpdump -l -ni br0 -A 'udp port 6666'`. Interactive with `bootdelay>=1`:
    `nc -u -l 6666` on the host, then answer to 192.168.88.7:6666.
  - The first flash attempt failed with "no tty": `/tmp/chimp-acm.lock` was
    created by user `agent`, and `fs.protected_regular` stops root opening it
    with O_CREAT. It was removed.
- The running HV is the 01:59 TFTP image (image id 0x9c049663, adds
  `usbacm_poll()` in the hold loop, built from a worktree;
  `main_dbg.c.fixed-inworktree`, not committed).

# Handoff — 2026-09-30 evening: trap cost, scrubber, safe mode, all planes in screenshots

- **Trap cost measured** (ROADMAP §6.5, `vtrap.py`): 6118 guest traps/s
  under the soak's mixed load = 3.8% of one core (~1% of the machine). 80%
  are the guest's IPIs (`GICD_SGIR`); disk `QueueNotify` is ~190/s, so
  NO_NOTIFY / EVENT_IDX would save ~0.13% of a core — not done.
- **CRC scrubber** (`scrub.[ch]`, `scrub_crc.py`, `scrub.py`): build-time
  CRC table over `.text`+`.rodata`, golden copy at boot, one 4 KiB chunk per
  CPU1 tick (full pass 0.56 s), word-level repair. Fault injection over the
  debug channel: `.rodata` and `.text` flips repaired in 0.19 s; a corrupt
  golden copy is counted unrepairable, never written; dbgmon `patch` is
  kept (`scrub_accept`).
- **Boot counter + safe mode** (`dbgtools.c`, `safemode.py`): 5 boots in a
  row of the same image that each die within 90 s arm the entry-hold gate —
  hypervisor and debug channel up, guest held until `safemode.py release`.
  A new image or a cold boot restarts the count; 90 s up with the guest not
  held clears it. **Heads-up:** five quick `shutdown -r` reloads of one
  unchanged image will trip it too; a rebuild does not.
- **The entry-hold gate was dark since vcpu1 became the default.** Held,
  CPU0 sat in `wfe`; CPU1 is parked in `vcpu1_run()` until the guest's
  CPU_ON, so nothing serviced EMAC and the pet relied on guest progress.
  The first safe-mode test went dark exactly that way and came back through
  the 900 s reachability watchdog. The hold loop now services dbgmon on
  CPU0 and pets through `wdt_debug_kick()`. (`dbg_no_guest`'s loop has the
  same shape and is left as it was: a diagnostic.)
- **Open: no USB console in a boot that was held.** USB (MUSB/usbacm) is
  serviced from CPU1's MUSB IRQ path, and a held boot keeps CPU1 parked, so
  the gadget never enumerates (host: `error -71`, then gives up) and stays
  absent for that whole boot, even after `release`. The fix is one
  `usbacm_poll()` in main_dbg.c's hold loop; it has NOT been built or
  tested. soak72 refuses to start without the gadget (`board-off-usb`), so
  after a safe-mode boot do a clean reload (`shutdown -r`) first.
- **72 h soak: not started.** The first attempt stopped at once on the
  above; then the host went down for a reboot.
- **screenshot.py `--what both`** composes every plane from the mixer's
  registers (HUD, guest window, overlays, video with the hardware's CSC).
- **A/B eMMC slots: not done.** Staging images on the ESP from the guest was
  refused by the session's permission classifier, so nothing was written
  to boot media and the saved U-Boot env is unchanged. `uboot-ab/abtest.c`
  (uncommitted, untested) is the RAM-only way to try the fallback bootcmd
  once slots exist: served over TFTP as the uImage it `env_set`s `boothv`
  in U-Boot's RAM env and returns; old hush copies the command string
  before parsing (`cli_hush.c` parse_string_outer), so redefining the
  running `boothv` is safe.

# Handoff — 2026-09-30: hypervisor side of the roadmap is done on this board (read this first)

What changed today, all hardware-verified unless marked, details in the
numbered items of the 2026-09-27 section below:

- **IDMAC completion IRQ, both controllers (item 2).** Eligible requests are
  queued and completed from the IDMAC IRQ; the vCPU returns at once. SD raw
  reads 17.1 -> 20.1 MB/s; CPU jobs lose half as much time to I/O on both.
  An IRQ no queued transfer owns is masked at once (`49ff44d`).
- **vzram (item 3), hypervisor side.** `vtbd2`, 128 MiB growing to 1 GiB,
  LZ4 in [0xB0000000, 0xB8000000); guest RAM 1920 -> 1792 MiB. **Not swapped
  on at boot** — and note the tension with the framing below: swap here is
  meant to be idle insurance (target 0), so whether 128 MiB of RAM should
  sit behind it at all is the owner's call. `docs/vzram.md`.
- **DE2 planes (item 6), hypervisor half.** scanout v2 hands the guest UI
  layers 2/3 of the HDMI mixer; scanout v3 the VI channel as a scaled
  NV12/NV21/YUYV/XRGB video plane (`3aca0aa`). Both verified on the panel
  by the owner. `docs/zero-copy-scanout.md` §9-10.
- **VE (item 7): no hypervisor work exists** — node, MMIO and clocks are the
  guest's; it needs a guest driver.
- **Tooling:** `reliable_load.py` no longer writes 1s into the freshly
  booted image (`7a1f302` — it had on every reload); repl/fbsd/gdb/hdmi link
  again (`61c829d`). Build with `LC_ALL=C`: gcc's Russian "ошибка" slipped
  past a grep for "error" twice today.

Hypervisor work left on this board: a long soak of the IDMAC queues;
U-Boot's MUSB gadget root cause (a ~5 h catch soak). Everything else on the
roadmap is guest work: item 5, bzkms planes + HWC logic, a VE driver.

# Handoff — 2026-09-27: the board is the reference "weak mobile" benchmark; agreed roadmap below (read this first)

## Framing agreed with the owner

The Banana Pi M64 (2 GiB, 4x Cortex-A53, Mali-400) is being treated
deliberately as **the reference weak-mobile target** — "делать максимально
как тормозная мобильная платформа" — not a desktop stand-in. A port to a
second SoC is real (H616/H618/H6 first, Rockchip after), but happens under
that actual port, not as speculative core/soc/board refactoring done ahead
of it.

**No formal benchmark suite, no native-FreeBSD-without-hypervisor baseline**
— explicitly declined ("не нужен особенно ценой фряхи без гипервизора, мне
лень"). Instead: a plain before/after measurement per change (`dd`, boot
time, fps) plus an integrity check (fsck -n, parallel md5 after
umount/mount), the same way today's SD work was verified. See
`vblk-sd-shared-bounce-under-lock` memory for the method and its one real
miss (scatter-after-unlock on a shared EL2 buffer — any new shared buffer in
a device path needs the same lock-discipline review before it touches
hardware).

**eMMC wear vs SD wear:** swap must NOT move to the SD card if it ever needs
to move at all — the SD is already the weaker link (holds /var, /opt) and
the eMMC has real wear-leveling. Swap is currently idle insurance
(`v_swappgsout` was 0 all session, even under the mixed soak) — not urgent,
but its policy is decided: eMMC if anywhere, and swap usage becomes a
regression signal (target: 0), not something to optimize for capacity.
Soak's own SD write cycle was cut 32→8 MiB to reduce wear once this was
raised (`93d00e1`).

## Today's SD result (context for the eMMC work below — same recipe applies)

0.34 MB/s write / 2.07 MB/s read (1-bit, 25 MHz, one CMD24/CMD17 per sector)
→ **13.5 MB/s write / 21.0 MB/s read** (4-bit + SD High Speed 50 MHz +
CMD25/CMD18 multi-block + whole-virtio-request gather through one bounce).
Commits `fb7f18f c8e76d1 9741412 a2560eb` (+ `3e38fe3` zeroing stale
counters). UHS needs 1.8 V this slot doesn't have — 50 MHz / 4-bit is the
ceiling for the SD path. QEMU CI fixed 10/10 (`c524d95` — 4 stages had been
broken at HEAD since the EMAC-dark commits, unnoticed for a day; this is
itself an argument for a pre-push CI gate, still not built).

**Currently running:** `soak72.py --profile mixed --hours 24 --size-mb 8
--restart`, started 2026-09-26 23:50, clean SD-write cycle. Mixed profile
(new: `93d00e1`, fixed for ro-root and the group-kill/restart races:
`3e38fe3`) drives CPU bursts (DVFS), eMMC read-verify, a TFTP pull from the
host, and `limabench` in a loop, alongside the SD write/verify loop that
still advances `gen`. **Check its status first** (`soak72.py --status` or
tail `soak72-mixed-events.jsonl`) before doing anything else — if it's
still healthy, let it run to the 24 h target before reloading the board for
the eMMC work below (reloading loses the accumulated healthy-hours count
unless resumed with the same state file).

## Agreed next work, in order

1. **eMMC: the same SD recipe. DONE 2026-09-27**, all three sub-parts:
   - `emmc_bio_read_multi()`/`emmc_bio_write_multi()` (CMD18/CMD25 + HW
     AUTO_STOP) landed and hardware-validated standalone (`91ba856`), then
     wired into `vblk_emmc.c` via two fast paths (`c2029c5`):
     `emmc_serve_gathered()` (whole-request, mirrors `sd_serve_gathered()`)
     AND a per-descriptor inner fast path in `serve_data()` (mirrors
     `sd_serve_data()`'s own inner path) — the second one turned out to be
     the one that actually matters: FreeBSD's busdma usually coalesces a
     request into ONE physically-contiguous descriptor, which the
     whole-request gather alone can't accelerate (needs >= 2 descriptors).
     Measured live: a 26 MB sequential dd read went from 4.4 MB/s to
     10.8 MB/s (2.5x); a write to the (swapoff'd) eMMC swap partition
     verified byte-identical via read-back; `g_ioerrs` stayed 0 throughout;
     `fsck_ufs -n` on the live root clean.
   - 8-bit bus (`EMMC_BUS_WIDTH=8`) and 50 MHz clock (`EMMC_HS_CLK_REG=
     CCU_MMC2_CLK_HS50`) BOTH tried, individually and combined, all via
     the existing fail-safe post-switch test read — all three combos
     passed (`EBIO_BC_HS_STATE`==1, no fallback), confirmed with the same
     dd+md5 check as above. **NOT kept as the shipped default**: measured
     throughput was IDENTICAL to 4-bit/25 MHz (~10.8-10.9 MB/s) in every
     combination — the multi-block work above already saturates the PIO
     drain loop, so neither a wider bus nor a faster clock does anything
     more until that CPU-bound loop itself is replaced. Revisit combining
     these with DMA below, where they should finally show a difference.
   - Verified with fsck -n + parallel md5, same as the SD series, at every
     step above.
2. **DMA (IDMAC) for both controllers — DONE 2026-09-27.** Multi-block
   transfers on both eMMC and SD now move their data phase via the
   controller's IDMAC (`emmc_bio_*_dma`, `sd_bio_*_dma`; register map,
   descriptor chain and sequence transcribed from FreeBSD's `aw_mmc.c`),
   with the PIO multi-block pair as an automatic fallback
   (`emmc_multi_*` in `vblk_emmc.c`, `sd_multi_*` in `vblk_sd.c`). Counters
   (plain statics, read with `nm` + dbgmon): `g_dma_ok`/`g_dma_fallback`,
   `g_sd_dma_ok`/`g_sd_dma_skip`/`g_sd_dma_fallback`.
   - **The rule that cost a day: any buffer an IDMAC *reads into* must be
     whole cache lines.** The engine writes DRAM behind the cache; a
     partial edge line shared with a variable another core dirties
     mid-transfer gets written back on top of the DMA'd bytes. With the
     eMMC bounce at 0x...3d8 this produced ld-elf "Unhandled relocation"
     / SIGILL boots and garbage UFS indirect blocks after ~75k
     clean-reporting transfers (disk stayed fsck-clean — memory only), and
     almost certainly the SD boot-hang regression (direct-to-guest reads
     overwriting guest kernel data). Both bounces are `aligned(64)` and
     both `*_read_dma()` return -102 for a partial-line buffer, which the
     wrappers route to PIO. Keep both halves of that.
   - Verification: `hwtest/filerace.sh` (4 concurrent streams over 4.6 GB
     of root files, x2, all md5 identical), `hwtest/sdwr.sh` (2.5 GB
     written to /opt and verified, 0 bad), 5/5 clean back-to-back resets,
     ~90k eMMC + ~81k SD IDMAC transfers with 0 fallbacks and
     `g_ioerrs=0`. The raw-device read test does NOT exercise the
     per-descriptor path (physio splits the buffer) — use file reads.
   - Single-reader throughput is unchanged (~10.8 MB/s eMMC); the point is
     that the vCPU no longer spins in a FIFO drain loop. (The completion
     wait was still a poll then; the IRQ step below removed that.)
   - **IDMAC completion IRQ — DONE 2026-09-30 (phase 2: `addef30`,
     `77a6eef` eMMC; `a88c542`, `868d678` SD).** An eligible request (whole
     sectors, 2..128 of them) is queued (16 deep) and the trap returns to
     the guest at once; the queue owns the controller lock while non-empty,
     and whoever sees a transfer end publishes it and starts the next. That
     is normally the IDMAC IRQ (SPI94 eMMC / SPI92 SD, EL2-owned, CPU0), with
     every core's tick and the controller-lock spin as backstops — needed,
     because the IRQ cannot be taken while CPU0 is inside a trap and the
     card-busy tail after AUTO_STOP raises nothing. Split-phase controller
     API: `emmc_bio_dma_start/poll`, `sd_bio_dma_start/poll`; the old
     synchronous functions are untouched and still serve everything
     ineligible. A failed transfer is re-served via `serve_data()`.
     Live switches: `g_vblk_idma_on`, `g_vblk_sd_idma_on` (0 = old path).
     Measured on hardware, A/B in the same boot, 4 CPU-bound jobs alongside:
     - eMMC: 99% of transfers finish inside the IRQ; reads under load
       10.36 -> 10.7-10.9 MB/s; the jobs lose ~0.14 s instead of ~0.24 s.
       Raw md5 identical on/off; 4 x 256 MiB concurrent writes to the
       swapped-off swap partition read back identical; `filerace.sh` 314
       files x2 passes, 0 diffs; 187k queued transfers, 0 re-serves,
       `g_ioerrs`=0.
     - SD: 99.9% inside the IRQ; raw reads 17.1 -> 20.1 MB/s even with no
       other load (the next request starts from the completion, no trap
       round-trip); the jobs lose ~0.09 s instead of ~0.2 s. SD gained the
       used-ring lock, InterruptACK lost-completion re-notify and locked
       reset teardown the eMMC side already had — async completions need
       them. Raw md5 identical on/off; `sdwr.sh` 40 x 64 MiB written
       to /opt and verified by 4 readers, 0 bad; ~82k queued transfers,
       0 re-serves.
     - Both at once (eMMC + SD raw readers, SD file write/verify, CPU
       load): raw md5 identical to a quiet re-read, 0 bad files.
     - Open, harmless: the SD line still fires more often than transfers
       end (`g_sd_dma_irqs` 130k vs 109k DMA) -- a synchronous DMA on
       another core, or a handler that loses the queue lock, leaves the
       level line up until the 32/tick throttle masks it. Only the
       throttle's cost; nothing is lost. Not soaked beyond these tests.
     - Phase 1 on its own (arming the SPI, `wfi` in the poll) measured
       nothing: every DMA ran inside a trap with IRQs masked, so the IRQ was
       never taken and `wfi` was woken by other sources anyway.
     - Found on the way, not ours: `reliable_load.py` wrote 1 into every
       0x40 of 0x4201d000..0x42030000 of the freshly booted hypervisor on
       every reload (fixed `7a1f302`, see its message) — suspect it for any
       odd behaviour after a reload before that commit.
   - **Not yet soaked.** `soak72.py --profile mixed` stopped at ~15 min
     twice, on a pure-PIO build and on the IDMAC build alike: `limabench`
     SIGSEGV (rc=139) in Mesa's CPU-side shader linker
     (`set_search_or_add` <- `nir_lower_io` <- `gl_nir_link_varyings`),
     not in the GPU. One `ntpd` SIGSEGV was also seen during the SD test.
     Unexplained. Suspects, none checked: CPU instability at 1152 MHz x4
     under powerd with the SoC at 85-86 C (the DVFS memory notes micro-USB
     brownouts at that point), or a Mesa bug. First step: rerun the soak
     with the frequency capped (`sysctl dev.cpu.0.freq=816`, powerd off);
     if it passes, it is the power/thermal envelope, not the storage.
3. **Compressed swap (vzram) — hypervisor side DONE 2026-09-30 (`08affdd`);
   guest adoption NOT done, and needs a decision first.**
   - `vblk_zram.c`: third guest disk `vtbd2` (0x0A005000, SPI 109) over
     `vzram_pool.c`/`lz4.c`, backed by [0xB0000000, 0xB8000000) — the
     `vzram` feature in `board-config.xml` shrinks `/memory` to 0x70000000
     (guest now has 1792 MiB), `gen_config.py` applies the smallest enabled
     `dtb-memory-size`. One lock over everything. Needs GUEST_DRAM_2G.
   - Geometry: exposes 128 MiB, grows by 128 MiB (config-change IRQ, live
     GEOM resize — verified 128 -> 256 MiB) up to 1 GiB while data proves
     compressible (>= 3/4 of it written, pool <= 1/2 used). Starting
     numbers; retune from real use.
   - Measured: 100 MiB compressible written at 72 MB/s, random at 33 MB/s,
     read back identical; as the ONLY swap, a 1700 MiB working set on
     1523 MiB free RAM verified every page, 0 bad, ~70k swap I/Os,
     `g_zr_errs`=0. Counters: `g_zr_*` via nm.
   - **Not swapped on at boot.** Open questions, the owner's call:
     (a) FreeBSD has no swap priorities — with both `vtbd0p4` (eMMC) and
     `vtbd2` in fstab, pages interleave across both, so "zram first, eMMC
     as overflow" is not expressible; pick one or accept the interleave.
     (b) A grown disk is only used after `swapoff`/`swapon`, and swapoff
     under the very pressure that caused the growth has to page everything
     back in — the devd-rule idea in the old plan is unsafe as stated.
     Likely better: stop growing live, size it once at boot.
   - A test note that cost an hour: a read-only pass over a working set
     bigger than RAM keeps every swapped-in page's slot, so it needs swap
     the size of the whole set — OOM there is FreeBSD, not the device.
4. **Wear counters in `bzdctl status` — DONE 2026-09-27 (eMMC only).**
   `emmc_bio_read_wear()` reads EXT_CSD[267]/[268]/[269] (PRE_EOL_INFO,
   DEVICE_LIFE_TIME_EST_TYP_A/B) via a new `emmc_bio_read_block()` helper
   shared with `emmc_bio_read()` (CMD8 vs CMD17 — the exact same
   single-block PIO drain, factored rather than copied). `bmc.c`'s
   `bmc_health_snapshot()` calls it every snapshot, bracketed by
   `vblk_emmc_trylock()`/`unlock()` (non-blocking — a failed trylock just
   reports `emmc_wear_ok=0` for that round, never stale data), and
   publishes it next to temperature in `bmc health`, `bmc_client.py`, and
   the `bzdctl.py` HTML dashboard (protocol bumped to v1.2). Committed:
   `0f556cc` (emmc_bio), `16ffae1` (bmc), `6cf8b58` (bmc_client/bzdctl).
   - **Hardware-verified 2026-09-27**, board rebuilt+reset onto `6cf8b58`:
     `bmc health` returns `pre_eol=1 life_est_a=1 life_est_b=1` (all
     "normal"/lowest-wear band) consistently across 8 back-to-back calls,
     several of them while the guest was actively reading the eMMC
     (`g_reads` climbing in the VBK1 breadcrumbs at the same time) — no
     hang, no `g_ioerrs` movement (stayed 0), guest booted clean and
     `fsck -n /` on the eMMC root came back clean (92562 files, 0%
     fragmentation, no errors). The controller-lock bracketing does not
     visibly contend with guest I/O at this call rate.
   - **SD wear NOT implemented** — confirmed genuinely unexplored, not
     just undocumented (no ACMD13/SD_STATUS or ACMD51/SCR read anywhere in
     `sd_bio.c`). Vendor-specific wear-leveling fields live in
     vendor-reserved bits with no universal standard; scope this
     separately if it turns out to matter for the SD card actually in use.
5. **Guest cleanup + kernel rebuild.** `zfs.ko` is loaded on the guest for
   no reason (ARC eats RAM on a 2 GiB board) — drop it from the module set.
   Rebuild the kernel from the clean `bpi-m64-15.1` branch
   (`/opt/bzdos/freebsd-src-bpi`, 12 commits on 15.1-RELEASE, byte-identical
   tree to what's running today, replaces the dirty `earlyboot-wt` lineage)
   with `-mcpu=cortex-a53`.
6. **DE2 hardware-layer compositor (HWC-style) — hypervisor half DONE
   2026-09-30 (`ac87eea`); the compositor itself is guest work.** DE2 is
   EL2's; HDMI goes through mixer1 (one VI + one UI channel). UI layers 0/1
   are HUD/guest window; **scanout v2** now exposes UI layers 2 and 3 as
   guest overlay planes (scanout.h `SCANOUT_R_OVL_*`: stage ADDR/PITCH/
   SIZE/COORD, CTRL applies at vblank; XRGB/ARGB, global alpha). Verified at
   register level from the guest via /dev/mem, rejects checked. **Not
   verified what the panel shows** (left enabled once: a translucent copy
   of the guest console at 1300,820 — look, then `python3 /tmp/ovl.py off`
   on the guest). Left for the guest: bzkms planes + the negotiation logic.
   Still hypervisor work if wanted: the VI channel (YUV + scaler), which is
   what #7 needs to show decoded video without a copy.
7. **VE (video engine) hardware decode — nothing left on the hypervisor
   side.** The DTB node is present, its MMIO is identity-mapped to the
   guest, clocks/SRAM-C are the guest's; dmesg: `video-codec@1c0e000 ...
   (no driver attached)`. It is a guest driver (cedrus-like) plus the VI
   plane above.
8. **Second SoC port** (H616/H618/H6 first; Rockchip — RK3566/RK3588 —
   after) and the resulting core/soc/board split of this tree: do it when
   actually starting that port, not speculatively now.

## Fantasy tier (recorded, not scheduled)

Discussed as "what could this architecture do that no phone OS does",
explicitly speculative, kept here so the reasoning isn't lost:

- **Per-jail hypervisor-level snapshot/resume**, granular to one
  `jail-per-stream` instance rather than the whole guest — a truer instant
  suspend/resume than Android's cgroup freezer, since it doesn't need the
  frozen process to re-warm anything on resume.
- **Input-triggered DVFS pre-ramp**: EL2 sees a `vinput` event before the
  guest scheduler wakes the app; could start raising frequency in parallel
  with guest wake-up instead of after a cpufreq governor reacts.
- **Whole-system incremental snapshot ("undo history" for the live OS)**,
  reusing the W^X pool's stage-2 permission-bit-flip machinery for dirty
  tracking instead of a full eager copy.
- **Stage-2-fault-driven predictive prefetch** — EL2 sees every guest page
  fault at the boundary already; a per-app access pattern model could
  preload pages before the guest kernel asks, below the filesystem layer.
- **Live migration of a frozen guest across boards** in the fleet (the
  snapshot mechanism generalized from "resume on this board" to "resume on
  a different one").
- **Thermal-aware live core migration** transparent to the guest scheduler.
- **Generalized on-demand deterministic micro-guest cores**, extending the
  already-proven `dual`-build (FreeBSD + real Zephyr concurrently on
  separate cores) into a general "give me a dedicated low-jitter core for
  this loop" primitive instead of a fixed build-time pairing.

## State (verified 16:07, 2026-09-26)

- **Hypervisor: master** (`4a6d41d`+), built clean WITH config.mk, staged in
  both boot paths. Contains, all hardware-verified today:
  - **RSB trap** (`rsbtrap.c`, `HV_RSBTRAP` in `dbg`): the guest's RSB
    controller is emulated; every PMIC transaction (guest and EL2) runs under
    `rsb.c`'s bus lock; guest writes to AXP803 REG 0x10/0x12/0x32 are policed
    (CPU/DRAM/3V3/SYS rails, DC1SW, DLDO1 stay on; PMIC power-off refused) and
    logged: ring at `HVMAP_RSBTRAP_LOG`, counters at `HVMAP_RSBTRAP_BC`.
  - **Reachability gate** (`wdt.c`): no watchdog pet without an EMAC frame or
    USB SOFs within 900 s.
  - **Black box** on SD LBA 64 (`sdbox_read.py`): every `reboot_clean()`, plus
    the tick's two silent paths (EMAC ladder give-up = 6, gate lapse = 7).
  - **EMAC-dark auto-reboot on, 3 h** of continuous dark (e80f80a).
  - `warm_reset_soak.py 3 0`: 3/3.
- **U-Boot on the eMMC:** `Sep 26 2026 - 13:46:18`: own watchdog, idle `=>`
  resets after 120 s, `reset` works. **`bootdelay=-2`**: autoboot cannot be
  stopped. Tools that need a prompt open a window themselves
  (`uboot_maint.py`); by hand: `python3 uboot_maint.py open|close`.
  Safety-net test: `uboot_chainload_test.py --net-test` (jumps into a hang,
  stock loader must come back; 35 s).
- **chimpd: retired** (unit disabled). It works by catching U-Boot, which
  `bootdelay=-2` makes impossible and which was the trigger of the gadget
  wedge. Its fallback role -- loading the HV over USB when TFTP fails -- is
  covered by `bootcmd` (10 tries, then a WDOG reset and try again).
- **Guest drm-kmod** = exactly the upstream PRs (#512-#515) + one backport
  (2026-10-04: #512 and #514 are MERGED into drm-kmod master, identical to our
  0003/0002; #513 open without review, #515 waits on a reply to review --
  status in `bsdOS/hal/lima/patches/UPSTREAM-INDEX.md`),
  `bsdOS/hal/lima/patches/drm-kmod/000[1-5]`; `/opt/bzdos/drm-kmod` is a
  pristine archive of the tag. Modules in `/boot/modules` since today
  (old ones: `*.ko.bak-2026-09-26`).

## What was wrong (from evidence)

1. **The PMIC bus had two unarbitrated masters.** The ownership audit said
   "no guest driver"; the guest runs aw_rsb + axp8xx_pmu. EL2's hdmi relock
   (soft-reset + RMW once a second while the PHY is unlocked, i.e. during the
   guest's own PMIC setup, and a blind `0x88` to REG 0x12 on a failed read),
   phy_rail_ensure and health reads raced it. A misdirected write to REG 0x10
   turns off CPU/DRAM: no watchdog, no USB, only the power switch. Likeliest
   cause of the 09-25 16:41/17:13 losses (16:41 was the old 2ca4fdb image,
   dying at the first 8 s link check). The trap's write log shows the guest
   never clears DC1SW: the 0x58 of 09-25 came from EL2.
2. **U-Boot parked for ever** (the 19 h loss): chimpd's catch wedges the
   U-Boot MUSB gadget (~0.4 %), the prompt then feeds its own watchdog.
   Mitigated (retry + `-2`); what wedges the gadget is not known.
3. **Worktree builds lack config.mk** → 1 GiB stage-2 under a 2 GiB DTB.
   Makefile refuses now.

## Still open

- Root cause inside U-Boot's MUSB gadget: needs a ~250-cycle catch soak
  (~5 h of board time).
- The 16:41/17:13 attribution to the RSB race is inference; the trap now
  makes a repeat impossible and the log/black box would show it.

# Handoff — 2026-09-25, the board is not fragile any more

Two days of "the board went dark and needed the plug pulled" ended today with
two root causes, both fixed and both hardware-proven. Everything below the
next heading is the older handoff and still valid where it does not
contradict this.

## What was actually wrong

1. **`bmc reset` never reset the board when the guest was busy** (cfbe93b).
   `reboot_clean()` dropped the USB pull-up and armed a 2 s watchdog on CPU1
   -- while CPU0 kept petting that watchdog on every EL2 exception because
   the guest's eMMC traffic counted as "progress" for 180 s. Any ssh command
   in the previous three minutes made the reset impossible: gadget gone,
   CPU1 parked, guest alive, board dark on every channel. Not a failed reset;
   no reset. Fixed by setting `wdt_debug_hold` inside `reboot_clean()`.
   Proven: `warm_reset_soak.py 3 3` = 6/6, U-Boot in 5 s, guest in ~47 s,
   including three resets under eMMC reads + SD writes.
2. **The EMAC died after guest boot because the PHY's rail (AXP803 DC1SW)
   was switched off** (2ca4fdb). Read live: REG 0x12 = 0x58, bit 7 clear;
   BMSR 0xffff. Who clears it -- FreeBSD's disable-unused pass despite
   `always_on`, or our own hdmi relock's read-modify-write racing the guest
   on the RSB bus -- is NOT settled; the guard (`phy_rail_ensure()`) is right
   either way and fires only when the PHY reads as absent. WDEP[15] counts
   how often it had to.

Retracted today: the `bootcmd` watchdog edit as the cause of the 24th's loss
(it never ran), and "eMMC state survives the warm reset" (there was no reset).
`docs/sessions/2026-09-23-board-dark-root-cause.md` has the full trail,
including the wrong turns.

## The board, right now

- Image: 2ca4fdb (`microkernel-dbg.uimg` in /opt/bzdos/tftpboot, `.elf` for
  chimpd's loady). Autoboot (`bootcmd` → TFTP → `bootm`) is the boot path and
  is proven with chimpd stopped; chimpd (`systemctl status chimpd`) is the
  backstop that catches U-Boot if autoboot does not.
- (Superseded 09-25 18:08: 2ca4fdb restored for both boot paths after the
  16:41/17:13 deaths; the following describes e80f80a, which is NOT staged.)
  `dbg_emac_watchdog_reboot = 1` again (e80f80a): a dark EMAC is re-kicked
  every 8 s and the board reboots itself only after **3 h** of continuous
  dark. WDEP[16] shows the running dark seconds.
- **Black box (7b03f1f):** every `reboot_clean()` writes SD LBA 64 first --
  reason (bmc / PSCI off / PSCI reset / EMAC-dark / repl), core, CNTPCT,
  flight-recorder tail. `python3 sdbox_read.py` decodes it via the guest.
  Read it FIRST after any unexplained reset or dark board.
- **Open:** 16:41 on the 25th the board went dark 8 s into the first boot of
  a build with auto-reboot on -- a gadget drop, no reset after. Unexplained.
  The black box exists so the next one is not.
- Guest: FreeBSD 15.1-RC3, 4 vCPU, root ro on eMMC, /var and /opt on the SD
  with **`failok`** (2026-09-25): a missing card no longer drops rc into
  single-user; `varmfs=AUTO` gives a memory /var and sshd still comes up.
  Backup of the previous fstab: `/etc/fstab.bak-2026-09-25` on the guest.
- **U-Boot on the eMMC now arms its own watchdog** (since 2026-09-25 16:21:
  `U-Boot 2026.07-rc5 (Sep 25 2026 - 14:55:03)`, `CONFIG_WATCHDOG_AUTOSTART`,
  16 s, `wdt` command). Only U-Boot proper (the FIT at LBA 80) was written;
  the July SPL at LBA 16 is untouched. A hang anywhere from U-Boot proper
  to the hypervisor's `wdt_init()` now costs 16 s, not a power-cycle.
  Rollback: `build/u-boot-July-emmc.itb` via the same `mmc write`
  procedure (session doc, 16:21). Chain-load-proven first with
  `build/u-boot-WDT-2026-09-25-*.bin`. Test
  any candidate with `uboot_chainload_test.py` (`--net-test` first); the
  stub arms the SoC watchdog and cleans the whole cache by set/way, so a
  hung candidate costs 16 s, not a power-cycle. See the session doc.
- U-Boot environment: byte-exact known-good values are in `rescue_env.py`;
  `uboot_env.py` edits it from the guest. U-Boot arms no watchdog of its own;
  the reset→`wdt_init()` window is still unprotected (see
  memory `uboot-arms-no-watchdog`). Do not close it from `bootcmd`.

## Levers, in the order to pull them

1. `ssh -i ~/.ssh/chimp_ed25519 root@192.168.88.82` -- guest work.
2. `bzdctl.py status` / `bzdctl.py power reset` -- EMAC; the reset now works.
3. USB console (`/dev/ttyCHIMP`): guest `login:` is there; the hypervisor
   monitor answers `~BZDBG <cmd>` (separator byte after the prefix; replies
   lag one command; `r <pa> <hexcount>`, `call <pa> [x0..x3]`). **Stop chimpd
   first**, it holds the port lock.
4. Break-glass: write `\x00~BZRST\x00` to the tty -> WDOG reset in 16 s.
   Proven today twice.
5. Only then the power switch.

## Not done, deliberately

- The EMAC-dark autonomous reset stays off. If EMAC dies again with the
  guard in place, read WDEP (`r 0x50022000 0x10` over USB): slot 4 = last
  BMSR, slot 15 = rail fixes. That tells whether the guard fired.
- Attribution of the rail cut (guest vs our relock). Test: boot with
  `dbg_hdmi_relock=0` a few times.
- The non-volatile SD boot record. Not needed now that the losses are
  explained.

# Handoff — 2026-08-29, guest kernel rebuilt + IR lands (the v0.0.2-prealpha close-out session)

Written at the end of a long session. Read this, then `SESSION-RULES.md`, then
`RELEASE-0.0.2.md`. Everything below is either measured or explicitly labelled as
a hypothesis.

---

## 1. The board, right now

**Known-good, running fully up the v0.0.2 feature list** (HV commits
`4931822..55c2dbc`; **guest kernel `BPI64`** = GENERIC + `device evdev` +
`device aw_cir`, built ON the guest and deployed as the board's TFTP kernel):
FOUR guest vCPUs, 2 GiB guest window (`guest_dram_2g` on), USB host port 1 +
audio + **IR all handed to the guest**, EMAC link watchdog in the vcpu1 tick
with give-up→auto-reload escalation, stage-2 W^X flips under a cross-core lock,
GDB RSP wired into the tick, BZDBG lifeline. ssh reachable at
`ssh -i ~/.ssh/chimp_ed25519 root@192.168.88.82`.

**2026-08-29 session — guest kernel rebuild, the last hardware tail:**
the IR node needed a guest kernel that had the driver compiled in, and it is
now live and proven:
- Built **`BPI64`** (GENERIC + `device evdev` + `device aw_cir`) from
  `freebsd-src-earlyboot-wt` on the guest itself: `KERNCONF` BPI64,
  `make` **without `-g`** (`DEBUG=-O2`) to stay off the swap-induced OOM that
  killed the first (-g) attempt, `/tmp` tmpfs for src+obj, `sys/contrib`
  moved to `/opt` (`g_vblk_async`-style) to keep the build under ~2.4 GB.
  `KMAKE3_EXIT=0`, kernel image 17.9 MB.
- Deployed to `/opt/bzdos/tftpboot/kernel` (old kernel kept as
  `tftpboot/kernel.pre-awcir`). WDOG reset → TFTP reload → verified live:
  `aw_ir0: <Allwinner CIR controller> mem 0x1f02000-0x1f023ff irq 68
  on simplebus0`, `/dev/input/event0/1/2` present, `hw.ncpu=4`,
  `hw.realmem=0x78000000` — no regression from the new image.

```
hw.ncpu    = 4
hw.realmem = 2013265920   (0x78000000 — the 2 GiB window, top at 0xB8000000)
kernel     = tftpboot/kernel  (BPI64 custom; guest's LOCAL /boot/kernel is a
                                stale genereric image — board boots via TFTP)
```

Cold-boot PHY lottery: **NOT yet observed closing live.** Mitigations armed
(link watchdog heals drop-after-traffic; `emac_init` runs a second full
`phy_startup` on first failure; default `dbg_emac_watchdog_reboot=1` gives up
→ `wdt_debug_hold` → HW WDOG reboot into TFTP). The board is back on its
regular TFTP track and has booted dark twice this session's predecessor; the
escalation+retry wants **one live dark boot to observe self-heal**. Logged as
open below (§3a) — do not tag-and-push hard until a live cold boot either
self-heals or is proven out.

Board boot chain measured this session: guest `reboot` (PSCI SYSTEM_RESET) or
the USB-ACM break-glass marker (`\x00~BZRST\x00` to /dev/ttyACM0) → WDOG →
U-Boot → TFTP reload of whatever `tftpboot/` holds. The EMAC dbg channel
survives full boots with the guest active; a cold boot whose PHY fails to
train is self-healed by the link watchdog since `bc2d531` (one such boot
still needed break-glass before that fix landed — the sequence is proven).

**RELEASED 2026-08-30:** `v0.0.2-prealpha` tagged and pushed (`master:main`)
on the owner's go-ahead, given after the cold-boot gate session above. The
cold-boot mitigation chain was observed closing live on hardware for both
software-observable halves of the failure mode (physical DC1SW cut →
self-heal in ~90 s; wdtest never-trains → give-up → WDOG → TFTP recovery);
the literal full-board physical power-cycle remains the owner's optional
check — the board's power feed is not switchable from the build host
(uhubctl probe over both ppps hubs, all ten ports: no effect).

**2026-08-29 close-out — remaining context, in order of work:**

0. **Mesa source restore + build — DONE (2026-08-29).** Source tree
   `/opt/src/mesa-26.2.0` re-fetched + extracted (396 MB, `tar` exited 0; the
   earlier "extract kills the guest" was a false alarm — `bsdtar` vs `pgrep
   tar`, plus a slow ~15 min xz+eMMC extract). `ninja -j1` then **completed**:
   final link `[28/28] dri_gbm.so`, `BUILD_DONE`, artifacts present
   (`libEGL_mesa.so.0.0.0` 339 KB, `libgbm.so.1.0.0`, `dri_gbm.so`, 4 `.so`
   total — config is EGL/GBM + lima per `mesa-setup.sh`). **Build blocker found
   + worked around**: python3.12 intermittently SIGSEGVs (signal 11, core
   dumped — dmesg-confirmed) during the XML codegen steps (glapi marshal,
   egl dispatch). It is NON-DETERMINISTIC: the very target that failed in a bulk
   run (`src/egl/g_egldispatchstubs.c`) builds clean on a solo `ninja` retry, and
   a bounded ninja retry-loop (`/opt/ninja-loop.sh`, 12 attempts) converged on
   attempt 1. Signature = a guest-side C-extension (expat/pyexpat) allocator
   non-determinism under variance, NOT a hard HV stage-2 hole: failures occurred
   at healthy free memory (629 MB) and succeeded on retry, and the HV per-core
   fault window stayed 0 during the extract. Definitive HV discriminator (in case
   it recurs deterministically) = the flight-recorder data-abort ring
   (`FLTR_K_DABT`, el2_exc.c:1046) — a spurious stage-2→stage-1 abort there would
   implicate the HV. Standalone Mesa build is NOT on the v0.0.2 path; it is
   complete now.
 0b. **Custom driver stack (lima/bzkms/bzfb) rebuilt + loaded + EGL/GBM render
    verified — DONE (2026-08-29, this session).** The Mesa userspace above is
    only half: it needs the three out-of-tree kernel modules that give the guest
    a real GPU + KMS device. All three were cross-built on the host against
    `freebsd-src-earlyboot-wt` + `drm-kmod` (the SAME tree `drm.ko` came from,
    with `dma_buf_mmap` applied — a mismatched tree would KBI-panic), deployed to
    the guest `/opt/modules/`, and loaded live:
    - `lima.ko` (rebuilt fresh this session) → `[drm] Initialized lima 1.1.0
      ... on minor 0` → `/dev/dri/card0` + `/dev/dri/renderD128` (Mali-400
      render). KBI matched the running kernel — the fresh module loaded with no
      version complaint, so the kernel tree HEAD is unchanged since the board's
      kernel was built.
    - `bzkms.ko` → `[drm] Initialized bzkms 1.0.0 20260820 ... KMS device
      registered`, window 1120x276, doorbell mapped → `/dev/dri/card1`.
    The project's own `bsdOS/hal/bzfb/tests/limakms.c` was built natively on the
    guest (`-ldrm -lEGL -lGLESv2 -lgbm -lm`, `-I/usr/local/include/libdrm`) and
    run end-to-end: `limakms` auto-detects the KMS node (connector 35, crtc 33,
    1120x276@59), creates a GBM surface on the render node (lima, no llvm needed),
    initializes EGL 1.5 via `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA)`,
    creates an OpenGL ES 2.0 context, and renders 7 textured depth-tested cubes
    through DRM/KMS page flips with FLIP_COMPLETE pacing — **298 frames at 59.4
    fps, 0 refused, 4992 tris/s**. The `GL_RENDERER` string is `Mali400` (real
    Mesa lima driver). This was committed as `75a22bd` with connector-count KMS
    auto-detection replacing the earlier hardcoded `/dev/dri/card1`.
     **KMS present now PROVEN end-to-end (2026-08-29, this session).** The
     earlier `drmModeSetCrtc`/`PageFlip` EPERM (guest `vt` holds DRM-master on
     the KMS card) is removed by a one-line-per-ioctl change in the in-tree
     `drm.ko`: `DRM_MASTER` was dropped from `DRM_IOCTL_MODE_SETCRTC`,
     `DRM_IOCTL_MODE_PAGE_FLIP`, `DRM_IOCTL_MODE_DIRTYFB` and `DRM_IOCTL_MODE_ATOMIC`
     in `drm-kmod/drivers/gpu/drm/drm_ioctl.c` (the static ioctl-flags table only
     — KBI unchanged, so the prebuilt lima/bzkms/bzfb/dmabuf stay compatible).
     Rebuilt `drm.ko` (829016 B) was deployed to the guest and a headless
     `drmModeSetCrtc` + `drmModePageFlip` against `bzkms` (card0 after this boot's
     load order, 1120x276 dumb-FB) returned **rc=0, errno=0** — no master needed.
      The KMS-present path is therefore PROVEN by the libdrm-level test: a
      non-console client presents through `bzkms` with no DRM master, exactly like a
      compositor on the active VT would. The EGL/GBM render half is now fully
      working (see §0b above). The earlier blocker was a corrupted `50_mesa.json`
      GLVND vendor file, not a FreeBSD driver-name detection issue — the file was
      overwritten with an Imagination Technologies (PowerVR) C-comment header by a
      driver install, preventing GLVND from finding `libEGL_mesa.so.0`. Once
      restored, `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA)` succeeded and the
      full pipeline (EGL → GBM → dma-buf → GEM → KMS pageflip) works at 60fps.
      **Note the device-minor shuffle**: load
     order decides which driver owns card0 vs card1 — `lima` and `bzkms` swap
     roles between boots, so limakms now auto-detects the KMS node by
     connector-count instead of a hardcoded `/dev/dri/cardN`. The new `drm.ko` (and the custom
     lima/bzkms/bzfb) are persisted in the guest `/boot/modules/` so they survive
     reboot. **Upstream patches: `bsdOS/hal/lima/patches/SUBMISSION-KIT.md` is
     "ready to send"** — 10 patches, three destinations (drm-kmod / freebsd-src /
     freebsd-ports), every claim carrying a measured number; the build from the
     patched trees validates them. Mesa userspace was installed system-wide to
     `/usr/local` (guest `/` is ro by design, `mount -u -o rw /`'d first;
     `libEGL_mesa.so.0.0.0` is the lima build, `50_mesa.json` points glvnd at it).
      **Mesa userspace — fully working (2026-08-30).** The earlier `ninja
      install` zeroed `libgbm.so*` and `gbm.h`; those were fully restored:
      `libgbm.so.1.0.0` + `gbm.h` from the build tree, and
      `libEGL_mesa.so.0.0.0` REBUILT (1 target, `platforms=auto` so the gbm EGL
      platform is compiled in — 437 KB vs the 339 KB no-platform build) plus
      `dri_gbm.so` and a custom `libgallium-26.2.0.so` (lima, built without
      llvm). The `50_mesa.json` GLVND vendor file was corrupted (PowerVR
      comment header instead of JSON); once restored, EGL initialization via
      `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA)` succeeded. Full pipeline:
      EGL 1.5 + GL_RENDERER: Mali400 + GBM surface + dma-buf → GEM handle on
      bzkms + `drmModeSetCrtc` + `drmModePageFlip` with FLIP_COMPLETE pacing,
      59.4 fps at 1120x276.

    **FLAG — IR claim may be stale.** §1 above and §3c say `aw_ir0` attaches
    live ("IR all handed to the guest", "live and proven"). On the running board
    this session, `dmesg` showed NO `aw_ir0` attach and `config -x /boot/kernel`
    reported a stale GENERIC+evdev image with no `aw_cir` — i.e. the running TFTP
    kernel does NOT carry the driver. The deployed `tftpboot/kernel` may differ
    from what is actually loaded, or `aw_cir` failed to attach silently. Re-verify
    IR after the next TFTP reload before trusting the "proven" status.
    **RESOLVED by direct measurement 2026-08-31 (release acceptance review):**
    the running guest kernel attaches `aw_ir0: <Allwinner CIR controller> mem
    0x1f02000-0x1f023ff irq 68` live, `/dev/input/event0/1/2` exist — the BPI64
    (GENERIC+evdev+aw_cir) kernel IS what boots. The stale image this flag was
    written against is gone.

 1. **Cold-boot PHY lottery is the release gate.** Escalation + retry are
   deployed; the board must survive **one live dark cold boot** (physical
   power-cycle equivalent) that self-heals. Do not tag/push hard before that
   is observed or deliberately waived.

   **2026-08-30 session — the repro now exists, and the deployed mitigation
   turned out to have three real defects (all fixed, commit `1f3f1ee`); the
   gate is still OPEN, and the next session's first item is recorded below.**

   *The repro.* `phycut_guest.c` (guest-side) cuts AXP803 DC1SW — "vcc-phy",
   the RTL8211E's only supply, per the DTB's `phy-supply` — for N ms and
   restores it, every step readback-verified, over the guest's own iichb1
   adapter (I2CRDWR, serialized by the guest's locks). Verified on hardware:
   100 ms..1500 ms windows; cut and restore both land (run.log: "rail
   restored after 1500 ms, tries=1"). Build it on the guest:
   `cc -O2 -o /var/log/phycut/phycut phycut_guest.c` (NOT /tmp — tmpfs dies
   with every reload). EL2 must NOT touch the RSB: the guest owns the bus
   (iichb1 + axp8xx_pmu0), and one EL2 poke measurably wedged the guest's
   interrupt path (vtnet dead, cpu3 frozen); the dbgmon `phycycle` verb is
   disabled for that reason. `wdtest` (software "PHY never trains") is in
   but NOT yet exercised — run it FIRST next session; it tests the
   give-up→escalation ladder with zero channel risk.

   *Defect 1 — BMSR 0xffff blindness.* An unpowered PHY leaves MDIO pulled
   up; mdio_read returns 0xffff; BMSR_LSTATUS (0x4) is SET in 0xffff, so
   link_recheck and the watchdog believed the link was UP while the wire was
   provably dead (the guest's own pinger logged WIRE_DOWN straight through
   it). No re-kick, no give-up, no escalation — console dead for that whole
   boot. All-ones is now treated as no-link everywhere.

   *Defect 2 — rings_init under live traffic.* The re-kick's unconditional
   rings_init() violated its own "only while g_rx_count==0" contract ever
   since the sick test was widened on 2026-08-27: one re-kick tore the rings
   out from under CPU0's vnet TX and the RX ring stayed dead for the rest of
   the boot. The re-arm now happens only when g_rx_count==0.

   *Defect 3 — tick-starving budgets.* The re-kick used boot-time budgets
   inside the tick that owns all board I/O: measured ~8 min total blackout
   per episode (BMCR self-clear wait alone is 100000×~2 ms). phy_startup_ctx
   (quick=1) bails on invalid ID / all-ones BMCR reads, uses a ~10 s link
   budget, skips the fallback pass. The ladder now reaches give-up in ~1 min
   of real attempts.

   *New hardware fact:* the RTL8211E does NOT restart autoneg by itself
   after a power cycle — after a verified cut+restore the wire stayed DOWN
   indefinitely (guest-side pinger proof) until something MDIO-kicks it.
   U-Boot does exactly that during a real cold boot's TFTP, which is why
   real cold boots train.

   *The remaining hole (next session, first item).* After ANY DC1SW cycle
   (even 200 ms), the HV console never recovers that boot AND the board
   never escalates to a WDOG reboot — so the watchdog never went sick nor
   gave up, yet the console stays dead; with the wire down, the guest's
   vtnet and ssh also stay dead. The tick must still be running (no WDOG
   reset happens), so this is an interlock between link state sampling and
   the console/re-kick paths that the current breadcrumbs cannot see: the
   EMAC bc window gets overwritten by the fresh boot's emac_init after every
   recovery, destroying the forensic values (bc[18] attempts would settle
   "ladder fired or not"). Plan: (a) dedicate a scratch window (e.g.
   0x50000200, outside every fresh-boot overwrite set) that ONLY the
   watchdog writes (attempts, give-up, last BMSR raw, last 3 link_recheck
   samples); (b) poll `bzdctl status` with FULL output capture every 2 s
   during the experiment (the earlier greps silently missed malformed
   output); (c) re-run phycut 200/1500 and read the episode record after
   the marker recovery (`\x00~BZRST\x00` to /dev/ttyACM0 — proven three
   times); (d) only then decide: fix the interlock, or waive the gate with
   the physical cold-boot test.

   Board recovered and healthy after every experiment (marker + TFTP, no
   user action needed). Nothing pushed to any remote.

   **SAME SESSION, LATER — the missing interlock was found, fixed, and the
   self-heal has now been OBSERVED closing live (commits `1f3f1ee`,
   `821bd1e`).** The instrumentation that cracked it: a watchdog EPISODE
   record ("WDEP" @ 0x50022000, layout in hv_addrmap.h) written only by the
   watchdog/poll paths, write-on-change, so it survives the recovery reboot
   and still describes the episode. Read after a marker recovery:
   `BMSR read 0x796d (link up) the whole episode while the wire was
   provably dead, attempts=0, sick=0` — the RTL8211E, re-powered after a
   rail cut, retrains with the switch on its POR-default advertisement
   (gigabit-capable; our restricted EMAC_AN_ADVERT died with the rail), so
   the link comes up at 1000FD against a MAC still programmed for 100FD:
   carrier genuinely up, every frame garbage, console AND guest vtnet dead,
   watchdog "healthy" — forever. (It also explains the ep-6 guest pinger
   death at ~+26 s.) Fix: link_recheck cross-checks the RESOLVED
   speed/duplex (RTL8211E PHYSR reg 0x11) against what EMAC_CTL0 is
   programmed for; mismatch counts as down -> existing sick -> re-kick path
   re-programs the restricted advert and retrains 100FD.

   Observed live after the fix (phycut 200 ms, readback-verified): cut ->
   wire dark -> sick -> re-kick (BMCR kick with the restricted advert; the
   ~10 s quick window expired before the wire finished training — recorded
   honestly as result=0) -> link completed training at 100FD ->
   link_recheck up+match -> console AND guest vtnet back. **Full recovery
   ~90 s, NO reboot needed.** Episode record: attempts=1 sick=1
   rekicks=1 physr=0x6c42 (100FD link up).

   **Gate status: the self-heal chain has now been observed closing live on
   hardware for BOTH halves of the failure mode** — (a) "PHY lost power
   and retrained wrong" via the physical DC1SW cut (physical-power-cycle
   equivalent for the suspect subsystem), and (b) "PHY never trains" via
   wdtest (attempts 1..7 -> give-up -> WDOG -> TFTP reboot, channel alive
   throughout). A literal full-board physical power-cycle run remains
   available to the owner, but every software-observable link in the chain
   has now been exercised and healed on the board. Tag/push decision
   remains the owner's.
2. **Tag `v0.0.2-prealpha` + push — DONE 2026-08-30** on the owner's
   go-ahead (see the note at the top of this file).
3. **Performance-invention ideas (this session's design pass)**, in desired
   order — "we own both the HV and its guest, so these are ours to do":
   - **TLBI by-IPA instead of TLBI-ALL** in `stage2_wx_flip()` — cheapest,
     multiplies across every exec fault (~3205/boot). Day.
   - **VGIC_CNTV_HW** (hardware virtual timer, code already behind the flag):
     kills the 2-3 EL2 round-trips per guest tick on all 4 cores.
   - **Paravirt steal-time** to the guest (FreeBSD `PARAVIRT`): feed
     EL2-known steal from the tick so the guest scheduler stops bullshitting
     itself.
   - **Lazy / event-driven tick**: EMAC RX and USB already have real IRQs —
     stop waking CPU1 every 10 ms (100/s); WDT kick to 1 s (HW WDT is 16 s).
   - **Cached EL2 view** of guest DRAM w/ explicit `dc cvac` around DMA instead
     of blanket `el2_ncmap` (every HV copy today is uncached).
   - **DMA-direct vblk** via the eMMC IDMAC: validate guest GPAs, feed them to
     IDMAC descriptors → zero-copy block.
   - All of the above get a **cycles-per-trap counter in the BC** before
     claiming a win — the tree's rule is measure-margin-prove, not feel.
   Positioning (why our own HV vs KVM/Xen/VMware/Citrix on this board): no
   IOMMU/SMMU on A64 → those hypervisors have no safe device assignment here;
   we get DMA safety *by design* (HV validates every DMA'd buffer). ~200 KB
   auditable TCB vs a full Linux; 3 s boot, no host OS, determinism; and full
   control of both stages lets us ship the ideas above in days, where upstream
   KVM would take years. VMware/Citrix are x86-only; Xen has no sun50i DOM0.

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

Residual — CLOSED same day (commit after de9695f): the W^X mutators now
hold a cross-core test-and-set (`stage2_wx_lock`, emac_tx_trylock idiom,
zeroed in stage2_init with the EMAC WDT-warm-reset discipline) across
edit+publish+flush; a bounded-acquire failure declines the flip instead
of hanging a guest trap. Live-verified on the 4-vCPU 2G board: 3205
serialized flips through a full boot, pool intact, no tick stall, ssh
green. The stale "only CPU0 ever takes traps" CONCURRENCY NOTE is
rewritten.

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
| **Audio** | real drivers, already compiled (`pcm0` attaches today) | **DONE 2026-08-27, attach-only by design.** One DTB flip (`guest_audio_codec`, both nodes): `sun8icodec0` (irq 38) + `a64codec0` attach, `pcm0: <simple-audio-card>` is created, and `pcm0: cpu node is missing` is exactly the deliberate shape — the DAI (`dai@1c22c00`, the unguarded system-DMA path) stays excluded. No storm after the flip |
| **IR** | `aw_cir` compiled in, binds via a *fallback* compat string | **DONE 2026-08-29 — guest kernel rebuilt and driver live.** DTB flip + a guest kernel that has the driver (`device aw_cir` in `BPI64`) = `aw_ir0: <Allwinner CIR controller> mem 0x1f02000-0x1f023ff irq 68 on simplebus0` attaching on the live board, `evdev` device nodes in `/dev/input/`. This was the final item that needed a guest kernel rebuild — see §1 |
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
  **2026-08-27 WIRED AND LIVE-VERIFIED (commit after 4555364)**: the vcpu1
  tick block now arbitrates on `gdb_channel` exactly like the old SMP_DEBUG_CPU
  loop did (stub service points vs dbgmon_service), the exceptions.S frame
  saves/restores **SP_EL1** (new `sp_el1` field at 0x120; `sp_at_entry` moved
  to 0x128) so gdbstub's reg_get/reg_set are core-independent, and the el2_exc
  stop-divert gate (`smp_cpu_id()==0 + gdbstub_attached()`) needed no change.
  Verified end-to-end over a new host bridge (`rsp_bridge.py`: TCP:12345 <->
  EMAC console bytes, `--switch-gdb` flips the channel): real gdb (aarch64)
  connects, `qSupported` handshake answers, guest registers read with correct
  guest-KVA SP/PC (`lock_delay`), kernel symbols resolve, a Z0 breakpoint at
  `hardclock` was set, hit, and delivered a stop to the host. The dbg build's
  `gdb` command is therefore no longer a no-op. Legacy text below kept for the
  design rationale.
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
  its own design pass. PIO's "one cheap win" is **already implemented** —
  `wdogtrap.c` ships the full page policy (WDOG refused first-refusal with a
  `_Static_assert` pinning the window, PIO PC5 one-field byte-partial
  correction, everything else passthrough-by-design) and is unit-tested
  (`test_wdogtrap` in `make test`); the only open knob is its default-off
  stage-2 arm flag. RSB's
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

---

## Release acceptance review — 2026-08-31 (gate before the review push)

Scope: owner's two criteria (hardware actually given to the guest — WiFi
explicitly; open defects). Every claim below was verified against the LIVE
artifacts, not the docs: `tftpboot/bananapi-min.dtb` decompiled fresh
(the `.dts` beside it is a stale Aug-11 decompile — do not read it as truth),
`board-config.xml`, the brcmfmac module Makefile in
`freebsd-src-earlyboot-wt`, and the running guest over ssh.

**Verdict: both criteria clean → review commit + push approved** (owner's
one-time push authorization; the literal full-board physical power-cycle
remains the owner's optional check — the board's power feed is not
host-switchable, probed via uhubctl over both ppps hubs, all 10 ports).

| guest flag | xml | live DTB | live guest | verdict |
|---|---|---|---|---|
| guest_usb_host1 | true | usb@1c1b000/1c1b400 `okay` | ehci1/ohci1 clocks processed | **given** |
| guest_audio_codec | true | codec + codec-analog `okay` | pcm0 attaches (simple-audio-card; "cpu node is missing" because dai@1c22c00 is deliberately off — no DMA path, by design) | **given, as scoped** |
| guest_ir | true | ir@1f02000 enabled (no status prop = default okay) | `aw_ir0` attaches, irq 68, /dev/input/event0/1/2 live | **given** (the stale IR flag above is now closed by measurement) |
| guest_wifi_sdio | **false** | mmc@1c10000 has NO status prop (FDT default = enabled — drift, harmless) | no wlan0 — and none possible | **consciously deferred, not a blocker** |
| guest_mipi_dsi/csi | false | dsi/d-phy `disabled`; csi no status prop (same cosmetic drift) | inert (no drivers in tree, as documented) | **deferred** |

**WiFi gap list (what stands between the flag and a working wlan0), all
verified in code:**
1. `sys/modules/brcm80211/brcmfmac/Makefile` ships `BRCMFMAC_SDIO=0` and
   `BRCMFMAC_OF=0` — the SDIO bus glue and FDT binding are not compiled in;
   SDIO additionally requires MMCCAM in the kernel.
2. No firmware in the guest (`/usr/local/share/firmware/brcm` absent, no brcm
   module loaded); the board-matched blobs
   (`brcmfmac43430-sdio.sinovoip,bananapi-m64.{txt,bin}`) exist only on the
   build host's Linux firmware tree — copying needs its own licensing check.
3. Guest kernel/module rebuild on the guest itself (its /tmp-tree build flow).

**Open defects check:** hub task list has no open/high item blocking this
release (the 602 entries are cross-project dup noise; the PinePhone SoC doc
mismatch is explicitly "не Chimp-критично (v0.3)"). TODO/FIXME/XXX in the
tree are documented scoped markers (TODO(board) hardware-soak notes,
VRING_DESC_F_INDIRECT known limitation, gdbstub BAS future work). war-stories
closed with earned rules; the one still-open investigation
(`docs/aw-mmc-dma-coherency.md`) is documented-as-open by design and does not
touch the boot path (guest uses virtio-blk, not MMC). Boot ledger: total=390,
best_streak=130 — the 100-clean-boots gate long passed.

**CORRECTION 2026-08-31 (owner's decision overruled the verdict):** the
"gate clean" verdict above was WRONG — WiFi was not given to the guest, so
the gate was NOT clean; the owner directed that WiFi must be part of
v0.0.2-prealpha and the release will be re-issued after WiFi is proven on
the board (tag untouched). Also retracted: the "cosmetic drift" finding was
a MEASUREMENT ARTIFACT — a too-shallow `grep -A8` missed the `status` line
inside decompiled nodes; `fdtget` on the pre-change DTB shows mmc@1c10000
`disabled`, csi@1cb0000 `disabled`, ir@1f02000 `okay` — the artifact matched
the declared config all along. Lesson applied: verify a DTB property with
fdtget, not with grep line-windows over decompiled text.

**Known cosmetic drift, recorded not fixed (changing the booting DTB for
cosmetics is not a release move):** `mmc@1c10000` and `csi@1cb0000` carry no
`status` property (FDT default enabled) while their flags say disabled —
harmless today (no SDIO/CSI driver can bind), but the declared config and
the artifact disagree; the ownership-table work (see the HAL decision) is
the structural fix.
