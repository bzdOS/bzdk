#!/usr/bin/env python3
# catch-or-diag.py — КОНСОЛИДИРОВАННЫЙ инструмент на ОДИН физический ресет.
# НИКОГДА не шлёт reboot/reset (софт-reset на этой плате не поддерживается и вешает
# U-Boot на «Please RESET»). termios-open (nonblock, CLOCAL) — не виснет на carrier.
#
# На загрузке после ресета:
#  1) флудит Enter+Ctrl-C, пытается поймать СТАБИЛЬНЫЙ U-Boot '=>' (echo-проверка).
#     Поймал → «mw.l 0x01c20cb8 0 ; setenv bootdelay -1 ; saveenv» ОДНОЙ строкой →
#     verify printenv. Успех = U-Boot навсегда ждёт → разблокировано всё.
#  2) U-Boot проскочил → ловит loader countdown → грузит НАШ diag c disk0p2 → boot →
#     ты читаешь LED (зелёный PE14 = вехи). Serial до EBS.
import os,sys,time,termios,select,re
TTY="/dev/ttyACM0"
CSI=re.compile(rb'\x1b\[[0-9;?]*[a-zA-Z]')
LOG="/tmp/catch-or-diag.log"; logf=open(LOG,"wb")
BOOTKW=("MMC:","Booting","Card did","Loading /boot","Trying:","currdev","BOOTAA64","ESP:","EFI console","no card")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return CSI.sub(b'',b).decode('latin1','replace')

