#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""triage.py -- ONE command that answers "what state is the board actually in".

WHY: diagnosing the 2026-07-29 guest freeze took ~15 separate ad-hoc board
queries, and the two facts that mattered most were both available in the first
30 seconds and both went unnoticed for an hour:

  * the board was running a DIFFERENT build than the source tree
    (79ae093f109c+ vs 08688afa045f+), so every conclusion drawn from reading
    stage2.c/el2_exc.c was about code that was not on the board; and
  * GICH_HCR = 0, i.e. the vGIC's virtual CPU interface was OFF, which is what
    makes gic_timer.c's mask-and-reinject vtimer path lose ticks forever.

Neither needed a new instrument -- only for something to LOOK. So: look at all
of it, every time, in one round trip's worth of work, and decode it (armdec.py)
rather than printing hex.

The single most load-bearing trick here is SAMPLING TWICE: a frozen counter and
a spinning one are indistinguishable in one snapshot, and "which cores are
still alive" is the question that separates "the guest is wedged" from "the
guest is idle" from "the whole HV is dead". Everything time-varying below is
sampled twice with a gap and reported as MOVING/frozen.

    python3 triage.py                 # full report
    python3 triage.py --gap 4         # longer settle for slow/degraded links

Read-only: issues no `call`, no `w`, nothing that changes board state. Safe to
run against a wedged board, and safe to run repeatedly.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armdec                                   # noqa: E402
import hvdbg                                    # noqa: E402

# Windows worth dumping unconditionally. Kept here rather than in each caller
# because the whole point is that nobody should have to remember which window
# holds the answer. Addresses mirror hv_addrmap.h / each module's own header.
STG2_BC = 0x50000C00
VBK_BC = 0x50020000
EBIO_BC = 0x50020200
ASYNC_BC = 0x50020600
USED_LOCK = 0x50020700
FLTR = 0x50012000
EXC_BC = 0x50000400

GICD = 0x01C81000
GICH = 0x01C84000

FLTR_MAGIC = 0x464C5452
FLTR_KINDS = {1: "FAULT", 2: "TRAP", 3: "IRQ", 4: "VIRTIO", 5: "CONSOLE",
              6: "TIMER", 7: "SYNC", 8: "HVVIOL"}

VBK_LABELS = {
    0: "magic", 6: "g_reads", 7: "g_writes", 8: "last_sector", 10: "g_irqs",
    13: "g_faults", 20: "g_truncated", 21: "isr_status_reads", 22: "isr_acks",
    24: "g_async_posts", 25: "g_async_completes", 26: "g_async_fallbacks",
    27: "g_gmem_oob", 28: "g_bad_queue_num", 29: "g_bad_desc_flags",
    30: "g_kick_not_ready", 31: "g_kicks_seen", 32: "g_heads_popped",
    33: "g_indirect_seen", 34: "g_short_chain", 35: "last_hdr_type",
    36: "last_hdr_sector_lo", 37: "g_hdrs_read", 39: "last_post_retval",
    40: "g_dabt_seen", 41: "last_fault_ipa",
    # Sticky S_IOERR forensics (see vblk_emmc.c's bc[42..56] block). Every
    # other completion breadcrumb above is clobbered by the NEXT request, so
    # these are the only ones that survive to describe a failure.
    42: "g_ioerrs", 43: "g_ioerr_busy", 44: "g_ioerr_badpa",
    45: "g_ioerr_unaligned", 46: "g_ioerr_emmc", 47: "g_ioerr_capacity",
    48: "g_ioerr_notready",
    49: "ioerr1_lba", 50: "ioerr1_rc", 51: "ioerr1_bytes", 52: "ioerr1_tag",
    53: "ioerrN_lba", 54: "ioerrN_rc", 55: "ioerrN_bytes", 56: "ioerrN_tag",
    # Transient write-stall retries. Non-zero retries WITH rescues tracking
    # them is the card stalling and recovering; retries climbing while rescues
    # stay flat means the failure is not transient after all.
    57: "write_retries", 58: "write_retry_ok",
    # Lock-acquire patience. [59] moving is ordinary two-core starvation being
    # absorbed instead of handed to the guest as an S_IOERR. [60] moving means
    # the lock is genuinely STUCK (a leaked unlock), which waiting cannot fix.
    59: "lock_retries", 60: "lock_giveups",
}

