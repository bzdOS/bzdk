#!/usr/bin/env python3
"""
bmc_client.py — host-side CLI for the bzdOS "software BMC" management plane.

This is the clean, named front end to the BMC verbs implemented in bmc.c. It
rides the EXACT SAME EMAC raw-Ethernet console (ethertype 0x88B5, board MAC
02:bd:05:00:00:01) that hvdbg.py/dbgmon already use — it simply subclasses HV
so all the low-level framing, draining and the reboot_clean() reset path come
for free. Every subcommand sends a `bmc <verb> ...` line and renders the reply.

    ./bmc_client.py health                 # structured status record
    ./bmc_client.py health --raw           # parse the BMC1 breadcrumb directly
    ./bmc_client.py temp
    ./bmc_client.py flags                  # list debug flags
    ./bmc_client.py flag usbacm 0          # (auto-arms) set a debug flag
    ./bmc_client.py console                # dump the guest console capture ring
    ./bmc_client.py console --stream       # follow live guest output (TX tee)
    ./bmc_client.py inject "mount -a"      # type a line into the guest console
    ./bmc_client.py reset                  # (auto-arms) clean reboot to U-Boot
    ./bmc_client.py wdt hold               # (auto-arms) let the HW WDOG reset
    ./bmc_client.py dump 0x50006000 32     # raw breadcrumb read (via dbgmon)

Design note: destructive verbs (reset/flag/wdt hold|arm) require a two-step
ARM on the board (see bmc.c's safety gate). This client performs the arm
automatically right before the destructive send, so the human types one
command — the on-board window (~10 s, one-shot) still protects against a
stray/replayed frame firing a reset on its own.
"""
import sys, time, struct, argparse

# Reuse the whole EMAC transport + reset machinery from hvdbg.py.
from hvdbg import HV

BMC_HEALTH_BASE = 0x50006000
BMC_HEALTH_MAGIC = 0x424D4331  # "BMC1"

# struct bmc_health field order (must match bmc.h). Names only; each is one
# 32-bit word except the lo/hi pairs which we recombine below.
HEALTH_WORDS = [
    "magic", "version", "uptime_lo", "uptime_hi", "tick_lo", "tick_hi",
    "tick_delta", "exc_count", "last_exc_kind", "last_exc_esr",
    "guest_pc_lo", "guest_pc_hi", "online_map",
    "hb_cpu0", "hb_cpu1", "hb_cpu2", "hb_cpu3",
    "cons_bytes", "cons_faults", "temp_mc", "flags", "wdt_hold", "ffv_count",
]

FLAG_BITS = [
    ("usbacm", 0), ("core_enable", 1), ("cpu1_wdog", 2),
    ("isolate_noemac", 3), ("no_guest", 4), ("block_reset", 5),
]


class BMC(HV):
    """Software-BMC client: thin verb wrappers over the shared HV EMAC link."""

    # ── core verb helpers ───────────────────────────────────────────────
    def bmc(self, verb, wait=3):
        """Send one `bmc <verb>` line, return the text reply."""
        return self.cmd("bmc " + verb, wait)

    def arm(self, nonce=None):
        """Arm the on-board destructive-verb gate. Uses a time-based nonce so
        each arm is distinct. Returns True if the board acknowledged."""
        if nonce is None:
            nonce = int(time.time()) & 0xffffffff
        r = self.bmc(f"arm 0x{nonce:x}", 2)
        return "armed" in r

    # ── domain verbs ────────────────────────────────────────────────────
    def health_text(self):
        return self.bmc("health", 3)

    def health_raw(self):
        """Read + decode the BMC1 breadcrumb straight out of DRAM (works even
        if the console text path is flaky — it's just a memory read)."""
        words = self.read_words(BMC_HEALTH_BASE, len(HEALTH_WORDS))
        if len(words) < len(HEALTH_WORDS):
            return None
        d = dict(zip(HEALTH_WORDS, words))
        if d["magic"] != BMC_HEALTH_MAGIC:
            return None
        d["uptime"] = (d["uptime_hi"] << 32) | d["uptime_lo"]
        d["ticks"] = (d["tick_hi"] << 32) | d["tick_lo"]
        d["guest_pc"] = (d["guest_pc_hi"] << 32) | d["guest_pc_lo"]
        d["flag_names"] = [n for (n, b) in FLAG_BITS if d["flags"] & (1 << b)]
        return d

    def temp(self):
        return self.bmc("temp", 2)

    def flags(self):
        return self.bmc("flags", 2)

    def set_flag(self, name, val):
        self.arm()
        return self.bmc(f"flag {name} 0x{int(val):x}", 2)

    def console(self, n=0):
        return self.bmc(f"con read {n:x}" if n else "con read", 4)

    def inject(self, text):
        return self.bmc(f"con inject {text}", 2)

    def tee(self, n=0):
        return self.bmc(f"con tee {n:x}" if n else "con tee", 2)

    def reset(self):
        self.arm()
        # reboot_clean never returns; the link just drops. Short wait is fine.
        return self.bmc("reset", 1)

    def wdt(self, sub):
        if sub in ("hold", "arm"):
            self.arm()
        return self.bmc(f"wdt {sub}", 2)