def opentty():
    fd=os.open(TTY, os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
    a=termios.tcgetattr(fd)
    a[0]&=~(termios.IGNBRK|termios.BRKINT|termios.PARMRK|termios.ISTRIP|termios.INLCR|termios.IGNCR|termios.ICRNL|termios.IXON)
    a[1]&=~termios.OPOST
    a[3]&=~(termios.ECHO|termios.ECHONL|termios.ICANON|termios.ISIG|termios.IEXTEN)
    a[2]&=~(termios.CSIZE|termios.PARENB); a[2]|=termios.CS8|termios.CLOCAL|termios.CREAD
    a[4]=termios.B115200; a[5]=termios.B115200
    termios.tcsetattr(fd,termios.TCSANOW,a)
    return fd

def rd(fd,t):
    b=b""; e=time.time()+t
    while time.time()<e:
        r,_,_=select.select([fd],[],[],0.05)
        if r:
            try:
                d=os.read(fd,65536)
                if d: b+=d
            except OSError: return b,True
    return b,False

def wait_open(dl):
    while time.time()<dl:
        if not os.path.exists(TTY): time.sleep(0.008); continue
        try: return opentty()
        except OSError: time.sleep(0.05)
    return None

def wait_fresh(dl):
    """Ждём СВЕЖУЮ загрузку: если ttyACM сейчас есть (труп зависшей платы) —
    ждём его ИСЧЕЗНОВЕНИЯ (твой ресет = USB disconnect), затем ПОЯВЛЕНИЯ
    (свежий U-Boot). Так флудим по реальному ресету, а не по zombie-порту."""
    if os.path.exists(TTY):
        log("вижу текущий ttyACM (возможно труп). Жду твой ресет → он должен ИСЧЕЗНУТЬ...")
        while os.path.exists(TTY) and time.time()<dl: time.sleep(0.05)
        if time.time()>=dl: return None
        log("✓ ttyACM исчез (ресет пошёл). Жду свежий U-Boot...")
    return wait_open(dl)

def send(fd,c,w=2.5):
    rd(fd,0.08); os.write(fd,(c+"\r").encode()); time.sleep(0.2)
    acc=b""
    for _ in range(4):
        x,dead=rd(fd,w)
        if x: acc+=x
        if dead: break
    save(acc); return clean(acc)

def stable_uboot(fd):
    good=0
    for _ in range(4):
        rd(fd,0.08); os.write(fd,b"\r"); time.sleep(0.35)
        r=clean(rd(fd,0.7)[0]); save(r)
        if any(k in r for k in BOOTKW): return False
        if "=>" in r: good+=1
        if good>=2: return True
    return good>=2

def do_save_bootdelay(fd):
    log("проверяю стабильность '=>' (echo-тест)...")
    if not stable_uboot(fd):
        log("  '=>' НЕстабилен (автобут идёт) — U-Boot упущен, ухожу в loader-путь")
        return False
    log("  ✓ стабильно. Пишу: mw.l 0x01c20cb8 0 ; setenv bootdelay -1 ; saveenv")
    rd(fd,0.3)
    os.write(fd,b"mw.l 0x01c20cb8 0 ; setenv bootdelay -1 ; saveenv\r")
    acc=b""; t0=time.time()
    while time.time()-t0<10:
        b,dead=rd(fd,0.3)
        if b: acc+=b; save(b)
        if dead: break
    time.sleep(0.4)
    os.write(fd,b"printenv bootdelay\r"); time.sleep(0.6)
    v=clean(rd(fd,2.0)[0]); save(v)
    if "bootdelay=-1" in v:
        sc=clean(acc)
        log("✅✅ bootdelay=-1 ПОДТВЕРЖДЁН. U-Boot теперь ВСЕГДА ждёт на '=>'.")
        log(f"   saveenv персист: {'OK' if any(x in sc for x in ('Writing','done','OK','Valid','Saving','Erasing')) else 'проверю на след. загрузке'}")
        log("   🎉 РАЗБЛОКИРОВАНО: дальше watchdog-авто-цикл + breadcrumb, физика не нужна.")
        return True
    log(f"  printenv не показал bootdelay=-1: v=…{v[-90:]!r}")
    return False

def boot_diag(fd):
    log("U-Boot упущен → loader-путь: гружу наш diag с disk0p2, ты смотри LED.")
    # добить loader countdown любым символом, дойти до OK
    os.write(fd,b" "); time.sleep(0.6); save(rd(fd,1.5)[0])
    send(fd,"echo OKPRB")
    klo=False
    for dv in ["disk0p2:","mmcsd1p2:","disk1p2:"]:
        send(fd,"unload",1.5); send(fd,'set currdev="%s"'%dv,1.2)
        rr=send(fd,"load /kernel",14.0)
        if re.search(r'text=|data=',rr) and all(x not in rr.lower() for x in ("error","cannot","not found","no such","can't")):
            log(f"✅ diag /kernel загружен с {dv}"); klo=True; break
    if not klo: log("⚠ load /kernel не подтвердился — всё равно boot")
    send(fd,"set boot_verbose=1",1.0); send(fd,"set kern.panic_reboot_wait_time=-1",1.0)
    print("""
╔════════════════════════════════════════════════════════════════════╗
║  >>> СМОТРИ НА ПЛАТУ: сейчас стартует НАШЕ ядро <<<                  ║
║   🟢 PE14 зелёный — СЧИТАЙ мигания (номер вехи).                    ║
║   🔴 PD24 красный — конечное состояние (чётность вехи).             ║
╚════════════════════════════════════════════════════════════════════╝""",flush=True)
    log(">>> BOOT diag <<<"); os.write(fd,b"boot\r")
    t=time.time(); last=time.time()
    while os.path.exists(TTY) and time.time()-t<120:
        b,dead=rd(fd,0.3)
        if b: save(b); last=time.time()
        if dead or time.time()-last>25: break
    log("serial умолк (EBS). Скажи: сколько мигнул зелёный и горит ли красный.")

log("Жду ТВОЙ ресет (disconnect→свежий U-Boot). Ловлю U-Boot; упущу — гружу diag для LED. reboot НЕ шлю.")
DL=time.time()+1200
attempt=0
while time.time()<DL:
    fd=wait_fresh(DL)
    if fd is None: log("⛔ за отведённое время свежей загрузки не было"); sys.exit(2)
    attempt+=1
    log(f"свежий ttyACM (попытка {attempt}) — флужу Enter+Ctrl-C, ищу U-Boot/loader...")
    buf=b""; t0=time.time(); kind=None
    while time.time()-t0<20:
        try: os.write(fd,b"\r\x03")
        except OSError: break
        b,dead=rd(fd,0.03)
        if b: buf+=b; save(b)
        cb=clean(buf)
        if "=>" in cb[-50:]: kind="uboot"; break
        if any(k in cb for k in ("Hit [Enter]","Type '?'","seconds...","Loading /boot")): kind="loader"; break
        if dead: break
    if kind=="uboot":
        if do_save_bootdelay(fd):
            log("Плата на '=>' (bootdelay=-1). ГОТОВО — дальше без тебя."); os.close(fd); sys.exit(0)
        else:
            boot_diag(fd); os.close(fd); sys.exit(0)   # упустили стабильный промпт → LED-путь, дальше стоп
    elif kind=="loader":
        boot_diag(fd); os.close(fd); sys.exit(0)
    else:
        log(f"попытка {attempt}: за 20с ни U-Boot, ни loader (пусто?). Жду следующий ресет...")
        try: os.close(fd)
        except: pass
logf.close()
