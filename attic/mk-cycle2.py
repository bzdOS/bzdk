#!/usr/bin/env python3
# mk-cycle2.py — цикл отладки БЕЗ физического reset: плата уже сидит на '=>'
# (bootdelay=-1). Захват промпта → WDT off → breadcrumb прошлого запуска →
# loady microkernel.ELF на staging 0x48000000 → `bootelf -p` (флашит кэши!) →
# ловим: маркеры MK / exception-dump U-Boot / WDT-reset → итоговый breadcrumb.
# reboot/reset НЕ шлю. Если порт не на промпте — жду reset как раньше.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
ELF="/opt/bzdos/microkernel/microkernel.elf"
STAGE_ADDR=0x48000000
WDT_MODE="0x01c20cb8"
LOG="/tmp/mk-cycle2.log"; logf=open(LOG,"wb")
STAGES={1:"main() entered",2:"wdt armed",3:"musb_init entered",4:"musb_init done",
        7:"banner flushed",8:"poll loop",9:"host configured",10:"ALIVE emitted",
        99:"gave up waiting for enumeration"}
EXKW=("Synchronous Abort","SError","undefined instruction","Resetting CPU","esr 0x","elr:")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def try_catch_prompt(secs=12):
    """Открыть порт и стабильно поймать '=>'. None если не вышло."""
    if not os.path.exists(L.TTY): return None
    try: fd=L.open_tty(True)
    except OSError: return None
    buf=b""; t0=time.time()
    while time.time()-t0<secs:
        try: os.write(fd,b"\r\x03")
        except OSError:
            try: os.close(fd)
            except: pass
            return None
        b,dead=L.rd(fd,0.05)
        if b: buf+=b; save(b)
        if dead: break
        if "=>" in clean(buf)[-60:]:
            good=0
            for _ in range(4):
                L.rd(fd,0.08); os.write(fd,b"\r"); time.sleep(0.25)
                r=clean(L.rd(fd,0.6)[0]); save(r)
                if "=>" in r: good+=1
            if good>=2: return fd
    try: os.close(fd)
    except: pass
    return None

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
    r=ucmd(fd,"md.l 0x50000000 24",3)
    words=[]
    for line in r.splitlines():
        m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s*){1,4})',line)
        if m: words+=[int(x,16) for x in m.group(1).split()]
    if len(words)>=1 and words[0]==0xB2D0C0DE:
        log(f"[{tag}] breadcrumb: СТАРАЯ магия FreeBSD-диага (=микроядро в DRAM не писало)"); return None
    if len(words)<8 or words[0]!=0xB2D0CAFE:
        log(f"[{tag}] breadcrumb: магии нет ({[hex(w) for w in words[:4]]})"); return None
    log(f"[{tag}] BREADCRUMB: веха {words[1]} «{STAGES.get(words[1],'?')}» | polls={words[2]} "
        f"state={words[3]} ready={words[4]} intr={words[5]:#x} setups={words[6]} w7={words[7]:#x}")
    # EP0 request ring (words 8..15): (bmReqType<<24)|(bReq<<16)|wValue
    if len(words)>=16:
        REQN={0x0080:"GET_DESC",0x0006:"GET_DESC",0x0005:"SET_ADDR",0x0009:"SET_CFG",
              0x0000:"GET_STATUS",0x000a:"GET_IF",0x0b01:"SET_IF"}
        ring=[]
        for w in words[8:16]:
            if w==0 or w==0xffffffff: continue
            bm=(w>>24)&0xff; br=(w>>16)&0xff; wv=w&0xffff
            key=(bm<<8)|br
            name=REQN.get(key, f"{bm:02x}:{br:02x}")
            # для GET_DESCRIPTOR wValue hi = тип дескриптора
            if br==0x06: name=f"GET_DESC(t{wv>>8},i{wv&0xff})"
            elif br==0x05: name=f"SET_ADDR({wv})"
            elif br==0x09: name=f"SET_CFG({wv})"
            ring.append(name)
        log(f"[{tag}] EP0 запросы (последние 8): {' → '.join(ring) if ring else '—'}")
    return words

def get_prompt(deadline):
    """Промпт сейчас ИЛИ после физического reset."""
    fd=try_catch_prompt()
    if fd: return fd
    log("порт не на промпте — жду reset (исчезнет/появится)...")
    while time.time()<deadline:
        if os.path.exists(L.TTY):
            fd=try_catch_prompt()
            if fd: return fd
        time.sleep(1.0)
    return None

log(f"payload {ELF} ({os.path.getsize(ELF)}B) → loady 0x{STAGE_ADDR:x} + bootelf -p")
fd=get_prompt(time.time()+1800)
if fd is None: log("⛔ промпт не получен"); sys.exit(2)
log("✅ U-Boot '=>'. WDT off, bootdelay=-1, breadcrumb прошлого запуска:")
ucmd(fd,f"mw.l {WDT_MODE} 0 ; setenv bootdelay -1 ; setenv autostart yes ; saveenv",8)
read_breadcrumb(fd,"prev-run")

log("── loady ELF + bootelf ──")
okg,msg=L.do_loady_and_go(fd, save, ELF, addr=STAGE_ADDR,
                          exec_cmd=f"bootelf -p 0x{STAGE_ADDR:x}")
log(f"loady/bootelf: {msg}")
if not okg: log("⛔ загрузка не удалась"); logf.close(); sys.exit(3)

log("── наблюдение: маркеры MK / exception U-Boot / WDT-reset ──")
m={"init":False,"cfg":False,"alive":False,"except":False}
t0=time.time(); acc=b""; kfd=None
while time.time()-t0<60:
    if kfd is None:
        if os.path.exists(L.TTY):
            time.sleep(0.3)
            try: kfd=L.open_tty(True)
            except OSError: kfd=None; time.sleep(0.3); continue
        else: time.sleep(0.1); continue
    b,dead=L.rd(kfd,0.3)
    if b:
        save(b); acc+=b; cb=clean(acc)
        m["init"]|= "musb_init done" in cb
        m["cfg"] |= "host configured" in cb
        m["alive"]|="BZDOS-MK-ALIVE" in cb
        if any(k in cb for k in EXKW): m["except"]=True
    if dead:
        log("порт дропнулся (reset/re-enum) — жду его снова")
        try: os.close(kfd)
        except: pass
        kfd=None; acc=b""
    if m["alive"] or m["except"]: break
if kfd is not None:
    try: os.close(kfd)
    except: pass
log(f"маркеры: init={'✅' if m['init'] else '—'} cfg={'✅' if m['cfg'] else '—'} "
    f"alive={'✅' if m['alive'] else '—'} exception={'⚠️' if m['except'] else '—'}")
if m["alive"] or m["cfg"]:
    log("🎉 КОНСОЛЬ МИКРОЯДРА ЖИВА!"); logf.close(); sys.exit(0)

log("── финал: ловлю промпт и читаю breadcrumb этого запуска ──")
fd2=get_prompt(time.time()+120)
if fd2 is None: log("⛔ промпт не пойман — плата не на U-Boot"); logf.close(); sys.exit(4)
ucmd(fd2,f"mw.l {WDT_MODE} 0 ; setenv bootdelay -1 ; saveenv",8)
w=read_breadcrumb(fd2,"this-run")
log("================ ИТОГ ЦИКЛА ================")
if w: log(f"свежая веха {w[1]} «{STAGES.get(w[1],'?')}», w7={w[7]:#x} — сюда следующий фикс.")
else: log("свежей магии нет — ядро опять не исполнилось (см. exception в логе).")
log("промпт жив — следующий цикл сразу, без reset.")
logf.close()
