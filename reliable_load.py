#!/usr/bin/env python3
"""reliable_load.py — DETERMINISTIC, self-healing HV loader.

Replaces the "usually a fresh chimpd catches it" dance with a bounded,
verifiable state machine. Root cause of the old flakiness: /dev/ttyCHIMP is
AMBIGUOUS — the udev rule maps BOTH the U-Boot download gadget (1f3a:efe8) AND
the running HV's own CDC-ACM console (1d6b:0010) to it. chimpd would open
ttyCHIMP expecting U-Boot but sometimes talk to the guest console gadget of a
still-running HV -> "=> not caught". This loader keys on VID:PID, guarantees a
FRESH U-Boot before loading, loads, then VERIFIES (EMAC alive + optional VBK
breadcrumb), and retries the whole cycle on any failure.

State machine per cycle:
  1. ensure_uboot(): if the HV is alive (EMAC responds) OR its console gadget
     (1d6b:0010) is present -> reboot_clean() over EMAC (clean USB disconnect,
     no -71). Then wait until the U-Boot gadget (1f3a:efe8) is present & stable.
  2. serial_load(): chimpd's proven catch-> TFTP dtb+kernel -> loady+bootelf.
  3. verify(): EMAC must respond within T; if expect_vbk, the VBK1 breadcrumb
     (0x50005000) must be present (proves the virtio-blk build actually ran).
  On any failure -> reboot_clean and retry, up to max_cycles.

Usage: python3 reliable_load.py [--expect-vbk] [--cycles N]
Exit 0 on verified load, 1 on exhaustion.
"""
import os, sys, time, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chimpd as C
from hvdbg import HV

UBOOT_VID, UBOOT_PID = "1f3a", "efe8"     # U-Boot download gadget
HVCON_VID, HVCON_PID = "1d6b", "0010"     # HV's bzdOS USB Console
USB_NODE = "/sys/bus/usb/devices/1-4"     # board's OTG port on this host


def _rd(path):
    try:
        return open(path).read().strip()
    except Exception:
        return None


def usb_vidpid():
    return _rd(f"{USB_NODE}/idVendor"), _rd(f"{USB_NODE}/idProduct")


def hv_alive(timeout=4.0):
    """True iff the EMAC debug channel ANSWERS AT ALL (HV/CPU1 dbgmon is live).
    NOTE: check that a read returns data — NOT that the vconsole magic is set.
    A guest in a fault-storm has a valid dbgmon (reads work, fault_count ticks)
    but vconsole magic can read 0; requiring magic wrongly reports 'dead' and
    skips the reset the board actually needs."""
    try:
        hv = HV()
        t0 = time.time()
        while time.time() - t0 < timeout:
            w = hv.read_words(0x50000f00, 4)   # any 4 words from the HV's DRAM
            if w and len(w) >= 4:
                return True
            time.sleep(0.3)
    except Exception:
        pass
    return False


def reboot_clean_via_emac():
    """BUILD-INDEPENDENT clean reset over EMAC. Does NOT rely on reboot_clean's
    address (that shifts per build; a stale addr no-ops). Instead:
      1. drop the MUSB D+/D- pull-up (clean USB disconnect, no host -71),
      2. set wdt_debug_hold=1 across the ~0x4201e6a0 range so the CPU1 debug
         core STOPS petting the HW WDOG (else it re-kicks and the reset never
         fires — see smp-debug-core-working),
      3. arm the A64 WDOG for a whole-system reset.
    All fixed MMIO / a small BSS range -> works regardless of which build ran."""
    try:
        hv = HV()
        MUSB = 0x01c19000
        def r1(a):
            for _ in range(15):
                w = hv.read_words(a, 1)
                if w:
                    return w[0]
                time.sleep(0.1)
            return None
        iscr = r1(MUSB + 0x400)
        if iscr is not None:
            hv.write_word(MUSB + 0x400, (iscr & ~0x70) & ~(1 << 16))  # clear pull-up
        poww = r1(MUSB + 0x40)
        if poww is not None:
            hv.write_word(MUSB + 0x40, poww & ~0x40)                  # clear SOFTCONN
        # wdt_debug_hold's address SHIFTS per build (BSS moves ~0x2000 between
        # builds). Resolve it from the elf we load (after a successful load the
        # running build IS this elf), and also sweep a wide window to cover a
        # stale/mismatched build. nm the elf; center a ±0x80 tight range on the
        # real symbol, plus a coarse sweep 0x4201d000..0x42030000 (covers every
        # observed location: 0x4201e6a0 old, 0x420206a0 virtio-blk) so the reset
        # fires regardless of which build is actually resident.
        import subprocess
        hold = None
        try:
            out = subprocess.check_output(
                ["aarch64-linux-gnu-nm", C.HYP_ELF],
                stderr=subprocess.DEVNULL).decode()
            for line in out.splitlines():
                p = line.split()
                if len(p) == 3 and p[2] == "wdt_debug_hold":
                    hold = int(p[0], 16); break
        except Exception:
            pass
        if hold is not None:
            for a in range(hold - 0x80, hold + 0x84, 4):
                hv.write_word(a, 1)
        for a in range(0x4201d000, 0x42030000, 0x40):   # coarse safety sweep
            hv.write_word(a, 1)
        time.sleep(0.4)
        for pa, val in [(0x01c20cb4, 1), (0x01c20cb8, 0x21), (0x01c20cb0, 0x14af)]:
            hv.write_word(pa, val)                                    # arm WDOG
        return True
    except Exception as e:
        C.slog(f"  [reliable] reset err: {e}")
        return False


