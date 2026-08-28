# SeMa — semantic claim markup

Every factual claim in a comment or doc carries an epistemic tag. The tag
answers one question: **how do we know?** A claim without a tag is assumed
to be the author's intent/design reasoning (safe); a claim about the
*world* (hardware behaviour, measured rates, what a driver does) without a
tag is a bug waiting to mislead the next session.

## Tags

| Tag | Meaning | Example |
|---|---|---|
| `[MEASURED YYYY-MM-DD]` | Observed on THIS hardware, with the instrument named | `[MEASURED 2026-08-27: vblank 60.8 Hz via BC_HDMI word 12]` |
| `[INFERENCE src]` | Derived from source reading, never observed live | `[INFERENCE files.arm64: aw_cir optional line]` |
| `[RETRACTED YYYY-MM-DD: why]` | Was claimed, proven wrong; keep for the tombstone value | `[RETRACTED 2026-08-27: driver absent from kernel image]` |
| `[QEMU-ONLY]` | Established under QEMU; unproven on the board | `[QEMU-ONLY: dram_copy FAR=0xC0000000 fault]` |
| `[HW-PROVEN YYYY-MM-DD]` | Hardware run exists and passed | `[HW-PROVEN 2026-08-27: hw.ncpu=4]` |

## Rules

1. Tags attach to CLAIMS, not files. One comment can carry several.
2. A `[MEASURED]` tag must name the instrument (which BC window, which
   command, which log) — an unnameable measurement is an INFERENCE.
3. Do not retro-tag old comments wholesale. Tag when you touch a claim, and
   when you prove one wrong (RETRACTED keeps the history — deletion loses
   the lesson; see `hv_addrmap.h`'s retired 0xC0000000 note).
4. Dates are UTC-date of the observation, not of the writing.
5. Contradictory tags on the same claim are resolved by the NEWER date; the
   loser becomes a RETRACTED line, it does not get deleted.

## Why

2026-08-27's session found at least three comments asserting things the
board had outgrown or that were only ever true under QEMU, and per
CLAUDE.md stale comments have produced multiple confidently-wrong
diagnoses in this project. SeMa makes the trust level of every claim
machine-greppable: `grep -rn "INFERENCE" *.c docs/` is now a work queue.
