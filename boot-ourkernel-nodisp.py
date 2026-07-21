#!/usr/bin/env python3
# boot-ourkernel-nodisp.py — ОДИН передёрг, всё по кабелю, С WATCHDOG:
#  1) ловим U-Boot (bootdelay=-1 → замрёт в prompt)
#  2) fatload НАШЕГО загрузчика bootaa64.efi c ESP → kernel_addr_r
#  3) loady НАШ pine64-hybrid-nodisp.dtb → fdt_addr_r (дисплейные драйверы + WD-узел OFF)
#  4) ВЗВОДИМ A64 watchdog (16с, reset всей системы) — ПОСЛЕДНИМ перед bootefi,
#     чтобы почти все 16с пришлись на ядро → зависло ядро → SoC сам ресетнётся в
#     U-Boot (bootdelay=-1) → БЕЗ вырывания питания.
#  5) bootefi → наш loader → verbose → boot НАШЕГО ядра (быстрая передача, экономим окно WD)
#  6) захват ttyACM до EBS; дальше СМОТРИ НА МОНИТОР (efifb переживает EBS).
#  7) ждём АВТО-возврат watchdog'ом в U-Boot (подтверждаем: передёрг не нужен).
import os,sys,time,re,subprocess
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

DTB="/opt/bzdos/build/pine64-hybrid-nodisp.dtb"
LOG="/tmp/ourkernel-nodisp.log"; logf=open(LOG,"wb")
# A64 watchdog @0x01C20CA0: CTRL+0x10, CFG+0x14, MODE+0x18
WDT_CTRL="0x01c20cb0"; WDT_CFG="0x01c20cb4"; WDT_MODE="0x01c20cb8"
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

d=open(DTB,'rb').read()
assert d[:4]==bytes.fromhex('d00dfeed'), "DTB bad magic"
log(f"nodisp DTB OK ({len(d)} б; дисплей+watchdog узлы disabled)")

def catch(total):
    dl=time.time()+total
    while time.time()<dl:
        if not os.path.exists(L.TTY): time.sleep(0.15); continue
        try: fd=L.open_tty(True)
        except Exception: time.sleep(0.3); continue
        for _ in range(8):
            L.wr(fd,b"\r\n"); time.sleep(0.12); b,_=L.rd(fd,0.5)
            if b"=>" in L._CSI.sub(b'',b): return fd
        try: os.close(fd)
        except: pass
        time.sleep(0.3)
    return None

def cmd(fd,c,w=2.0):
    L.rd(fd,0.1); L.wr(fd,c.encode()+b"\r\n"); time.sleep(0.3)
    b,_=L.rd(fd,w); save(b); return L._CSI.sub(b'',b).decode('latin1','replace')

log("ПЕРЕДЁРНИ питание ОДИН раз. Ловлю U-Boot prompt (bootdelay=-1 → сам замрёт)...")
fd=catch(600)
if fd is None: log("⛔ prompt не пойман за 10 мин"); sys.exit(2)
log("✅ U-Boot prompt")
cmd(fd,f"mw.l {WDT_MODE} 0")   # снять возможный залипший WD (на случай если reset его не гасит)

# адреса
r=cmd(fd,"printenv kernel_addr_r fdt_addr_r",1.5)
m=re.search(r'kernel_addr_r=(0x[0-9a-fA-F]+)',r); KA=m.group(1) if m else "0x40080000"
m=re.search(r'fdt_addr_r=(0x[0-9a-fA-F]+)',r);   FA=m.group(1) if m else "0x4fa00000"
log(f"kernel_addr_r={KA}  fdt_addr_r={FA}")

# 1) fatload загрузчика с ESP
loaded=False
for dp in ["mmc 1:2","mmc 2:2","mmc 1:1","mmc 0:1","mmc 2:1"]:
    r=cmd(fd,f"fatload {dp} {KA} efi/boot/bootaa64.efi",4.0)
    if re.search(r'\d+\s+bytes read',r) and not re.search(r'error|unable|bad|not found|failed',r,re.I):
        log(f"✅ loader: {dp} efi/boot/bootaa64.efi"); loaded=True; break
if not loaded:
    log("⛔ bootaa64.efi не найден на ESP");
    try: os.close(fd)
    except: pass
    sys.exit(3)

# 2) loady нашего nodisp DTB в fdt_addr_r
log(f"loady nodisp DTB → {FA} (ymodem)")
L.wr(fd,f"loady {FA}\r\n".encode()); save(f"\n# loady {FA}\n".encode())
saw=False; t0=time.time()
while time.time()-t0<10:
    b,dead=L.rd(fd,0.2)
    if b: save(b)
    if b and b"\x43" in b: saw=True; break
    if dead: break
