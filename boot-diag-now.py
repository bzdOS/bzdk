#!/usr/bin/env python3
# boot-diag-now.py — плата УЖЕ на FreeBSD loader OK-промпте (проверено probe-port).
# Ресет НЕ нужен. Выходим из пейджера, грузим НАШ diag /kernel с disk0p2 (ESP),
# verbose, boot. LED — главный сигнал: PE14 зелёный щёлкает на каждой вехе,
# PD24 красный = чётность. Ловим serial до EBS.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/boot-diag-now.log"; logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

if not os.path.exists(L.TTY):
    log("⛔ ttyACM0 исчез — плата ушла в ребут; перезапусти скрипт когда вернётся"); sys.exit(1)
fd=L.open_tty(True)

def send(c,w=2.5,nl=b"\r"):
    L.rd(fd,0.08)
    L.wr(fd,(c.encode() if isinstance(c,str) else c)+nl)
    time.sleep(0.25)
    acc=b""
    for _ in range(4):
        x,dead=L.rd(fd,w)
        if x: acc+=x
        if dead: break
    save(acc); return clean(acc)

log("Выхожу из пейджера (q) и проверяю, что это loader OK...")
L.wr(fd,b"q"); time.sleep(0.3); save(L.rd(fd,1.0)[0])
probe=send("echo LOADERPROBE_%s" % "OK")
if "LOADERPROBE_OK" not in probe:
    # ещё раз, вдруг пейджер не вышел
    L.wr(fd,b"q\r"); time.sleep(0.3); save(L.rd(fd,1.0)[0])
    probe=send("echo LOADERPROBE_OK")
if "LOADERPROBE_OK" not in probe:
    log(f"⚠ не подтвердил loader OK (echo не вернулся). Ответ: {probe[-160:]!r}")
    log("  Возможно это U-Boot '=>' или пейджер завис. Пробую как loader всё равно.")
else:
    log("✅ FreeBSD loader OK-промпт подтверждён (echo вернулся)")

log("lsdev — какие диски видит loader:")
send("lsdev",2.0)

# грузим НАШ diag /kernel c disk0p2 (ESP). unload сначала — сбросить авто-загруженный оригинал.
klo=False
for dv in ["disk0p2:","mmcsd1p2:","disk1p2:","mmcsd0p2:","disk0p2"]:
    send("unload",1.5)
    send('set currdev="%s"' % dv,1.2)
    rr=send("load /kernel",14.0)
    ok = bool(re.search(r'text=|data=|entry=|/kernel\s+text',rr)) and \
         all(x not in rr.lower() for x in ("error","cannot","fail","not found","no such","can't","unknown"))
    if ok:
        log(f"✅ НАШ /kernel загружен с {dv} (diag-ядро #175)"); klo=True; break
    else:
        log(f"  {dv}: не вышло (…{rr[-90:]!r})")
if not klo:
    log("⚠ явного подтверждения загрузки /kernel нет — всё равно пробую boot (см. лог)")

send("set boot_verbose=1",1.0)
send("set kern.panic_reboot_wait_time=-1",1.0)  # при панике — застыть (LED покажет веху), не ребутать
log("lsmod (что загружено перед boot):")
send("lsmod",2.0)

print("""
╔══════════════════════════════════════════════════════════════════════╗
║  >>> СЕЙЧАС ЗАГРУЖУ НАШЕ ЯДРО. СМОТРИ НА СВЕТОДИОДЫ ПЛАТЫ <<<          ║
║   • PE14 ЗЕЛЁНЫЙ — щёлкает на КАЖДОЙ вехе локора/раннего init.        ║
║   • PD24 КРАСНЫЙ  — чётность номера вехи (вкл на нечётных).           ║
║   Докуда доходит (по числу зелёных щелчков / финальному состоянию):  ║
║     0=locore 1=pmap_dmap 2=pmap 3=ДО bus_probe 4=ПОСЛЕ bus_probe     ║
║     5=cninit 6=TSEXIT 7=VM 8=CPU 9=DEVFS 10=ROOT 11=pre-mountroot    ║
║   Замер «3 без 4» = хенг ВНУТРИ bus_probe (CCU/clock) — целевая гипотеза.║
╚══════════════════════════════════════════════════════════════════════╝
""",flush=True)
log(">>> BOOT diag-ядра <<<")
L.wr(fd,b"boot\r")

# ловим serial до EBS (гаджет умрёт → ttyACM исчезнет)
t=time.time(); last=time.time()
while os.path.exists(L.TTY) and time.time()-t<150:
    b,dead=L.rd(fd,0.3)
    if b: save(b); last=time.time()
    if dead: break
    if time.time()-last>30: break
try: os.close(fd)
except: pass
alive = os.path.exists(L.TTY)
log("===== serial замолчал =====")
log(f"ttyACM всё ещё есть: {alive}  (нет → прошли ExitBootServices, ядро взяло HW; есть → застряли в loader/раннем)")
print("""
────────────────────────────────────────────────────────────────────────
СКАЖИ, ЧТО ВИДЕЛ НА LED (сколько раз щёлкнул зелёный / горит ли красный /
на чём замерло). Это и есть ответ «докуда доходит наше ядро».
Полный serial-лог: /tmp/boot-diag-now.log
────────────────────────────────────────────────────────────────────────
""",flush=True)
logf.close()
