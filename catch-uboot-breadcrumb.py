#!/usr/bin/env python3
# catch-uboot-breadcrumb.py — ДЕФИНИТИВНЫЙ ловец.
# Ждёт ttyACM (авто-цикл платы ИЛИ ручной RST). Агрессивно ловит U-Boot флудом
# Enter(\r)+Ctrl-C (фиксер слал только \x03 → был слеп к idle-промпту). При поимке:
#   1) снимает watchdog,
#   2) ЧИТАЕТ breadcrumb 0x50000000 (веха от только что зависшего diag-прогона —
#      выживает при ТЁПЛОМ ресете/авто-цикле; гибнет только при выдёргивании питания),
#   3) ставит bootdelay=-1 + saveenv → U-Boot теперь ВСЕГДА ждёт (передёргивания кончились).
# Если проскочил U-Boot и попал в loader — до 3× пробует reboot чтобы поймать U-Boot;
# иначе грузит diag с disk0p2 для LED-наблюдения (смотри на плату вживую).
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

LOG="/tmp/catch-uboot.log"; logf=open(LOG,"wb")
BC=0x50000000
WDT_CFG="0x01c20cb4"; WDT_MODE="0x01c20cb8"; WDT_CTRL="0x01c20cb0"
MS={0:"locore _start",1:"post pmap_bootstrap_dmap",2:"post pmap_bootstrap",
    3:"ПЕРЕД bus_probe",4:"ПОСЛЕ bus_probe",5:"post cninit",6:"pre TSEXIT",
    7:"SI_SUB_VM",8:"SI_SUB_CPU",9:"SI_SUB_DEVFS",10:"SI_SUB_ROOT_CONF",
    11:"init_main pre vfs_mountroot"}

def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def wait_open(deadline):
    while time.time()<deadline:
        if not os.path.exists(L.TTY): time.sleep(0.01); continue
        try: return L.open_tty(True)
        except Exception: time.sleep(0.03)
    return None

def cmd(fd,c,w=2.0):
    L.rd(fd,0.1); os.write(fd,(c+"\r\n").encode()); time.sleep(0.3)
    b,_=L.rd(fd,w); save(b); return clean(b)

def read_breadcrumb(fd):
    r=cmd(fd,f"md.l 0x{BC:x} 4",2.0)
    words=[]
    for line in r.splitlines():
        m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s+){1,4})',line)
        if m: words+=[int(x,16) for x in m.group(1).split()]
    log("=============== BREADCRUMB 0x50000000 ===============")
    if len(words)<3 or words[0]!=0xB2D0C0DE:
        log(f"⛔ magic НЕ найдена (words={[hex(w) for w in words[:4]]})")
        log("   → DRAM не сохранился (был выдернут power?) ИЛИ ядро не дошло даже до locore.")
        return None
    ms=words[1]
    log(f"✅ magic OK.  ПОСЛЕДНЯЯ ВЕХА = {ms} → «{MS.get(ms,'?')}»   (счётчик вызовов={words[2]})")
    if ms==3:   log("👉 ГИПОТЕЗА ПОДТВЕРЖДЕНА: хенг ВНУТРИ bus_probe() — CCU/clock/pinmux.")
    elif ms>=4: log(f"👉 bus_probe ПРОЙДЕН, дошли до {ms}. Устройства частично поднимаются — хенг дальше.")
    else:       log(f"👉 хенг ОЧЕНЬ рано (до bus_probe): «{MS.get(ms)}».")
    return ms

def catch(deadline):
    loader_tries=0
    while time.time()<deadline:
        fd=wait_open(deadline)
        if fd is None: return (None,None)
        log("ttyACM появился — молочу Enter+Ctrl-C, ищу '=>' / loader...")
        buf=b""; t0=time.time()
        while time.time()-t0<12 and time.time()<deadline:
            try: os.write(fd,b"\r"); os.write(fd,b"\x03")
            except OSError: break
            b,dead=L.rd(fd,0.04)
            if b: buf+=b; save(b)
            cb=clean(buf)
            if "=>" in cb[-80:]:
                return ("uboot",fd)
            if any(k in cb for k in ("Hit [Enter]","seconds...","Type '?'","Loading /boot","Booting FreeBSD","OK\n")):
                log("это FreeBSD loader countdown/OK")
                try: os.write(fd,b" ")
                except OSError: break
                time.sleep(0.5); b2,_=L.rd(fd,1.5)
                if b2: save(b2)
                if loader_tries<3:
                    loader_tries+=1
                    log(f"пробую reboot ({loader_tries}/3) чтобы всё же поймать U-Boot...")
                    cmd(fd,"reboot",2.0)
                    try: os.close(fd)
                    except: pass
                    buf=b""; break   # выходим во внешний цикл ждать нового ttyACM
                else:
                    return ("loader",fd)
            if dead: break
        else:
            try: os.close(fd)
            except: pass
            time.sleep(0.05); continue
        # попали сюда из break (reboot) — продолжаем внешний цикл
        continue
    return (None,None)

