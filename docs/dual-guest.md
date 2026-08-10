# Dual-guest: FreeBSD (CPU0) + Zephyr (CPU3), genuinely concurrent

## Status, 2026-08-10

| Claim | True? | How it was checked |
|---|---|---|
| The mechanism **exists** (new stage-2 table, disjoint loader, CPU-routed console/fault dispatch) | ✅ | `smp.c`, `zguest_cpu3.c`, `zload2.c`, `stage2_zephyr.c`, `mmio_absorb.c`, `vconsole.c`, `el2_exc.c` — commit `7e5303c` |
| PSCI `CPU_ON` bring-up of all 4 cores works under QEMU | ✅ | `smp-qemu-ci.sh`, 5/5 — commit `c78280d` |
| CPU0 and CPU3 run **genuinely concurrently** under QEMU (not sequentially) | ✅ | `dual-qemu-ci.sh`, real hand-built ELF through the real `zguest_cpu3`/`zload2` path, 10/10 — commit `fc20b63` |
| **Real Zephyr** boots concurrently under QEMU | ✅ | `dual-zephyr-qemu-ci.sh`, real banner + heartbeats via channel-1 vconsole, 10/10 — commit `779153d` (needed a new `bpi_m64_hv_dual` Zephyr board port, `sram0` moved to `0xBE000000`) |
| CPU3 runs **genuinely concurrently with FreeBSD on the real board** | ✅ **confirmed 2026-08-10** | see below |
| **Real Zephyr** on CPU3 on the real board | ✅ **confirmed 2026-08-10** | see "Real Zephyr on the real board" below |
| A real guest image can be **loaded** onto the board at all (bulk loader) | ✅ **confirmed 2026-08-10** | `zstage.c` + `chimpd --zguest`, see below |
| A failed `zboot` can be **retried** without a board reload (`zunhalt`) | ⚠️ **partly** — re-arm works, re-arm→successful-boot does NOT | see "zunhalt: what is and isn't proven" |