# bc[42..56] decode. Read as signed: serve_data's codes are negative.
VBK_IOERR_RC = {0: "n/a (capacity/not-ready reject)",
                -100: "VBLK_RC_UNALIGNED (partial-sector stitch)",
                -200: "VBLK_RC_BUSY (eMMC cross-core lock timeout)",
                -300: "VBLK_RC_BADPA (data PA outside DRAM)"}

# Interrupts this tree actually cares about, so the GIC section is a verdict
# rather than a register dump. The third field says whether the enable/pending
# state is BANKED per CPU interface: for SGIs (0-15) and PPIs (16-31) it is, so
# a read issued by the CPU1 debug core reports CPU1's OWN view and says nothing
# about the guest's core -- printing it without that warning is an invitation to
# conclude "the guest disabled its timer" from another core's register bank.
# (Exactly the shape of the 2026-07-24 misread where CPU1's HCR was mistaken
# for the guest's; see memory fsck-hang-guest-side-dead-timer.)
WATCH_INTIDS = [(137, "virtio-blk (SPI 105)", False),
                (138, "virtio-net (SPI 106)", False),
                (27, "CNTV virtual timer PPI", True),
                (106, "EHCI (SPI 74) -- known storm source", False)]


def rd(hv, pa, n=1, tries=12):
    """Read n words, retrying: read_words() returns [] on ANY dropped word."""
    for _ in range(tries):
        w = hv.read_words(pa, n)
        if w:
            return w
    return None


def section(title):
    return ["", f"=== {title} " + "=" * max(0, 62 - len(title)), ""]


def check_build(hv, out):
    out += section("BUILD IDENTITY (check this FIRST, always)")
    try:
        match, on_disk, running = hv.check_build_id()
    except Exception as e:                                   # noqa: BLE001
        out.append(f"  build-id check unavailable: {e}")
        return
    out.append(f"  on disk : {on_disk}")
    out.append(f"  running : {running}")
    if match:
        out.append("  OK -- source in this tree describes the running image.")
    else:
        out.append("  *** MISMATCH ***  The board is NOT running this tree's")
        out.append("  build. Every conclusion drawn from reading .c files here")
        out.append("  may be about code that is not on the board, and any")
        out.append("  hv.call() to an nm-resolved address is an arbitrary jump.")
        out.append("  Reload before diagnosing anything else.")


