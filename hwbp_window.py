#!/usr/bin/env python3
"""Decode hwbp.c's breadcrumb window (HWBP_BC_BASE 0x50000600, 21 words).

Exists because reading this window by hand cost real time twice, and both times
the mistake was the same shape: treating a slot that was never written as data.
Two specific traps this decoder refuses to let you fall into.

  1. UNWRITTEN slots read back 0xFFFFFFFF, not 0. hwbp_init() now zeroes [1..20]
     so a real zero means "nothing happened", but an OLDER build on the board
     does not -- so all-ones is reported as "never written", never as a value.

  2. A hit record is only fresh if hits >= 1 AND the recorded PC matches the
     breakpoint the build was compiled for. Slots [12..15] once returned a
     15-day-old PC from an unrelated experiment and were very nearly reported as
     the answer.

The [17] slot is the point of the whole thing: DBGBCR0_EL1 as the HARDWARE reads
it. The [10] bitmap is only hwbp.c's own bookkeeping variable and cannot know the
guest overwrote the register behind it -- FreeBSD's dbg_monitor_init() zeroes
DBGBCR/DBGBVR on every CPU it brings up. If [10] says armed and [17] does not,
[17] is right. [16] counts how many times the keep-alive had to put a slot back.
"""
import argparse
import sys

import hvdbg

BASE = 0x50000600
NWORDS = 21
MAGIC = 0x48574250  # "HWBP"
BCR_ARM = (1 << 0) | (0x3 << 1) | (0xF << 5)  # 0x1e7, what hwbp_set() programs

NAMES = {
    0: "magic",
    10: "armed bp bitmap (SOFTWARE bookkeeping -- may lie)",
    11: "armed wp bitmap (SOFTWARE bookkeeping -- may lie)",
    16: "re-arms performed by hwbp_reassert()",
    17: "DBGBCR0_EL1 read back from the HARDWARE",
    18: "MDSCR_EL1", 19: "MDCR_EL2", 20: "OSLSR_EL1",
}


def u64(lo, hi):
    return ((hi & 0xFFFFFFFF) << 32) | (lo & 0xFFFFFFFF)


def unwritten(v):
    return v == 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect-pc", type=lambda x: int(x, 0), default=None,
                    help="the GUEST_BP_ADDR this build was compiled with; "
                         "a hit record is only trusted if it matches")
    ap.add_argument("--iface", default="br0")
    a = ap.parse_args()

    hv = hvdbg.HV()
    # read_words_stable, not read_words: this window is written by CPU0 with an
    # explicit dc civac + dsb, but it is READ over the EMAC channel serviced by
    # CPU1, and a single sample of a concurrently-written window has handed this
    # project a torn 64-bit value before.
    w = hv.read_words_stable(BASE, NWORDS)

    if w[0] != MAGIC:
        if unwritten(w[0]):
            print("magic slot is all-ones: this build never wrote the hwbp "
                  "window at all. Nothing here is data.")
        else:
            print(f"magic = {w[0]:#010x}, expected {MAGIC:#010x} ('HWBP'). "
                  "Some other instrument owns this address in this build "
                  "(ring.c also claims 0x50000600) -- do not read on.")
        return 1

    hits = w[1]
    armed_sw = w[10]
    reasserts = w[16]
    bcr_hw = w[17]

    # Builds before 2026-08-18 wrote MDSCR/MDCR/OSLSR to a hardcoded 0x50000638,
    # i.e. slots [14],[15],[16] of this very window. On such a build the argument
    # probe reports MDCR<<32|MDSCR as "sgl->dma_map" and [16] reports OSLSR as a
    # re-arm count -- both entirely plausible-looking. Detect it and say so.
    squatted = (len(w) > 20 and w[18] == 0xFFFFFFFF and w[14] != 0xFFFFFFFF
                and (w[15] & ~0xFFF) == 0)
    if squatted:
        print("** THIS BUILD PREDATES THE SLOT-OWNERSHIP FIX: el2_exc.c is\n"
              "   squatting slots [14..16] with MDSCR/MDCR/OSLSR. The argument\n"
              "   probe and the re-arm count below are NOT data.\n")

    print(f"hits                  = {'<never written>' if unwritten(hits) else hits}")
    print(f"armed bitmap [10]     = {armed_sw:#010x}   (software bookkeeping)")
    if unwritten(bcr_hw):
        print("DBGBCR0_EL1 [17]      = <never written> -- build predates the "
              "hardware read-back; 'armed' is UNVERIFIABLE on this build")
    else:
        state = "ARMED" if (bcr_hw & 1) else "DISARMED BY THE GUEST"
        note = "" if bcr_hw == BCR_ARM else f"  (expected {BCR_ARM:#x} when armed)"
        print(f"DBGBCR0_EL1 [17]      = {bcr_hw:#010x}  -> {state}{note}")
        if (armed_sw & 1) and not (bcr_hw & 1):
            print("  ** [10] claims armed and the hardware disagrees. The "
                  "hardware is right: the slot is dark and no hit can fire.")
    if unwritten(reasserts):
        print("re-arms [16]          = <never written> (no keep-alive in this build)")
    else:
        print(f"re-arms [16]          = {reasserts}"
              + ("   <- the guest really was clearing the slot"
                 if reasserts else "   (guest never cleared it)"))

    # Printed BEFORE the no-hit early return: "why did nothing fire" is exactly
    # the question these three answer, so they must not be reachable only on the
    # success path.
    if not unwritten(w[18]):
        mde = "MDE=1" if (w[18] & (1 << 15)) else "MDE=0 **"
        tde = "TDE=1" if (w[19] & (1 << 8)) else "TDE=0 **"
        oslk = "OSLK=0" if not (w[20] & (1 << 1)) else "OSLK=1 (LOCKED) **"
        print(f"preconditions         = MDSCR {w[18]:#x} {mde} | "
              f"MDCR {w[19]:#x} {tde} | OSLSR {w[20]:#x} {oslk}")

    if unwritten(hits) or hits == 0:
        print("\nNo hit recorded. Slots [12..15] are NOT the answer -- they may "
              "hold values from an entirely different, older experiment.")
        return 0

    pc = u64(w[6], w[7])
    far = u64(w[8], w[9])
    x1 = u64(w[12], w[13])
    probed = u64(w[14], w[15])
    print(f"\nkind                  = {'breakpoint' if w[2] == 0 else 'watchpoint'}"
          f"  slot {w[3]}")
    print(f"EC                    = {w[5]:#x}")
    print(f"hit PC                = {pc:#018x}")
    print(f"hit FAR               = {far:#018x}")
    if a.expect_pc is not None and pc != a.expect_pc:
        print(f"  ** STALE: does not match this build's breakpoint "
              f"{a.expect_pc:#018x}. Treat every field below as garbage, "
              f"including x1 -- that exact mistake was made here before.")
        return 0
    if a.expect_pc is None:
        print("  ** no --expect-pc given, so freshness is UNCHECKED. Pass the "
              "build's GUEST_BP_ADDR before believing anything below.")
    else:
        print("  (PC matches the compiled breakpoint -- this record is fresh)")
    print(f"x1 (first argument)   = {x1:#018x}")
    if probed == 0xDEADBEEF:
        print(f"*(x1 + {24})            = <guest VA did not translate>")
    else:
        print(f"*(x1 + {24})            = {probed:#018x}"
              "   (sgl->dma_map, for the linux_dma_unmap_sg_attrs case)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