if not saw: log("⛔ нет ymodem 'C'"); sys.exit(4)
try: os.close(fd)
except: pass
sbfd=L.open_tty(False)
try:
    proc=subprocess.run([L.SB_BIN,"--ymodem","-v",DTB],stdin=sbfd,stdout=sbfd,stderr=subprocess.PIPE,timeout=120)
finally:
    try: os.close(sbfd)
    except: pass
save(b"\n# [sb stderr]\n"+(proc.stderr or b"")+b"\n")
if proc.returncode!=0: log(f"⛔ sb exit={proc.returncode}"); sys.exit(5)
log("✅ DTB в RAM")
fd=L.open_tty(True); L.rd(fd,2.0)

# 3) ВЗВОД WATCHDOG (последним перед bootefi → окно 16с почти целиком на ядро)
log("Взвожу A64 watchdog: 16с, reset всей системы")
cmd(fd,f"mw.l {WDT_CFG} 0x00000001")    # CFG: whole-system reset
cmd(fd,f"mw.l {WDT_MODE} 0x000000b1")   # MODE: интервал idx11=16с | enable
cmd(fd,f"mw.l {WDT_CTRL} 0x000014af")   # CTRL: key(0x0A57<<1) | restart(pet)
rv=cmd(fd,f"md.l {WDT_MODE} 1",1.0)
log(f"WD MODE readback: {rv.strip().splitlines()[-1] if rv.strip() else '?'} (ждём ...000000b1)")

# 4) bootefi: наш loader + наш DTB (БЫСТРО — экономим окно WD)
log(f">>> bootefi {KA} {FA} — наш loader + nodisp DTB (WD тикает) <<<")
L.wr(fd,f"bootefi {KA} {FA}\r\n".encode())

# перехват loader countdown → verbose → boot (без повторной загрузки ядра — экономим время)
buf=b""; t=time.time(); inter=False
while time.time()-t<45 and os.path.exists(L.TTY):
    b,dead=L.rd(fd,0.2)
    if b: buf+=b; save(b)
    if b"Hit [Enter]" in L._CSI.sub(b'',buf) and not inter:
        time.sleep(0.25); L.wr(fd,b" "); inter=True; b,_=L.rd(fd,2); save(b); break
    if dead: break
def send(c,w=3):
    L.rd(fd,0.1); L.wr(fd,c.encode()+b"\r"); time.sleep(0.25)
    dd=L.rd(fd,w)[0]; save(dd); return L._CSI.sub(b'',dd).decode('latin1','replace')
if inter:
    send('set boot_verbose=1',1)               # максимум сообщений на HDMI
    send('set kern.panic_reboot_wait_time=-1',1) # застыть на панике; WD всё равно ресетнёт
    log(">>> BOOT НАШЕГО ЯДРА <<<")
    send("boot",2)
else:
    log("⚠ не поймал loader countdown (bootefi не завёл loader?)")

# 5) захват до EBS
log("Захват ttyACM до ExitBootServices...")
t=time.time(); last=time.time()
while os.path.exists(L.TTY) and time.time()-t<90:
    b,dead=L.rd(fd,0.3)
    if b: save(b); last=time.time()
    if dead or time.time()-last>20: break
try: os.close(fd)
except: pass
log("===== ttyACM ЗАМОЛЧАЛ = EBS пройден =====")
print("""
╔════════════════════════════════════════════════════════════════════╗
║  СМОТРИ НА МОНИТОР СЕЙЧАС (окно ~секунд до авто-ресета watchdog'ом):  ║
║   (A) БЕЛЫЙ ТЕКСТ на чёрном = лог НАШЕГО ядра — ПОБЕДА. СФОТКАЙ       ║
║       последние строки (CPU:, real memory, mmcsd, awg0, musb,        ║
║       mountroot — там место зависа).                                 ║
║   (B) осталось лого U-Boot   (C) чёрный экран                        ║
║   (D) 'No cable'/погас = PHY всё равно уронили → план Б              ║
║   (E) экран мигает/перезагружается по кругу = окно WD мало (тюним)   ║
╚════════════════════════════════════════════════════════════════════╝
""",flush=True)

# 6) подтверждаем hands-free авто-возврат watchdog'ом
log("Жду АВТО-возврат watchdog'ом в U-Boot (~до 45с, БЕЗ передёрга)...")
fd2=catch(45)
if fd2:
    cmd(fd2,f"mw.l {WDT_MODE} 0")   # разоружить WD, чтобы prompt не ресетился по кругу
    log("✅ WATCHDOG вернул плату в U-Boot САМ — передёрг не нужен, плата ждёт в prompt (WD снят).")
    try: os.close(fd2)
    except: pass
else:
    log("⚠ авто-возврат не пойман за 45с — либо ядро живо и тикает дальше, либо завис ДО EBS. Смотри монитор+лог.")
log(f"полный лог: {LOG}")
logf.close()