def check_liveness(hv, out, gap):
    """Which cores are still executing? Sample twice; deltas are the answer."""
    out += section(f"LIVENESS (two samples, {gap}s apart)")

    def snap():
        return {
            "fltr_total": rd(hv, FLTR + 4),
            "used_lock": rd(hv, USED_LOCK),
            "async_bc": rd(hv, ASYNC_BC, 3),
            # 64 words == the whole HVMAP_VBLK_BC_SIZE window (bc[0..63]), so
            # the sticky IOERR fields at bc[42..56] are included. Was 48, which
            # silently cut them off.
            "vbk": rd(hv, VBK_BC, 64),
            "peek": hv.dbgtools_peek(),
        }

    a = snap()
    time.sleep(gap)
    b = snap()

    def cmp1(name, key, idx=0, note=""):
        va = a[key][idx] if a[key] else None
        vb = b[key][idx] if b[key] else None
        if va is None or vb is None:
            out.append(f"  {name:<24s} READ FAILED")
            return None
        tag = "MOVING" if va != vb else "frozen"
        d = f"  (+{vb - va})" if va != vb else ""
        out.append(f"  {name:<24s} {tag:<7s} {va} -> {vb}{d} {note}")
        return va != vb

    hb_a = a["peek"]["heartbeat"] if a["peek"] else None
    hb_b = b["peek"]["heartbeat"] if b["peek"] else None
    cpu1 = hb_a is not None and hb_a != hb_b
    out.append(f"  {'CPU1 heartbeat':<24s} {'MOVING' if cpu1 else 'frozen':<7s} "
               f"{hb_a} -> {hb_b}   (debug core)")
    cpu0 = cmp1("CPU0 flightrec total", "fltr_total", note="(EL2 event logging)")
    async_moving = (a["async_bc"] and b["async_bc"]
                    and a["async_bc"][1:] != b["async_bc"][1:])
    out.append(f"  {'CPU2 async poll':<24s} "
               f"{'MOVING' if async_moving else 'frozen':<7s} "
               f"{a['async_bc']} -> {b['async_bc']}")
    lock = rd(hv, USED_LOCK)
    out.append(f"  {'vblk used_lock':<24s} = {lock[0] if lock else '?'} "
               f"({'HELD -- suspect a core died inside the critical section'
                   if lock and lock[0] else 'free'})")

    out.append("")
    if cpu1 and async_moving and cpu0 is False:
        out.append("  VERDICT: only CPU0 is stopped. The HV and its helper")
        out.append("  cores are healthy, so this is the GUEST not being given")
        out.append("  anything to run -- either idle in WFI awaiting an")
        out.append("  interrupt that will never arrive, or wedged at EL1.")
        out.append("  NOTE: `gr` shows the SAVED frame of the LAST EL2 entry,")
        out.append("  not CPU0's live PC -- identical `gr` dumps do NOT prove a")
        out.append("  re-fault loop. The frozen counter above is what proves it.")
    return a["vbk"], b["vbk"]


def dump_vbk(out, vbk_a, vbk_b):
    if not vbk_a:
        return
    out += section("virtio-blk breadcrumbs (VBK1 @0x50020000)")
    for i, v in enumerate(vbk_a):
        if v == 0xFFFFFFFF:
            continue
        lbl = VBK_LABELS.get(i, "")
        moved = ""
        if vbk_b and vbk_b[i] != v:
            moved = f"  -> {vbk_b[i]}  MOVING"
        extra = ""
        if i == 41:
            extra = "   " + ("virtio-mmio window + 0x%02x" % (v & 0xFFF)
                             if 0x0A000000 <= v < 0x0A200000 else "")
            if (v & 0xFFF) == 0x064:
                extra += "  = InterruptACK"
            elif (v & 0xFFF) == 0x060:
                extra += "  = InterruptStatus"
            elif (v & 0xFFF) == 0x050:
                extra += "  = QueueNotify"
        out.append(f"  [{i:2d}] {lbl:<18s} = {v:#010x}  {v}{extra}{moved}")
    # The cross-checks that actually mean something.
    g = {VBK_LABELS[i]: vbk_a[i] for i in VBK_LABELS if i < len(vbk_a)}
    out.append("")
    irqs, acks = g.get("g_irqs"), g.get("isr_acks")
    if irqs is not None and acks is not None:
        out.append(f"  g_irqs={irqs} vs isr_acks={acks}: a small shortfall is")
        out.append("  normal EDGE coalescing (several set-pendings before the")
        out.append("  guest reads IAR collapse into one ISR), NOT lost IRQs.")
    posts, comps = g.get("g_async_posts"), g.get("g_async_completes")
    if posts is not None and posts == comps:
        out.append(f"  g_async_posts == g_async_completes ({posts}): CPU2 owes")
        out.append("  the guest nothing -- so a stalled guest is NOT waiting on")
        out.append("  virtio. Look at the timer.")
    dump_vbk_ioerr(out, g)


