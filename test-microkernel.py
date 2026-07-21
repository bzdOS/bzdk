#!/usr/bin/env python3
# test-microkernel.py — ДВА теста в ОДНОЙ U-Boot-сессии (максимум с 1 передёрга):
#   Stage-0: loady microkernel-stage0.bin → go → возврат в U-Boot → md.l 0x42010000
#            → ждём сигнатуру B2D05000 == пайплайн loady/go/RETURN доказан.
#   Stage-1: loady microkernel.bin → go → ловим ре-энумерацию OTG + маркер
#            BZDOS-MK-ALIVE == надёжная USB-консоль работает.
# Переиспользует функции из loady_over_acm.py (агент C).
import os,sys,time
sys.path.insert(0,"/opt/bzdos/microkernel")
import loady_over_acm as L

MK="/opt/bzdos/microkernel"
LOG="/tmp/mk-test.log"
logf=open(LOG,"wb")
def save(d):
    if isinstance(d,str): d=d.encode()
    logf.write(d); logf.flush()
    try: sys.stdout.buffer.write(L._CSI.sub(b'',d)); sys.stdout.flush()
    except: pass
def log(m): print(f"\n[{time.strftime('%H:%M:%S')}] {m}",flush=True); save(f"\n# {m}\n")

log("Жду плату (ПЕРЕДЁРНИ ПИТАНИЕ) — ловлю U-Boot...")
if not L.wait_present(1800): log("⛔ ttyACM не появился"); sys.exit(2)
try: tfd=L.open_tty(True)
except Exception as e: log(f"⛔ open_tty: {e}"); sys.exit(2)
ok,buf=L.catch_uboot(tfd,10); save(buf)
if not ok:
    try: os.close(tfd)
    except: pass
    log("⛔ U-Boot не пойман"); sys.exit(2)
log("✅ U-Boot пойман")

# ── STAGE-0 ──
log("STAGE-0: loady stage0 → go → возврат")
ok,msg=L.do_loady_and_go(tfd,save,f"{MK}/microkernel-stage0.bin")
log(f"stage0 loady/go: {msg}")
sig_ok=False
if ok:
    time.sleep(0.5)
    try: tfd=L.open_tty(True)
    except Exception as e: log(f"reopen: {e}"); tfd=None
    if tfd is not None:
        L.catch_uboot(tfd,5)                       # stage0 вернулся → снова prompt
        L.wr(tfd,b"md.l 0x42010000 4\r\n")
        r,_=L.rd(tfd,2.5); save(r)
        clean=L._CSI.sub(b'',r).lower()
        sig_ok=b"b2d05000" in clean
        log(f"STAGE-0 сигнатура B2D05000: {'✅ ЕСТЬ — пайплайн load/exec/RETURN работает' if sig_ok else '⛔ НЕТ (см. лог md)'}")
else:
    log("stage0 не загрузился — пайплайн под вопросом, но пробую микроядро")

# ── STAGE-1 ──
if tfd is None:
    if not L.wait_present(60): log("⛔ ttyACM пропал перед stage1"); sys.exit(3)
    tfd=L.open_tty(True); L.catch_uboot(tfd,8)
log("STAGE-1: loady микроядро → go → ловлю ре-энумерацию + ALIVE")
ok,msg=L.do_loady_and_go(tfd,save,f"{MK}/microkernel.bin")
log(f"microkernel loady/go: {msg}")
if not ok:
    log(f"⛔ микроядро не загрузилось: {msg}"); sys.exit(3)
v=L.capture_reenum(save,reenum_timeout=15.0,alive_wait=25.0)
log("================ ИТОГ ================")
log(f"STAGE-0 (пайплайн):   {'OK' if sig_ok else 'FAIL/skip'}")
log(f"STAGE-1 (OTG-консоль): {v}")
if v=="ENUMERATED+ALIVE":
    log("🎉 МИКРОЯДРО ЖИВО: надёжная USB-OTG консоль поднялась. Основа готова.")
elif v=="no re-enumeration":
    log("микроядро не переэнумерировало OTG — падает до/в musb_init. Дальше: разбор MUSB-инициализации.")
else:
    log(f"ре-энум был, но ALIVE не пришёл ({v}) — консоль частично; разбор TX/дескрипторов.")
log(f"полный лог: {LOG}")
logf.close()
