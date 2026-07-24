#!/usr/bin/env python3
# fix-bootdelay.py — РАЗ И НАВСЕГДА разрубить кольцо передёргиваний.
# Единственная задача: поймать U-Boot '=>' (агрессивный флуд Ctrl-C с нулевой
# задержкой на появление ttyACM) и записать bootdelay=-1 + saveenv. После этого
# U-Boot ВСЕГДА замирает в prompt → все дальнейшие ребуты делаются софтом (`reset`),
# питание не трогаем. Ловит на любом флапе/ребуте платы; если плата молчит (уже
# за EBS) — ждёт следующего её ребута/флапа, ловить будет мгновенно.
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

LOG="/tmp/fix-bootdelay.log"; logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

def clean(b): return L._CSI.sub(b'',b)

log("Охочусь за U-Boot '=>' (флуд Ctrl-C). Поймаю на ЛЮБОМ ребуте/флапе платы.")
log("Если плата сейчас молчит (за EBS) — ОДИН передёрг/reset её, и это ПОСЛЕДНИЙ.")
DEADLINE=time.time()+1800   # 30 мин
fixed=False; last_node_gone=True
while time.time()<DEADLINE and not fixed:
    if not os.path.exists(L.TTY):
        last_node_gone=True; time.sleep(0.02); continue
    # ttyACM только что появился (или уже есть) — открываем МГНОВЕННО и молотим Ctrl-C
    try: fd=L.open_tty(True)
    except Exception: time.sleep(0.05); continue
    buf=b""; t0=time.time()
    while time.time()-t0<8:
        try: os.write(fd,b"\x03")
        except OSError: break
        b,dead=L.rd(fd,0.03)
        if b: buf+=b; save(b)
        if dead: break
        if b"=>" in clean(buf)[-60:]:
            # ПОЙМАЛИ U-Boot. Фиксируем bootdelay=-1.
            log("✅ U-Boot '=>' пойман! Пишу bootdelay=-1 + saveenv")
            os.write(fd,b"\r\n"); time.sleep(0.3); save(L.rd(fd,0.8)[0])
            os.write(fd,b"setenv bootdelay -1\r\n"); time.sleep(0.4); save(L.rd(fd,1.0)[0])
            os.write(fd,b"saveenv\r\n"); time.sleep(1.0); r=L.rd(fd,3.0)[0]; save(r)
            rc=clean(r).decode('latin1','replace')
            if any(x in rc for x in ("OK","done","Written","Valid","Saving")):
                log("✅✅ bootdelay=-1 СОХРАНЁН. Теперь U-Boot всегда ждёт в prompt.")
            else:
                log(f"⚠ saveenv неоднозначно (…{rc[-100:]!r}); bootdelay применён на сессию, персист под вопросом")
            # подтверждаем текущий bootdelay
            os.write(fd,b"printenv bootdelay\r\n"); time.sleep(0.4); save(L.rd(fd,1.0)[0])
            fixed=True
            try: os.close(fd)
            except: pass
            break
    if fixed: break
    try: os.close(fd)
    except: pass
    time.sleep(0.05)

if fixed:
    log("ГОТОВО. Дальше — только софт: `reset` для ребута, загрузка нашего ядра, md breadcrumb.")
    log("Питание больше НЕ дёргаем.")
else:
    log("⛔ за 30 мин U-Boot не пойман (плата не ребутилась/флап не дал окна)")
    sys.exit(2)
logf.close()