def dump_vbk_ioerr(out, g):
    """Verdict on the sticky S_IOERR fields (bc[42..56]).

    Worth its own block because a guest-visible "vtbd0: hard error" is the ONE
    symptom whose HV-side cause the rest of this dump cannot show: bc[9]/bc[12]
    describe only the most recent request, so a single later success erases the
    failure. Silence here (g_ioerrs==0) is a real result: it means the HV
    completed every request it saw with S_OK, and a guest-reported hard error
    therefore came from the guest side (or from a request the HV never saw)."""
    n = g.get("g_ioerrs")
    if n is None or n == 0xFFFFFFFF:
        # Unwritten scratch reads back 0xFFFFFFFF: the running image predates
        # these fields. Distinct from a real 0, which vblk_init() publishes.
        return
    out.append("")
    if n == 0:
        out.append("  g_ioerrs=0: the HV never completed a request with S_IOERR.")
        out.append("  Any guest 'vtbd0: hard error' is then NOT the HV failing an")
        out.append("  I/O -- look guest-side, or for a request never kicked to us.")
        return
    out.append(f"  *** g_ioerrs={n}: the HV DID fail {n} request(s) with S_IOERR."
               "  Causes:")
    for key, why in (("g_ioerr_busy", "eMMC cross-core lock timeout (contention)"),
                     ("g_ioerr_emmc", "real emmc_bio read/write failure"),
                     ("g_ioerr_badpa", "data descriptor PA outside DRAM"),
                     ("g_ioerr_unaligned", "partial-sector stitch (unimplemented)"),
                     ("g_ioerr_capacity", "read past advertised capacity"),
                     ("g_ioerr_notready", "emmc_ready == 0")):
        c = g.get(key)
        if c:
            out.append(f"      {c:6d}  {key:<18s} {why}")
    for which, pfx in (("FIRST", "ioerr1"), ("LAST", "ioerrN")):
        lba, bytes_, tag = (g.get(pfx + "_lba"), g.get(pfx + "_bytes"),
                            g.get(pfx + "_tag"))
        rc = g.get(pfx + "_rc")
        if lba is None:
            continue
        if rc is not None and rc >= 0x80000000:
            rc -= 0x100000000                       # stored as a raw u32
        rcs = VBK_IOERR_RC.get(rc, f"emmc_bio rc={rc}")
        out.append(f"    {which}: LBA {lba} ({lba:#x}), rc={rc} -> {rcs}")
        out.append(f"      served {bytes_} bytes before failing "
                   f"(guest saw used-ring len={bytes_ + 1})")
        if tag is not None:
            out.append(f"      head={tag >> 16}, "
                       f"{'READ' if tag & 2 else 'WRITE'}, on "
                       f"{'CPU2 (async offload)' if tag & 1 else 'CPU0 (sync)'}")


