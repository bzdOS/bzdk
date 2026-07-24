#!/usr/bin/env python3
# run-diag.py — гоняет diag v2. Env уже с bootdelay=-1 (U-Boot ждёт в prompt),
# поэтому ловля без гонки. loady → go → diag сам вернётся ~8с (или watchdog
# ресетнёт ~16с → U-Boot снова ждёт) → читаем крошки. Физ.передёрг нужен только
# для выхода из текущего жёсткого зависания; дальше — reset/watchdog.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
MK="/opt/bzdos/microkernel"; LOG="/tmp/mk-diag2.log"
logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def prompt_fd(timeout):
    t=time.time()
    while time.time()-t<timeout:
        if not os.path.exists(L.TTY): time.sleep(0.15); continue
        try: fd=L.open_tty(True)
        except Exception: time.sleep(0.3); continue
        # разбудить и подтвердить prompt (flood Ctrl-C безвреден)
        for _ in range(6):
            L.wr(fd,b"\x03\r\n"); time.sleep(0.15)
            b,_=L.rd(fd,0.5)
            if b"=>" in L._CSI.sub(b'',b): return fd
        try: os.close(fd)
        except: pass
        time.sleep(0.3)
    return None

log("Жду U-Boot в prompt (ПЕРЕДЁРНИ для выхода из зависа; env=bootdelay-1 → замрёт сам)...")
fd=prompt_fd(1200)
if fd is None: log("⛔ prompt не появился за 20 мин"); sys.exit(2)
log("✅ U-Boot prompt")
log("loady microkernel-diag.bin (v2) → go")
ok,msg=L.do_loady_and_go(fd,save,f"{MK}/microkernel-diag.bin")
log(f"loady/go: {msg}")
if not ok: log("⛔ не загрузился"); sys.exit(3)

log("Жду возврат/ресет (diag ~8с, watchdog ~16с+ребут). Ловлю prompt до 60с...")
fd=prompt_fd(60)
if fd is None:
    log("⛔ prompt не вернулся за 60с — даже watchdog не спас (не арминулся?). Нужен разбор.")
    sys.exit(4)
log("✅ prompt вернулся — читаю крошки")
L.wr(fd,b"md.l 0x42010000 0x10\r\n"); time.sleep(0.4); b,_=L.rd(fd,2.5); save(b)
r=L._CSI.sub(b'',b).decode('latin1','replace')
words=[]
for line in r.splitlines():
    m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s+){1,4})',line)
    if m: words+=[int(x,16) for x in m.group(1).split()]
log("================ КРОШКИ ================")
if len(words)<13 or words[0]!=0xB2D0D1A6:
    log(f"⛔ магия не найдена (words={[hex(w) for w in words[:8]]}). Возможно DRAM стёрт watchdog-ресетом → значит diag ФОЛТНУЛ (не дошёл до чистого возврата). Место падения ниже по [1].")
else:
    prog={0x11:"вошли в main — musb_init УПАЛ/завис",0x22:"init ок, цикл не финишировал",
          0x44:"READY (энумерация удалась!)",0x55:"цикл 8с БЕЗ ready (SETUP не отвечен)",0x99:"дошли до конца (чистый возврат)"}
    log(f"[1] прогресс = {hex(words[1])} → {prog.get(words[1],'?')}")
    log(f"[2] pre-init  DEVCTL/POWER = {hex(words[2])}")
    log(f"[3] post-init DEVCTL/POWER = {hex(words[3])}")
    log(f"[6] ready на итер = {words[6]}   [8] musb_ready = {words[8]}")
    log(f"[7] финал DEVCTL/POWER = {hex(words[7])}")
    log(f"[10] итераций опроса = {words[10]}   [11] тиков прошло = {words[11]}   [12] CNTFRQ = {words[12]}")
    log("POWER: б2=RESET б5=HS б6=SOFTCONN | DEVCTL: б0=session б7=Bdev")
log(f"полный лог: {LOG}")
try: os.close(fd)
except: pass
logf.close()
