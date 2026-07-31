#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# repl-cmd.py — шлёт команды резидентному REPL по сети (raw 0x88B5) и печатает
# ответы. Аргументы: пары "cmd" wait_sec ... Пример:
#   repl-cmd.py "mi" 3 "mpN 400000" 12 "bc 0x50000000 24" 3
import os,sys,socket,struct,select,time
IFACE="br0"; ETYPE=0x88B5
BOARD=bytes.fromhex("02bd05000001"); BCAST=b"\xff"*6
s=socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETYPE))
s.bind((IFACE,ETYPE)); SRC=s.getsockname()[4]
def send(text):
    p=(text+"\r").encode()
    if len(p)<46: p=p+b"\x00"*(46-len(p))
    s.send(BCAST+SRC+struct.pack("!H",ETYPE)+p)
def drain(sec):
    buf=""; t0=time.time()
    while time.time()-t0<sec:
        r,_,_=select.select([s],[],[],0.3)
        if not r: continue
        f=s.recv(2048)
        if len(f)<14 or struct.unpack("!H",f[12:14])[0]!=ETYPE: continue
        if f[6:12]!=BOARD: continue
        pl=f[14:].split(b"\x00",1)[0]
        if pl: buf+=pl.decode("latin1","replace")
    return buf
# первый дренаж — сбросить накопившееся эхо/prompt
drain(0.5)
args=sys.argv[1:]
i=0
while i<len(args):
    cmd=args[i]; wait=float(args[i+1]) if i+1<len(args) else 3.0; i+=2
    print(f"\n=== → {cmd}  (слушаю {wait}s) ===",flush=True)
    send(cmd); out=drain(wait)
    # ответ: убираем посимвольное эхо (склеиваем), печатаем строки с данными
    lines=[l for l in out.replace("\r","\n").split("\n") if l.strip() and l.strip()!="mk>"]
    for l in lines: print("  "+l,flush=True)
s.close()
