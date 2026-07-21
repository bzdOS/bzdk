#!/usr/bin/env python3
# mk-net.py — тест СЕТЕВОГО микроядра (EMAC, независимый канал). Плата на '=>'.
# Захват промпта → WDT off → loady microkernel-net.elf на staging → bootelf →
# ПАРАЛЛЕЛЬНО tcpdump на br0 ловит наши кадры ethertype 0x88B5 (BZDOS-NET-ALIVE) →
# после WDT-reset читаю СЕТЕВОЙ breadcrumb на 0x50000100 (magic E3AC0DE1).
# reboot/reset НЕ шлю. bootdelay=-1 держится → без физического reset.
import os,sys,time,re,subprocess,signal
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
ELF="/opt/bzdos/microkernel/microkernel-net.elf"
STAGE_ADDR=0x48000000
WDT_MODE="0x01c20cb8"
NETBC=0x50000100
LOG="/tmp/mk-net.log"; logf=open(LOG,"wb")
STAGES={1:"enter",2:"clk ungated",3:"syscon",4:"pinmux",5:"emac soft-reset",
        6:"mdio",7:"phy reset",8:"link up",9:"dma rings",10:"tx/rx enabled",
        11:"poll loop",99:"no link — gave up"}
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def try_catch_prompt(secs=12):
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

def read_netbc(fd,tag):
    r=ucmd(fd,f"md.l 0x{NETBC:x} 16",3)
    words=[]
    for line in r.splitlines():
        m=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s*){1,4})',line)
        if m: words+=[int(x,16) for x in m.group(1).split()]
    if len(words)<10 or words[0]!=0xE3AC0DE1:
        log(f"[{tag}] net-breadcrumb: магии нет ({[hex(w) for w in words[:4]]})"); return None
    log(f"[{tag}] NET-BREADCRUMB: веха {words[1]} «{STAGES.get(words[1],'?')}» | "
        f"clk={words[2]} phyrst={words[3]} link={words[4]} speed={words[5]} "
        f"tx={words[6]} rx={words[7]} int_sta={words[8]:#x} phyid={words[9]:#010x}")
    return words

def get_prompt(deadline):
    fd=try_catch_prompt()
    if fd: return fd
    log("порт не на промпте — жду reset...")
    while time.time()<deadline:
        if os.path.exists(L.TTY):
            fd=try_catch_prompt()
            if fd: return fd
        time.sleep(1.0)
    return None

log(f"payload {ELF} ({os.path.getsize(ELF)}B) → loady 0x{STAGE_ADDR:x} + bootelf")
fd=get_prompt(time.time()+1800)
if fd is None: log("⛔ промпт не получен"); sys.exit(2)
log("✅ U-Boot '=>'. WDT off, bootdelay=-1, старый net-breadcrumb:")
ucmd(fd,f"mw.l {WDT_MODE} 0 ; setenv bootdelay -1 ; setenv autostart yes ; saveenv",8)
read_netbc(fd,"prev-run")

# запускаем tcpdump ДО go — ловим наши кадры (env без прокси, LAN)
log("── запускаю tcpdump br0 ether proto 0x88b5 ──")
env=dict(os.environ); env["NO_PROXY"]="*"; env.pop("http_proxy",None); env.pop("https_proxy",None)
tcap="/tmp/mk-net-tcpdump.txt"; tf=open(tcap,"wb")
td=subprocess.Popen(["tcpdump","-i","br0","-l","-e","-A","ether","proto","0x88b5"],
                    stdout=tf,stderr=subprocess.STDOUT,env=env)
time.sleep(1.0)

log("── loady net-ELF + bootelf ──")
okg,msg=L.do_loady_and_go(fd, save, ELF, addr=STAGE_ADDR,
                          exec_cmd=f"bootelf -p 0x{STAGE_ADDR:x}")
log(f"loady/bootelf: {msg}")
if not okg:
    td.terminate(); log("⛔ загрузка не удалась"); logf.close(); sys.exit(3)

log("── наблюдаю 60с: кадры BZDOS-NET на br0 ──")
seen=False; t0=time.time()
while time.time()-t0<60:
    time.sleep(2)
    try:
        with open(tcap,"rb") as f: cap=f.read()
        if b"BZDOS-NET" in cap or b"88b5" in cap.lower():
            seen=True; break
    except: pass
td.terminate()
try: td.wait(timeout=3)
except: td.kill()
capsz=os.path.getsize(tcap)
log(f"tcpdump: {capsz}B захвачено, BZDOS-NET кадры: {'✅ ЕСТЬ' if seen else '— нет'}")
if seen:
    log("🎉 СЕТЕВОЙ КАНАЛ ЖИВ — EMAC шлёт кадры на хост!")

log("── читаю net-breadcrumb после запуска (ловлю промпт) ──")
fd2=get_prompt(time.time()+120)
if fd2 is None:
    log("⛔ промпт не пойман"); logf.close(); sys.exit(4)
ucmd(fd2,f"mw.l {WDT_MODE} 0 ; setenv bootdelay -1 ; saveenv",8)
w=read_netbc(fd2,"this-run")
log("================ ИТОГ NET-ЦИКЛА ================")
if seen:
    log("канал поднялся — дальше делаем его основным для отладки.")
elif w:
    log(f"кадров нет; EMAC дошёл до вехи {w[1]} «{STAGES.get(w[1],'?')}» — сюда фикс.")
else:
    log("ни кадров, ни свежего breadcrumb — смотрим лог.")
log(f"полный лог: {LOG}, дамп tcpdump: {tcap}")
logf.close()
