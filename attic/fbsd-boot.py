#!/usr/bin/env python3
# fbsd-boot.py — оркестрация загрузки настоящей FreeBSD под наш EL2-гипервизор.
# На U-Boot: autostart off → tftp ядра (16МБ) на 0x44000000 + DTB на 0x4a000000
# → loady нашего hyp-REPL на 0x42000000 → bootelf. Дальше в REPL шлём `fbsd`,
# который парсит ELF ядра, кладёт сегменты на 0x46000000, строит modinfo,
# включает stage-2 и eret'ит в EL1-entry ядра. Наш EL2-обработчик ловит фолт.
# Ядро+DTB (0x44/0x4a) и наш образ (0x42, staging 0x48) не пересекаются.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
HYP="/opt/bzdos/microkernel/microkernel-fbsd.elf"  # auto-boots FreeBSD, no net cmd
KADDR=0x44000000; DTBADDR=0x4a000000; STAGE=0x48000000
IP="192.168.88.7"; SRV="192.168.88.2"
def clean(b): return L._CSI.sub(b'',b).decode('latin1','replace')
def save(d):
    if isinstance(d,str): d=d.encode()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True)
def wput(fd,d,b=1.5):
    t=time.time()
    while time.time()-t<b:
        try: os.write(fd,d); return True
        except OSError: time.sleep(0.05)
    return False
def catch(fd,secs=20):
    buf="";t0=time.time()
    while time.time()-t0<secs:
        wput(fd,b"\r\x03",0.5); b,_=L.rd(fd,0.1)
        if b: buf+=clean(b)
        if "=>" in buf[-30:]: return True
    return False
def cmd(fd,c,w=8,quiet=False):
    L.rd(fd,0.2); wput(fd,(c+"\r").encode()); r="";t=time.time()
    while time.time()-t<w:
        b,_=L.rd(fd,0.3)
        if b:
            r+=clean(b)
            if not quiet: save(b)
        if "=>" in r[-4:] and len(r)>len(c)+4: break
    return r

def wait_port(dl):
    while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.03)
    return os.path.exists(L.TTY)

log("жду U-Boot (плата должна быть на промпте)")
if not wait_port(time.time()+60): log("⛔ порт нет"); sys.exit(2)
if not L.tftp_preflight(): log("⛔ TFTP-сервер не поднялся"); sys.exit(2)
fd=L.open_tty(True)   # serialized via L.acquire_port_lock() inside open_tty()
if not catch(fd,25): log("⛔ '=>' не пойман"); sys.exit(2)
log("✅ U-Boot. autostart off, net setup.")
# autostart YES: needed so `bootelf -p` actually JUMPS to our image (autostart=no
# only loads it). The harmless "Bad FIT" error U-Boot prints after each tftpboot
# (it tries to auto-boot the file as a FIT and fails back to the prompt) is ignored.
cmd(fd,"setenv autostart yes ; setenv ipaddr %s ; setenv serverip %s"%(IP,SRV),3)
log("── TFTP ядра FreeBSD (16МБ, ~55с) на 0x%x ──"%KADDR)
r=cmd(fd,"tftpboot 0x%x kernel"%KADDR,120)
if "Bytes transferred" not in r: log("⛔ tftp ядра не удался"); sys.exit(3)
log("── TFTP DTB на 0x%x ──"%DTBADDR)
r=cmd(fd,"tftpboot 0x%x bananapi-min.dtb"%DTBADDR,30)
if "Bytes transferred" not in r: log("⛔ tftp dtb не удался"); sys.exit(3)
log("── loady + bootelf нашего гипервизора на 0x42000000 ──")
ok,msg=L.do_loady_and_go(fd, save, HYP, addr=STAGE, exec_cmd="bootelf -p 0x%x"%STAGE)
log("loady/bootelf: %s"%msg)
if not ok: log("⛔ загрузка hyp не удалась"); sys.exit(4)
log("✅ гипервизор запущен → АВТО-boot FreeBSD в EL1. Жду фолт+WDT(~6с)→U-Boot.")
try: os.close(fd)
except: pass
# main_fbsd auto-boots the kernel; it faults, WDT ~6s resets to U-Boot. Wait + read breadcrumbs.
dl=time.time()+40
while os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.1)   # port drops at reset
while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.1) # U-Boot back
if not os.path.exists(L.TTY): log("⛔ U-Boot не вернулся"); sys.exit(5)
time.sleep(0.4); fd=L.open_tty(True)
if not catch(fd,25): log("⛔ '=>' после boot не пойман"); sys.exit(5)
def md(a,n):
    L.rd(fd,0.2); wput(fd,("md.l %s %d\r"%(a,n)).encode()); r="";t=time.time()
    while time.time()-t<3:
        b,_=L.rd(fd,0.25)
        if b: r+=clean(b)
        if "=>" in r[-4:] and len(r)>20: break
    return r
log("── FBS1 progress 0x50000e00 (докуда дошёл наш загрузчик) ──"); save(md("0x50000e00",8))
log("── KLD1 kernel-load 0x50000d00 ──"); save(md("0x50000d00",11))
log("── STG2 0x50000c00 ──"); save(md("0x50000c00",7))
log("── EXC fault 0x50000400 (ESR/ELR/FAR/HPFAR) ──"); save(md("0x50000400",13))
log("── UART ring 0x50000f00 (magic/bytes/faults) ──"); save(md("0x50000f00",4))
def mdb(a,n):
    L.rd(fd,0.2); wput(fd,("md.b %s %x\r"%(a,n)).encode()); r="";t=time.time()
    while time.time()-t<6:
        b,_=L.rd(fd,0.25)
        if b: r+=clean(b)
        if "=>" in r[-4:] and len(r)>20: break
    return r
log("── FreeBSD console text (0x50000f10, ASCII column) ──"); save(mdb("0x50000f10",0x200))
log("── GTRC guest trace 0x50002000 (magic/events/last_SCTLR/faults + log) ──"); save(md("0x50002000",24))
try: os.close(fd)
except: pass
log("=== прогон завершён ===")
