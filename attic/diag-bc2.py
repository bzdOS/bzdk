#!/usr/bin/env python3
# diag-bc2.py — чистый автономный прогон diag + чтение breadcrumb.
# Фиксы над v1: (1) SETTLE — жду тишины и стабильного '=>' перед каждой командой
# (после WDT-ребута U-Boot сыплет boot-шум, md.l тонул в нём → words=[]);
# (2) sentinel 0xFEEDFACE @0x50000010 до boot → после reset проверяю, выжил ли он
# (отличить «DRAM стёрт SPL» от «diag не записал breadcrumb»); (3) termios-порт,
# EIO-терпимость, стабильный /dev/ttyCHIMP. reboot НЕ шлю (только WDT).
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/diag-bc2.log"; logf=open(LOG,"wb")
BC=0x50000000; SENT=0x50000010; SENTVAL=0xFEEDFACE
CFG="0x01c20cb4"; MODE="0x01c20cb8"; CTRL="0x01c20cb0"
MS={0:"locore _start",1:"post pmap_bootstrap_dmap",2:"post pmap_bootstrap",
    3:"ПЕРЕД bus_probe",4:"ПОСЛЕ bus_probe",5:"post cninit",6:"pre TSEXIT",
    7:"SI_SUB_VM",8:"SI_SUB_CPU",9:"SI_SUB_DEVFS",10:"SI_SUB_ROOT_CONF",
    11:"init_main pre vfs_mountroot"}
MAXRUN=6
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
        except OSError: time.sleep(0.05)
    return None

def settle(fd,mx=10):
    """Дренаж до тишины (boot-шум U-Boot закончился), затем стабильный '=>'."""
    t0=time.time(); last=time.time()
    while time.time()-t0<mx:
        b,dead=L.rd(fd,0.2)
        if b: save(b); last=time.time()
        elif time.time()-last>1.2: break
        if dead: return False
    for _ in range(6):
        try: L.rd(fd,0.1); os.write(fd,b"\r")
        except OSError: return False
        time.sleep(0.3); r=clean(L.rd(fd,0.7)[0]); save(r)
        if "=>" in r and not any(k in r for k in ("Card","MMC","Booting","Loading","voltage")):
            return True
    return False

def ucmd(fd,c,w=4):
    try: L.rd(fd,0.15); os.write(fd,(c+"\r").encode())
    except OSError: return "__EIO__"
    acc=b""; t0=time.time()
    while time.time()-t0<w:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-6:] and len(clean(acc))>len(c)+1: break
        if dead: break
    return clean(acc)

def md_words(fd,addr,n=6):
    r=ucmd(fd,f"md.l 0x{addr:x} {n}",3.5)
    words=[]
    for line in r.splitlines():
        m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s*)+)',line)
        if m: words+=[int(x,16) for x in m.group(1).split()]
    return words,r

def one_run(fd):
    ucmd(fd,f"mw.l {MODE} 0",1.2)                      # снять WDT
    ucmd(fd,f"mw.l 0x{SENT:x} 0x{SENTVAL:x}",1.2)      # sentinel до boot
    r=ucmd(fd,"printenv kernel_addr_r",1.5)
    m=re.search(r'kernel_addr_r=(0x[0-9a-fA-F]+)',r); KA=m.group(1) if m else "0x40080000"
    rr=ucmd(fd,f"fatload mmc 1:2 {KA} efi/boot/bootaa64.efi",4)
    if "bytes read" not in rr: log("  fatload не удался — повтор"); return "fatload"
    ucmd(fd,f"mw.l {CFG} 0x00000001",1); ucmd(fd,f"mw.l {MODE} 0x000000b1",1)
    ucmd(fd,f"mw.l {CTRL} 0x000014af",1)               # WDT взведён, t=0
    log("  WDT 16с взведён, bootefi → loader → diag (fast)")
    try: os.write(fd,f"bootefi {KA}\r".encode())
    except OSError: return "eio"
    buf=b""; t=time.time(); inter=False
    while time.time()-t<20:
        b,dead=L.rd(fd,0.15)
        if b: buf+=b; save(b)
        if "Hit [Enter]" in clean(buf):
            try: os.write(fd,b" ")
            except OSError: break
            inter=True; time.sleep(0.35)
            b2,_=L.rd(fd,1.2); save(b2); break
        if dead: break
    if not inter: log("  loader countdown не пойман"); return "nocountdown"
    def s(c,w=3):
        try: L.rd(fd,0.06); os.write(fd,(c+"\r").encode()); time.sleep(0.1); d=L.rd(fd,w)[0]; save(d); return clean(d)
        except OSError: return "__EIO__"
    s('set currdev="disk0p2:"',0.5); s("unload",0.6); s("load /kernel",4)
    try: os.write(fd,b"boot\r")
    except OSError: pass
    log("  >>> BOOT diag — ядро идёт, пишет breadcrumb, зависнет → WDT reset <<<")
    last=time.time()
    while os.path.exists(L.TTY) and time.time()-last<60:
        try: b,dead=L.rd(fd,0.3)
        except OSError: break
        if b: save(b); last=time.time()
        if dead: break
        if time.time()-last>18: break
    return "booted"

