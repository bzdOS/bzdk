#!/usr/bin/env python3
# mk-test2.py — тест УЛУЧШЕННОГО микроядра (self-WDT + фикс re-enum + EP0 multi-packet).
# На ТВОЙ reset (старое микроядро застряло): ловлю U-Boot → снимаю WDT → loady
# microkernel.bin → go → жду СВЕЖУЮ re-enumeration (микроядро форсит свой гаджет
# 1d6b:0010 → /dev/ttyCHIMP) → ловлю маркеры MK/ALIVE. Новое ядро само-WDT: зависнет
# → 16с → авто-reset в U-Boot (НЕ залипнет). reboot НЕ шлю.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
MK="/opt/bzdos/microkernel/microkernel.bin"
WDT_MODE="0x01c20cb8"
LOG="/tmp/mk-test2.log"; logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def wait_gone_then_present(dl, was_present):
    """Если порт был — ждём исчезновения (disconnect), потом появления."""
    if was_present:
        while os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.03)
    while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.02)
    return os.path.exists(L.TTY)

def ucmd(fd,c,w=3):
    L.rd(fd,0.12); os.write(fd,(c+"\r").encode())
    acc=b""; t0=time.time()
    while time.time()-t0<w:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-6:] and len(clean(acc))>len(c): break
        if dead: break
    return clean(acc)

log(f"payload: {MK} ({os.path.getsize(MK)}B)")
log("Жду ТВОЙ reset (старое микроядро застряло) → U-Boot. Порт /dev/ttyCHIMP.")
DL=time.time()+900
# ждём исчезновения текущего порта (reset) и появления U-Boot
if os.path.exists(L.TTY):
    log("вижу застрявший порт — жду reset (порт исчезнет)...")
    while os.path.exists(L.TTY) and time.time()<DL: time.sleep(0.05)
    log("✓ порт исчез — reset пошёл")
while not os.path.exists(L.TTY) and time.time()<DL: time.sleep(0.02)
if not os.path.exists(L.TTY): log("⛔ U-Boot не появился"); sys.exit(2)
time.sleep(0.4)
try: tfd=L.open_tty(True)
except Exception as e: log(f"⛔ open: {e}"); sys.exit(2)
ok,buf=L.catch_uboot(tfd,10); save(buf)
if not ok: log("⚠ '=>' не пойман флудом — пробую всё равно")
else: log("✅ U-Boot '=>'")
ucmd(tfd,f"mw.l {WDT_MODE} 0",1.5)   # снять залипший WDT (если был)

log("── loady + go нового микроядра ──")
okg,msg=L.do_loady_and_go(tfd, save, MK)
log(f"loady/go: {msg}")
if not okg:
    log("⛔ загрузка не удалась — см. лог"); logf.close(); sys.exit(3)

log("── жду re-enumeration микроядра (1d6b:0010 → ttyCHIMP) и ловлю консоль ──")
# do_loady_and_go закрыл fd; после go U-Boot-гаджет дропнется, микроядро поднимет свой
if not wait_gone_then_present(time.time()+25, True):
    log("⛔ микроядро не переэнумерировалось за 25с (или WDT уже ребутнул). См. лог/дальше.")
    logf.close(); sys.exit(4)
log("✓ порт появился заново — открываю консоль микроядра")
time.sleep(0.4)
try: kfd=L.open_tty(True)
except Exception as e: log(f"⛔ open MK console: {e}"); sys.exit(4)
markers={"init":False,"configured":False,"alive":False}
t1=time.time(); acc=b""
while time.time()-t1<25:
    b,dead=L.rd(kfd,0.3)
    if b:
        save(b); acc+=b
        cb=clean(acc)
        if "musb_init done" in cb: markers["init"]=True
        if "host configured" in cb: markers["configured"]=True
        if "BZDOS-MK-ALIVE" in cb: markers["alive"]=True
    if dead: log("порт дропнулся (WDT-reset? или disconnect)"); break
    if markers["alive"]: break
try: os.close(kfd)
except: pass
log("================ ИТОГ ================")
log(f"  MK: musb_init done      : {'✅' if markers['init'] else '—'}")
log(f"  MK: host configured     : {'✅' if markers['configured'] else '—'}")
log(f"  BZDOS-MK-ALIVE          : {'✅' if markers['alive'] else '—'}")
if markers["alive"] or markers["configured"]:
    log("🎉 КОНСОЛЬ МИКРОЯДРА РАБОТАЕТ — TX доставляет текст (фикс re-enum/EP0 сработал).")
    log("   Дальше: расширяем в диагностику устройств (USB/сеть), это и есть наш тонкий слой.")
elif markers["init"]:
    log("частично: init виден, но host configured нет — enumeration ещё не до конца (смотрим EP0).")
else:
    log("маркеров нет. Либо re-enum/TX ещё не тот, либо WDT ребутнул рано. Итерируем (безопасно, self-WDT).")
log(f"полный лог: {LOG}")
logf.close()
