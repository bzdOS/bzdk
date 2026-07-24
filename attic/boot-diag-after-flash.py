#!/usr/bin/env python3
# boot-diag-after-flash.py — продолжение flash-diag-kernel.py: плата ещё в ums
# (на хосте /dev/sdb, только что записали новый /kernel вручную). Выходим из
# ums (Ctrl-C, без доп. передёрга если получится), затем verbose-boot нового
# ядра с watchdog auto-recovery, читаем DRAM-breadcrumb 0x50000000.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

LOG="/tmp/boot-diag-after-flash.log"; logf=open(LOG,"wb")
WDT_CFG="0x01c20cb4"; WDT_MODE="0x01c20cb8"; WDT_CTRL="0x01c20cb0"
BREADCRUMB=0x50000000

def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

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

log("ums не даёт ttyACM (другая USB-функция) — ПЕРЕДЁРНИ питание, ловлю свежий U-Boot...")
fd=catch(900)
if fd is None: log("⛔ prompt не пойман"); sys.exit(2)
log("✅ U-Boot prompt")
cmd(fd,f"mw.l {WDT_MODE} 0")

r=cmd(fd,"printenv kernel_addr_r fdt_addr_r",1.5)
m=re.search(r'kernel_addr_r=(0x[0-9a-fA-F]+)',r); KA=m.group(1) if m else "0x40080000"
m=re.search(r'fdt_addr_r=(0x[0-9a-fA-F]+)',r);   FA=m.group(1) if m else "0x4fa00000"
log(f"kernel_addr_r={KA} fdt_addr_r={FA}")

loaded=False
for dp in ["mmc 1:2","mmc 2:2","mmc 1:1","mmc 0:1","mmc 2:1"]:
    r=cmd(fd,f"fatload {dp} {KA} efi/boot/bootaa64.efi",4.0)
    if re.search(r'\d+\s+bytes read',r) and not re.search(r'error|unable|bad|not found|failed',r,re.I):
        log(f"✅ loader: {dp} efi/boot/bootaa64.efi"); loaded=True; break
if not loaded:
    log("⛔ bootaa64.efi не найден"); sys.exit(3)

log("Взвожу watchdog (16с) перед bootefi")
cmd(fd,f"mw.l {WDT_CFG} 0x00000001")
cmd(fd,f"mw.l {WDT_MODE} 0x000000b1")
cmd(fd,f"mw.l {WDT_CTRL} 0x000014af")

log(f">>> bootefi {KA} — loader читает /kernel = НАШ ДИАГНОСТ-ВАРИАНТ <<<")
L.wr(fd,f"bootefi {KA}\r\n".encode())

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
    send("lsdev",2)
    for dv in ["disk0p2:","mmcsd1p2:","disk1p2:","mmcsd0p2:"]:
        send(f'set currdev="{dv}"',1); send("unload",1)
        rr=send("load /kernel",10)
        if all(x not in rr.lower() for x in ("error","cannot","fail","not found","no such")):
            log(f"/kernel загружен с {dv}"); break
    send('set boot_verbose=1',1)
    send('set kern.panic_reboot_wait_time=-1',1)
    log(">>> BOOT ДИАГНОСТ-ЯДРА (следи за LED: PE14 зелёный/PD24 красный) <<<")
    send("boot",2)
else:
    log("⚠ не поймал loader countdown — bootcmd дефолт")

log("Захват ttyACM до ExitBootServices...")
t=time.time(); last=time.time()
while os.path.exists(L.TTY) and time.time()-t<90:
    b,dead=L.rd(fd,0.3)
    if b: save(b); last=time.time()
    if dead or time.time()-last>20: break
try: os.close(fd)
except: pass
log("===== ttyACM замолчал = EBS пройден (или упало раньше) =====")

log("Жду авто-возврат watchdog'ом в U-Boot (до 45с)...")
fd2=catch(45)
if fd2 is None:
    log("⛔ авто-возврат не пойман за 45с")
else:
    cmd(fd2,f"mw.l {WDT_MODE} 0")
    log("✅ вернулись — читаю крошки breadcrumb")
    r=cmd(fd2,f"md.l 0x{BREADCRUMB:x} 4",2.0)
    words=[]
    for line in r.splitlines():
        m2=re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{8}\s+){1,4})',line)
        if m2: words+=[int(x,16) for x in m2.group(1).split()]
    log("================ BREADCRUMB ================")
    if len(words)<3 or words[0]!=0xB2D0C0DE:
        log(f"⛔ magic не найдена (words={[hex(w) for w in words[:4]]}) — либо DRAM стёрт, либо не дошли даже до locore")
    else:
        MS={0:"locore _start",1:"post pmap_bootstrap_dmap",2:"post pmap_bootstrap",
            3:"ПЕРЕД bus_probe",4:"ПОСЛЕ bus_probe",5:"post cninit",6:"pre TSEXIT",
            7:"SI_SUB_VM",8:"SI_SUB_CPU",9:"SI_SUB_DEVFS",10:"SI_SUB_ROOT_CONF",11:"init_main pre vfs_mountroot"}
        log(f"[1] последний milestone = {words[1]} → {MS.get(words[1],'?')}")
        log(f"[2] счётчик вызовов = {words[2]}")
        if words[1]==3: log("👉 ГИПОТЕЗА ПОДТВЕРЖДЕНА: хенг ВНУТРИ bus_probe() (CCU/clock/pinmux)")
        elif words[1]>=4: log("👉 bus_probe ПРОЙДЕН — хенг дальше, гипотеза не подтверждена")
    try: os.close(fd2)
    except: pass
log(f"полный лог: {LOG}")
logf.close()