def wait_uboot_gadget(timeout=90, stable=2.0):
    """Wait until 1f3a:efe8 is present on the port AND stays present `stable`s
    (so we don't open it mid-enumeration)."""
    t0 = time.time()
    since = None
    while time.time() - t0 < timeout:
        v, p = usb_vidpid()
        if v == UBOOT_VID and p == UBOOT_PID:
            if since is None:
                since = time.time()
            elif time.time() - since >= stable:
                return True
        else:
            since = None
        time.sleep(0.3)
    return False


def ensure_uboot(reset_settle=3.0):
    """Guarantee the board is at a FRESH U-Boot download gadget."""
    v, p = usb_vidpid()
    hv = hv_alive()
    if hv or (v == HVCON_VID and p == HVCON_PID) or not (v == UBOOT_VID and p == UBOOT_PID):
        C.slog(f"  [reliable] state vid:pid={v}:{p} hv_alive={hv} -> reboot_clean")
        if hv:
            reboot_clean_via_emac()
        # wait for the HV/old gadget to vanish, then the U-Boot gadget to appear
        time.sleep(reset_settle)
    else:
        C.slog(f"  [reliable] already at U-Boot gadget {v}:{p}")
    if not wait_uboot_gadget():
        C.slog("  [reliable] ⛔ U-Boot gadget (1f3a:efe8) never appeared")
        return False
    C.slog("  [reliable] ✅ fresh U-Boot gadget present & stable")
    return True


def verify(expect_vbk):
    """EMAC must answer; if expect_vbk, VBK1 breadcrumb must be laid."""
    if not hv_alive(timeout=20):
        C.slog("  [reliable] ⛔ verify: EMAC did not come up")
        return False
    if expect_vbk:
        try:
            w = HV().read_words(0x50020000, 1)
            magic = w[0] if w else 0
            # "VBK1" little-endian = 0x56424b31
            if magic != 0x56424b31:
                C.slog(f"  [reliable] ⛔ verify: VBK breadcrumb=0x{magic:08x} (want 0x56424b31)")
                return False
            C.slog("  [reliable] ✅ VBK1 breadcrumb present (virtio-blk build ran)")
        except Exception as e:
            C.slog(f"  [reliable] verify vbk err: {e}")
            return False
    C.slog("  [reliable] ✅ verified: EMAC alive")
    return True


def reliable_load(expect_vbk=False, max_cycles=5):
    for cyc in range(1, max_cycles + 1):
        C.slog(f"━━━ reliable cycle #{cyc}/{max_cycles} ━━━")
        if not ensure_uboot():
            continue
        sess = C.Session(cyc)
        try:
            ok = C.serial_load(sess)
        except Exception as e:
            C.slog(f"  [reliable] serial_load raised: {e}")
            ok = False
        finally:
            sess.close()
        if not ok:
            C.slog("  [reliable] serial_load failed — retrying cycle")
            continue
        if verify(expect_vbk):
            C.slog(f"🎉 [reliable] LOADED & VERIFIED on cycle #{cyc}")
            return True
        C.slog("  [reliable] load unverified — retrying cycle")
    C.slog(f"⛔ [reliable] exhausted {max_cycles} cycles")
    return False


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect-vbk", action="store_true",
                    help="require the VBK1 breadcrumb (virtio-blk build)")
    ap.add_argument("--cycles", type=int, default=5)
    a = ap.parse_args()
    sys.exit(0 if reliable_load(a.expect_vbk, a.cycles) else 1)
