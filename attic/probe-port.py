#!/usr/bin/env python3
# probe-port.py — что РЕАЛЬНО сидит на ttyACM0 прямо сейчас?
# Гипотеза: U-Boot уже висит на '=>' 33 мин, а fix-bootdelay слеп (шлёт только
# Ctrl-C, без Enter → idle-промпт не переэхуется). Убиваем фиксер, шлём Enter,
# смотрим. '=>'=U-Boot, 'OK'/'loader'=FreeBSD loader, 'login'=ядро поднялось.
import os,sys,time,signal
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

# 1) убить фиксер, дождаться освобождения порта
for pid in (132580,):
    try:
        os.kill(pid,signal.SIGTERM); print(f"SIGTERM -> {pid}")
    except ProcessLookupError:
        print(f"{pid} уже мёртв")
# ждём смерти
dl=time.time()+6
while time.time()<dl:
    try: os.kill(132580,0); time.sleep(0.15)
    except ProcessLookupError: break
# добить всё, что держит ttyACM (кроме нас)
time.sleep(0.3)

if not os.path.exists(L.TTY):
    print("⛔ ttyACM0 исчез — плата ушла в ребут прямо сейчас"); sys.exit(1)

fd=L.open_tty(True)
print(f"открыл {L.TTY}, слушаю ДО отправки (что там само по себе):")
b,_=L.rd(fd,1.0); print(repr(b[-200:]) if b else "(тишина)")

def ping(label,payload,w=1.2):
    if isinstance(payload,str): payload=payload.encode()
    L.rd(fd,0.05)
    L.wr(fd,payload)
    time.sleep(0.25)
    acc=b""
    for _ in range(3):
        x,dead=L.rd(fd,w)
        if x: acc+=x
        if dead: break
    print(f"\n--- {label} -> {payload!r} ---")
    print(clean(acc) if acc else "(нет ответа)")
    return clean(acc)

r1=ping("Enter","\r\n")
r2=ping("Enter x2","\r\n\r\n")
# идентификация
blob=(r1+r2)
verdict="?"
if "=>" in blob: verdict="U-BOOT (=>)"
elif "OK" in blob and ("loader" in blob.lower() or "Type '?'" in blob or "\nOK" in blob): verdict="FreeBSD LOADER (OK)"
elif "login:" in blob.lower(): verdict="KERNEL up (login)"
elif not blob.strip(): verdict="idle/no-echo (шлю ? и version)"
print(f"\n==================  ВЕРДИКТ: {verdict}  ==================")
# доп. зонды если непонятно
if verdict.startswith("?") or "idle" in verdict:
    ping("U-Boot version","version\r\n",1.5)
    ping("loader help?","?\r\n",1.5)
try: os.close(fd)
except: pass
