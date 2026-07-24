#!/usr/bin/env python3
# boot-watch.py — грузит FreeBSD с ЖИВОГО U-Boot (проверенной verbose-последовательностью
# из devstand), ловит вывод до ExitBootServices, и печатает ЧТО СМОТРЕТЬ НА МОНИТОРЕ.
# Смысл: ядро печатает баннер и всё дальнейшее УЖЕ ПОСЛЕ EBS → это НЕ приходит на
# ttyACM, а идёт в efifb (0xBE000000, 1920x1080, XRGB8888). Если HDMI оживёт — мы
# из «совсем слепые» становимся «виден весь лог ядра». Ровно это и проверяем.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/boot-watch.log"; logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

def catch(total):
    dl=time.time()+total
    while time.time()<dl:
        if not os.path.exists(L.TTY): time.sleep(0.15); continue
        try: fd=L.open_tty(True)
        except Exception: time.sleep(0.3); continue
        for _ in range(6):
            L.wr(fd,b"\r\n"); time.sleep(0.12); b,_=L.rd(fd,0.5)
            if b"=>" in L._CSI.sub(b'',b): return fd
        try: os.close(fd)
        except: pass
        time.sleep(0.3)
    return None

log("Ловлю живой U-Boot (25с). Если молчит — 1 передёрг (bootdelay=-1 → замрёт в prompt).")
fd=catch(25) or (log("нужен передёрг, жду до 5 мин...") or catch(300))
if fd is None: log("⛔ prompt не появился"); sys.exit(2)
log("✅ U-Boot prompt — запускаю verbose-загрузку FreeBSD")

def send(cmd,w=3):
    L.rd(fd,0.1); L.wr(fd,cmd.encode()+b"\r"); time.sleep(0.3)
    d=L.rd(fd,w)[0]; save(d); return L._CSI.sub(b'',d).decode('latin1','replace')

# 1) run bootcmd → перехватить countdown loader'а пробелом
L.wr(fd,b"run bootcmd\r\n")
buf=b""; t=time.time(); inter=False
while time.time()-t<45 and os.path.exists(L.TTY):
    b,dead=L.rd(fd,0.2)
    if b: buf+=b; save(b)
    if b"Hit [Enter]" in L._CSI.sub(b'',buf) and not inter:
        time.sleep(0.3); L.wr(fd,b" "); inter=True
        b,_=L.rd(fd,2); save(b); break
    if dead: break

# 2) в loader'е: currdev → load /kernel → verbose → НЕ ребутать по панике → boot
if inter:
    send("lsdev",2); loaded=False
    for dev in ["disk0p2:","mmcsd1p2:","disk1p2:","mmcsd0p2:"]:
        send(f'set currdev="{dev}"',1); send("unload",1)
        r=send("load /kernel",10)
        if all(x not in r.lower() for x in ("error","cannot","fail","not found","no such")):
            loaded=True; log(f"/kernel загружен с {dev}"); break
    if not loaded: log("⚠ /kernel не найден на ESP — грузится дефолтный из bootcmd")
    send('set kern.panic_reboot_wait_time=-1',1)   # застыть на панике, не ребутать
    send('set boot_verbose=1',1)                    # максимум сообщений → на экран
    log(">>> BOOT (ядро уходит за ExitBootServices — дальше только HDMI/efifb) <<<")
    send("boot",2)
else:
    log("⚠ не поймал loader countdown — ядро само стартует из bootcmd (не-verbose)")

# 3) захват ttyACM до EBS (он умрёт на 'EFI framebuffer information')
log("Захват до ExitBootServices (ttyACM умрёт ~на строке про framebuffer)...")
t=time.time(); last=time.time()
while os.path.exists(L.TTY) and time.time()-t<90:
    b,dead=L.rd(fd,0.3)
    if b: save(b); last=time.time()
    if dead or (time.time()-last>20): break
try: os.close(fd)
except: pass

log("================= ttyACM ЗАМОЛЧАЛ = ExitBootServices пройден =================")
log("С этого момента ядро пишет ТОЛЬКО в efifb (0xBE000000, 1920x1080). Подожди ~30с,")
log("пока ядро дойдёт до зависания/паники и картинка ЗАСТЫНЕТ, потом смотри на монитор.")
print("""
╔══════════════════════════════════════════════════════════════════╗
║  СМОТРИ НА МОНИТОР. Что на экране ПОСЛЕ лого U-Boot? Варианты:      ║
║   (A) белый текст на чёрном (лог ядра FreeBSD) — ПОБЕДА:            ║
║       прочитай/сфоткай ПОСЛЕДНИЕ 3-4 строки (там место зависа).    ║
║   (B) экран остался на лого U-Boot / не изменился.                 ║
║   (C) чёрный экран (подсветка есть, текста нет).                   ║
║   (D) 'No signal' / монитор погас.                                 ║
║   (E) цветной мусор/полосы.                                        ║
╚══════════════════════════════════════════════════════════════════╝
""",flush=True)
log(f"полный лог: {LOG}")
logf.close()
