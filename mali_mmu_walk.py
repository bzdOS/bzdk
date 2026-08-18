#!/usr/bin/env python3
"""Walk the Mali-400 MMU page tables from EL2, over the debug channel.

WHY THIS EXISTS

A "mmu page fault at 0x286000 ... on gpmmu" line says where the GPU faulted but
not why, and the two candidate causes need opposite fixes:

  - the page was never mapped          -> a driver mapping bug
  - the page IS mapped but unseen      -> page-table coherency / stale MMU TLB

Answering that normally means instrumenting the lima kmod, and this host has no
drm-kmod source tree to rebuild it with (it lives on a dev VM that is off limits
for this project). But the Mali MMU is ordinary MMIO and its page tables are
ordinary guest DRAM, and EL2 can read both without the guest's cooperation. So
the hypervisor answers the question instead of the driver.

Mali-400 MMU is a two-level 32-bit walk, same shape as ARMv7 short descriptors:

    PD index = VA >> 22                  (1024 entries, base = DTE_ADDR)
    PT index = (VA >> 12) & 0x3FF        (1024 entries, base = PDE & ~0xFFF)
    PTE      = PA | flags(low 12 bits)

CAVEAT, stated because it decides whether the output means anything: the tables
only exist while a lima VM is loaded. Read while a job is actually in flight --
run the GL test in a loop on the guest first. With no VM loaded, DTE_ADDR points
at freed memory and every number below is stale, not current.
"""
import argparse
import sys

import hvdbg

GPU_BASE = 0x01C40000
IP = {"gpmmu": 0x3000, "ppmmu0": 0x4000, "ppmmu1": 0x5000}
MMU_DTE_ADDR = 0x0000
MMU_STATUS = 0x0004
MMU_PAGE_FAULT_ADDR = 0x000C

# LIMA_MMU_STATUS bits worth naming (lima_regs.h).
STATUS_BITS = (
    (0, "PAGING_ENABLED"),
    (1, "PAGE_FAULT_ACTIVE"),
    (2, "STALL_ACTIVE"),
    (3, "IDLE"),
    (4, "REPLAY_BUFFER_EMPTY"),
    (5, "PAGE_FAULT_IS_WRITE"),
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="gpmmu", choices=sorted(IP))
    ap.add_argument("--va", type=lambda x: int(x, 0), default=None,
                    help="GPU virtual address to resolve; default is whatever "
                         "the MMU's own PAGE_FAULT_ADDR reports")
    a = ap.parse_args()

    hv = hvdbg.HV()
    base = GPU_BASE + IP[a.ip]

    def rd(off):
        w = hv.read_words(base + off, 1)
        return w[0] if w else None

    dte = rd(MMU_DTE_ADDR)
    status = rd(MMU_STATUS)
    fault = rd(MMU_PAGE_FAULT_ADDR)

    if dte is None or status is None:
        print(f"{a.ip}: no reply reading MMIO at {base:#x}. Either the debug "
              f"channel is down or the GPU is unclocked -- a dead GPU reads as "
              f"nothing here, which is NOT the same as 'tables are empty'.")
        return 1

    print(f"{a.ip} @ {base:#010x}")
    print(f"  DTE_ADDR        = {dte:#010x}   (page directory PA)")
    print(f"  STATUS          = {status:#010x}   "
          + " ".join(n for b, n in STATUS_BITS if status & (1 << b)))
    print(f"  PAGE_FAULT_ADDR = {fault:#010x}")

    if dte == 0 or dte == 0xFFFFFFFF:
        print("  -> no page directory loaded: no lima VM is active right now. "
              "Start the GL test in a loop and read again; nothing below would "
              "describe the faulting job.")
        return 0

    va = a.va if a.va is not None else fault
    pde_i = (va >> 22) & 0x3FF
    pte_i = (va >> 12) & 0x3FF
    print(f"\nresolving VA {va:#010x}   PD[{pde_i}] -> PT[{pte_i}]")

    pd_base = dte & ~0xFFF
    w = hv.read_words(pd_base + pde_i * 4, 1)
    if not w:
        print("  PDE unreadable")
        return 1
    pde = w[0]
    print(f"  PDE = {pde:#010x}")
    if pde == 0:
        print("  -> PDE is 0: no page table for this 4 MiB region at all. The "
              "address was NEVER MAPPED -- this is a driver mapping bug, not a "
              "coherency problem.")
        return 0
    if pde == 0xFFFFFFFF:
        print("  -> PDE reads all-ones, which is what unwritten memory looks "
              "like here. Treat as 'not a real entry', not as a value.")
        return 0

    pt_base = pde & ~0xFFF
    w = hv.read_words(pt_base + pte_i * 4, 1)
    if not w:
        print("  PTE unreadable")
        return 1
    pte = w[0]
    print(f"  PTE = {pte:#010x}  (PT base {pt_base:#010x})")
    if pte == 0:
        print("  -> PTE is 0: the page table exists but this page is UNMAPPED. "
              "Driver mapping bug.")
    elif pte == 0xFFFFFFFF:
        print("  -> all-ones: unwritten, not a value.")
    else:
        print(f"  -> MAPPED to PA {pte & ~0xFFF:#010x} (flags {pte & 0xFFF:#03x}). "
              "The entry is present in DRAM, so a fault on it means the GPU is "
              "not seeing this table: page-table coherency, or a stale MMU TLB "
              "that was never invalidated after the mapping.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
