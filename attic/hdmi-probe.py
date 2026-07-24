#!/usr/bin/env python3
# hdmi-probe.py — БЕЗ передёрга (использует уже живой U-Boot). Проверяет главную
# гипотезу нового плана: можно ли отдать FreeBSD ЖИВОЙ HDMI-фреймбуфер (EFI GOP),
# чтобы её штатная efifb/vt-консоль пережила ExitBootServices и показала, где виснет.
#
# Ничего не пишет в persist (saveenv НЕ вызывается), FreeBSD НЕ грузит — только:
#   printenv (stdout/stdin/video/console/splash) | coninfo (есть ли vidconsole?)
#   bdinfo (Video active/inactive + карта памяти) | попытка активировать vidconsole
#   (cls;echo) | повторный bdinfo. Всё обратимо.
import os,sys,time,re
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L
LOG="/tmp/hdmi-probe.log"; logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

def catch_prompt(total):
    """Ловим живой '=>' через реконнекты. Возвращает fd или None."""
    dl=time.time()+total
    while time.time()<dl:
        if not os.path.exists(L.TTY): time.sleep(0.15); continue
        try: fd=L.open_tty(True)
        except Exception: time.sleep(0.3); continue
        for _ in range(6):
            L.wr(fd,b"\r\n"); time.sleep(0.12)
            b,_=L.rd(fd,0.5)
            if b"=>" in L._CSI.sub(b'',b): return fd
        try: os.close(fd)
        except: pass
        time.sleep(0.3)
    return None

def cmd(fd,c,wait=2.0):
    L.wr(fd,c.encode()+b"\r\n"); time.sleep(0.35); b,_=L.rd(fd,wait); save(b)
    return L._CSI.sub(b'',b).decode('latin1','replace')

log("Ловлю ЖИВОЙ U-Boot (без передёрга, 25с). Если не отвечает — попрошу 1 передёрг.")
fd=catch_prompt(25)
if fd is None:
    log("U-Boot молчит → нужен ОДИН передёрг (bootdelay=-1 → замрёт в prompt). Жду до 5 мин...")
    fd=catch_prompt(300)
if fd is None:
    log("⛔ prompt так и не появился"); sys.exit(2)
log("✅ U-Boot prompt")

# 1) окружение — что сейчас настроено на вывод
r=cmd(fd,"printenv",3.0)
keys=[l for l in r.splitlines() if re.match(r'\s*(stdout|stderr|stdin|console|video|splash|panel|hdmi|bootargs|fdtfile|boot_efi|bootcmd)\b',l,re.I)]
log("ENV (вывод/видео/boot):\n  "+("\n  ".join(keys) if keys else "(ключей не видно — см. лог)"))

# 2) зарегистрированные stdio-устройства: есть ли vidconsole? (значит DE2 пробован)
r=cmd(fd,"coninfo",2.0)
has_vid="vidconsole" in r.lower()
log(f"coninfo → vidconsole {'ЕСТЬ (дисплей пробован драйвером)' if has_vid else 'НЕ зарегистрирован (DE2 не поднят)'}")
save(("\n[coninfo raw]\n"+r+"\n").encode())

# 3) статус видео сейчас
r=cmd(fd,"bdinfo",2.5)
vline=[l.strip() for l in r.splitlines() if re.search(r'video',l,re.I)]
active_now = any("inactive" not in v.lower() for v in vline) and bool(vline)
log(f"bdinfo Video: {' | '.join(vline) if vline else '(строки нет)'}  → {'АКТИВЕН' if active_now else 'НЕактивен'}")

# 4) попытка активировать вывод на экран (обратимо, без saveenv)
if has_vid and not active_now:
    log("Пробую активировать vidconsole (setenv stdout serial,vidconsole; cls; echo)...")
    cmd(fd,"setenv stdout serial,vidconsole",1.5)
    cmd(fd,"setenv stderr serial,vidconsole",1.5)
    cmd(fd,"cls",1.0)
    cmd(fd,"echo ==BZDOS-HDMI-TEST==",1.0)
    r=cmd(fd,"bdinfo",2.5)
    vline=[l.strip() for l in r.splitlines() if re.search(r'video',l,re.I)]
    active_now = any("inactive" not in v.lower() for v in vline) and bool(vline)
    log(f"после активации Video: {' | '.join(vline) if vline else '(нет)'} → {'АКТИВЕН ✅' if active_now else 'всё ещё НЕактивен'}")
    if active_now:
        log("👉 ПОСМОТРИ НА МОНИТОР: видна ли надпись '==BZDOS-HDMI-TEST=='? (это докажет живой FB)")

log("================ ВЕРДИКТ ================")
if active_now:
    log("HDMI-фреймбуфер поднимается в U-Boot → есть шанс отдать FreeBSD живой GOP.")
    log("Дальше: убедиться что GOP доходит до efifb (грузим FreeBSD и СМОТРИМ на экран).")
else:
    log("DE2 не активируется тривиально. Нужен разбор video-драйвера U-Boot / EDID,")
    log("либо запасной канал: color-bars прямо в FB + LED-мигание из пропатченной FreeBSD.")
log(f"vidconsole={has_vid}  video_active={active_now}")
log(f"полный лог: {LOG}")
try: os.close(fd)
except: pass
logf.close()
