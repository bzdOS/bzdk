#!/usr/bin/env python3
# mk-load.py — поймать U-Boot АГРЕССИВНО (bootdelay слетел с -1 → узкое окно) и сразу
# загрузить новое само-WDT микроядро. Не нужен bootdelay=-1: поймал раз → loady → go,
# дальше микроядро само-recoverable. reboot НЕ шлю.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
MK="/opt/bzdos/microkernel/microkernel.bin"
WDT_MODE="0x01c20cb8"
LOG="/tmp/mk-load.log"; logf=open(LOG,"wb")
BOOTKW=("Hit [Enter]","Type '?'","Executing script","Starting kernel","Booting","Loading Environment","## Booting")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def wait_present(dl):
    # порт сейчас отсутствует (плата за EBS). Ждём появления (reset → U-Boot),
    # открываем МГНОВЕННО.
    while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.006)
    if not os.path.exists(L.TTY): return None
    try: return L.open_tty(True)
    except OSError: return None

def aggressive_catch(fd,dl):
    buf=b""; t0=time.time()
    while time.time()-t0<20 and time.time()<dl:
        try: os.write(fd,b"\r\x03")
        except OSError: return None
        b,dead=L.rd(fd,0.03)
        if b: buf+=b; save(b)
        cb=clean(buf)
        if "=>" in cb[-50:]: return "uboot"
        if any(k in cb for k in BOOTKW): return "loader"
        if dead: return None
    return None

def stable_uboot(fd):
    good=0
    for _ in range(5):
        try: L.rd(fd,0.08); os.write(fd,b"\r")
        except OSError: return False
        time.sleep(0.3); r=clean(L.rd(fd,0.7)[0]); save(r)
        if any(k in r for k in ("Card","MMC","Booting","Loading","Starting")): return False
        if "=>" in r: good+=1
        if good>=2: return True
    return good>=2

def ucmd(fd,c,w=3):
    L.rd(fd,0.1); os.write(fd,(c+"\r").encode())
    acc=b""; t0=time.time()
    while time.time()-t0<w:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-6:] and len(clean(acc))>len(c): break
        if dead: break
    return clean(acc)

log(f"payload {MK} ({os.path.getsize(MK)}B). Жду ТВОЙ reset → агрессивно ловлю U-Boot.")
DL=time.time()+900
# если порт сейчас есть (не за EBS) — ждём его исчезновения (reset)
if os.path.exists(L.TTY):
    log("порт есть — жду reset (исчезнет)...")
    while os.path.exists(L.TTY) and time.time()<DL: time.sleep(0.05)
fd=wait_present(DL)
if fd is None: log("⛔ порт не появился за 15 мин"); sys.exit(2)
log("порт появился — флужу за '=>'...")
k=aggressive_catch(fd,DL)
if k=="loader":
    log("⛔ окно U-Boot проскочило → loader (bootdelay не -1). Из loader loady нельзя. Нужен ещё reset (ловлю быстрее) ИЛИ чиним bootdelay.")
    # попробуем поймать след. reset автоматически? нет — просто сообщим
    try: os.close(fd)
    except: pass
    sys.exit(3)
if k!="uboot":
    log("⛔ ни U-Boot, ни loader за 20с"); sys.exit(3)
log("✅ '=>' мелькнул — проверяю стабильность")
if not stable_uboot(fd):
    log("⚠ '=>' нестабилен (автобут) — упустили. Нужен ещё reset."); os.close(fd); sys.exit(3)
log("✅ стабильный U-Boot. Снимаю WDT, фиксирую bootdelay=-1 (чтоб дальше было легче), затем loady.")
ucmd(fd,f"mw.l {WDT_MODE} 0 ; setenv bootdelay -1 ; saveenv",8)
ucmd(fd,"printenv bootdelay",2)
log("── loady + go нового само-WDT микроядра ──")
okg,msg=L.do_loady_and_go(fd, save, MK)
log(f"loady/go: {msg}")
if not okg: log("⛔ загрузка не удалась"); logf.close(); sys.exit(4)
log("── жду re-enum микроядра (1d6b:0010 → ttyCHIMP), ловлю консоль ──")
dl2=time.time()+25
while os.path.exists(L.TTY) and time.time()<dl2: time.sleep(0.03)   # disconnect
while not os.path.exists(L.TTY) and time.time()<dl2: time.sleep(0.02)  # reconnect
if not os.path.exists(L.TTY): log("⛔ микроядро не переэнумерировалось (или WDT-reset). См. лог."); logf.close(); sys.exit(5)
time.sleep(0.4)
try: kfd=L.open_tty(True)
except Exception as e: log(f"⛔ open MK: {e}"); sys.exit(5)
m={"init":False,"cfg":False,"alive":False}; t1=time.time(); acc=b""
while time.time()-t1<25:
    b,dead=L.rd(kfd,0.3)
    if b:
        save(b); acc+=b; cb=clean(acc)
        m["init"]= m["init"] or ("musb_init done" in cb)
        m["cfg"]=  m["cfg"]  or ("host configured" in cb)
        m["alive"]=m["alive"]or ("BZDOS-MK-ALIVE" in cb)
    if dead: log("порт дропнулся (WDT-reset?)"); break
    if m["alive"]: break
try: os.close(kfd)
except: pass
log("================ ИТОГ ================")
log(f"  musb_init done : {'✅' if m['init'] else '—'}")
log(f"  host configured: {'✅' if m['cfg'] else '—'}")
log(f"  BZDOS-MK-ALIVE : {'✅' if m['alive'] else '—'}")
if m["alive"] or m["cfg"]:
    log("🎉 КОНСОЛЬ МИКРОЯДРА ЖИВА — TX доставляет текст! Фикс сработал.")
else:
    log("маркеров нет — итерируем фикс (безопасно, само-WDT вернёт в U-Boot).")
logf.close()
