#!/usr/bin/env python3
# hdmi-boot.py — грузит microkernel-hdmi.elf (автономный, без TFTP), демо крутится
# ~16с (смотри монитор!), потом WDT→U-Boot, читаю breadcrumb конвейера 0x50003000.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
HYP="/opt/bzdos/microkernel/microkernel-hdmi.elf"; STAGE=0x48000000
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
def catch(fd,secs=25):
    buf="";t0=time.time()
    while time.time()-t0<secs:
        wput(fd,b"\r\x03",0.5); b,_=L.rd(fd,0.1)
        if b: buf+=clean(b)
        if "=>" in buf[-30:]: return True
    return False
def cmd(fd,c,w=6):
    L.rd(fd,0.2); wput(fd,(c+"\r").encode()); r="";t=time.time()
    while time.time()-t<w:
        b,_=L.rd(fd,0.3)
        if b: r+=clean(b)
        if "=>" in r[-4:] and len(r)>len(c)+4: break
    return r
dl=time.time()+60
while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.1)
time.sleep(0.3); fd=L.open_tty(True)
if not catch(fd,25): log("⛔ '=>' не пойман"); sys.exit(2)
log("✅ U-Boot. autostart yes.")
cmd(fd,"setenv autostart yes",2)
log("── loady + bootelf microkernel-hdmi ──")
ok,msg=L.do_loady_and_go(fd, save, HYP, addr=STAGE, exec_cmd="bootelf -p 0x%x"%STAGE)
log("loady/bootelf: %s"%msg)
if not ok: log("⛔ загрузка не удалась"); sys.exit(3)
log("✅ HDMI demo запущен — СМОТРИ МОНИТОР (~16с). Потом WDT→U-Boot, читаю breadcrumb.")
try: os.close(fd)
except: pass
# wait for WDT (~16s) then read the HDMI pipeline breadcrumb
dl=time.time()+40
while os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.1)
while not os.path.exists(L.TTY) and time.time()<dl: time.sleep(0.1)
time.sleep(0.4); fd=L.open_tty(True)
if not catch(fd,25): log("⛔ '=>' после demo не пойман"); sys.exit(4)
def md(a,n):
    L.rd(fd,0.2); wput(fd,("md.l %s %d\r"%(a,n)).encode()); r="";t=time.time()
    while time.time()-t<3:
        b,_=L.rd(fd,0.25)
        if b: r+=clean(b)
        if "=>" in r[-4:] and len(r)>20: break
    return r
log("── HDMI pipeline breadcrumb 0x50003000 (stage 1-6, 99=timeout) ──")
save(md("0x50003000",8))
try: os.close(fd)
except: pass
log("=== HDMI прогон завершён ===")
