#!/usr/bin/env python3
# uboot-net-test.py — после ТВОЕГО ресета (плата застряла в микроядре): ловлю U-Boot
# (bootdelay=-1 → ждёт сам), проверяю сеть EMAC: dhcp → ping хоста 192.168.88.2.
# Если сеть жива — это путь к TFTP-netboot и SSH-в-FreeBSD (обход USB-консоли).
# reboot НЕ шлю. Порт /dev/ttyCHIMP (termios).
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/uboot-net.log"; logf=open(LOG,"wb")
HOST="192.168.88.2"
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')

def wait_fresh(dl):
    if os.path.exists(L.TTY):
        log("вижу текущий ttyACM (застрявшее микроядро). Жду твой RESET → порт ИСЧЕЗНЕТ...")
        while os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.05)
        if time.time()>=dl: return None
        log("✓ порт исчез (reset пошёл). Жду свежий U-Boot...")
    while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.01)
    try: return L.open_tty(True)
    except OSError: return None

def catch(fd):
    for _ in range(40):
        try: os.write(fd,b"\r")
        except OSError: return False
        time.sleep(0.15); b,_=L.rd(fd,0.4); save(b)
        if b"=>" in L._CSI.sub(b'',b): return True
    return False

def ucmd(fd,c,w=8):
    L.rd(fd,0.15); os.write(fd,(c+"\r").encode())
    acc=b""; t0=time.time()
    while time.time()-t0<w:
        b,dead=L.rd(fd,0.3)
        if b: acc+=b; save(b)
        if "=>" in clean(acc)[-6:] and len(clean(acc))>len(c)+2: break
        if dead: break
    return clean(acc)

log("Жду ТВОЙ ресет (плата застряла) → U-Boot. Порт /dev/ttyCHIMP.")
DL=time.time()+900
fd=wait_fresh(DL)
if fd is None: log("⛔ свежий U-Boot не появился"); sys.exit(2)
if not catch(fd): log("⚠ '=>' не пойман — но пробую команды");
else: log("✅ U-Boot '=>'")

log("── ТЕСТ СЕТИ EMAC ──")
r=ucmd(fd,"dhcp",25)
ip=None
m=re.search(r'(?:our IP address is|DHCP client bound to address)\s+(\d+\.\d+\.\d+\.\d+)',r)
if m: ip=m.group(1)
if ip:
    log(f"✅ DHCP получил IP платы: {ip}  → EMAC РАБОТАЕТ в U-Boot")
else:
    log(f"⚠ DHCP без явного IP (…{r[-160:]!r})")
r2=ucmd(fd,f"ping {HOST}",12)
if "is alive" in r2:
    log(f"✅✅ PING {HOST} OK — плата пингует хост. Сеть двусторонняя. TFTP-netboot доступен.")
elif "alive" in r2:
    log(f"✅ ping похоже прошёл (…{r2[-100:]!r})")
else:
    log(f"⚠ ping не подтверждён (…{r2[-160:]!r})")
log("── ИТОГ ──")
if ip and "is alive" in r2:
    log(f"🎉 Сеть U-Boot РАБОТАЕТ (IP {ip}, ping хоста OK).")
    log("   Дальше: TFTP-netboot микроядра/ядра ИЛИ загрузка полного bsdOS + SSH.")
else:
    log("Сеть частично/не поднялась — см. лог. Возможно нет DHCP-сервера или EMAC/PHY нюанс.")
log("Плата на '=>'. reboot НЕ слал. Жду решения по следующему шагу.")
try: os.close(fd)
except: pass
logf.close()
