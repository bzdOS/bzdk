#!/usr/bin/env python3
# catch-uboot-v3.py — чинит провал v1/v2: ловили '=>' во время отсчёта автобута
# (гаджет-буфер задерживает байты → отсчёт истекает → U-Boot бутает, команда
# сталкивается с boot). ФИКС: (1) после '=>' ПРОВЕРЯЕМ стабильность промпта
# (echo-тест, нет boot-мусора); (2) setenv+saveenv ОДНОЙ строкой через ';';
# (3) только '\r'. Если промпт нестабилен (плата бутает) — не пишем, ребут и ретрай.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/uboot-v3.log"; logf=open(LOG,"wb")
MAX_TRIES=30
BOOTKW=("MMC","Booting","Card did","Loading","Trying","currdev","BOOTAA64","ESP:","EFI console")
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

def drain(fd,secs=0.8):
    t0=time.time(); acc=b""
    while time.time()-t0<secs:
        b,dead=L.rd(fd,0.15)
        if b: acc+=b; save(b)
        if dead: break
    return clean(acc)

def stable_prompt(fd):
    # echo-тест: шлём \r, ждём чистый '=>' без boot-мусора, дважды подряд
    good=0
    for _ in range(4):
        L.rd(fd,0.1); os.write(fd,b"\r"); time.sleep(0.35)
        r=clean(L.rd(fd,0.7)[0]); save(r)
        if any(k in r for k in BOOTKW): return False       # плата бутает
        if "=>" in r: good+=1
        if good>=2: return True
    return good>=2

def secure(fd):
    if not stable_prompt(fd):
        log("  промпт НЕСТАБИЛЕН (плата уходит в boot) — не пишу, ретрай")
        return False
    log("  промпт стабилен ✓ — пишу setenv bootdelay -1 ; saveenv (одной строкой)")
    drain(fd,0.4)
    os.write(fd,b"setenv bootdelay -1 ; saveenv\r")
    acc=b""; t0=time.time()
    while time.time()-t0<9:
        b,dead=L.rd(fd,0.3)
        if b: acc+=b; save(b)
        if dead: break
    time.sleep(0.4)
    os.write(fd,b"printenv bootdelay\r"); time.sleep(0.6)
    v=clean(L.rd(fd,2.0)[0]); save(v)
    if "bootdelay=-1" in v:
        log("✅✅ ПОДТВЕРЖДЕНО printenv: bootdelay=-1. U-Boot ВСЕГДА ждёт. РЕСЕТЫ КОНЧИЛИСЬ.")
        sc=clean(acc)
        log(f"   saveenv персист: {'OK' if any(x in sc for x in ('Writing','done','OK','Valid','Saving','Erasing')) else 'проверю позже'}")
        return True
    log(f"  printenv не показал bootdelay=-1: …{clean(acc)[-100:]!r} / v=…{v[-80:]!r}")
    return False

def reboot_loader(fd):
    try:
        os.write(fd,b"\r"); time.sleep(0.15); os.write(fd,b"reboot\r")
        time.sleep(0.4); save(L.rd(fd,1.0)[0])
    except OSError: pass
    try: os.close(fd)
    except: pass
    t0=time.time()
    while os.path.exists(L.TTY) and time.time()-t0<10: time.sleep(0.05)

log(f"v3: гонка за U-Boot, до {MAX_TRIES} ребутов. Стабильность промпта проверяю перед записью.")
DL=time.time()+700
tries=0
while tries<MAX_TRIES and time.time()<DL:
    fd=wait_open(DL)
    if fd is None: log("⛔ ttyACM не появился"); break
    buf=b""; t0=time.time(); kind=None
    while time.time()-t0<15 and time.time()<DL:
        try: os.write(fd,b"\r\x03")
        except OSError: break
        b,dead=L.rd(fd,0.03)
        if b: buf+=b; save(b)
        cb=clean(buf)
        if "=>" in cb[-50:]: kind="uboot"; break
        if any(k in cb for k in ("Hit [Enter]","Type '?'","unknown command","seconds...")): kind="loader"; break
        if dead: break
    if kind=="uboot":
        log("'=>' мелькнул — проверяю и фиксирую...")
        if secure(fd):
            log("Плата на '=>' (bootdelay=-1). Готово."); os.close(fd); sys.exit(0)
        # не вышло — плата, вероятно, бутает; уйдёт в loader → следующий виток ребутнёт
        try: os.close(fd)
        except: pass
        time.sleep(0.2)
    elif kind=="loader":
        tries+=1
        log(f"[{tries}/{MAX_TRIES}] loader (U-Boot проскочил) → reboot, ретрай...")
        reboot_loader(fd)
    else:
        try: os.close(fd)
        except: pass
        time.sleep(0.1)
log(f"⛔ за {tries} попыток bootdelay=-1 не зафиксирован. Перехожу на план Б (пересборка ядра, без ресетов).")
logf.close(); sys.exit(2)