def boot_diag_from_loader(fd):
    log("Гружу diag с disk0p2 через loader (breadcrumb/bootdelay недоступны — только LED).")
    def send(c,w=2.5):
        L.rd(fd,0.08); os.write(fd,(c+"\r").encode()); time.sleep(0.25)
        acc=b""
        for _ in range(4):
            x,dead=L.rd(fd,w)
            if x: acc+=x
            if dead: break
        save(acc); return clean(acc)
    send("echo OKPROBE")
    klo=False
    for dv in ["disk0p2:","mmcsd1p2:","disk1p2:"]:
        send("unload",1.5); send('set currdev="%s"'%dv,1.2)
        rr=send("load /kernel",14.0)
        if re.search(r'text=|data=|entry=',rr) and all(x not in rr.lower() for x in ("error","cannot","fail","not found","no such")):
            log(f"✅ diag /kernel загружен с {dv}"); klo=True; break
    if not klo: log("⚠ load /kernel не подтвердился — пробую boot всё равно")
    send("set boot_verbose=1",1.0); send("set kern.panic_reboot_wait_time=-1",1.0)
    print("""
╔════════════════════════════════════════════════════════════════════╗
║  >>> СМОТРИ НА LED ПЛАТЫ ПРЯМО СЕЙЧАС <<<                            ║
║   PE14 ЗЕЛЁНЫЙ щёлкает на КАЖДОЙ вехе — СЧИТАЙ ЩЕЛЧКИ.               ║
║   Кол-во щелчков = номер вехи: 3=ДО bus_probe, 4=ПОСЛЕ, …           ║
║   PD24 КРАСНЫЙ = чётность (горит на нечётной вехе).                 ║
╚════════════════════════════════════════════════════════════════════╝""",flush=True)
    log(">>> BOOT diag <<<")
    os.write(fd,b"boot\r")
    t=time.time(); last=time.time()
    while os.path.exists(L.TTY) and time.time()-t<120:
        b,dead=L.rd(fd,0.3)
        if b: save(b); last=time.time()
        if dead or time.time()-last>25: break
    log("serial умолк (EBS). Скажи, сколько раз щёлкнул зелёный LED.")

# ─────────── main ───────────
log("Жду ttyACM (АВТО-цикл платы ИЛИ твой RST). Дедлайн 25 мин.")
log("⚠ Если ресетишь вручную — жми КНОПКУ RST, НЕ выдёргивай питание:")
log("   тёплый ресет сохранит DRAM → прочту breadcrumb (точную веху) от текущего зависона.")
DEADLINE=time.time()+1500
kind,fd=catch(DEADLINE)

if kind is None:
    log("⛔ за 25 мин плата не появилась / не поймана. Перезапусти после ресета."); sys.exit(2)

if kind=="uboot":
    log("✅✅ ПОЙМАЛ U-Boot '=>' !")
    cmd(fd,f"mw.l {WDT_MODE} 0")           # снять залипший watchdog
    ms=read_breadcrumb(fd)                 # ← ГЛАВНОЕ: веха от зависшего прогона
    r=cmd(fd,"setenv bootdelay -1",1.0)
    r=cmd(fd,"saveenv",3.0)
    if any(x in r for x in ("OK","done","Written","Valid","Saving","Erasing","Writing")):
        log("✅✅ bootdelay=-1 СОХРАНЁН. U-Boot теперь ВСЕГДА замирает в prompt.")
        log("    ПЕРЕДЁРГИВАНИЯ КОНЧИЛИСЬ: дальше всё софтом (reset/bootefi/md).")
    else:
        log(f"⚠ saveenv неоднозначно (…{r[-120:]!r}); на сессию bootdelay=-1 применён.")
    cmd(fd,"printenv bootdelay",1.0)
    log("Плата на '=>'. Следующий шаг — контролируемый diag-boot с watchdog (zero-touch).")
    try: os.close(fd)
    except: pass
else:  # loader
    log("⚠ U-Boot проскочить не удалось (гонка), но поймал FreeBSD loader.")
    boot_diag_from_loader(fd)
    try: os.close(fd)
    except: pass

log(f"полный лог: {LOG}")
logf.close()
