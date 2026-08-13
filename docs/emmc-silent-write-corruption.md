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

## The other two defects, fixed afterwards (`d57607e`)

Both were found in the same reading and deliberately kept out of the CNTPCT fix
so that one could be measured alone. Both are now closed, and measuring them
produced a result worth more than the fixes.

### The read path checked no error bits

`emmc_bio_read()` returned 0 the moment 128 words had drained and `DATA_OVER` had
latched, so a block the controller had flagged went to the guest as good data —
invisible to every vblk IOERR counter, since those only count non-zero `rc`. The
write path had checked error bits all along. That asymmetry was the defect.

`RINT_READ_ERR_MASK` covers response/data CRC, data timeout, FIFO-run,
hardware-locker, and start/end-bit errors. **It excludes bit 8**, and the reason
turned into the interesting part — see below.

Reads are not retried below this layer (`vblk_emmc.c` has `VBLK_WRITE_RETRIES`
and no read equivalent), so a flagged read becomes `S_IOERR` and FreeBSD retries.
A reported error the guest can retry beats silently plausible wrong bytes.

**Validated no-op on healthy hardware:** across a boot plus a 68 MB raw round
trip — 6763 reads — `[20] read_errs = 0` and `[19] read_err_bits = 0x0000`. Zero
false positives, exactly as the accumulator predicted.

### Bit 8 was a symptom, not a mystery

The mask excluded bit 8 because `[14]` — an OR across every *successful* write
since boot — had it set, and a bit that behaves unlike its documented name
`RESP_TIMEOUT` is the wrong thing to start failing I/O on.

With the CNTPCT fix in place, **bit 8 disappeared**: `[14]` went from `0x011c` to
`0x001c`. So bit 8 was never anomalous. It was the retry storm issuing CMD24 at
cards that were still programming and collecting genuine response timeouts —
`RESP_TIMEOUT` was correctly named the whole time, and it was a *consequence* of
the bug, observed while the bug was still live.

The exclusion is therefore conservative rather than necessary, and can be
revisited. It costs nothing to leave in place, and the reasoning is recorded here
so a future reader does not have to re-derive why bit 8 looked suspicious.

Also worth noting: this is a warning about OR accumulators generally. `[14]`
having a bit set means *at least one* operation set it, never *every* one — a
distinction easy to overstate, and overstated once during this hunt.

### The FIFO reset now waits — and probably never needed to

Both hot paths reset the FIFO with a fixed `small_delay()` (64 register reads)
instead of polling the self-clearing bits. `ebio_fail_settle()`'s own comment
calls that approach *"reasoned from first principles and it was wrong"* and polls
`GCTL_RESET_ALL` — the lesson was applied in one function and not in the two that
run on every sector. Now shared as `gctl_reset_and_wait()`, bounded, and used by
both.

**But the measurement does not support calling this a live bug.** `[21]`, the
worst spin count before the reset bits self-cleared, reads **1** — the reset is
not instantaneous, but it completes in about one register-read time, which
`small_delay()`'s 64 reads comfortably covered. `[22]` (resets that never
cleared) is 0.

So: correct by construction, and honestly not a defect that was firing. The value
is that a slow reset can no longer be silently overrun, and the claim is now
measured instead of assumed. `emmc_bio_init()` is left alone — it writes
`GCTL_RESET_ALL` wholesale as a bring-up reset, which is a different operation and
is hardware-validated.

### Regression check

Both fixes in, same test: readback hash identical to the reference,
`TOTAL DAMAGED SECTORS: 0`. `write_retries`, `busy_timeouts`, `settles`,
`ebio_fails`, `read_errs` and `g_ioerrs` all 0 across 6763 reads and 4655 writes.

## The third hole, closed afterwards (`97e107b`)

`emmc_bio_write()`'s `ri & 0x0180` check sits after
`if (ri & RINT_DATA_OVER) break;`, so any error bit latching no later than
`DATA_OVER` was never examined and the write completed as a success. The same
defect as the read path's missing check, in a different shape.

### The precondition mattered more than the check

Failing a write here makes `vblk_emmc.c` **retry** it, and a retry landing on a
still-programming card is the corruption mechanism this entire document is about.
So the check could not be made fatal until the retry path was safe.

It was not safe. `ebio_fail_settle()` waited for card-idle using
`wait_card_idle()`, bounded by `EMMC_POLL_CAP` **register reads** — a count, not a
duration. A flash program takes tens of milliseconds, so that wait could return
with the card still busy, and the caller's next act is the retry. **The settle was
never sufficient protection**; it only looked like it because the FIFO/DMA reset
half of it is real. That is worth stating plainly, because "we settle before
retrying" was the standing reassurance in this file's comments.

`wait_card_idle_timed()` is bounded by time and uses the **same two-read
discipline** as the timeout loops — writing it the obvious way would have
re-introduced, inside the fix, the exact CNTPCT-goes-backwards bug the fix exists
to prevent. Its fast path reads one register and no clock at all, so
`emmc_bio_read()`'s entry guard keeps the cheapness the original had. Used by
both the settle and that entry guard.

### Measured

Same test, all three fixes in: readback and a second independent read both
identical to the reference, **`TOTAL DAMAGED SECTORS: 0`**. Across a boot plus a
full 68 MB round trip (6762 reads, 4645+ writes):

```
ebio_fails 0   settles 0   busy_timeouts 0   write_retries 0   g_ioerrs 0
read_errs 0    dataover_fails 0              cnt_anomalies 0
RINT_or_read 0x002c   RINT_or_write 0x001c   write_dataover_errs 0x0000
```

`[24] dataover_fails = 0`: the new fatal check has not fired once — no false
positives, as `[23]`'s prior `0x0000` predicted.

