#!/usr/bin/env python3
# diag-run.py — один передёрг: ловим U-Boot → bdinfo (адрес FB для HDMI) →
# loady microkernel-diag.bin → go → diag отрабатывает ограниченный цикл и
# ВОЗВРАЩАЕТСЯ в U-Boot → ловим prompt снова → md.l 0x42010000 → разбор крошек.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

MK="/opt/bzdos/microkernel"; LOG="/tmp/mk-diag.log"
logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

def catch_prompt(timeout=120):
    """Ждать возврата ttyACM (re-enum U-Boot после diag) и живого prompt."""
    t=time.time()
    while time.time()-t<timeout:
        if not os.path.exists(L.TTY): time.sleep(0.2); continue
        try: fd=L.open_tty(True)
        except Exception: time.sleep(0.3); continue
        L.wr(fd,b"\r\n"); time.sleep(0.3); b,_=L.rd(fd,1.0)
        if b"=>" in L._CSI.sub(b'',b):
            return fd
        try: os.close(fd)
        except: pass
        time.sleep(0.3)
    return None

log("Жду плату (ПЕРЕДЁРНИ) — ловлю U-Boot...")
if not L.wait_present(1800): log("⛔ ttyACM не появился"); sys.exit(2)
fd=L.open_tty(True)
ok,buf=L.catch_uboot(fd,10); save(buf)
if not ok:
    log("⛔ U-Boot не пойман"); sys.exit(2)
log("✅ U-Boot пойман")

# bdinfo → адрес фреймбуфера
log("bdinfo (ищу fb_base для HDMI):")
L.wr(fd,b"bdinfo\r\n"); time.sleep(0.4); b,_=L.rd(fd,2.5); save(b)
txt=L._CSI.sub(b'',b).decode('latin1','replace')
fb=[l for l in txt.splitlines() if re.search(r'fb|FB|video|lcd|frame',l,re.I)]
log("FB-строки bdinfo: "+(" | ".join(fb) if fb else "(нет — fallback 0xBE000000 из EFI GOP)"))

# diag: loady + go (do_loady_and_go закроет fd)
log("loady microkernel-diag.bin → go (diag вернётся сам)")
ok,msg=L.do_loady_and_go(fd,save,f"{MK}/microkernel-diag.bin")
log(f"loady/go: {msg}")
if not ok: log("⛔ diag не загрузился"); sys.exit(3)

log("Жду возврат diag в U-Boot (re-enum ttyACM, до 120с)...")
fd=catch_prompt(120)
if fd is None:
    log("⛔ U-Boot не вернулся за 120с → diag ЗАВИС/УПАЛ (не дошёл до return). "
        "Крошки в DRAM, но прочитать нельзя без reset (сотрёт). Возможно падение в musb_init/poll.")
    sys.exit(4)
log("✅ U-Boot вернулся — читаю крошки")
L.wr(fd,b"md.l 0x42010000 0x10\r\n"); time.sleep(0.4); b,_=L.rd(fd,2.5); save(b)
clean=L._CSI.sub(b'',b).decode('latin1','replace')

# разбор слов из вывода md
words=[]
for line in clean.splitlines():
    m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s+){1,4})',line)
    if m: words+= [int(x,16) for x in m.group(1).split()]
log("================ КРОШКИ ================")
if len(words)<11 or words[0]!=0xB2D0D1A6:
    log(f"⛔ магия diag не найдена (words={[hex(w) for w in words[:8]]}) — md не распарсился/diag не писал")
else:
    prog={0x11:"вошли в main (musb_init УПАЛ)",0x22:"init ок, но цикл не финишировал",
          0x44:"дошли до READY (энумерация!)",0x55:"цикл кончился БЕЗ ready",0x99:"дошли до конца (return)"}
    p=words[1]
    log(f"[1] прогресс = {hex(p)} → {prog.get(p,'?')}")
    log(f"[2] pre-init  DEVCTL/POWER = {hex(words[2])}")
    log(f"[3] post-init DEVCTL/POWER = {hex(words[3])}")
    log(f"[6] ready на итерации     = {words[6]}")
    log(f"[7] финал DEVCTL/POWER    = {hex(words[7])}")
    log(f"[8] musb_ready()          = {words[8]}")
    log(f"[10] последняя итерация   = {words[10]}  (=ITERS-1 → цикл прошёл целиком)")
    # интерпретация DEVCTL: бит0=session, бит2/1=VBUS, бит7=B-device
    log("подсказка: POWER бит2=RESET,бит5=HS,бит6=SOFTCONN; DEVCTL бит0=session,бит7=Bdev")
log(f"полный лог: {LOG}")
try: os.close(fd)
except: pass
logf.close()
