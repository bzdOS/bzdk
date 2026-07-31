#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# hotload.py <file> <hexaddr> — надёжный hot-reload через netcon (0x88B6):
# 1) "nrx <addr> <len>" по консоли 0x88B5 → плата входит в netcon_recv
# 2) send_blob по 0x88B6 (seq/ack/crc, ретрансмиты) — не теряет кадры
# 3) читает "nrx got=" → 4) "c <addr>" исполняет → печатает.
import os,sys,socket,struct,select,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import netcon
IFACE="br0"; C_ETYPE=0x88B5
BOARD=bytes.fromhex("02bd05000001"); BCAST=b"\xff"*6
if len(sys.argv)<3: print("usage: hotload.py <file> <hexaddr> [--call]"); sys.exit(2)
path=sys.argv[1]; addr=int(sys.argv[2],16); do_call="--call" in sys.argv
blob=open(path,"rb").read(); n=len(blob)
# console socket (0x88B5)
cs=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(C_ETYPE)); cs.bind((IFACE,C_ETYPE)); CSRC=cs.getsockname()[4]
def csend(t):
    p=t.encode()+b"\r"
    if len(p)<46: p+=b"\x00"*(46-len(p))
    cs.send(BCAST+CSRC+struct.pack("!H",C_ETYPE)+p)
def cdrain(sec):
    buf="";t0=time.time()
    while time.time()-t0<sec:
        r,_,_=select.select([cs],[],[],0.2)
        if not r: continue
        f=cs.recv(2048)
        if len(f)<14 or struct.unpack("!H",f[12:14])[0]!=C_ETYPE or f[6:12]!=BOARD: continue
        pl=f[14:].split(b"\x00",1)[0]
        if pl: buf+=pl.decode("latin1","replace")
    return buf
# netcon data socket (0x88B6)
ns,_=netcon.open_netcon_socket(IFACE)
cdrain(0.3)
print(f"[hotload] nrx {addr:x} {n:x}"); csend(f"nrx {addr:x} {n:x}"); time.sleep(0.05)
ok=netcon.send_blob(ns, blob, addr_hint=addr, dst_mac=b"\xff"*6)
r=cdrain(2.0)
import re
m=re.search(r"nrx got=([0-9a-fA-F]+)", r)
got=int(m.group(1),16) if m else None
print(f"[hotload] send_blob ok={ok}, board nrx got={got} (want {n})")
if got==n:
    print("✅ blob delivered intact via netcon")
    if do_call:
        csend(f"c {addr:x}"); time.sleep(0.3); print("call:",cdrain(1.5).strip()[-60:])
    sys.exit(0)
print("⛔ mismatch/loss"); sys.exit(1)
