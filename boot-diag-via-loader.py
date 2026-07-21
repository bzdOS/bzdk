#!/usr/bin/env python3
# boot-diag-via-loader.py — реальность: плата автозагружается в FreeBSD-loader
# (U-Boot bootdelay сброшен), а loader по умолчанию грузит disk0p3:/boot/kernel/kernel
# (ОРИГИНАЛ), не наш diag disk0p2:/kernel. Ловим отсчёт loader'а → OK → грузим НАШ
# /kernel с disk0p2 → verbose boot. Если попали в U-Boot (=>) — чиним bootdelay=-1
# и идём в loader. Плата флапает → бесконечный reconnect до дедлайна.
# LED — главный сигнал (виден на плате): PE14 green=каждая веха, PD24 red=чётность.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/boot-diag-loader.log"; logf=open(LOG,"wb")
BREADCRUMB=0x50000000
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

log("ЖДУ ПИТАНИЕ платы (ловлю loader-отсчёт или U-Boot; reconnect до 20 мин)...")
DEADLINE=time.time()+1200
fd=None; at_loader=False
while time.time()<DEADLINE and not at_loader:
    if not os.path.exists(L.TTY): time.sleep(0.1); continue
    try: fd=L.open_tty(True)
    except Exception: time.sleep(0.2); continue
    buf=b""; t0=time.time(); interrupted=False; uboot_fixed=False
    # читаем поток, флудим Ctrl-C (сбить U-Boot autoboot), ловим маркеры
    while time.time()-t0<45:
        try: L.wr(fd,b"\x03")
        except OSError: break
        b,dead=L.rd(fd,0.25)
        if b: buf+=b; save(b)
        if dead: break
        cb=clean(buf)
        # U-Boot prompt пойман — чиним bootdelay и идём в loader
        if "=>" in cb[-40:] and not uboot_fixed:
            log("поймал U-Boot => : чиню bootdelay=-1 + saveenv, затем boot")
            L.wr(fd,b"\r\nsetenv bootdelay -1\r\n"); time.sleep(0.4); save(L.rd(fd,1.0)[0])
            L.wr(fd,b"saveenv\r\n"); time.sleep(0.8); save(L.rd(fd,2.0)[0])
            L.wr(fd,b"boot\r\n"); uboot_fixed=True; buf=b""; t0=time.time(); continue
        # loader countdown — сбиваем любым символом
        if ("Hit [Enter]" in cb or "seconds..." in cb) and not interrupted:
            time.sleep(0.2); L.wr(fd,b" "); interrupted=True
            time.sleep(0.6); save(L.rd(fd,1.5)[0]); buf=clean(buf)[-1:].encode()
        # loader OK prompt
        if interrupted or re.search(r'\nOK ?$', clean(buf)) or "Type '?' for a list" in cb:
            # подтверждаем OK
            L.wr(fd,b"\r"); time.sleep(0.4); b2,_=L.rd(fd,1.0); save(b2)
            if "OK" in clean(buf+b2)[-20:] or interrupted:
                at_loader=True; break
    if at_loader: break
    try: os.close(fd)
    except: pass
    time.sleep(0.3)

if not at_loader or fd is None:
    log("⛔ не удалось поймать loader/U-Boot за 20 мин (плата не запитана/флапает)")
    sys.exit(2)

log("✅ FreeBSD loader OK-prompt")
def send(c,w=3):
    L.rd(fd,0.1); L.wr(fd,c.encode()+b"\r"); time.sleep(0.3)
    dd=L.rd(fd,w)[0]; save(dd); return clean(dd)

send("lsdev",2)
klo=False
for dv in ["disk0p2:","mmcsd1p2:","disk1p2:","mmcsd0p2:"]:
    send(f'set currdev="{dv}"',1); send("unload",1)
    rr=send("load /kernel",12)
    if re.search(r'text=|data=|/kernel ',rr) and all(x not in rr.lower() for x in ("error","cannot","fail","not found","no such","can't")):
        log(f"✅ НАШ /kernel загружен с {dv} (diag)"); klo=True; break
if not klo:
    log("⚠ /kernel с disk0p2 не грузится явно — см. лог; всё равно пробую boot")
send('set boot_verbose=1',1)
send('set kern.panic_reboot_wait_time=-1',1)   # застыть, не ребутать по панике
log(">>> BOOT ДИАГНОСТ-ЯДРА — СМОТРИ НА LED ПЛАТЫ (PE14 green=веха, PD24 red=чётность) <<<")
send("boot",2)

log("Захват ttyACM до ExitBootServices...")
t=time.time(); last=time.time()
while os.path.exists(L.TTY) and time.time()-t<120:
    b,dead=L.rd(fd,0.3)
    if b: save(b); last=time.time()
    if dead or time.time()-last>25: break
try: os.close(fd)
except: pass
log("===== ttyACM замолчал = EBS/останов =====")
print("""
╔════════════════════════════════════════════════════════════════════╗
║  СМОТРИ НА СВЕТОДИОДЫ ПЛАТЫ (главный сигнал, каналу не нужен USB):    ║
║   • PE14 зелёный щёлкает на каждой вехе; PD24 красный = чётность.    ║
║   • Докуда доходит: 0=locore 1=pmap_dmap 2=pmap 3=ДО bus_probe       ║
║     4=ПОСЛЕ bus_probe 5=cninit … 11=pre mountroot.                   ║
║   • Замер на «3 без 4» = хенг в bus_probe() (CCU/clock) — цель.      ║
╚════════════════════════════════════════════════════════════════════╝
Скажи, что видел на LED. Breadcrumb 0x50000000 прочитаю с U-Boot после (если DRAM переживёт).
""",flush=True)
log(f"полный лог: {LOG}")
logf.close()