### What is NOT validated, and why

`[25] settle_busy_ms = 0` with `[8] settles = 0`. The settle path is only reached
on a failure, and there are no failures any more — so the timed card-idle wait
has **not been exercised as the retry safeguard at all**. What `[25] = 0` does
show is that `emmc_bio_read()`'s entry guard never finds the card busy for a
measurable millisecond.

So the retry-path fix is insurance whose value appears only when something fails.
It is correct by construction and unproven under load, and those are different
claims. The same honesty applies to `[21] gctl_rst_spins = 1`: the FIFO reset
takes about one register-read time, which the old fixed `small_delay()` of 64
reads comfortably covered.

Two of the three secondary fixes, then, are hardening rather than caught bugs.
Only the read-side missing error check closed a hole that was demonstrably
capable of returning bad data as good.

## What fault injection found (`35d2b91`..`1b3c02e`)

The retry safeguard shipped unproven, so injection was built to execute it on
demand: bail out of a write at a chosen point, with a chosen card-idle wait, at a
chosen rate, confined above an LBA floor so it can never reach the guest's root.
It refuted two things I had already written down and committed, and found a bug
the CNTPCT fix had hidden rather than removed.

### It named the wrong loop

Injecting **after `DATA_OVER`** — the post-write `CARD_BUSY` path, which the
document blamed — produced **zero** corruption with both the timed and the legacy
wait. 267 injections, 267 retries, 267 rescues, 0 damaged, twice.

Correctly so: once `DATA_OVER` has latched, all 128 words have reached the card,
so no partial block is possible — and a partial block is what the signature
requires. Re-reading the original counters agrees, and I had read them the wrong
way round: 160 corrupt sectors against `ebio_fails = 950` is 1 in 6, while against
`busy_timeouts = 14710` it is 1 in 92. **The data-phase timeout was the corrupting
path; the busy timeout supplied the volume.** Both loops shared the unguarded
subtraction, so `58c3624` fixes both — but the mechanism was mis-attributed.

### The timed card-idle wait does not prevent this

Injecting at the **data-phase** point with the timed wait in place: **267
injections, 267 damaged sectors. One for one.** So the retry-path hardening was
not the precondition this document claimed it was. It was worth doing on its own
terms and it does not prevent this.

### And it exposed a live bug the CNTPCT fix had only hidden

This file asserted, as a note of fact:

> NOTE the FIFO reset discards the undelivered words. That is correct here and
> only here: these are single-block CMD24 transfers, so the caller re-pushes the
> whole 512 B sector from the bounce buffer on retry — nothing is half-written
> from the host's side.

The host side is not half-written. **The card side is.** Discarding the FIFO
abandons a block the card has already begun accepting, and the retry's 128 words
are appended to what it already took rather than replacing it — exactly the
measured shape. So **any genuine data-phase failure** — a real CRC error, a real
timeout — would still corrupt on retry. Fixing `CNTPCT` removed the trigger and
left the mechanism armed.

The remedy was written two lines below the wrong claim (*"an explicit CMD12
STOP_TRANSMISSION ... is the next step"*), gated on `ebio_fails` climbing, which
it stopped doing once the spurious failures went away.

### CMD12, and what it took to aim it

| run | damaged sectors |
|---|---|
| no CMD12 | **267** of 133632 |
| CMD12 from every settle | 75 (in a run that aborted early — see below) |
| CMD12 only where the card can have a transfer open | **2** |

Sending it from every settle was wrong and measurement said so at once: CMD12
failed 106 of 547 times, `ebio_fails` went 0 → 354, and one write reached the
guest as `S_IOERR`, aborting the workload at 49 MB of 68. `STOP_TRANSMISSION` with
no transfer open is an illegal command, and the settle is called from every
failure path — most of them past `DATA_OVER`.

Now `ebio_fail_settle_full(stop_card)`, with the plain wrapper defaulting to **no**
CMD12 so the dangerous case must be spelled out. Sent from exactly the three write
paths that can leave words undelivered (push loop short, data-phase error branch,
data-phase timeout branch) and from the injection that mimics the third.

**Remaining gap, precisely bounded:** CMD12 still fails **141 of 268** times
(53%), and the 2 sectors that still get damaged have a *different* signature —
`first_off 0`, the whole sector wrong rather than a head-plus-own-head hybrid —
consistent with the abort not taking. Damage is down 130× with a CMD12 that works
barely half the time; making its delivery reliable is the path to 0. Candidates,
in order: issue it *before* the FIFO reset while the transfer is still live on the
bus; handle its R1b busy response rather than polling `CMD_DONE` alone; or
re-program the controller clock after the reset, which `ebio_fail_settle()`'s
comment records as removed for breaking the guest and would need its own soak.

## Still open

- **CMD12 delivery** — see the table above: 53% failure rate, 2 sectors, a clear
  candidate list.
- **Why `CNTPCT_EL0` reads backwards at all.** The guard makes it harmless, not
  absent. `[18]` and the timeout counters are the instruments; note that `[18]`
  counts only the over-then-under pair, and the *backwards* read is caught
  silently by the ternary — which is itself how the direction of the glitch was
  established.
- **The retry path's safety is untested**, per the section above. A deliberate
  fault-injection run (force a write failure and confirm the retry waits for
  card-idle before re-issuing CMD24) is the missing verification.
- **Why `CNTPCT_EL0` reads backwards at all.** The guard makes it harmless, not
  absent. `[18]` and the timeout counters are the instruments; note that `[18]`
  counts only the over-then-under pair, and the *backwards* read is caught
  silently by the ternary — which is itself how the direction of the glitch was
  established.
