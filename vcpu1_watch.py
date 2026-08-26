#!/usr/bin/env python3
"""vcpu1_watch.py -- one-shot instrument for the CPU1-as-second-vCPU bring-up.

WHY THIS EXISTS, and why it samples what it samples. The 2026-08-25 attempts at
arming dbg_vcpu1 both ended with the guest wedged just after FreeBSD prints
"Release APs...done.", and both were mis-diagnosed from a SNAPSHOT taken after
the fact (first as a GICD_SGIR masking bug in vgicd.c, then as a CPU0<->CPU1
cache-coherency failure). A snapshot cannot distinguish "this counter is zero
because the code never ran" from "this counter is zero because nothing needed
masking" -- and that exact ambiguity is what cost two cycles. So this samples
CONTINUOUSLY, prints only on change, and puts all four relevant windows on one
timeline:

  VGIC lane 0/1 (0x50001c00 / +0x80)  the per-core vGIC lanes added 2026-08-26.
      Lane 1 is THE thing being proven: nr_lr/active/hcr non-zero means CPU1
      really brought up its own virtual CPU interface, and inject_ok climbing
      means physical IRQs are actually reaching CPU1's guest -- the precise
      thing whose absence wedged the guest before (GICH is per-PE-banked, so a
      core that never ran its own vgic_init() has GICH_HCR.En==0, vgic_active()
      false, and gic_timer_irq() silently drops every INTID it takes).
  VCPU1_BC (0x5009e100)   state machine: 1=parked, 2=CPU_ON accepted,
      3=entering EL1, plus the refused/why counters -- so "the guest never
      asked for cpu@1" is distinguishable from "we refused it" from "it
      entered and died", which look identical from the console.
  VGICD_BC (0x50094000)   the GICD trap counters, including last_off/last_val/
      last_cpu. last_cpu==1 is independent proof CPU1's guest reached its own
      intr_pic_init_secondary(); last_off at the moment the console freezes is
      the "what was the guest's final GIC transaction" datum that was missing
      both previous times.
  console bytes           the guest's own liveness. A frozen count with all of
      the above still advancing means EL2 is fine and the GUEST is starved --
      the distinction that exonerated both earlier hypotheses.

Read-only: every access is an hvdbg read over EMAC, nothing is written to the
board. Safe to run against a healthy board at any time.
"""
import sys
import time

sys.path.insert(0, "/opt/bzdos/microkernel")
import hvdbg  # noqa: E402

VGIC_BC = 0x50001C00
VGIC_STRIDE = 0x80
VCPU1_BC = 0x5009E100
VGICD_BC = 0x50094000

# vgic.c's vg_bc() index map, only the slots that answer a question here.
VGIC_FIELDS = [(1, "vtr"), (2, "nr_lr"), (3, "hcr"), (4, "inj"), (5, "ok"),
               (6, "drop"), (12, "active"), (8, "cntv_inj"), (13, "vmcr")]
# vcpu1.h's breadcrumb map.
VCPU1_FIELDS = [(0, "magic"), (1, "state"), (2, "req"), (6, "refused"),
                (7, "why"), (8, "vtcr"), (11, "hcr"), (12, "sctlr")]
# vgicd.c's breadcrumb map.
VGICD_FIELDS = [(1, "total"), (2, "rd"), (3, "wr"), (4, "itgt_mask"),
                (5, "sgir_mask"), (6, "last_off"), (7, "last_val"),
                (8, "isv0"), (9, "last_cpu")]


def fmt(words, fields):
    return " ".join("%s=%d" % (n, words[i]) if n in ("nr_lr", "active", "state",
                                                     "req", "refused", "why",
                                                     "inj", "ok", "drop",
                                                     "cntv_inj", "total", "rd",
                                                     "wr", "itgt_mask",
                                                     "sgir_mask", "isv0",
                                                     "last_cpu")
                    else "%s=0x%08x" % (n, words[i])
                    for i, n in fields)


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 180
    hv = hvdbg.HV()
    t0 = time.time()
    last = {}
    while time.time() - t0 < secs:
        t = time.time() - t0
        try:
            rows = {
                "vgic0": fmt(hv.read_words(VGIC_BC, 20), VGIC_FIELDS),
                "vgic1": fmt(hv.read_words(VGIC_BC + VGIC_STRIDE, 20),
                             VGIC_FIELDS),
                "vcpu1": fmt(hv.read_words(VCPU1_BC, 16), VCPU1_FIELDS),
                "vgicd": fmt(hv.read_words(VGICD_BC, 10), VGICD_FIELDS),
            }
            try:
                # vconsole() returns the self-describing ring HEADER as a dict
                # (hvdbg.py); total_bytes is the guest's cumulative console
                # output, which is the liveness signal wanted here -- not the
                # ring contents, which would be a far heavier read per sample.
                vc = hv.vconsole()
                rows["console"] = "bytes=%d faults=%d" % (
                    vc.get("total_bytes", -1), vc.get("fault_count", -1))
            except Exception as e:                       # noqa: BLE001
                rows["console"] = "ERR %s" % e
        except Exception as e:                           # noqa: BLE001
            print("[%7.1fs] read error: %s" % (t, e), flush=True)
            time.sleep(1.0)
            continue
        for k, v in rows.items():
            if last.get(k) != v:
                print("[%7.1fs] %-8s %s" % (t, k, v), flush=True)
                last[k] = v
        time.sleep(0.7)


if __name__ == "__main__":
    main()
