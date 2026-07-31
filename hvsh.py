#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""hvsh.py -- persistent hvdbg session behind a Unix socket.

WHY: the debug channel itself is fast. Measured live 2026-07-29 on a healthy
board, one dbgmon command round-trips in 0.061s -- but a one-shot
`python3 -c "import hvdbg; hvdbg.HV().cmd(...)"` costs 0.57s of setup before
it does anything (0.036s interpreter start + 0.22s `import hvdbg` +
0.31s HV() construction, which includes its own startup _drain). So ~90% of
the wall-clock of interactive board poking was scaffolding being rebuilt and
thrown away, once per command. Over a debugging session of hundreds of
commands that is the dominant cost -- far bigger than anything in the
transport.

This keeps ONE HV instance (and one measured RTT, see HV.rtt()) alive in a
server process; clients are thin enough to skip importing hvdbg entirely.

    python3 hvsh.py --serve &              # once
    python3 hvsh.py 'hv.cmd("gr")'         # ~0.09s instead of ~0.63s
    python3 hvsh.py 'hv.read_words(0x50000400, 20)'

The request is a Python expression evaluated with `hv` (the live HV) and
`hvdbg` in scope; the reply is its repr(), or a traceback. That is
deliberately unrestricted: this is a root-owned socket on a developer host
driving a debug channel that can already call arbitrary hypervisor
addresses, so a sandbox here would protect nothing while getting in the way
of exactly the ad-hoc queries it exists to make cheap.

Statements work too (anything `eval` rejects is retried with `exec`), so
multi-step sequences can share state across calls:

    python3 hvsh.py 'w = hv.read_words(0x50020000, 48)'
    python3 hvsh.py '[hex(x) for x in w[:8]]'

--serve is idempotent-ish: it refuses to start if the socket is already
live, so a stale background server is reused rather than double-bound.
"""
import os
import socket
import sys
import traceback

SOCK_PATH = "/tmp/bzdos-hvsh.sock"


def _run(src, ns):
    """Run a request and return what a REPL would print.

    Requests are usually multi-statement (that is the whole point -- one
    client round trip driving many board commands, sharing state), so a
    plain eval() is not enough, and a plain exec() would silently swallow
    the answer. Do what an interactive interpreter does: exec everything up
    to the last statement, then eval the last one IF it is an expression.
    A trailing assignment/loop legitimately produces no output."""
    import ast

    tree = ast.parse(src, mode="exec")
    if not tree.body:
        return ""
    last = tree.body[-1]
    if isinstance(last, ast.Expr):
        if len(tree.body) > 1:
            exec(compile(ast.Module(body=tree.body[:-1], type_ignores=[]),
                         "<hvsh>", "exec"), ns)
        val = eval(compile(ast.Expression(body=last.value), "<hvsh>", "eval"), ns)
        return "" if val is None else (val if isinstance(val, str) else repr(val))
    exec(compile(tree, "<hvsh>", "exec"), ns)
    return ""


def _recv_all(conn):
    chunks = []
    while True:
        b = conn.recv(65536)
        if not b:
            break
        chunks.append(b)
        if b.endswith(b"\x00"):
            break
    return b"".join(chunks).rstrip(b"\x00").decode("utf-8", "replace")


def serve(sock_path=SOCK_PATH):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import hvdbg

    if os.path.exists(sock_path):
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            probe.connect(sock_path)
            probe.close()
            print(f"hvsh: a live server already owns {sock_path}", flush=True)
            return 1
        except OSError:
            os.unlink(sock_path)        # stale socket from a dead server
        finally:
            probe.close()

    hv = hvdbg.HV()
    # Prime the RTT measurement once here rather than lazily inside the
    # first client's request, so no single client eats it.
    rtt = hv.rtt()
    ns = {"hv": hv, "hvdbg": hvdbg}

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(8)
    print(f"hvsh: serving on {sock_path} (channel RTT {rtt:.3f}s)", flush=True)

    try:
        while True:
            conn, _ = srv.accept()
            try:
                req = _recv_all(conn)
                if req.strip() in ("--quit", "--shutdown"):
                    conn.sendall(b"bye")
                    return 0
                try:
                    out = _run(req, ns)
                except Exception:
                    out = traceback.format_exc()
                conn.sendall(out.encode("utf-8", "replace"))
            finally:
                conn.close()
    finally:
        srv.close()
        try:
            os.unlink(sock_path)
        except OSError:
            pass


def client(expr, sock_path=SOCK_PATH):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sock_path)
    except OSError as e:
        sys.stderr.write(
            f"hvsh: no server on {sock_path} ({e}); start one with:\n"
            f"    python3 {sys.argv[0]} --serve &\n")
        return 2
    s.sendall(expr.encode("utf-8") + b"\x00")
    s.shutdown(socket.SHUT_WR)
    out = []
    while True:
        b = s.recv(65536)
        if not b:
            break
        out.append(b)
    s.close()
    reply = b"".join(out).decode("utf-8", "replace")
    if reply:
        print(reply)
    return 1 if reply.startswith("Traceback") else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    if sys.argv[1] == "--serve":
        sys.exit(serve())
    if sys.argv[1] == "-f":
        # Read the request from a file. Anything with nested quoting is
        # painful to pass through a shell argv intact (a stray backslash in
        # an f-string is a SyntaxError on the server, not a shell error, so
        # it fails confusingly); a file sidesteps the shell entirely.
        with open(sys.argv[2]) as f:
            sys.exit(client(f.read()))
    sys.exit(client(" ".join(sys.argv[1:])))