log("Автономный прогон diag+breadcrumb. Порт /dev/ttyCHIMP. reboot НЕ шлю (только WDT).")
DL=time.time()+600
fd=wait_open(DL)
if fd is None: log("⛔ порт не появился"); sys.exit(2)
if not settle(fd): log("⚠ '=>' не устаканился на старте, продолжаю осторожно")
result=None
for run in range(1,MAXRUN+1):
    log(f"═══════════ ПРОГОН {run}/{MAXRUN} ═══════════")
    st=one_run(fd)
    try: os.close(fd)
    except: pass
    if st in ("fatload","nocountdown","eio"):
        # плата в неопределённом состоянии; ждём/переоткрываем и пробуем снова
        fd=wait_open(DL)
        if fd is None: log("⛔ порт пропал"); break
        settle(fd); continue
    # st == booted: ядро зависло за EBS → ждём WDT-ребут → свежий U-Boot
    log("  serial умолк (EBS/или reset). Жду WDT-возврат в U-Boot...")
    fd=wait_open(DL)
    if fd is None: log("⛔ после boot порт не вернулся (WDT не сработал?)"); break
    if not settle(fd): log("  ⚠ промпт не устаканился после reset")
    ucmd(fd,f"mw.l {MODE} 0",1.2)   # снять WDT
    w,raw=md_words(fd,BC,6)
    sent,_=md_words(fd,SENT,1)
    log("=============== BREADCRUMB ===============")
    log(f"  raw md 0x50000000: {[hex(x) for x in w]}")
    log(f"  sentinel 0x50000010: {[hex(x) for x in sent]} (ждали 0x{SENTVAL:x})")
    dram_ok = bool(sent) and sent[0]==SENTVAL
    if w and w[0]==0xB2D0C0DE:
        ms=w[1]
        log(f"✅✅ РЕЗУЛЬТАТ: наше ядро доходит до вехи {ms} → «{MS.get(ms,'?')}» (счётчик={w[2] if len(w)>2 else '?'})")
        if ms==3: log("   → хенг ВНУТРИ bus_probe (CCU/clock). Гипотеза подтверждена.")
        elif ms==11: log("   → ВСЁ device-init прошло, встало на mountroot — устройства ПОДНЯЛИСЬ! 🎉")
        elif ms>=4: log(f"   → bus_probe пройден, встало на {MS.get(ms)}.")
        result=ms; break
    else:
        if dram_ok:
            log("  DRAM ПЕРЕЖИЛ reset (sentinel цел), но magic нет → diag НЕ дописал breadcrumb")
            log("  (возможно WDT ребутнул до записи locore, или адрес breadcrumb иной). Повтор.")
        else:
            log("  sentinel НЕ выжил → DRAM стирается при warm-reset → breadcrumb этим путём не прочесть.")
            log("  Нужен план: LED-сигнал или breadcrumb в сохраняемой памяти. Стоп ретраев.")
            break
log(f"полный лог: {LOG}")
try: os.close(fd)
except: pass
logf.close()