None of these rows are interchangeable — "the mechanism exists" is not "it boots," and "boots under QEMU" is not "boots on the board." This project has burned time before on reports that blurred exactly this kind of distinction (see `zephyr-guest.md`'s own header note). The `⚠️` row is deliberately not rounded up to ✅.

## Confirmed on real hardware (2026-08-10)

Flashed `make dual` (FreeBSD/CPU0 path reuses `main_dbg.c` verbatim — zero changes) to the real board via the same reversible `chimpd.HYP_ELF` monkeypatch technique used for every prior real-hardware test this session. **FreeBSD booted and ran completely normally first** — `ssh` answered, root mounted `rw`, `kldstat`/`uptime` all ordinary — proving the safety property the plan itself insisted on checking before ever touching CPU3.

**Staging problem found immediately, and it's real**: `zguest_cpu3.h`'s own header comment already flagged this honestly — *"No such bulk-loader script is added by this pass; wiring one up is host-side tooling, out of scope here."* Sending `zboot` with nothing staged at `ZG3_ELF_STAGE_PA` produced breadcrumb `0xBAD1` (`zload2_parse_and_place()` correctly rejected the garbage/empty staging buffer) — the safety check worked exactly as designed, and FreeBSD/CPU0 was completely unaffected by the failed attempt (confirmed via `bzdctl.py status`: `exceptions none recorded`, cores online, console quiet).

**Operational quirk found**: `zephyr_cpu3_run()` halts permanently (`for(;;) wfi()`) on a failed placement or a failed isolation self-check — by design ("fail loud and stopped, not silently proceed"), but this means CPU3 will **never re-check the start flag** after a failure. A second `zboot` after staging the ELF correctly got the same `"CPU3 start requested"` acknowledgment from `dbgmon` but produced **no effect at all** (breadcrumb unchanged) — CPU3 had already given up for good. Recovering from *any* failed `zboot` attempt currently requires a **full board reload**, not just a retry. Worth revisiting if this becomes a frequent operator workflow (e.g. a `zunhalt`/re-arm command), but left as-is for this milestone — the safety property (halt loud, don't guess) is more important than retry convenience.

**Staging a real payload, done ad hoc (no bulk loader exists yet)**: for this proof, the Step-1 trivial payload (`zg3_trivial_payload.elf`, a 4-instruction counter loop) was staged by hand — `zload2_parse_and_place()` reads every field via absolute `p_offset` (`memcpy(dest, elf_addr + p_offset, ...)`, confirmed by reading `zload2.c` directly), so only the ELF header + one program header (120 bytes, offsets `0x0`-`0x78`) and the actual 24-byte code segment (offset `0x10000`) needed pushing — 36 words total, via `hvdbg.py`'s existing `write_word_verified()`, in seconds. **A real Zephyr image is 100s of KB and this does not scale** — pushing one word at a time over the EMAC debug channel at the measured per-write RTT would take on the order of tens of minutes to hours. This is the real, unresolved gap for "real Zephyr on the real board, concurrently" — see below.

**The actual concurrency proof**: after a clean re-flash + restage + `zboot`, `zguest_cpu3`'s own breadcrumb (`0x50070000`) advanced past both failure codes (word[1] went from `0xBAD1` to `3`, word[2] showed the real entry PA `0xBE000000`), and the payload's own counter (a plain word it increments at `0xBE000100`, identity-mapped inside Zephyr's 32 MiB slice) was sampled twice, ~5 seconds apart, over the EMAC debug channel:

```
counter sample 1: 0x12e7b777 (317175671)
counter sample 2: 0x13b84cc8 (330845384)
```

— climbing at roughly 2.7M increments/second, on real Allwinner A64 silicon, while FreeBSD on CPU0 stayed fully healthy and responsive over `ssh` throughout. This is the first-ever confirmation of this project's dual-guest mechanism actually running on hardware, not just under QEMU.

**FreeBSD-side diagnostics confirmed unaffected**: `gr` (reads `g_last_guest_frame`, the seqlock-protected snapshot `el2_exc.c` now gates to `smp_cpu_id() == 0`) returned a real, sensible FreeBSD trap frame (`elr` matched the same idle-guest PC `bzdctl.py status` has shown all session) — proving CPU3's concurrent activity does not clobber the diagnostic state FreeBSD-side tooling (`dbgmon`'s `gr`, and by extension `gdbstub.c`) depends on. (`sr` returned all-zeros, exactly as expected and already documented elsewhere in this tree: `sr` has always read CPU1's own, irrelevant EL1 bank — a pre-existing limitation, unrelated to this milestone.)

Board restored to the normal `dbg` configuration immediately afterward (two hard resets in total during this test — once to clear CPU3's permanent halt-on-failure, once to return to plain FreeBSD; both needed the standard `fsck_ffs -y` + `netif restart` + `sshd start` recovery this project has needed every time a running guest gets an uncooperative `power reset`). Confirmed recovered: `ssh` answers, root `rw`, `fsck` found nothing outstanding on the final check.

## The bulk loader (2026-08-10)

Open item 1 below — "a real bulk loader for staging a full guest image" — is now
closed. The route deliberately avoids the debug protocol entirely: `chimpd`
already TFTPs the FreeBSD kernel and the DTB from U-Boot before jumping into the
hypervisor, so a guest image is a **third file** through machinery that already
works. `chimpd --zguest <elf>` publishes it into the TFTP root and fetches it to
`ZSTAGE_LOW_PA` (`0x4E000000`); `zstage.c`, called from `main_dbg.c` on CPU0
**before `smp_init()` and before `kload_enter()`**, copies it into the second
guest's own slice. See `zstage.h` for why it does not TFTP straight to
`0xBF000000` (U-Boot relocates itself to the top of DRAM) and why the position of
the copy is load-bearing rather than stylistic (the landing window is inside
FreeBSD's own gigabyte).

Confirmed live, first attempt, with the real Zephyr image:

```
ZSTAGE breadcrumb @0x5007A000:  state=4 (copied OK)  span=36924  copied=36924  dest_pa=0xBF000000
```

`span` is exactly `0x1000 + 0x803c` — the ELF header, the program headers, and
the single `PT_LOAD`'s file content, out of a 481,296-byte file. Both numbers fit
the 16 MiB window, so excluding the section headers and `.debug_*` is a 13×
efficiency win here rather than a correctness save; the shape it avoids is
nonetheless exactly what real images look like.

## Real Zephyr on the real board (2026-08-10)

`zboot` over EMAC, then read back Zephyr's own channel-1 console ring:

```
*** Booting Zephyr OS build v4.4.1 ***
bzdOS/Zephyr: hello from EL1 (board: bpi_m64_hv_dual)
heartbeat 0 ... heartbeat 41   (still climbing, 60 bytes / 10 s, steady)
```

— while FreeBSD on CPU0 was simultaneously healthy: `ssh` responsive, root
mounted rw, a 384 MB `dd` off `vtbd0p3` at ~4.2 MB/s under load 2.00, and zero
error lines in `dmesg`. `gr` (which `el2_exc.c` now gates to `smp_cpu_id() == 0`)
still returned a real FreeBSD frame — every register a `ffff…` kernel KVA, EL1
`spsr`, real `esr`/`far` — proving CPU3's concurrent activity does not pollute
the diagnostic state FreeBSD-side tooling depends on.
`stage2_zephyr_isolation_selfcheck()` passed on every boot (`ISOL` flags 1/1).

**One honest caveat, stated because it happened.** Across the dual-guest boots
this session, one was fully healthy as described above, and a *different* one
showed FreeBSD wedged: 451 k stage-2 console faults/second with **zero** console
bytes produced — i.e. the guest spinning on a UART register and emitting nothing
— and the guest's network gone, while Zephyr on CPU3 stayed perfectly healthy
(2 faults per byte, output flowing). That boot was the abnormal "ghost image"
configuration described below, which the fix in this same change now makes
impossible, so the two cannot be causally linked from the evidence available.
It is recorded here rather than dropped: **"FreeBSD is unaffected by the second
guest" is supported by the healthy boots, not established as a general
property.** A soak run (see `docs/soak-and-breakglass.md`) is what would settle
it.

## The ghost-image bug, found on hardware and fixed

A warm reset **preserves DRAM**, and `halt -p` on the guest goes PSCI
`SYSTEM_OFF` → the HV's own clean warm reset. So the previous boot's copied image
is still sitting at `ZG3_ELF_STAGE_PA` when the next boot starts.

Observed live: an image was staged that `zstage_span()` correctly rejected
(breadcrumb `state=2`, `span=0`, `copied=0`) — and `zboot` **booted a guest
anyway**, reaching state 3, because `zload2_parse_and_place()` found the
*previous* boot's perfectly valid Zephyr image at that address. Nothing anywhere
reported a problem. An operator whose TFTP silently failed, or who forgot
`--zguest`, would have booted the old image with every visible indicator
agreeing that all was well — the breadcrumb said "no valid image staged", the
guest ran regardless, and the reassuring reading won.

Fixed: when a boot stages no usable image, `zstage_copy_to()` now zeroes the
destination's header region (`zstage_invalidate_dest()`), so nothing bootable can
survive from a previous boot. Confirmed live — `invalidated=4096`, and `zboot`
then correctly reports `0xBAD1` instead of booting a ghost.

This makes staging *directly* at `ZG3_ELF_STAGE_PA` a permanently unsupported
route, which is why `dual-qemu-ci.sh`'s and `dual-zephyr-qemu-ci.sh`'s second
pass now asserts that a destination-only image is **refused** rather than that it
works. Hand-staging over EMAC after boot is unaffected: the copy-in runs during
HV init, long before `dbgmon` exists to write anything.

## zunhalt: what is and isn't proven

**Proven on hardware.** `zboot` while CPU3 is halted is fully inert; `zunhalt`
returns CPU3 to an *observable* parked state with `attempts` unchanged,
`rearms` incremented, and `last_fail` sticky:

```
after zboot #1   state=0xbad1  attempts=1  last_fail=0xbad1  rearms=0
after zboot #2   state=0xbad1  attempts=1  last_fail=0xbad1  rearms=0   (inert)
after zunhalt    state=0x1     attempts=1  last_fail=0xbad1  rearms=1   (parked)
```

Two real bugs were found getting there, both only visible on hardware:

1. The halt loop originally waited on `wfi`. CPU3 takes **no interrupts at all**
   in this design, and `sev` (which `zguest_cpu3_rearm_set()` sends) wakes only
   `wfe` — so it was a permanent sleep. `zunhalt` was acknowledged over EMAC and
   CPU3 slept through it, `rearms` stuck at 0 across repeated attempts.
2. A `zboot` issued *while* halted stayed latched and fired the instant the
   re-arm released the core, so `zunhalt` alone silently started an attempt and
   the parked state could never be observed at all. The request is now discarded
   on re-arm.

**NOT proven.** After a re-arm, a `zboot` with a freshly hand-staged trivial
payload reached `kload_enter` — `state=3`, `attempts=2`, `entry_pa=0xBE000000`,
`zload2` breadcrumb clean (`elf_valid=1`, `bytes_copied=24`, `fail_reason=0`),
isolation self-check passed, and the placed bytes at `0xBE000000` verified
byte-for-byte against `objdump` — and **the payload never executed**: its counter
at `0xBE000100` was static across repeated samples, with zero absorbed MMIO
faults, zero channel-1 faults and no exception recorded. Note that `state=3` is
written *before* `kload_enter()`, so it means "about to enter", never "running".

The same firmware boots the real Zephyr image correctly on a **first** attempt
(verified immediately afterwards), so the suspicion is narrowed to the
second-trip-through-the-loop path specifically. Cause not identified; not chased
further this pass. So: **`zunhalt` gets you back to parked, but a retry is not
yet known to produce a working guest.** A failed `zboot` should still be treated
as needing a reload until this is understood.

## What's still open

1. ~~A real bulk loader~~ — **done**, see above.
2. ~~Real Zephyr, on the real board, concurrently with FreeBSD~~ — **done**, see
   above.
3. **`zunhalt` re-arm → successful boot.** The re-arm itself works; the retry
   reaching a running guest does not. Precise evidence above — this is the one
   concrete, reproducible-looking defect left in this feature.
4. **Whether the second guest can destabilise FreeBSD.** One boot this session
   showed FreeBSD wedged spinning on its console while Zephyr stayed healthy. Not
   attributable from the evidence, not reproduced on the healthy boots. Needs a
   soak run rather than more single-shot tests.
5. **Phase 2** (repurposing CPU1/CPU2 for further guests) remains out of scope
   here — CPU1's unconditional watchdog-kick is still this project's only
   automatic crash-recovery path. It now has its own design document,
   `docs/phase2-all-cores.md`, which also corrects an assumption in this file:
   the *first* blocker is not CPU1's watchdog but the single-instance module
   state in `gic_timer.c`/`vgic.c`.
