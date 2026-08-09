# Dual-guest: FreeBSD (CPU0) + Zephyr (CPU3), genuinely concurrent

## Status, 2026-08-10

| Claim | True? | How it was checked |
|---|---|---|
| The mechanism **exists** (new stage-2 table, disjoint loader, CPU-routed console/fault dispatch) | ✅ | `smp.c`, `zguest_cpu3.c`, `zload2.c`, `stage2_zephyr.c`, `mmio_absorb.c`, `vconsole.c`, `el2_exc.c` — commit `7e5303c` |
| PSCI `CPU_ON` bring-up of all 4 cores works under QEMU | ✅ | `smp-qemu-ci.sh`, 5/5 — commit `c78280d` |
| CPU0 and CPU3 run **genuinely concurrently** under QEMU (not sequentially) | ✅ | `dual-qemu-ci.sh`, real hand-built ELF through the real `zguest_cpu3`/`zload2` path, 10/10 — commit `fc20b63` |
| **Real Zephyr** boots concurrently under QEMU | ✅ | `dual-zephyr-qemu-ci.sh`, real banner + heartbeats via channel-1 vconsole, 10/10 — commit `779153d` (needed a new `bpi_m64_hv_dual` Zephyr board port, `sram0` moved to `0xBE000000`) |
| CPU3 runs **genuinely concurrently with FreeBSD on the real board** | ✅ **confirmed 2026-08-10** | see below |
| **Real Zephyr** on CPU3 on the real board | ❌ **not attempted** — needs a bulk loader that doesn't exist yet | see "What's still open" |

None of these five rows are interchangeable — "the mechanism exists" is not "it boots," and "boots under QEMU" is not "boots on the board." This project has burned time before on reports that blurred exactly this kind of distinction (see `zephyr-guest.md`'s own header note).

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

## What's still open

1. **A real bulk loader for staging a full guest image over EMAC.** `zguest_cpu3.h` flagged this as out of scope from the start; this test confirms exactly why it's needed — the manual per-word technique used here only works because the Step-1 trivial payload's real content is 144 bytes. A real Zephyr image (or anything bigger) needs either a proper bulk-write extension to the debug protocol, or reusing U-Boot's own `tftpboot` for a *second* image before `bootelf` jumps into the HV (chimpd's own flow already does exactly this for the DTB and the FreeBSD kernel — extending it to stage a third file at `ZG3_ELF_STAGE_PA` is probably the more natural fix, since the infrastructure already exists for two files).
2. **Real Zephyr, on the real board, concurrently with FreeBSD** — blocked purely on (1); the mechanism itself is proven both under QEMU (`dual-zephyr-qemu-ci.sh`) and on hardware (this test, with a stand-in payload).
3. **CPU3's halt-on-failure has no re-arm path.** A `zunhalt` (or similar) dbgmon command that resets `zephyr_cpu3_run()`'s internal state and re-enters the wfe-poll loop, without a full board reload, would make iterating on this much cheaper — not implemented, not required for this milestone's own DoD.
4. **Phase 2** (repurposing CPU1/CPU2 for further guests) remains explicitly out of scope, per the original architecture plan — CPU1's unconditional watchdog-kick is still this project's only automatic crash-recovery path.