def dump_gic(hv, out):
    out += section("GIC / interrupt virtualization")
    gh = rd(hv, GICH)
    if gh:
        out += ["  " + s for s in armdec.decode_gich_hcr(gh[0])]
    lrs = rd(hv, GICH + 0x100, 4)
    if lrs:
        for i, v in enumerate(lrs):
            out.append(f"  GICH_LR{i}: {armdec.decode_gich_lr(v)}")
    elrsr = rd(hv, GICH + 0x30)
    if elrsr:
        out.append(f"  GICH_ELRSR0 = 0x{elrsr[0]:08x} (set bit = LR free)")
    out.append("")
    ctlr = rd(hv, GICD)
    if ctlr:
        out.append(f"  GICD_CTLR = 0x{ctlr[0]:08x}")
    banked_seen = False
    for intid, what, banked in WATCH_INTIDS:
        w = 4 * (intid // 32)
        bit = intid % 32
        en = rd(hv, GICD + 0x100 + w)
        pend = rd(hv, GICD + 0x200 + w)
        act = rd(hv, GICD + 0x300 + w)
        cfg = rd(hv, GICD + 0xC00 + 4 * (intid // 16))
        if not (en and pend and act and cfg):
            out.append(f"  INTID {intid:<4d} READ FAILED")
            continue
        f = (cfg[0] >> (2 * (intid % 16))) & 3
        mark = "  [BANKED - CPU1's view, NOT the guest's]" if banked else ""
        banked_seen = banked_seen or banked
        out.append(f"  INTID {intid:<4d} en={(en[0] >> bit) & 1} "
                   f"pend={(pend[0] >> bit) & 1} act={(act[0] >> bit) & 1} "
                   f"cfg={'EDGE' if f & 2 else 'LEVEL'}   {what}{mark}")
    if banked_seen:
        out.append("")
        out.append("  Lines marked BANKED are SGI/PPI state, which the GIC banks")
        out.append("  per CPU interface. These reads are serviced by the CPU1")
        out.append("  debug core, so they describe CPU1 -- do NOT read them as")
        out.append("  the guest core's timer configuration. To learn the guest's")
        out.append("  PPI state you need a read executed ON the guest's core.")


def dump_stg2(hv, out):
    out += section("stage-2 / A1 isolation (STG2 @0x50000c00)")
    w = rd(hv, STG2_BC, 20)
    if not w:
        out.append("  READ FAILED")
        return
    if w[0] != 0x53544732:
        out.append(f"  magic = 0x{w[0]:08x} -- not 'STG2', window never written")
        return
    out += ["  " + s for s in armdec.decode_vtcr(w[1])]
    out.append("")
    out.append(f"  VTTBR = 0x{w[2]:08x}   table_base = 0x{w[4]:08x}   "
               f"ndesc = {w[5]}")
    out += ["  " + s for s in armdec.decode_hcr(w[3])]
    out.append("")
    out.append(f"  self-check          = {w[6]} (1 = our own DRAM entry verified)")
    if w[14] == 0x49534F4C:
        out.append(f"  ISOL hv-image PAR.F = {w[15]} (want 1 = guest CANNOT reach)")
        out.append(f"  ISOL hv-scratch     = {w[16]} (want 1)")
        out.append(f"  ISOL control PAR.F  = {w[17]} (want 0 = ordinary DRAM ok)")
        out.append(f"  ISOL hv-fb          = {w[10]} (HV_HDMI builds only)")
        out.append(f"  ISOL overall pass   = {w[18]}")


def dump_exc(hv, out):
    out += section("last EL2 fault (EXC1 @0x50000400)")
    w = rd(hv, EXC_BC, 20)
    if not w:
        out.append("  READ FAILED")
        return
    if w[0] != 0x45584331:
        out.append(f"  magic = 0x{w[0]:08x} -- not 'EXC1'.")
        out.append("  This build does not write EXC breadcrumbs (they were")
        out.append("  added later). Use the flightrec ring and each device's")
        out.append("  own breadcrumbs instead; do NOT read the all-ones as data.")
        return
    esr = w[3]
    elr = (w[5] << 32) | w[4]
    far = (w[7] << 32) | w[6]
    ipa = (w[12] << 32) | w[11]
    out.append(f"  count={w[1]} kind={w[2]}")
    out += ["  " + s for s in armdec.decode_esr(esr)]
    out.append(f"  ELR = 0x{elr:016x}")
    out.append(f"  FAR = 0x{far:016x}   {armdec.classify_guest_va(far)}")
    out.append(f"  IPA = 0x{ipa:016x}   (from HPFAR -- the trustworthy one)")
    for lo, hi, what in armdec.STAGE2_L2_HOLES:
        if lo <= ipa < hi:
            out.append(f"  -> IPA is inside: {what}")
    out.append(f"  HV-window violations (hvviol_count) = {w[19]}")


def dump_fltr(hv, out, tail=40):
    out += section("flight recorder (FLTR @0x50012000)")
    h = rd(hv, FLTR, 8)
    if not h or h[0] != FLTR_MAGIC:
        out.append(f"  magic = 0x{h[0]:08x} -- ring not present" if h
                   else "  READ FAILED")
        return
    total, head, cap, stride = h[1], h[2], h[3], h[4]
    out.append(f"  total={total} head={head} capacity={cap} stride={stride}")
    slots = None
    for _ in range(15):
        slots = hv.read_words_stable(FLTR + 32, cap * stride, tries=4)
        if slots:
            break
    if not slots:
        out.append("  ring body READ FAILED")
        return

    # Per-kind histogram AND, crucially, how long ago each kind last appeared:
    # "TIMER stopped 697 events ago and never came back" is what cracked the
    # 2026-07-29 freeze, and no single snapshot of counters can show it.
    hist, last_seen = {}, {}
    for j in range(cap):
        i = (head - 1 - j) % cap
        k = slots[i * stride]
        hist[k] = hist.get(k, 0) + 1
        if k not in last_seen:
            last_seen[k] = j
    out.append("  kind        count   last seen")
    for k in sorted(hist):
        ago = last_seen[k]
        flag = ""
        if k == 6 and ago > 50:
            flag = "  <-- TIMER events STOPPED. Suspect a permanently masked"
        out.append(f"    {FLTR_KINDS.get(k, k):<10s} {hist[k]:>6d}   "
                   f"{ago:>5d} events ago{flag}")
        if flag:
            out.append("        CNTV (EL2 set IMASK, vGIC never delivered the")
            out.append("        replacement) -- see memory")
            out.append("        vtimer-masked-vgic-off-deadlock.")

    out.append("")
    out.append(f"  --- last {tail} events (newest first), repeats collapsed ---")
    prev_key, run, lines = None, 0, []
    for j in range(tail):
        i = (head - 1 - j) % cap
        o = i * stride
        k = slots[o]
        a0 = (slots[o + 2] << 32) | slots[o + 1]
        a1 = (slots[o + 4] << 32) | slots[o + 3]
        key = (k, a0)
        txt = f"    -{j + 1:<3d} {FLTR_KINDS.get(k, k):<8s} a0={a0:#018x} a1={a1:#018x}"
        if k == 7:                       # SYNC: a0 = (EC<<32)|ESR, a1 = ELR
            # EC alone is nearly useless in a timeline (every guest fault is
            # 0x24); DFSC + WnR is what distinguishes "read InterruptStatus"
            # from "write InterruptACK" at a glance.
            d = armdec.decode_esr(a0 & 0xFFFFFFFF)
            keep = [ln.strip() for ln in d
                    if ln.strip().startswith(("EC ", "DFSC", "WnR", "SAS", "SRT"))]
            txt += "\n         " + "; ".join(keep)
        if k == 6:                       # TIMER: a0 = CNTV_CTL
            txt += f"\n         {armdec.decode_cntv_ctl(a0)[0]}"
        if key == prev_key:
            run += 1
        else:
            if run > 1:
                lines.append(f"         ... x{run} identical")
            lines.append(txt)
            run = 1
            prev_key = key
    if run > 1:
        lines.append(f"         ... x{run} identical")
    out += lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gap", type=float, default=2.5,
                    help="seconds between the two liveness samples")
    ap.add_argument("--tail", type=int, default=40,
                    help="flightrec events to print")
    args = ap.parse_args()

    hv = hvdbg.HV()
    out = [f"channel RTT {hv.rtt():.3f}s"]
    check_build(hv, out)
    vbk_a, vbk_b = check_liveness(hv, out, args.gap)
    dump_vbk(out, vbk_a, vbk_b)
    dump_gic(hv, out)
    dump_stg2(hv, out)
    dump_exc(hv, out)
    dump_fltr(hv, out, args.tail)
    print("\n".join(out))


if __name__ == "__main__":
    main()