# ── pretty printers ─────────────────────────────────────────────────────
def print_health(d):
    if not d:
        print("no BMC1 record (board down, or magic mismatch)")
        return
    up_s = d["uptime"] // 24000000  # A64 arch timer 24 MHz -> seconds (approx)
    print(f"BMC health  v{d['version'] >> 16}.{d['version'] & 0xffff}")
    print(f"  uptime      ~{up_s}s  (cnt=0x{d['uptime']:x})")
    print(f"  timer       ticks={d['ticks']}  delta={d['tick_delta']}  "
          f"({'LIVE' if d['tick_delta'] else 'STALLED'})")
    print(f"  guest_pc    0x{d['guest_pc']:x}")
    print(f"  exceptions  count={d['exc_count']}  last_kind=0x{d['last_exc_kind']:x}"
          f"  last_esr=0x{d['last_exc_esr']:08x}")
    print(f"  cores       online=0x{d['online_map']:x}  "
          f"hb=[{d['hb_cpu0']} {d['hb_cpu1']} {d['hb_cpu2']} {d['hb_cpu3']}]")
    print(f"  console     bytes={d['cons_bytes']}  faults={d['cons_faults']}  "
          f"ffv={d['ffv_count']}")
    t = d["temp_mc"]
    print(f"  temperature {'n/a' if t == 0 else f'{t/1000:.1f} C'}")
    print(f"  flags       0x{d['flags']:x}  {d['flag_names']}")
    print(f"  wdt_hold    {d['wdt_hold']}")


# ── CLI ─────────────────────────────────────────────────────────────────
def main(argv):
    ap = argparse.ArgumentParser(description="bzdOS software-BMC client")
    ap.add_argument("--iface", default="br0")
    sub = ap.add_subparsers(dest="cmd", required=True)

    h = sub.add_parser("health", help="structured status record")
    h.add_argument("--raw", action="store_true",
                   help="decode the BMC1 breadcrumb directly (memory read)")
    sub.add_parser("temp", help="SoC temperature")
    sub.add_parser("flags", help="list debug flags")
    f = sub.add_parser("flag", help="set a debug flag (auto-arms)")
    f.add_argument("name"); f.add_argument("value", type=int)
    c = sub.add_parser("console", help="dump / stream the guest console")
    c.add_argument("-n", type=lambda x: int(x, 0), default=0)
    c.add_argument("--stream", action="store_true", help="follow live TX tee")
    i = sub.add_parser("inject", help="type a line into the guest console")
    i.add_argument("text")
    sub.add_parser("reset", help="clean reboot to U-Boot (auto-arms)")
    w = sub.add_parser("wdt", help="watchdog control")
    w.add_argument("sub", choices=["hold", "release", "arm", "disarm"])
    d = sub.add_parser("dump", help="raw physical read (via dbgmon)")
    d.add_argument("addr", type=lambda x: int(x, 0))
    d.add_argument("count", type=lambda x: int(x, 0), default=16, nargs="?")

    args = ap.parse_args(argv)
    bmc = BMC(iface=args.iface)
    try:
        if args.cmd == "health":
            if args.raw:
                print_health(bmc.health_raw())
            else:
                print(bmc.health_text())
        elif args.cmd == "temp":
            print(bmc.temp())
        elif args.cmd == "flags":
            print(bmc.flags())
        elif args.cmd == "flag":
            print(bmc.set_flag(args.name, args.value))
        elif args.cmd == "console":
            if args.stream:
                print("streaming guest console (Ctrl-C to stop)...")
                try:
                    while True:
                        out = bmc.tee(256)
                        s = out.replace("(tee empty)", "").strip("\r\n ")
                        if s:
                            sys.stdout.write(s)
                            sys.stdout.flush()
                        time.sleep(0.1)
                except KeyboardInterrupt:
                    print()
            else:
                print(bmc.console(args.n))
        elif args.cmd == "inject":
            print(bmc.inject(args.text))
        elif args.cmd == "reset":
            print(bmc.reset())
            print("(link dropped — board resetting to U-Boot)")
        elif args.cmd == "wdt":
            print(bmc.wdt(args.sub))
        elif args.cmd == "dump":
            for i, w_ in enumerate(bmc.read_words(args.addr, args.count)):
                if i % 4 == 0:
                    sys.stdout.write(f"\n{args.addr + i*4:08x}:")
                sys.stdout.write(f" {w_:08x}")
            print()
    finally:
        bmc.close()


if __name__ == "__main__":
    main(sys.argv[1:])
