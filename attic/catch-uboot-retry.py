#!/usr/bin/env python3
# catch-uboot-retry.py — ПОБЕДИТЕЛЬ ГОНКИ за U-Boot.
# U-Boot на автобуте — узкое окно (ловится вероятностно). Но из FreeBSD loader
# можно дать `reboot` → свежий цикл → новое окно U-Boot. Долбим Enter+Ctrl-C на
# КАЖДОМ ребуте, ловим '=>'. Поймали → СРАЗУ bootdelay=-1+saveenv (навсегда) →
# СТОП на '='. До MAX_TRIES ребутов. Плата на eMMC-U-Boot, питание не трогаем.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/uboot-retry.log"; logf=open(LOG,"wb")
WDT_MODE="0x01c20cb8"
MAX_TRIES=25
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def wait_open(dl):
    while time.time()<dl:
        if not os.path.exists(L.TTY): time.sleep(0.008); continue
        try: return L.open_tty(True)
        except Exception: time.sleep(0.02)
    return None

def reboot_from_loader(fd):
    try:
        os.write(fd,b"\r\n"); time.sleep(0.15); os.write(fd,b"reboot\r\n")
        time.sleep(0.4); save(L.rd(fd,1.0)[0])
    except OSError: pass
    try: os.close(fd)
    except: pass
    # ждём исчезновения ttyACM (плата ресетится)
    t0=time.time()
    while os.path.exists(L.TTY) and time.time()-t0<10: time.sleep(0.05)

def ucmd(fd,c,timeout=5.0):
    L.rd(fd,0.12); os.write(fd,(c+"\r\n").encode())
    acc=b""; t0=time.time()
    while time.time()-t0<timeout:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-8:] and len(clean(acc))>len(c)+2: break
        if dead: break
    return clean(acc)

def secure_uboot(fd):
    log("✅ '=>' ПОЙМАН. Снимаю WD, ставлю bootdelay=-1 (приоритет #1)...")
    ucmd(fd,f"mw.l {WDT_MODE} 0",2.0)
    ucmd(fd,"",1.2)
    ucmd(fd,"setenv bootdelay -1",2.5)
    r2=ucmd(fd,"saveenv",8.0)
    r3=ucmd(fd,"printenv bootdelay",2.5)
    if "bootdelay=-1" in r3:
        log("✅✅ ПОДТВЕРЖДЕНО: bootdelay=-1. U-Boot ВСЕГДА ждёт. ПЕРЕДЁРГИВАНИЯ КОНЧИЛИСЬ.")
    else:
        log(f"⚠ printenv не подтвердил: …{r3[-140:]!r}")
    save_ok=any(x in r2 for x in ("Writing","done","OK","Valid","Saving","Erasing"))
    log(f"saveenv персист: {'OK' if save_ok else 'ПОД ВОПРОСОМ'} …{r2[-110:]!r}")
    log("Плата на '=>' (НЕ бутаю). Готово к контролируемому diag-boot+breadcrumb.")

log(f"Гонка за U-Boot: до {MAX_TRIES} ребутов из loader. Долблю Enter+Ctrl-C на каждом окне.")
DL=time.time()+600
tries=0
while tries<MAX_TRIES and time.time()<DL:
    fd=wait_open(DL)
    if fd is None: log("⛔ ttyACM не появился"); break
    buf=b""; t0=time.time(); got=None
    while time.time()-t0<14 and time.time()<DL:
        try: os.write(fd,b"\r\x03")
        except OSError: break
        b,dead=L.rd(fd,0.03)
        if b: buf+=b; save(b)
        cb=clean(buf)
        if "=>" in cb[-60:]: got="uboot"; break
        if any(k in cb for k in ("Hit [Enter]","seconds...","Type '?'","unknown command","Booting FreeBSD")):
            got="loader"; break
        if dead: break
    if got=="uboot":
        secure_uboot(fd)
        try: os.close(fd)
        except: pass
        sys.exit(0)
    elif got=="loader":
        tries+=1
        log(f"[{tries}/{MAX_TRIES}] окно U-Boot проскочило → loader; reboot, пробую снова...")
        reboot_from_loader(fd)
    else:
        try: os.close(fd)
        except: pass
        time.sleep(0.1)
log(f"⛔ за {tries} попыток U-Boot не пойман. Гонка тугая — нужен план Б (см. чат).")
logf.close(); sys.exit(2)
