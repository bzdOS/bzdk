#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""uboot_maint.py -- open a window in which U-Boot's prompt can be caught.

Since 2026-09-26 uboot.env has `bootdelay=-2`: autoboot cannot be
interrupted, so nothing can park U-Boot at a prompt with a wedged gadget (see
HANDOFF 2026-09-26). Tools that need the prompt (uboot_chainload_test.py,
uboot_flash_fit.py) open a window with `bootdelay=3` for their run and put
`-2` back afterwards -- in a `finally`, after waiting (bounded) for the guest
to come back, because the environment is written from the guest.

    with maintenance():
        ...catch the prompt, test, flash...

    python3 uboot_maint.py open|close     # by hand
"""
import contextlib
import subprocess
import sys
import time

ENV_TOOL = '/opt/bzdos/microkernel/uboot_env.py'
SSH = ['ssh', '-o', 'ConnectTimeout=6', '-o', 'BatchMode=yes',
       '-i', '/root/.ssh/chimp_ed25519', 'root@192.168.88.82']
GUEST_WAIT_S = 420      # an idle prompt resets itself after 120 s, then ~100 s to a guest


def _env(*args):
    r = subprocess.run(['python3', ENV_TOOL, *args], capture_output=True, text=True,
                       timeout=240, stdin=subprocess.DEVNULL)
    return r.returncode, (r.stdout + r.stderr).strip()


def bootdelay():
    rc, out = _env('get', 'bootdelay')
    return out.splitlines()[-1] if rc == 0 and out else None


def set_bootdelay(v):
    rc, out = _env('set', f'bootdelay={v}')
    if rc != 0:
        raise RuntimeError(f"uboot_env set bootdelay={v} failed: {out[-300:]}")
    got = bootdelay()
    if got != str(v):
        raise RuntimeError(f"bootdelay reads back {got!r}, wanted {v}")


def wait_guest(limit_s=GUEST_WAIT_S):
    t0 = time.time()
    while time.time() - t0 < limit_s:
        r = subprocess.run(SSH + ['true'], capture_output=True, timeout=20,
                           stdin=subprocess.DEVNULL)
        if r.returncode == 0:
            return True
        time.sleep(5)
    return False


@contextlib.contextmanager
def maintenance(log=print):
    log("maintenance window: bootdelay=3 (prompt catchable)")
    set_bootdelay(3)
    try:
        yield
    finally:
        if not wait_guest():
            log(f"!!! guest not back within {GUEST_WAIT_S} s: bootdelay is still 3 -- "
                "close the window by hand: python3 uboot_maint.py close")
        else:
            try:
                set_bootdelay(-2)
                log("maintenance window closed: bootdelay=-2")
            except RuntimeError as e:
                log(f"!!! {e} -- close by hand: python3 uboot_maint.py close")


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] not in ('open', 'close'):
        sys.exit(__doc__.strip().splitlines()[-1])
    set_bootdelay(3 if sys.argv[1] == 'open' else -2)
    print(f"bootdelay={bootdelay()}")
