#!/usr/bin/env python3
# mk-cycle.py — ОДИН полный цикл отладки микроядра с breadcrumb-обратной связью.
#   1) жду физический reset (порт исчез → появился = свежий U-Boot)
#   2) агрессивно ловлю '=>', снимаю WDT, bootdelay=-1 + saveenv
#   3) читаю СТАРЫЙ breadcrumb (что успел прошлый запуск)
#   4) loady microkernel.bin + go
#   5) ветка A: появился порт микроядра (1d6b:0010) → ловлю маркеры MK/ALIVE
#      ветка B: WDT-reset (~16-30с) → порт вернулся как U-Boot → ловлю '=>' →
#               md.l 0x50000000 8 → печатаю, на какой вехе ядро остановилось.
# reboot/reset НЕ шлю никогда (guardrail: софт-reset вешает плату).
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
MK="/opt/bzdos/microkernel/microkernel.bin"
WDT_MODE="0x01c20cb8"
LOG="/tmp/mk-cycle.log"; logf=open(LOG,"wb")
BOOTKW=("Hit [Enter]","Type '?'","Executing script","Starting kernel","## Booting")
STAGES={1:"main() entered",2:"wdt armed",3:"musb_init entered",4:"musb_init done",
        7:"banner flushed",8:"poll loop",9:"host configured",10:"ALIVE emitted",
        99:"gave up waiting for enumeration (ENUM_GIVEUP)"}
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def aggressive_catch(fd,secs=20):
    buf=b""; t0=time.time()
    while time.time()-t0<secs:
        try: os.write(fd,b"\r\x03")
        except OSError: return None
        b,dead=L.rd(fd,0.03)
        if b: buf+=b; save(b)
        cb=clean(buf)
        if "=>" in cb[-50:]: return "uboot"
        if any(k in cb for k in BOOTKW): return "loader"
        if dead: return None
    return None

def stable_uboot(fd):
    good=0
    for _ in range(5):
        try: L.rd(fd,0.08); os.write(fd,b"\r")
        except OSError: return False
        time.sleep(0.3); r=clean(L.rd(fd,0.7)[0]); save(r)
        if any(k in r for k in ("Card","MMC","## Booting","Loading Env","Starting")): return False
        if "=>" in r: good+=1
        if good>=2: return True
    return good>=2

def ucmd(fd,c,w=3):
    L.rd(fd,0.1); os.write(fd,(c+"\r").encode())
    acc=b""; t0=time.time()
    while time.time()-t0<w:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-6:] and len(clean(acc))>len(c): break
        if dead: break
    return clean(acc)

def read_breadcrumb(fd,tag):
    r=ucmd(fd,"md.l 0x50000000 8",3)
    words=[]
    for line in r.splitlines():
        m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s*){1,4})',line)
        if m: words+=[int(x,16) for x in m.group(1).split()]
    if len(words)<8 or words[0]!=0xB2D0C0DE:
        log(f"[{tag}] breadcrumb: магии нет ({[hex(w) for w in words[:4]]})"); return None
    st=words[1]
    log(f"[{tag}] BREADCRUMB: веха {st} «{STAGES.get(st,'?')}» | polls={words[2]} "
        f"state={words[3]} ready={words[4]} intr_seen={words[5]:#x} setups={words[6]} csr0={words[7]:#x}")
    return words

def catch_fresh_uboot(dl,label):
    """порт должен ПОЯВИТЬСЯ (после reset/WDT); открыть, поймать '=>' стабильно."""
    while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.02)
    if not os.path.exists(L.TTY): return None
    time.sleep(0.35)
    try: fd=L.open_tty(True)
    except OSError: return None
    k=aggressive_catch(fd, min(25, max(5, dl-time.time())))
    if k!="uboot" or not stable_uboot(fd):
        log(f"[{label}] '=>' не пойман/нестабилен (k={k})")
        try: os.close(fd)
        except: pass
        return None
    return fd

log(f"payload {MK} ({os.path.getsize(MK)}B). ЖДУ ФИЗИЧЕСКИЙ RESET.")
DL=time.time()+1800
if os.path.exists(L.TTY):
    log("порт есть (зомби) — жду его исчезновения (reset)...")
    while os.path.exists(L.TTY) and time.time()<DL: time.sleep(0.05)
    log("✓ порт исчез — reset пошёл")
fd=catch_fresh_uboot(DL,"boot")
if fd is None: log("⛔ не поймал U-Boot после reset"); sys.exit(2)
log("✅ стабильный U-Boot. WDT off, bootdelay=-1, читаю старый breadcrumb.")
ucmd(fd,f"mw.l {WDT_MODE} 0 ; setenv bootdelay -1 ; saveenv",8)
read_breadcrumb(fd,"prev-run")

log("── loady + go ──")
okg,msg=L.do_loady_and_go(fd, save, MK)
log(f"loady/go: {msg}")
if not okg: log("⛔ загрузка не удалась"); logf.close(); sys.exit(3)

log("── ветка A: жду re-enum микроядра ──")
dl2=time.time()+30
while os.path.exists(L.TTY) and time.time()<dl2: time.sleep(0.03)
while not os.path.exists(L.TTY) and time.time()<dl2: time.sleep(0.02)
m={"init":False,"cfg":False,"alive":False}
if os.path.exists(L.TTY):
    time.sleep(0.4)
    try:
        kfd=L.open_tty(True); acc=b""; t1=time.time()
        while time.time()-t1<25:
            b,dead=L.rd(kfd,0.3)
            if b:
                save(b); acc+=b; cb=clean(acc)
                m["init"]|= "musb_init done" in cb
                m["cfg"] |= "host configured" in cb
                m["alive"]|="BZDOS-MK-ALIVE" in cb
            if dead: log("порт микроядра дропнулся (WDT-reset?)"); break
            if m["alive"]: break
        try: os.close(kfd)
        except: pass
    except Exception as e: log(f"open MK: {e}")
log(f"маркеры: init={'✅' if m['init'] else '—'} cfg={'✅' if m['cfg'] else '—'} alive={'✅' if m['alive'] else '—'}")
if m["alive"] or m["cfg"]:
    log("🎉 КОНСОЛЬ МИКРОЯДРА ЖИВА!"); logf.close(); sys.exit(0)

log("── ветка B: жду WDT-reset → свежий U-Boot → breadcrumb ──")
dl3=time.time()+150
# порт (чей бы ни был) должен исчезнуть при WDT-reset, затем появиться U-Boot
while os.path.exists(L.TTY) and time.time()<dl3: time.sleep(0.05)
fd2=catch_fresh_uboot(dl3,"post-wdt")
if fd2 is None:
    log("⛔ U-Boot после WDT не пойман — возможно плата снова зомби. См. лог."); logf.close(); sys.exit(4)
ucmd(fd2,f"mw.l {WDT_MODE} 0 ; setenv bootdelay -1 ; saveenv",8)
w=read_breadcrumb(fd2,"this-run")
log("================ ИТОГ ЦИКЛА ================")
if w: log(f"ядро дошло до вехи {w[1]} «{STAGES.get(w[1],'?')}» — это и есть место следующего фикса.")
else: log("breadcrumb не прочитан — смотрим лог.")
log("U-Boot жив, bootdelay=-1, WDT снят — следующий цикл БЕЗ физического reset (порт остаётся).")
logf.close()
