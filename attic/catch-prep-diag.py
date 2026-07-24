#!/usr/bin/env python3
# catch-prep-diag.py — РОБАСТНЫЙ ловец + prep + диагностика.
#   1) Ловит U-Boot, переживая устаревший ttyACM (ретраит через реконнекты).
#   2) PREP: setenv bootdelay -1 ; saveenv → плата ВСЕГДА ждёт в prompt после
#      reset. Дальше подключение без гонки с 2-сек окном; ребуты через `reset`.
#   3) bdinfo → адрес фреймбуфера (HDMI).
#   4) loady microkernel-diag.bin → go → diag вернётся → md крошек.
# После этого физические передёрги не нужны: reset + watchdog.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

MK="/opt/bzdos/microkernel"; LOG="/tmp/mk-prep.log"
logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

def robust_catch(total=1200):
    """Ловим '=>' через любые реконнекты/устаревший ttyACM. Возвращает открытый fd или None."""
    deadline=time.time()+total; attempt=0
    while time.time()<deadline:
        if not os.path.exists(L.TTY): time.sleep(0.1); continue
        attempt+=1
        try: fd=L.open_tty(True)
        except Exception: time.sleep(0.2); continue
        ok,buf=L.catch_uboot(fd,5); save(buf)
        if ok: log(f"поймал U-Boot (попытка {attempt})"); return fd
        try: os.close(fd)
        except: pass
        time.sleep(0.2)
    return None

def catch_prompt(timeout=120):
    t=time.time()
    while time.time()-t<timeout:
        if not os.path.exists(L.TTY): time.sleep(0.2); continue
        try: fd=L.open_tty(True)
        except Exception: time.sleep(0.3); continue
        L.wr(fd,b"\r\n"); time.sleep(0.3); b,_=L.rd(fd,1.0)
        if b"=>" in L._CSI.sub(b'',b): return fd
        try: os.close(fd)
        except: pass
        time.sleep(0.3)
    return None

def cmd(fd,c,wait=2.0):
    L.wr(fd,c.encode()+b"\r\n"); time.sleep(0.3); b,_=L.rd(fd,wait); save(b)
    return L._CSI.sub(b'',b).decode('latin1','replace')

log("РОБАСТНЫЙ ловец вооружён (окно 20 мин). ПЕРЕДЁРНИ (оба источника).")
fd=robust_catch(1200)
if fd is None: log("⛔ U-Boot не пойман за 20 мин"); sys.exit(2)

# ── PREP: bootdelay -1 + saveenv (гонки с окном больше не будет) ──
log("PREP: setenv bootdelay -1 ; saveenv (плата будет ждать в prompt после reset)")
cmd(fd,"setenv bootdelay -1")
r=cmd(fd,"saveenv",3.0)
if re.search(r'(Saving|Writing|done|OK|Valid)',r,re.I) and 'error' not in r.lower():
    log("✅ env сохранён — reset теперь даёт ждущий prompt (без 2-сек гонки)")
else:
    log(f"⚠ saveenv неоднозначно: ...{r[-120:]!r} (bootdelay применён на сессию; персист под вопросом)")

# ── bdinfo → FB для HDMI ──
r=cmd(fd,"bdinfo",2.5)
fb=[l for l in r.splitlines() if re.search(r'fb|FB|video|lcd',l,re.I)]
log("FB-строки bdinfo: "+(" | ".join(fb) if fb else "(нет — fallback 0xBE000000 из EFI GOP)"))

# ── диагностика ──
log("loady microkernel-diag.bin → go (diag вернётся сам)")
ok,msg=L.do_loady_and_go(fd,save,f"{MK}/microkernel-diag.bin")
log(f"loady/go: {msg}")
if not ok: log("⛔ diag не загрузился"); sys.exit(3)

log("Жду возврат diag (re-enum ttyACM, до 120с)...")
fd=catch_prompt(120)
if fd is None:
    log("⛔ U-Boot не вернулся → diag завис/упал (не дошёл до return). Крошки не прочитать без reset.")
    sys.exit(4)
log("✅ вернулись — читаю крошки")
r=cmd(fd,"md.l 0x42010000 0x10",2.5)
words=[]
for line in r.splitlines():
    m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s+){1,4})',line)
    if m: words+=[int(x,16) for x in m.group(1).split()]
log("================ КРОШКИ ================")
if len(words)<11 or words[0]!=0xB2D0D1A6:
    log(f"⛔ магия не найдена (words={[hex(w) for w in words[:8]]})")
else:
    prog={0x11:"вошли в main (musb_init УПАЛ)",0x22:"init ок, цикл не финишировал(?)",
          0x44:"дошли до READY (энумерация!)",0x55:"цикл БЕЗ ready",0x99:"дошли до конца"}
    log(f"[1] прогресс={hex(words[1])} → {prog.get(words[1],'?')}")
    log(f"[2] pre-init  DEVCTL/POWER={hex(words[2])}")
    log(f"[3] post-init DEVCTL/POWER={hex(words[3])}")
    log(f"[6] ready на итерации={words[6]}   [8] musb_ready={words[8]}")
    log(f"[7] финал DEVCTL/POWER={hex(words[7])}   [10] последняя итер={words[10]}")
    log("POWER: б2=RESET б5=HS б6=SOFTCONN | DEVCTL: б0=session б7=Bdev")
log(f"полный лог: {LOG}")
try: os.close(fd)
except: pass
logf.close()
