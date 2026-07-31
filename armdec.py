#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""armdec.py -- decoders for the AArch64 / GICv2 register values that show up
in this tree's breadcrumbs and dbgmon dumps.

WHY THIS EXISTS: hand-decoding these live is where real diagnostic errors come
from, not from reading the wrong address. Confirmed 2026-07-29: a guest fault
at FAR=0xffff00004032b064 was read as a DMAP address (FreeBSD 12 arm64 layout,
DMAP at 0xffff000000000000) and "therefore" PA 0x4032b064, when this guest is
FreeBSD 13+ where 0xffff0000... is KVA and DMAP lives at 0xffffa000...  That
one slip produced a whole false theory about a hole in the stage-2 tables. The
give-away was sitting in the same register dump the whole time (x11/x19/x21/x25
all 0xffffa000...), but nothing was printing "these are DMAP, so KERNBASE is
not".

So: decode once, here, with the reasoning attached -- and make the decoder say
what the value IMPLIES about this hypervisor, not just which bits are set
(e.g. "DFSC=6, translation fault level 2" is the raw fact; "so the IPA is
inside one of the four 2 MiB entries stage2.c leaves INVALID" is the useful
part).

Pure functions on ints, no board access -- safe to unit-test and to call from
hvsh requests.
"""

# ---------------------------------------------------------------------------
# FreeBSD arm64 address-space layout
# ---------------------------------------------------------------------------
# FreeBSD 13 moved the direct map. Getting this backwards silently turns a KVA
# into a bogus PA (see module docstring), so both layouts are spelled out and
# classify_guest_va() reports which one a value is consistent with rather than
# assuming.
FBSD_KVA_BASE = 0xFFFF000000000000     # VM_MIN_KERNEL_ADDRESS (13+)
FBSD_DMAP_BASE_13 = 0xFFFFA00000000000  # DMAP_MIN_ADDRESS (13+)
FBSD_DMAP_BASE_12 = 0xFFFF000000000000  # DMAP_MIN_ADDRESS (12.x) -- collides
                                        # with 13+'s KVA base, hence the trap


def classify_guest_va(va):
    """Say what a FreeBSD arm64 kernel virtual address IS, honestly.

    Returns a short string. Deliberately does NOT return a PA: for a KVA the
    physical address is whatever the guest's own stage-1 tables say, and the
    only trustworthy source for the faulting PA/IPA is HPFAR_EL2 (or, for
    virtio, vblk's own last_fault_ipa breadcrumb) -- never arithmetic on FAR.
    """
    if va >= FBSD_DMAP_BASE_13:
        off = va - FBSD_DMAP_BASE_13
        return f"DMAP (FreeBSD 13+), PA = 0x{off:x}"
    if va >= FBSD_KVA_BASE:
        off = va - FBSD_KVA_BASE
        return (f"KVA +0x{off:x} on FreeBSD 13+ (PA unknown -- needs HPFAR); "
                f"would be DMAP PA 0x{off:x} ONLY on FreeBSD 12.x")
    return "not a kernel address"


# ---------------------------------------------------------------------------
# ESR_EL2
# ---------------------------------------------------------------------------
_EC = {
    0x00: "unknown reason",
    0x01: "WFI/WFE trapped",
    0x03: "MCR/MRC cp15",
    0x07: "SVE/SIMD/FP access trapped",
    0x0E: "illegal execution state",
    0x11: "SVC (AArch32)",
    0x15: "SVC (AArch64)",
    0x16: "HVC (AArch64)",
    0x17: "SMC (AArch64)",
    0x18: "MSR/MRS/system-insn trapped (TVM/TGE/etc)",
    0x20: "instruction abort from lower EL",
    0x21: "instruction abort from same EL",
    0x22: "PC alignment fault",
    0x24: "data abort from lower EL (guest)",
    0x25: "data abort from same EL (EL2 itself!)",
    0x26: "SP alignment fault",
    0x2C: "trapped FP exception",
    0x2F: "SError",
    0x30: "breakpoint from lower EL",
    0x32: "software step from lower EL",
    0x34: "watchpoint from lower EL",
    0x3C: "BRK instruction (AArch64)",
}

_DFSC = {
    0x00: "address size fault, level 0",
    0x01: "address size fault, level 1",
    0x02: "address size fault, level 2",
    0x03: "address size fault, level 3",
    0x04: "translation fault, level 0",
    0x05: "translation fault, level 1",
    0x06: "translation fault, level 2",
    0x07: "translation fault, level 3",
    0x09: "access flag fault, level 1",
    0x0A: "access flag fault, level 2",
    0x0B: "access flag fault, level 3",
    0x0D: "permission fault, level 1",
    0x0E: "permission fault, level 2",
    0x0F: "permission fault, level 3",
    0x10: "synchronous external abort",
    0x21: "alignment fault",
    0x30: "TLB conflict abort",
}

# stage2.c leaves exactly these entries INVALID inside the ONE 1 GiB DRAM block
# it splits (0x40000000-0x7FFFFFFF), plus the low MMIO L2 table. A level-2
# stage-2 translation fault can therefore only come from one of these -- which
# is what turns "DFSC=6" into an actual list of suspects.
STAGE2_L2_HOLES = [
    (0x0A000000, 0x0A200000, "virtio-mmio trap window (BY DESIGN -> vblk_mmio_fault)"),
    (0x42000000, 0x42200000, "hv-image carve-out (A1 isolation violation)"),
    (0x4D000000, 0x4D800000, "hv-fb carve-out, HV_HDMI only (A1 isolation violation)"),
    (0x50000000, 0x50200000, "hv-scratch carve-out (A1 isolation violation)"),
]


def decode_esr(esr):
    """Decode ESR_EL2 into human-readable lines."""
    ec = (esr >> 26) & 0x3F
    il = (esr >> 25) & 1
    iss = esr & 0x1FFFFFF
    out = [f"ESR = 0x{esr:08x}",
           f"  EC   = 0x{ec:02x}  {_EC.get(ec, 'reserved/unknown')}",
           f"  IL   = {il} ({'32' if il else '16'}-bit instruction)"]
    if ec in (0x24, 0x25, 0x20, 0x21):
        dfsc = iss & 0x3F
        out.append(f"  DFSC = 0x{dfsc:02x}  {_DFSC.get(dfsc, 'reserved')}")
        if ec in (0x24, 0x25):
            wnr = (iss >> 6) & 1
            out.append(f"  WnR  = {wnr} ({'WRITE' if wnr else 'READ'})")
            s1ptw = (iss >> 7) & 1
            out.append(f"  S1PTW= {s1ptw} "
                       f"({'fault was ON a stage-1 page-table walk'
                          if s1ptw else 'ordinary access, not a PTW'})")
            fnv = (iss >> 10) & 1
            out.append(f"  FnV  = {fnv} "
                       f"({'FAR is NOT valid -- ignore it'
                          if fnv else 'FAR is valid'})")
            isv = (iss >> 24) & 1
            out.append(f"  ISV  = {isv} (syndrome below valid)")
            if isv:
                sas = (iss >> 22) & 3
                out.append(f"  SAS  = {sas} ({1 << sas}-byte access)")
                out.append(f"  SRT  = {(iss >> 16) & 0x1F} (transfer register Xt)")
                out.append(f"  SF   = {(iss >> 15) & 1} (1 = 64-bit register)")
        if (iss & 0x3F) == 0x06:
            out.append("  -> a LEVEL-2 stage-2 translation fault can only be an")
            out.append("     INVALID 2 MiB entry; in this tree that is one of:")
            for lo, hi, what in STAGE2_L2_HOLES:
                out.append(f"       0x{lo:08x}-0x{hi:08x}  {what}")
            out.append("     Get the real IPA from HPFAR_EL2, never from FAR.")
    return out


# ---------------------------------------------------------------------------
# PAR_EL1
# ---------------------------------------------------------------------------
def decode_par(par):
    """Decode PAR_EL1 after an AT instruction."""
    f = par & 1
    if not f:
        return [f"PAR = 0x{par:016x}",
                "  F = 0 (translation SUCCEEDED)",
                f"  PA = 0x{par & 0x0000FFFFFFFFF000:x}"]
    return [f"PAR = 0x{par:016x}",
            "  F = 1 (translation FAULTED)",
            f"  FST = 0x{(par >> 1) & 0x3F:02x}"
            f"  {_DFSC.get((par >> 1) & 0x3F, 'reserved -- see caveat')}",
            f"  PTW = {(par >> 8) & 1}   S = {(par >> 9) & 1} "
            f"(1 = fault was at stage 2)",
            "  CAVEAT: an AT S1E1x issued from EL2 on a core that is NOT the",
            "  one running the guest resolves against THAT core's TTBR_EL1,",
            "  which for this tree's CPU1 debug core is unset -- the result is",
            "  meaningless (typically F=1 with FST=0). Only trust AT S1E1x run",
            "  on the guest's own core.",
            ]


# ---------------------------------------------------------------------------
# HCR_EL2 / VTCR_EL2
# ---------------------------------------------------------------------------
def decode_hcr(hcr):
    bits = [(0, "VM (stage-2 enabled)"), (1, "SWIO"), (2, "PTW"),
            (3, "FMO (phys FIQ -> EL2)"), (4, "IMO (phys IRQ -> EL2)"),
            (5, "AMO (SError -> EL2)"), (13, "TWI (trap WFI)"),
            (14, "TWE (trap WFE)"), (26, "TVM (trap VM regs)"),
            (27, "TGE"), (31, "RW (EL1 is AArch64)")]
    out = [f"HCR_EL2 = 0x{hcr:016x}"]
    for b, name in bits:
        out.append(f"  {'x' if (hcr >> b) & 1 else '.'} bit{b:<2d} {name}")
    if not ((hcr >> 4) & 1):
        out.append("  -> IMO=0: physical IRQs go straight to EL1. Any code path")
        out.append("     that masks a source and re-injects through the vGIC is")
        out.append("     BROKEN in this configuration (the injection is a no-op)")
        out.append("     -- see memory vtimer-masked-vgic-off-deadlock.")
    return out


def decode_vtcr(vtcr):
    t0sz = vtcr & 0x3F
    sl0 = (vtcr >> 6) & 3
    tg0 = (vtcr >> 14) & 3
    ps = (vtcr >> 16) & 7
    psbits = {0: 32, 1: 36, 2: 40, 3: 42, 4: 44, 5: 48, 6: 52}
    gran = {0: "4 KiB", 1: "64 KiB", 2: "16 KiB"}
    out = [f"VTCR_EL2 = 0x{vtcr:08x}",
           f"  T0SZ = {t0sz} -> IPA size {64 - t0sz} bits",
           f"  SL0  = {sl0} -> initial lookup LEVEL "
           f"{ {0:2, 1:1, 2:0}.get(sl0, '?') }",
           f"  TG0  = {tg0} ({gran.get(tg0, '?')} granule)",
           f"  PS   = {ps} ({psbits.get(ps, '?')}-bit PA)"]
    if sl0 == 1:
        out.append("  -> level-1 entries are 1 GiB blocks, level-2 are 2 MiB,")
        out.append("     so a 'level 2' fault means an invalid 2 MiB entry.")
    return out


# ---------------------------------------------------------------------------
# GICv2 virtualization
# ---------------------------------------------------------------------------
def decode_gich_hcr(hcr):
    out = [f"GICH_HCR = 0x{hcr:08x}",
           f"  En   = {hcr & 1} (virtual CPU interface enabled)",
           f"  UIE  = {(hcr >> 1) & 1}   LRENPIE = {(hcr >> 2) & 1}   "
           f"NPIE = {(hcr >> 3) & 1}"]
    if not (hcr & 1):
        out.append("  -> En=0: EVERY GICH_LR write is inert. Nothing injected")
        out.append("     through the vGIC reaches the guest.")
    return out


def decode_gich_lr(lr):
    state = (lr >> 28) & 3
    return (f"LR=0x{lr:08x} vINTID={lr & 0x3FF} pINTID={(lr >> 10) & 0x3FF} "
            f"prio={(lr >> 23) & 0x1F} "
            f"state={state}({('invalid', 'pending', 'active', 'pend+act')[state]}) "
            f"HW={(lr >> 31) & 1 and (lr >> 30) & 1 or (lr >> 30) & 1} "
            f"Grp1={(lr >> 31) & 1}")


def decode_cntv_ctl(ctl):
    """CNTV_CTL_EL0 -- the register at the heart of the lost-tick bug."""
    en, imask, istatus = ctl & 1, (ctl >> 1) & 1, (ctl >> 2) & 1
    out = [f"CNTV_CTL = 0x{ctl:x}  ENABLE={en} IMASK={imask} ISTATUS={istatus}"]
    if en and imask:
        out.append("  -> ENABLED but MASKED: the timer is running and will never")
        out.append("     interrupt. If EL2 set IMASK to hand the tick to the")
        out.append("     guest via the vGIC, and the vGIC is off, the guest can")
        out.append("     never clear this -- permanent tick loss, guest sleeps")
        out.append("     in WFI forever. THE bug found 2026-07-29.")
    elif en and istatus and not imask:
        out.append("  -> firing right now (condition met, not masked)")
    elif not en:
        out.append("  -> timer disabled by the guest")
    return out


if __name__ == "__main__":
    import sys
    if len(sys.argv) >= 3:
        kind, val = sys.argv[1], int(sys.argv[2], 0)
        fn = {"esr": decode_esr, "par": decode_par, "hcr": decode_hcr,
              "vtcr": decode_vtcr, "gich_hcr": decode_gich_hcr,
              "cntv_ctl": decode_cntv_ctl}.get(kind)
        if fn:
            print("\n".join(fn(val)))
        elif kind == "va":
            print(classify_guest_va(val))
        elif kind == "lr":
            print(decode_gich_lr(val))
        else:
            print(f"unknown: {kind}")
    else:
        print(__doc__)
        print("usage: armdec.py {esr|par|hcr|vtcr|gich_hcr|cntv_ctl|lr|va} <value>")
