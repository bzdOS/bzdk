#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# repl-client.py — грузит microkernel-repl.elf в U-Boot и говорит с резидентным
# REPL по сырым Ethernet-кадрам (ethertype 0x88B5) через AF_PACKET на br0.
# Фаза 1: catch U-Boot → WDT off → autostart/bootdelay → loady ELF → bootelf.
# Фаза 2: raw-сокет: ждём баннер "REPL ready", шлём команды, печатаем ответы.
# reboot/reset НЕ шлю. REPL резидентен → без перезаливок после загрузки.
import os,sys,time,socket,struct,select
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
ELF="/opt/bzdos/microkernel/microkernel-repl.elf"
STAGE=0x48000000
WDT_MODE="0x01c20cb8"
IFACE="br0"
ETYPE=0x88B5
BOARD_MAC=bytes.fromhex("02bd05000001")
BCAST=b"\xff"*6
LOG=open("/tmp/repl-client.log","wb")
def clean(b): return L._CSI.sub(b'',b)
def save(d):
    if isinstance(d,str): d=d.encode()
    LOG.write(d); LOG.flush()
    try: sys.stdout.buffer.write(clean(d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

# ---- Phase 1: load REPL via U-Boot ----
def wput(fd,d,budget=1.0):
    t0=time.time()
    while time.time()-t0<budget:
        try: os.write(fd,d); return True
        except OSError: time.sleep(0.05)
    return False

def wait_fresh_port(deadline):
    """Ждём ФИЗИЧЕСКИЙ reset: порт исчезнет (или уже нет) → появится заново."""
    # если порт есть сейчас (возможно зомби) — ждём исчезновения
    if os.path.exists(L.TTY):
        log("жду reset — порт должен исчезнуть...")
        while os.path.exists(L.TTY) and time.time()<deadline: time.sleep(0.05)
    # ждём появления свежего U-Boot
    while not os.path.exists(L.TTY) and time.time()<deadline: time.sleep(0.02)
    return os.path.exists(L.TTY)

def _try_catch(deadline, window=8):
    """Попытка поймать '=>' на ТЕКУЩЕМ порту."""
    if not os.path.exists(L.TTY): return None
    time.sleep(0.2)
    try: fd=L.open_tty(True)
    except OSError: return None
    buf=b""; t0=time.time()
    while time.time()<deadline and time.time()-t0<window:
        wput(fd,b"\r\x03",0.5)
        b,dead=L.rd(fd,0.05)
        if b: buf+=b; save(b)
        if dead: break
        if b"=>" in clean(buf)[-60:]:
            good=0
            for _ in range(4):
                L.rd(fd,0.08); wput(fd,b"\r",0.5); time.sleep(0.25)
                r=clean(L.rd(fd,0.6)[0]); save(r)
                if b"=>" in r: good+=1
            if good>=2: return fd
    try: os.close(fd)
    except: pass
    return None

def catch_prompt(deadline):
    # плата, возможно, УЖЕ на U-Boot (только что ресетнули) — пробуем сразу
    fd=_try_catch(deadline)
    if fd: return fd
    # иначе ждём свежую энумерацию (исчезнет→появится) и ловим
    log("текущий порт не ответил — жду свежий U-Boot...")
    if not wait_fresh_port(deadline):
        log("⛔ свежий порт не появился"); return None
    log("порт появился — агрессивно ловлю '=>'")
    return _try_catch(deadline, window=25)

def ucmd(fd,c,w=3):
    L.rd(fd,0.1); os.write(fd,(c+"\r").encode())
    acc=b""; t0=time.time()
    while time.time()-t0<w:
        b,dead=L.rd(fd,0.2)
        if b: acc+=b; save(b)
        if b"=>" in clean(acc)[-6:] and len(clean(acc))>len(c): break
        if dead: break
    return clean(acc)

# ---- Phase 2: raw ethernet REPL ----
def open_raw():
    s=socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETYPE))
    s.bind((IFACE, ETYPE))
    src=s.getsockname()[4]  # host br0 MAC
    return s, src

def send_cmd(s, src, text):
    payload=(text+"\r").encode()
    if len(payload)<46: payload=payload+b"\x00"*(46-len(payload))
    frame=BCAST+src+struct.pack("!H",ETYPE)+payload
    s.send(frame)

def drain(s, secs):
    out=[]; t0=time.time()
    while time.time()-t0<secs:
        r,_,_=select.select([s],[],[],0.3)
        if not r: continue
        f=s.recv(2048)
        if len(f)<14: continue
        et=struct.unpack("!H",f[12:14])[0]
        if et!=ETYPE: continue
        if f[6:12]!=BOARD_MAC: continue
        pl=f[14:].split(b"\x00",1)[0]
        if pl: out.append(pl.decode("latin1","replace"))
    return out

log(f"payload {ELF} ({os.path.getsize(ELF)}B). ЖДУ ОДИН физический reset.")
fd=catch_prompt(time.time()+1800)
if fd is None: log("⛔ U-Boot промпт не пойман"); sys.exit(2)
log("✅ U-Boot. WDT off, autostart=yes, bootdelay=-1.")
ucmd(fd,f"mw.l {WDT_MODE} 0 ; setenv autostart yes ; setenv bootdelay -1 ; saveenv",8)
log("── loady REPL ELF + bootelf ──")
ok,msg=L.do_loady_and_go(fd, save, ELF, addr=STAGE, exec_cmd=f"bootelf -p 0x{STAGE:x}")
log(f"loady/bootelf: {msg}")
if not ok: log("⛔ загрузка не удалась"); sys.exit(3)

log("── raw-сокет br0, жду баннер REPL (30с) ──")
s,src=open_raw()
log(f"host MAC={src.hex(':')}, слушаю ethertype 0x{ETYPE:04x} от {BOARD_MAC.hex(':')}")
banner=""; t0=time.time(); got=False
while time.time()-t0<30:
    for line in drain(s,1.0):
        save(f"RX< {line}\n"); banner+=line
        if "REPL ready" in banner or "mk>" in banner: got=True
    if got: break
if not got:
    log("⚠ баннер не пойман за 30с (линк мог флапнуть). Пробую слать команды всё равно.")
else:
    log("🎉 REPL ОТВЕЧАЕТ по сети!")

# демонстрация: несколько команд
for cmd in ["h", "bc 0x50000300 4", "r 0x01c19040 2", "bc 0x50000000 8"]:
    log(f"→ команда: {cmd}")
    send_cmd(s,src,cmd)
    time.sleep(0.5)
    for line in drain(s,2.0): save(f"RX< {line}\n")
log("готово — REPL-сессия отработала (полный лог /tmp/repl-client.log)")
s.close(); LOG.close()
