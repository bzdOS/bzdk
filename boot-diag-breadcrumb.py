#!/usr/bin/env python3
# boot-diag-breadcrumb.py — ПОСЛЕ того как bootdelay=-1 зафиксирован (U-Boot всегда
# ждёт на '=>'). Контролируемый прогон: из U-Boot взводим watchdog (16с) → bootefi →
# в loader грузим НАШ /kernel c disk0p2 → boot → ядро идёт, пишет breadcrumb, зависает
# → watchdog делает ТЁПЛЫЙ reset (DRAM цел!) → U-Boot ждёт (bootdelay=-1) → читаем
# md 0x50000000 → ТОЧНАЯ веха. Авто-ретрай пока magic не появится (всё софтом, 0 рук).
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/diag-breadcrumb.log"; logf=open(LOG,"wb")
BC=0x50000000
WDT_CFG="0x01c20cb4"; WDT_MODE="0x01c20cb8"; WDT_CTRL="0x01c20cb0"
MS={0:"locore _start",1:"post pmap_bootstrap_dmap",2:"post pmap_bootstrap",
    3:"ПЕРЕД bus_probe",4:"ПОСЛЕ bus_probe",5:"post cninit",6:"pre TSEXIT",
    7:"SI_SUB_VM",8:"SI_SUB_CPU",9:"SI_SUB_DEVFS",10:"SI_SUB_ROOT_CONF",
    11:"init_main pre vfs_mountroot"}
MAXRUN=5
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def wait_open(dl):
    while time.time()<dl:
        if not os.path.exists(L.TTY): time.sleep(0.01); continue
        try: return L.open_tty(True)
        except Exception: time.sleep(0.03)
    return None

def catch_uboot(dl):
    fd=wait_open(dl)
    if fd is None: return None
    t0=time.time()
    while time.time()-t0<20 and time.time()<dl:
        try: os.write(fd,b"\r")
        except OSError: break
        b,_=L.rd(fd,0.25);
        if b: save(b)
        if "=>" in clean(b)[-8:] or (b and b"=>" in b): return fd
    return fd  # всё равно вернём; bootdelay=-1 → должен быть =>

def ucmd(fd,c,timeout=4.0):
    L.rd(fd,0.1); os.write(fd,(c+"\r\n").encode()); time.sleep(0.2)
    acc=b""; t0=time.time()
    while time.time()-t0<timeout:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-6:] and len(clean(acc))>len(c): break
        if dead: break
    return clean(acc)

def read_breadcrumb(fd):
    r=ucmd(fd,f"md.l 0x{BC:x} 4",3.0)
    words=[]
    for line in r.splitlines():
        m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s+){1,4})',line)
        if m: words+=[int(x,16) for x in m.group(1).split()]
    if len(words)<3 or words[0]!=0xB2D0C0DE:
        log(f"breadcrumb: magic нет (words={[hex(w) for w in words[:4]]})")
        return None
    ms=words[1]
    log(f"✅ BREADCRUMB: веха={ms} → «{MS.get(ms,'?')}»  счётчик={words[2]}")
    return ms

