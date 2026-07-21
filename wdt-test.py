#!/usr/bin/env python3
# wdt-test.py — безопасная проверка: РЕЗЕТИТ ли аппаратный watchdog A64 плату?
# Плата на U-Boot '=>' (bootdelay=-1). Взводим WDT 16с. Если плата сама ребутнётся
# и вернётся на '=>' — watchdog работает (это наш механизм авто-восстановления после
# зависания ядра). Если за 30с ничего — WDT не резетит, разоружаем, плата цела на '='.
# reboot/reset НЕ шлём. termios-open (nonblock,CLOCAL).
import os,sys,time,termios,select,re
TTY="/dev/ttyACM0"; CSI=re.compile(rb'\x1b\[[0-9;?]*[a-zA-Z]')
LOG="/tmp/wdt-test.log"; logf=open(LOG,"wb")
CFG="0x01c20cb4"; MODE="0x01c20cb8"; CTRL="0x01c20cb0"
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")
def clean(b): return CSI.sub(b'',b).decode('latin1','replace')
def opentty():
    fd=os.open(TTY, os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
    a=termios.tcgetattr(fd)
    a[0]&=~(termios.IGNBRK|termios.BRKINT|termios.PARMRK|termios.ISTRIP|termios.INLCR|termios.IGNCR|termios.ICRNL|termios.IXON)
    a[1]&=~termios.OPOST
    a[3]&=~(termios.ECHO|termios.ECHONL|termios.ICANON|termios.ISIG|termios.IEXTEN)
    a[2]&=~(termios.CSIZE|termios.PARENB); a[2]|=termios.CS8|termios.CLOCAL|termios.CREAD
    a[4]=termios.B115200; a[5]=termios.B115200
    termios.tcsetattr(fd,termios.TCSANOW,a); return fd
def rd(fd,t):
    b=b""; e=time.time()+t
    while time.time()<e:
        r,_,_=select.select([fd],[],[],0.05)
        if r:
            try:
                d=os.read(fd,65536)
                if d: b+=d
            except OSError: return b,True
    return b,False
def cmd(fd,c,w=2.0):
    rd(fd,0.1); os.write(fd,(c+"\r").encode()); time.sleep(0.25)
    b,_=rd(fd,w); save(b); return clean(b)
def at_prompt(fd):
    for _ in range(6):
        rd(fd,0.05); os.write(fd,b"\r"); time.sleep(0.3)
        r=clean(rd(fd,0.6)[0]); save(r)
        if "=>" in r: return True
    return False

if not os.path.exists(TTY): log("⛔ нет ttyACM"); sys.exit(2)
fd=opentty()
log("Проверяю, что мы на U-Boot '=>'...")
if not at_prompt(fd): log("⚠ '=>' не подтверждён — но продолжаю осторожно")
else: log("✓ на '=>'")
cmd(fd,f"mw.l {MODE} 0",1.0)   # снять всё что было
log("Взвожу watchdog: CFG=1(reset system), MODE=0xb1(вкл,16с), CTRL=0x14af(restart)")
cmd(fd,f"mw.l {CFG} 0x00000001",0.8)
cmd(fd,f"mw.l {MODE} 0x000000b1",0.8)
t_arm=time.time()
cmd(fd,f"mw.l {CTRL} 0x000014af",0.8)
log("WDT взведён. Жду reset (ttyACM должен ИСЧЕЗНУТЬ, потом вернуться на '=>')... до 30с")
try: os.close(fd)
except: pass
# ждём исчезновения
gone_at=None
while time.time()-t_arm<30:
    if not os.path.exists(TTY): gone_at=time.time(); break
    time.sleep(0.05)
if gone_at is None:
    log("⛔ за 30с ttyACM НЕ исчез → watchdog НЕ ребутит плату. Разоружаю.")
    if os.path.exists(TTY):
        fd=opentty(); cmd(fd,f"mw.l {MODE} 0",1.0); log("WDT снят, плата на '='."); os.close(fd)
    sys.exit(3)
log(f"✓ ttyACM исчез через {gone_at-t_arm:.1f}с после арма → плата РЕЗЕТНУЛАСЬ! Жду возврат...")
# ждём возврата
back=None
while time.time()-gone_at<25:
    if os.path.exists(TTY): back=time.time(); break
    time.sleep(0.02)
if back is None:
    log("⛔ плата исчезла и не вернулась за 25с — что-то не так"); sys.exit(4)
time.sleep(0.5)
fd=opentty()
if at_prompt(fd):
    log(f"✅✅ WATCHDOG РАБОТАЕТ: плата сама ребутнулась ({gone_at-t_arm:.0f}с) и вернулась на '=>' (bootdelay=-1 пережил reset).")
    log("   → механизм авто-восстановления после зависания ядра ГОДЕН. Физика больше не нужна.")
    cmd(fd,f"mw.l {MODE} 0",1.0)  # снять WDT чтоб не ребутил дальше
    log("WDT снят. Плата на '=>', готова к контролируемому diag+breadcrumb.")
else:
    log("⚠ плата вернулась, но '=>' не подтверждён — проверю отдельно")
os.close(fd); logf.close()
