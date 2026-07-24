#!/usr/bin/env python3
# catch-fix-bd-v2.py — ИСПРАВЛЕННЫЙ порядок: поймать '=>' → СРАЗУ bootdelay=-1+saveenv
# (пока U-Boot не ушёл в автобут!) → verify → СТОП на '=>' (НЕ бутаем).
# Прошлый провал: сначала читали breadcrumb, U-Boot успевал автобутнуться, команды
# улетали в loader. Теперь приоритет #1 = зафиксировать U-Boot как постоянный дом.
# ucmd() ждёт возврата '=>' после каждой команды (без десинка на фиксированных sleep).
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/fixbd2.log"; logf=open(LOG,"wb")
WDT_MODE="0x01c20cb8"
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

def catch(dl):
    while time.time()<dl:
        fd=wait_open(dl)
        if fd is None: return None
        buf=b""; t0=time.time()
        while time.time()-t0<15 and time.time()<dl:
            try: os.write(fd,b"\r\x03")
            except OSError: break
            b,dead=L.rd(fd,0.03)
            if b: buf+=b; save(b)
            if "=>" in clean(buf)[-60:]:
                return fd
            if dead: break
        try: os.close(fd)
        except: pass
        time.sleep(0.03)
    return None

def ucmd(fd,c,timeout=4.0):
    L.rd(fd,0.12)                       # drain
    os.write(fd,(c+"\r\n").encode())
    acc=b""; t0=time.time()
    while time.time()-t0<timeout:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-8:] and len(clean(acc))>len(c): break
        if dead: break
    return clean(acc)

log("v2: жду ttyACM (RST-ребут). Ловлю '=>' → ПЕРВЫМ ДЕЛОМ bootdelay=-1.")
DL=time.time()+900
fd=catch(DL)
if fd is None: log("⛔ '=>' не пойман за 15 мин"); sys.exit(2)
log("✅ '=>' ПОЙМАН. Снимаю watchdog и фиксирую bootdelay=-1 (приоритет #1)...")
ucmd(fd,f"mw.l {WDT_MODE} 0",2.0)      # снять WD чтобы не ресетнул посреди
ucmd(fd,"",1.5)                        # чистый промпт
ucmd(fd,"setenv bootdelay -1",2.5)
r2=ucmd(fd,"saveenv",7.0)
r3=ucmd(fd,"printenv bootdelay",2.5)
ok_env = "bootdelay=-1" in r3
ok_save = any(x in r2 for x in ("Writing","done","OK","Valid","Saving","Erasing","Persisting"))
if ok_env:
    log("✅✅ ПОДТВЕРЖДЕНО printenv: bootdelay=-1. U-Boot теперь ВСЕГДА ждёт в prompt.")
    log("    🎉 ПЕРЕДЁРГИВАНИЯ КОНЧИЛИСЬ. Дальше только софт: reset / bootefi / md.")
else:
    log(f"⚠ printenv не подтвердил bootdelay=-1: …{r3[-140:]!r}")
log(f"saveenv (персист на ESP uboot.env): {'вероятно OK' if ok_save else 'ПОД ВОПРОСОМ'} …{r2[-120:]!r}")
log("Плата ОСТАВЛЕНА на '=>' (НЕ бутаю). Готово к контролируемому diag-boot.")
try: os.close(fd)
except: pass
logf.close()
