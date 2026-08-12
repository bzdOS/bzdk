# Silent eMMC write corruption: a clock that goes backwards

Found, root-caused, fixed and validated on real hardware on 2026-08-12, with a
before/after number on the same test. `emmc_bio_write()` was damaging roughly one
sector in eight hundred and reporting every one of them to the guest as a
success.

## The measurement first

68 MB written to the raw device (`/dev/vtbd0p4`, swap off — 1.1 GB, unused,
outside the root filesystem), then read back twice. No filesystem on the path, no
buffer cache, both reads agreeing with each other:

| | before the fix | after the fix |
|---|---|---|
| damaged sectors | **160** of 133632 (0.120%) | **0** |
| `write_retries` `[57]` | 15660 | 0 |
| `busy_timeouts` `[10]` | 14710 | 0 |
| error-recovery `settles` `[8]` | 15660 | 0 |
| `ebio_fails` `[0]` | 950 | 0 |
| `g_ioerrs` `[42]` | 0 | 0 |

`g_ioerrs` is the row that matters most: it was **zero on both sides**. Every one
of those 160 damaged sectors was reported to the guest as a completed, successful
write.

An independent instance of the same damage was found first, in the guest's copy
of a 68 MB tarball: identical length to the host's, differing in **94 of 133715
sectors**.

## The causal chain

1. **`rd_cntpct()` transiently returns a value smaller than one read moments
   earlier.** It already carries the `isb` the architecture requires before
   reading `CNTPCT_EL0` — that was checked first, and it was not the gap. *Why*
   the counter does this is still open; the fix does not depend on the answer.
2. `now - start` is unsigned, so a backwards read **underflows to ~2^64**, which
   exceeds any cap.
3. The post-write `CARD_BUSY` wait therefore times out **immediately** and
   returns `-2` while the card is still programming the block.
4. `-2` is retryable, so `vblk_emmc.c`'s write-retry loop re-issues CMD24
   against that still-busy card.
5. The block lands on the medium with an intact head and **the sector's own
   opening bytes re-appended** to fill out the rest.
6. Nothing reports an error, because the final attempt returns 0.

## The signature, and how to tell it from its neighbours

Damage is **whole-sector-granular**, in **isolated single sectors**, always a
**contiguous tail running to byte 511**, with the split point scattered
(measured: 272..508) and always a multiple of 4. In the tarball, **71 of the 94**
damaged sectors held, verbatim, their own first `512-K` bytes at offset `K`.

Never a copy of a *different* sector — which is what rules out stale-block
substitution, the other candidate this project has chased before.

This is the exact inverse of the discriminator in
`docs/`-adjacent project memory (`corruption-signature-exonerates-block-layer`):
field-granular damage *inside* a 512-byte sector, with the neighbouring inode
intact, cannot come from a whole-sector writer and exonerates the block layer.
Damage aligned to whole sectors, with the sector's own head re-appended, indicts
it.

## How the contradiction was spotted

The breadcrumbs disagreed with themselves:

```
[10] busy_timeouts = 14710     the post-write busy wait timed out
[11] elapsed ms    = 0         ...having waited under 1 ms
[12] CNTFRQ        = 24000000  ...computed from a correct frequency
```

Those cannot all be true. The branch is entered only when
`rd_cntpct() - start > cap`, and `cap = ms_to_ticks(4000)` = 96e6 ticks, so the
elapsed time must exceed 4000 ms; the branch then re-reads the counter and
computes 7627 ticks, or 0.32 ms.

`emmc_bio.c`'s own comment at that branch had already committed to the fork, a
fortnight earlier: *"If `[11]` is ~4000 the card really does stall that long and
the retry count has to be explained some other way; if it is tiny, the cap is
expiring early and the timeout math is the bug."* It was tiny.

What closed it was recording the **ingredients instead of a derived number**:
`cap` as computed, and `el` as the raw 64-bit difference split across two words
so an unsigned underflow could not hide inside a truncating cast. `cap` came back
exactly right (96000000) and `el` came back tiny — which leaves only "the two
reads disagree".

## The fix

`58c3624`. Guard the subtraction and require confirmation:

```c
uint64_t now = rd_cntpct();
uint64_t el  = (now >= start) ? (now - start) : 0ull;   /* backwards -> 0 */
if (el > cap) {
        if (++over < 2)
                continue;          /* re-read before believing it */
} else {
        if (over) { ebio_bc(18, ++g_cnt_anom); over = 0; }
        continue;
}
```

Applied to both timeout loops in `emmc_bio_write()`. The principle is the
transferable part: **a bounded wait must never be decidable by a single read of
an unreliable clock.**

Note which half of the guard actually fires. `[18]` counts an over-cap read
followed by an under-cap read; after the fix it stays at 0 while the timeouts
stop entirely, which says the anomaly is the **backwards** read caught by the
ternary, not an over-cap spike. That asymmetry is how the direction of the
counter glitch was established.

## What this supersedes

Guest filesystem corruption on this rig was attributed to unclean stops, and
`docs/guest-rootfs-persistence.md` describes that ratchet in detail. The ratchet
is real and the `/sbin/shutdown` ENOEXEC finding stands — but it was not the
principal source. A write path damaging one sector in eight hundred produces the
same corrupt inodes, unexecutable binaries and userland SIGSEGVs *whether stops
are clean or not*. "Unclean stop" was an explanation that fit the evidence
without being the cause.

## How to test this without fooling yourself

Two traps, both of which caught this investigation before it got a real number:

- **The guest's buffer cache will serve your verification read and prove
  nothing.** A 68 MB file fits in the guest's 1 GiB several times over. The tell
  is `g_reads` `[6]` not moving at all across a verification pass — that happened
  here, and it invalidated a "reads are deterministic, the round trip is clean"
  conclusion that had already been drawn. Read the **raw device** and confirm two
  independent raw reads agree.
- **Do not drive it from the HV side over EMAC.** `emmc_raw.py`'s calls are
  paced roughly 80 ms apart by debug-channel round trips, and `emmc_bio.c`'s own
  comment says that is exactly why `dbgmon`/CPU1 callers never hit back-to-back
  timing bugs. A paced test structurally cannot reproduce this.

## Deliberately left open

Two defects found in the same reading and **not** bundled into this fix, so that
this one could be measured on its own:

- `emmc_bio_read()` checks **no** RINT error bits at all, while the write path
  checks two. A read can therefore return `rc = 0` on data the controller
  flagged. Measured this boot, the read-side accumulator `[13]` holds only
  `{CMD_DONE, DATA_OVER, RX_DATA_REQ}` — no error bit — so the missing check has
  not been swallowing anything yet. It is still a missing check.
- Both hot paths reset the FIFO with a fixed `small_delay()` (64 register reads)
  rather than polling the self-clearing reset bits. `ebio_fail_settle()`'s own
  comment documents that approach as *"reasoned from first principles and it was
  wrong"* and polls `GCTL_RESET_ALL` instead — the lesson was applied in one
  function and not in the two on the hot path.
- **Why `CNTPCT_EL0` reads backwards at all.** The guard makes it harmless, not
  absent. `[18]` and the timeout counters are the instruments to watch.
