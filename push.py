#!/usr/bin/env python3
# push.py <file> <hexaddr> — HOT RELOAD: залить блоб в резидентный REPL по сети
# (raw 0x88B5), без reset/U-Boot/YMODEM. Шлёт "rx <addr> <len>", стримит байты
# кадрами, сверяет sum. Повтор при потере (канал лоссовый). Затем можно `c <addr>`.
import os,sys,socket,struct,select,time
IFACE="br0"; ETYPE=0x88B5
BOARD=bytes.fromhex("02bd05000001"); BCAST=b"\xff"*6
def usage(): print("usage: push.py <file> <hexaddr> [--call]"); sys.exit(2)
if len(sys.argv)<3: usage()
path=sys.argv[1]; addr=int(sys.argv[2],16); do_call="--call" in sys.argv
blob=open(path,"rb").read(); n=len(blob); want=sum(blob)&0xffffffff
s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(ETYPE))
s.bind((IFACE,ETYPE)); SRC=s.getsockname()[4]
def send(text):
    p=text.encode()+b"\r"
    if len(p)<46: p+=b"\x00"*(46-len(p))
    s.send(BCAST+SRC+struct.pack("!H",ETYPE)+p)
def send_raw(chunk):
    p=chunk
    if len(p)<46: p+=b"\x00"*(46-len(p))
    s.send(BCAST+SRC+struct.pack("!H",ETYPE)+p)
def drain(sec):
    buf=""; t0=time.time()
    while time.time()-t0<sec:
        r,_,_=select.select([s],[],[],0.2)
        if not r: continue
        f=s.recv(2048)
        if len(f)<14 or struct.unpack("!H",f[12:14])[0]!=ETYPE or f[6:12]!=BOARD: continue
        pl=f[14:].split(b"\x00",1)[0]
        if pl: buf+=pl.decode("latin1","replace")
    return buf
def attempt():
    drain(0.3)
    send(f"rx {addr:x} {n:x}")
    time.sleep(0.3)
    # HEX wire format (no NUL bytes — the EMAC console truncates binary at 0x00).
    hexb=blob.hex().encode()   # 2 ASCII chars per byte
    off=0
    while off<len(hexb):
        send_raw(hexb[off:off+44]); off+=44
        time.sleep(0.003)   # не топим RX-ring платы
    r=drain(3.0)
    return r
for tryn in range(1,6):
    r=attempt()
    got=None; sm=None
    import re
    m=re.search(r"got=([0-9a-fA-F]+)\s+sum=([0-9a-fA-F]+)", r)
    if m: got=int(m.group(1),16); sm=int(m.group(2),16)
    if sm is not None:
        print(f"[try {tryn}] sent {n}B want_sum={want:#x} -> board got={got} sum={sm:#x}")
    else:
        print(f"[try {tryn}] no ack (tail={r[-50:]!r})")
    if got==n and sm==want:
        print("✅ blob delivered intact")
        if do_call:
            send(f"c {addr:x}"); time.sleep(0.3); print("call reply:",drain(2.0).strip())
        sys.exit(0)
    time.sleep(0.5)
print("⛔ delivery failed after retries"); sys.exit(1)