def one_run(fd):
    ucmd(fd,f"mw.l {WDT_MODE} 0",2.0)      # снять WD
    r=ucmd(fd,"printenv kernel_addr_r",1.5)
    m=re.search(r'kernel_addr_r=(0x[0-9a-fA-F]+)',r); KA=m.group(1) if m else "0x40080000"
    loaded=False
    for dp in ["mmc 1:2","mmc 2:2","mmc 1:1"]:
        rr=ucmd(fd,f"fatload {dp} {KA} efi/boot/bootaa64.efi",4.0)
        if re.search(r'\d+\s+bytes read',rr) and not re.search(r'error|unable|bad|not found|failed',rr,re.I):
            log(f"loader: {dp}"); loaded=True; break
    if not loaded: log("⛔ bootaa64.efi не найден"); return None
    log("взвожу watchdog 16с")
    ucmd(fd,f"mw.l {WDT_CFG} 0x00000001",1.0)
    ucmd(fd,f"mw.l {WDT_MODE} 0x000000b1",1.0)
    ucmd(fd,f"mw.l {WDT_CTRL} 0x000014af",1.0)
    log(f">>> bootefi {KA} → loader → наш diag /kernel <<<  (смотри на плату: зелёный LED = вехи)")
    os.write(fd,f"bootefi {KA}\r\n".encode())
    # ловим loader countdown, быстро гоним boot нашего /kernel
    buf=b""; t=time.time(); inter=False
    while time.time()-t<40 and os.path.exists(L.TTY):
        b,dead=L.rd(fd,0.2)
        if b: buf+=b; save(b)
        if "Hit [Enter]" in clean(buf) and not inter:
            time.sleep(0.2); os.write(fd,b" "); inter=True; time.sleep(0.5); save(L.rd(fd,1.5)[0]); break
        if dead: break
    def send(c,w=3):
        try:
            L.rd(fd,0.1); os.write(fd,(c+"\r").encode()); time.sleep(0.12)
            d=L.rd(fd,w)[0]; save(d); return clean(d)
        except OSError:
            return "__EIO__"   # плата ребутнулась (WDT) — терпим
    if inter:
        # СТРИМЛАЙН ради 16с-бюджета WDT: disk0p2 напрямую, БЕЗ verbose/panic, boot СРАЗУ после load
        send('set currdev="disk0p2:"',0.5)
        send("unload",0.7)
        rr=send("load /kernel",4)
        if "text=" in rr or "data=" in rr: log("diag /kernel загружен (disk0p2)")
        log(">>> BOOT diag (fast) <<<")
        try: os.write(fd,b"boot\r")
        except OSError: pass
    else:
        log("⚠ loader countdown не пойман — bootcmd дефолт (оригинал?)")
    # захват serial до EBS
    t=time.time(); last=time.time()
    while os.path.exists(L.TTY) and time.time()-t<90:
        b,dead=L.rd(fd,0.3)
        if b: save(b); last=time.time()
        if dead or time.time()-last>20: break
    log("serial умолк (EBS). Жду watchdog-тёплый-reset → U-Boot...")
    try: os.close(fd)
    except: pass
    return "rebooting"

log("Жду U-Boot '=>' (bootdelay=-1 уже стоять должен)...")
DL=time.time()+900
fd=catch_uboot(DL)
if fd is None: log("⛔ нет ttyACM"); sys.exit(2)
ms=None
for run in range(1,MAXRUN+1):
    log(f"═══════════ ПРОГОН {run}/{MAXRUN} ═══════════")
    res=one_run(fd)
    if res is None: break
    # watchdog должен был ребутнуть → ждём U-Boot снова
    fd=catch_uboot(DL)
    if fd is None: log("⛔ после reset U-Boot не пойман"); break
    ucmd(fd,f"mw.l {WDT_MODE} 0",2.0)  # снять WD
    ms=read_breadcrumb(fd)
    if ms is not None:
        log("=================== РЕЗУЛЬТАТ ===================")
        log(f"👉 НАШЕ ЯДРО доходит до вехи {ms}: «{MS.get(ms,'?')}»")
        if ms==3: log("   → застряли ВНУТРИ bus_probe (CCU/clock). Гипотеза подтверждена.")
        elif ms==11: log("   → ПРОШЛО ВСЁ device-init, встало на mountroot (ждёт корень). Устройства ПОДНЯЛИСЬ! 🎉")
        elif ms>=4: log(f"   → bus_probe пройден, устройства инициализируются; встало на {MS.get(ms)}.")
        break
    else:
        log(f"прогон {run}: breadcrumb не прочитан (DRAM/тайминг), повторяю...")
log(f"полный лог: {LOG}")
try: os.close(fd)
except: pass
logf.close()
