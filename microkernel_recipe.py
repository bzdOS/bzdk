#!/usr/bin/env python3
"""
microkernel_recipe.py -- dev-stand recipe for the bzdOS microkernel.

Implements the same catch-U-Boot -> loady -> go -> capture flow as
loady_over_acm.py (imported from here, same directory, so the tricky
sb<->tty fd handoff logic lives in exactly one place), wrapped up as a
dev-stand "recipe" with the same contract as supervisor.py's own session():
write everything to a per-session log file and return a short verdict
string.

Two ways for the parent (supervisor.py) to use this -- pick whichever is
less invasive to wire in. Neither requires editing supervisor.py's helper
functions, only session().

--------------------------------------------------------------------------
OPTION A (recommended -- ~4 line change, fully self-contained recipe):

In supervisor.py's `session(sess_log)`, right at the top (before it opens
its own tty / calls its own catch_uboot), add an early branch for the new
recipe so this module owns the whole session for it:

    def session(sess_log):
        recipe_name=recipe()
        if recipe_name=="microkernel":            # <-- new
            import microkernel_recipe              # <-- new
            return microkernel_recipe.session(sess_log)  # <-- new
        ...                                        # (existing code below,
                                                     #  unchanged, still
                                                     #  reads recipe_name
                                                     #  again -- harmless)

Then add "microkernel" as a valid value written to /opt/bzdos/devstand/recipe.

--------------------------------------------------------------------------
OPTION B (reuse supervisor's already-open, already-caught tty fd):

If you'd rather have supervisor.py keep owning catch_uboot() (e.g. to keep
one uniform "no U-Boot prompt" path for all recipes), call run() instead,
right after supervisor.session()'s existing `save(f,"\\n# [U-Boot prompt
пойман]\\n".encode())` line, before its `if recipe_name=="usbkernel":`
block:

    elif recipe_name=="microkernel":
        import microkernel_recipe
        v=microkernel_recipe.run(tfd, sys.modules[__name__], save)
        try: os.close(tfd)
        except: pass
        f.close(); return v

`run(tty, helpers, save)` takes the already-open/-caught fd, an object
exposing rd/wr/ensure_chardev/TTY (supervisor.py's own module satisfies
this -- `sys.modules[__name__]` from inside supervisor.py *is* supervisor),
and a save(bytes) callable (supervisor's local `save` closure already has
the right signature). It closes `tty` itself before returning.

--------------------------------------------------------------------------
Verdicts returned (both entry points): "no U-Boot", "loady-failed (...)",
"MK ALIVE (enumerated)", "MK silent (no re-enum)",
"MK silent (enumerated, no ALIVE marker)".
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loady_over_acm as loader  # noqa: E402  (same dir -- the loady/sb/go engine)

DEFAULT_PAYLOAD = "/opt/bzdos/microkernel/microkernel.bin"
DEFAULT_ADDR = loader.DEFAULT_ADDR  # 0x42000000


def _map_verdict(loady_ok, loady_msg, reenum_verdict=None):
    if not loady_ok:
        return f"loady-failed ({loady_msg})"
    if reenum_verdict == "ENUMERATED+ALIVE":
        return "MK ALIVE (enumerated)"
    if reenum_verdict == "no re-enumeration":
        return "MK silent (no re-enum)"
    return "MK silent (enumerated, no ALIVE marker)"


def session(sess_log_path, payload=DEFAULT_PAYLOAD, addr=DEFAULT_ADDR,
            present_timeout=30.0, uboot_timeout=8.0,
            reenum_timeout=15.0, alive_wait=20.0):
    """
    Fully self-contained recipe entry point -- same contract as
    supervisor.py's own session(sess_log): opens the log file, drives the
    whole board interaction, returns a verdict string. Uses
    loady_over_acm's tty helpers (which are themselves copies of
    supervisor.py's proven patterns: ensure_chardev fake-chardev fix,
    open_tty raw/-echo @115200, catch_uboot Ctrl-C flood).
    """
    f = open(sess_log_path, "wb")

    def save(d):
        if isinstance(d, str):
            d = d.encode()
        f.write(d)
        f.flush()

    save(f"# recipe=microkernel payload={payload} addr=0x{addr:x} "
         f"start={time.strftime('%Y-%m-%d %H:%M:%S')}\n".encode())

    if not loader.wait_present(present_timeout):
        save(b"\n# [ttyACM0 never appeared]\n")
        f.close()
        return "no U-Boot"

    try:
        tfd = loader.open_tty(True)
    except Exception as e:
        save(f"\n# [open_tty failed: {e}]\n".encode())
        f.close()
        return "no U-Boot"

    ok, buf = loader.catch_uboot(tfd, uboot_timeout)
    save(buf)
    if not ok:
        try:
            os.close(tfd)
        except OSError:
            pass
        f.close()
        return "no U-Boot"
    save(b"\n# [U-Boot prompt caught]\n")

    loady_ok, loady_msg = loader.do_loady_and_go(tfd, save, payload, addr)
    if not loady_ok:
        f.close()
        return _map_verdict(loady_ok, loady_msg)

    reenum_v = loader.capture_reenum(save, stream_stdout=False,
                                      reenum_timeout=reenum_timeout,
                                      alive_wait=alive_wait)
    f.close()
    return _map_verdict(loady_ok, loady_msg, reenum_v)


def run(tty, helpers, save, payload=DEFAULT_PAYLOAD, addr=DEFAULT_ADDR,
        reenum_timeout=15.0, alive_wait=20.0):
    """
    Compatibility entry point for a caller (supervisor.py) that already has
    an open, non-blocking `tty` fd sitting at the U-Boot '=>' prompt (i.e.
    it already ran its own catch_uboot() successfully before calling this).

    helpers: object/module exposing rd(fd,t), wr(fd,d), ensure_chardev(),
             and TTY (path string). supervisor.py itself qualifies -- pass
             `sys.modules[__name__]` from inside supervisor.py.
    save:    callable(bytes) appending to the session log (matches
             supervisor.session()'s local `save` closure).

    Closes `tty` itself before returning (U-Boot's gadget is expected to
    drop at `go`, so the fd is no longer valid afterwards regardless).
    """
    rd_fn = getattr(helpers, "rd", loader.rd)
    wr_fn = getattr(helpers, "wr", loader.wr)
    ensure_fn = getattr(helpers, "ensure_chardev", loader.ensure_chardev)
    tty_path = getattr(helpers, "TTY", loader.TTY)

    def open_tty_h(nonblock=True):
        ensure_fn()
        os.system(f"stty -F {tty_path} 115200 raw -echo 2>/dev/null")
        flags = os.O_RDWR | os.O_NOCTTY | (os.O_NONBLOCK if nonblock else 0)
        return os.open(tty_path, flags)

    loady_ok, loady_msg = loader.do_loady_and_go(
        tty, save, payload, addr, open_tty_fn=open_tty_h, rd_fn=rd_fn, wr_fn=wr_fn)
    if not loady_ok:
        return _map_verdict(loady_ok, loady_msg)

    reenum_v = loader.capture_reenum(
        save, stream_stdout=False, reenum_timeout=reenum_timeout,
        alive_wait=alive_wait, open_tty_fn=open_tty_h, rd_fn=rd_fn)
    return _map_verdict(loady_ok, loady_msg, reenum_v)


if __name__ == "__main__":
    # manual smoke run: python3 microkernel_recipe.py [payload] [logfile]
    payload = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PAYLOAD
    log = sys.argv[2] if len(sys.argv) > 2 else "/tmp/microkernel_recipe_session.log"
    print(session(log, payload=payload))
