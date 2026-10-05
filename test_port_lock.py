#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""The ACM port lock (loady_over_acm.acquire_port_lock) must work across users.

2026-10-02: a lock file left by the agent user in /tmp made a root flash run
fail with "no tty". The lock now lives in a setgid root:fleet directory. This
test builds a directory with the same ownership and mode, then checks that
the lock excludes in both directions between root and the agent user and that
the file stays openable by the group whoever created it.

Needs root and an 'agent' user in group 'fleet'; skips otherwise.
    python3 test_port_lock.py
"""
import grp, os, pwd, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
CHILD = r'''
import os, sys, time
sys.path.insert(0, %r)
import loady_over_acm as L
hold = float(sys.argv[1]); to = float(sys.argv[2])
t = time.time()
try:
    L.acquire_port_lock(settle=0, timeout=to)
except TimeoutError:
    print("TIMEOUT"); sys.exit(3)
print("GOT %%.1f" %% (time.time() - t), flush=True)
time.sleep(hold)
L.release_port_lock()
''' % HERE


def run(user, hold, to, lock, child):
    cmd = ['env', 'CHIMP_LOCK_PATH=' + lock, sys.executable, child, str(hold), str(to)]
    if user != 'root':
        cmd = ['su', user, '-s', '/bin/sh', '-c', ' '.join(cmd)]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def main():
    if os.geteuid() != 0:
        print('SKIP: needs root'); return 0
    try:
        pwd.getpwnam('agent'); fleet = grp.getgrnam('fleet').gr_gid
    except KeyError:
        print("SKIP: no 'agent' user / 'fleet' group"); return 0
    d = tempfile.mkdtemp(prefix='chimp-lock-test.', dir='/run')
    os.chown(d, 0, fleet); os.chmod(d, 0o2775)
    lock = os.path.join(d, 'acm.lock')
    child = os.path.join(d, 'child.py')
    open(child, 'w').write(CHILD)
    os.chmod(child, 0o644)
    bad = 0
    try:
        for first, second in (('agent', 'root'), ('root', 'agent')):
            a = run(first, 3, 10, lock, child)
            line = a.stdout.readline()
            assert line.startswith('GOT'), '%s could not take a free lock: %s' % (first, line + a.stdout.read())
            t = time.time()
            b = run(second, 0, 15, lock, child)
            out = b.communicate()[0].strip()
            waited = time.time() - t
            ok = 'GOT' in out.splitlines()[-1] and 'busy' in out and waited >= 1.5
            print('%-5s holds, %-5s waits: %s (waited %.1fs) %s' %
                  (first, second, 'ok  ' if ok else 'FAIL', waited, '' if ok else out))
            bad += not ok
            a.communicate()
        st = os.stat(lock)
        mode_ok = (st.st_mode & 0o777) == 0o660 and st.st_gid == fleet
        print('lock file mode %o group %s: %s' % (st.st_mode & 0o777, grp.getgrgid(st.st_gid).gr_name,
                                                 'ok' if mode_ok else 'FAIL'))
        bad += not mode_ok
        # a holder that never lets go: the waiter must time out, not hang
        a = run('agent', 6, 10, lock, child); a.stdout.readline()
        b = run('root', 0, 2, lock, child); out = b.communicate()[0].strip()
        print('timeout while held: %s' % ('ok' if out.splitlines()[-1] == 'TIMEOUT' else 'FAIL ' + out))
        bad += out.splitlines()[-1] != 'TIMEOUT'
        a.communicate()
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('PASS' if not bad else 'FAILED (%d)' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
