# Chimp / BPI-M64 — session handoff (2026-07-16)

**Прочти ПЕРВЫМ.** Это полный контекст сессии для любого агента, который
продолжит работу. Файлы: `/opt/bzdos/microkernel/`. Плата: Banana Pi M64
(Allwinner A64, 4× Cortex-A53). Гипервизор на EL2, FreeBSD arm64 — гость на EL1.

## 📏 ПРАВИЛА РАБОТЫ → `SESSION-RULES.md` (читай ПЕРВЫМ)

Операционные правила (ресеты, live-фикс, watchdog, chimpd) вынесены в отдельный
канонический документ **`SESSION-RULES.md`** — прочти его до любых действий с платой.
Краткая выжимка ниже.

## 🚫 ПРАВИЛО №1: НИКАКИХ РЕСЕТОВ РУКАМИ ПОЛЬЗОВАТЕЛЯ

Не предлагать и не делать `wdt_reset`/power-cycle/reload. Вся инфра (EMAC-отладчик
`dbgmon`: `sw`/`patch`/`call`/`w`/`gr`/`sr2`, `hvdbg.py`) построена чтобы чинить
живой HV ПО СЕТИ без ресетов. Любой фикс — сперва живьём (`sw`/`patch`/`call`),
и только если нужно в бинарь навсегда — правим исходник и ждём СЛЕДУЮЩЕЙ
естественной загрузки (её триггерит пользователь, не агент).

## TL;DR — на чём стоим (обновлено 2026-07-16)

FreeBSD arm64 грузится под гипервизором, проходит EHCI/USB и probe PCI.

**Свежая победа (2026-07-16):** найден и убит ЖИВЬЁМ (по сети, без ресета)
interrupt storm на **INTID 106 = EHCI USB** (SPI 74, level-high). При `IMO=1`
физ. прерывание уходило в EL2, `gic_timer_irq` делал EOI но не гасил устройство
и не форвардил гостю → шторм ~145 кГц → гость голодал в `ehci_reset()`.
Фикс = снять `HCR_EL2.IMO/FMO` (`sw hcr 0x84000003` живьём; в исходник —
убраны `vgic_init`/`gic_timer_init` из `main_dbg.c`, IMO=0). Шторм 145к/с → 0,
консоль 18519 → 20343 байт. Прерывания устройств идут напрямую гостю (`IMO=0`),
`dbgmon` опрашивается в vconsole-ловушках UART0 (таймер EL2 не нужен).

**Текущий блокер (2026-07-16):** гость залипает в poll-цикле на **UART1**
(`serial@1c28400`, тот же 4КиБ-page что UART0). vconsole эмулировал только UART0
(регистры на offset 0x000); чтение IIR UART1 (page-offset 0x408) промахивалось
мимо `off==UART_REG_IIR` → возвращало 0 (= «int pending») → ns8250 ISR крутится
вечно. **Фикс в исходнике готов** (`vconsole.c`: `reg = off & 0x3FF` сворачивает
UART0/1/2 на единую 16550-раскладку), собирается чисто — применится на следующей
загрузке. Живьём не ткнуть (это логика обработчика, не сисрег).

```
BROM → SPL → BL31 → U-Boot → [наш HV: loady + bootelf] → FreeBSD@EL1 (IMO=0)
         │                                        │
         └─ anti-brick: SPL@8KiB даёт U-Boot      └─ HV опрашивает EMAC в traps:
            всегда (никирпичить софтом нельзя)       dbgmon жив за счет UART-опроса гостя
```

## Корневая причина зависаний (исправлена)

1. **Watchdog Disable**: гостевой драйвер `aw_wdog.c` при загрузке отключал аппаратный watchdog, взведенный в EL2. Исправлено отключением узла watchdog в дереве устройств (`status = "disabled"` в DTS).
2. **Interrupt Storm (EHCI/MMC)**: включенное прерывание таймера на стороне EL2 (`IMO = 1`) приводило к шторму прерываний гостя. Исправлено полным пробросом прерываний гостю (`IMO = 0`, `gic_timer_init()` отключен). Отладчик переведен на поллинг внутри Stage-2 ловушек UART0 (`vconsole_handle_fault`).

## Текущее состояние платы

- **Гость активен**: гость успешно прошел probes (включая MMC и USB EHCI) и опрашивает виртуальный UART0 в цикле (регистры IIR, MSR и др.).
- **USB**: ttyCHIMP отсутствует (HV сделал `usb_gadget_disconnect` при старте).
- **Не сброшена**: отладчик полностью доступен через EMAC прямо сейчас, гость выполняется live.

## Текущий билд: microkernel-dbg.bin (21:21)

Содержит 4 hot-debug команды (новые в этой сессии):

| Команда | Что делает |
|---|---|
| `sr2` | дамп EL2 сисрегов (CNTHCTL, HCR, VTCR, VTTBR, CPTR, SCTLR_EL2) |
| `sw <name> <val>` | запись сисрега LIVE (cnthctl, hcr, sctlr1/2, vtcr, vttbr...) |
| `call <pa> [x0..x3]` | вызов функции гипервизора по PA (валидация .text + crash recovery → 0xDEAD) |
| `patch <pa> <word>` | запись инструкции + I-cache flush (hot-patch) |

Кодовые изменения в этом билде:
- `main_dbg.c`: отключены `gic_timer_init()` и прерывания на EL2 (`daifclr`), занулен `cntvoff_el2`.
- `el2_exc.c`: добавлен опрос `dbgmon_service()` внутри `vconsole_handle_fault` для работы отладки без прерываний таймера.
- `bananapi-min.dts`: отключен watchdog (`status = "disabled"`), добавлен `/memory`.

## Переиспользуемые инструменты (созданы в этой сессии)

### `hvdbg.py` — Python-библиотека для EMAC-отладки
```python
import sys; sys.path.insert(0,'/opt/bzdos/microkernel')
from hvdbg import HV
hv = HV()
hv.cmd('gr')                  # гостевые регистры
hv.cmd('sr2')                 # EL2 сисреги
hv.read_words(pa, n)          # чтение physical memory
hv.dump(pa, length)           # hex+ascii дамп
hv.write_word(pa, val)        # запись physical memory
hv.patch(pa, insn)            # hot-patch инструкции
hv.call(fn_pa, a0, a1, a2, a3)  # RPC в гипервизор
hv.wdt_reset()                # сброс платы через watchdog
hv.gict() / hv.vconsole() / hv.ffv() / hv.gr() / hv.sr2()  # breadcrumb shortcuts
hv.walk_stage2(ipa)           # walk stage-2 page tables (учитывает VTCR.SL0=1)
hv.reenter_guest()            # перезапуск гостя без ресета (ВНИМАНИЕ: см. ниже)
```

### `chimpd.py` — автономный supervisor
Ловит плату (ttyCHIMP), льёт kernel+DTB (TFTP), loady HV ELF, мониторит
breadcrumbs, логает консоль. `--no-reset` чтобы не сбрасывать при hang.
Запуск: `cd /opt/bzdos/microkernel && python3 chimpd.py --once --no-reset`

### udev rule `/dev/ttyCHIMP`
`/etc/udev/rules.d/99-chimp.rules` — symlink на ttyACM для обоих gadget ID
(U-Boot 1f3a:efe8, наш 1d6b:0010). Появляется когда плата на U-Boot.

## DELAY()-hang — что известно

**Симптом:** гость доходит до device probes (18КБ консоля), потом ELR
замерзает в теле `DELAY()` (busy-wait). Таймер HV жив (GST1 ring растёт).

**Данные из live-чтения:**
- CNTHCTL_EL2 = 0x3 (от ATF, не от нас)
- CNTP_CTL_EL0 = 0x1 (ENABLE, от нашего gic_timer_init)
- CNTPCT доступен гостю нативно (CNTHCTL=0x3 → нет трэпа)
- ELR в DELAY body: 0xffff00000091bec4 / 0xffff00000091b698
- CNTV_CTL_EL0 = 0 (гостевой virtual timer выключен — не наша зона)

**Гипотеза (из PROGRESS.md):** DELAY() computes deadline from CNTFRQ_EL0 ×
delay_count. Либо CNTFRQ неправильный → абсурдно большой deadline, либо
counter не advancing с точки зрения гостя (но CNTHCTL=0x3 → должен).

**Следующий шаг:** через `hv.cmd('sr')` прочитать гостевые timer regs.
Проверить CNTFRQ_EL0 (ожидается ~24MHz для A64). Если 0 или мусор —
калибровка ломается. Можно также `sw` для live-тоглирования.

**Путь в FreeBSD-исходниках:**
`/opt/bzdos/build/freebsd-src/sys/arm64/arm64/machdep.c` — initarm,
DELAY. sparse-checkout: `sys/arm64/arm64`, `sys/kern`, `sys/sys`.

## USB (MUSB) — что делать

**Цель:** MUSB CDC-ACM гаджет как второй канал консоли (виден как
`/dev/ttyCHIMP` на хосте, без raw-ethernet инструментов).

**Что есть:** `musb.c` (1589 строк, полный драйвер), API: `musb_init` /
`musb_poll` / `musb_putc` / `musb_getc`. В REPL-сборке работает. В DBG —
отсутствует.

**Предыдущая попытка (провалилась):** добавил musb.o в DBG_OBJS + dual
console → 0 байт консоля. Но корневая причина была НЕ musb, а
отсутствующий `/memory`. Теперь `/memory` восстановлен — MUSB можно
пробовать снова.

**План интеграции:**
1. `Makefile` DBG_OBJS: добавить `musb.o`
2. `main_dbg.c`: `#include "musb.h"`, `musb_init()` после EMAC, brief poll
3. `el2_exc.c`: добавить weak stub `musb_poll()`, вызывать на тике
4. Console hooks: `console_putc` → `emac_putc + musb_putc` (dual)
5. **STAGE-2 защита:** MUSB MMIO (0x01c19000) и PHY (0x01c19400) — в той
   же 4К странице, можно добавить в `stage2_l3_uart[]` (invalid entry →
   trap guest access). FreeBSD's `musb_otg` не должен трогать наши
   регистры. См. `stage2.c` `stage2_build_mmio_tables()`.

**Риск:** если FreeBSD пробует musb_otg и получает stage-2 fault — нужен
обработчик (emulate-nop) или просто отключить musb в DTB (`status =
"disabled"` для `usb@1c19000`).

## HDMI / HUD — что делать

**Цель:** heads-up display на HDMI — живой дашборд с гостевым консолем,
регистрами, breadcrumbs, profiler. Для работы без UART/USB (только монитор).

**Что есть:** `hdmi.c` (979 строк, форсит 1920×1080@60), `fb.c`
(primitives + font), `hud.c` (1034 строки, композитор). В REPL-сборке
работает. Breadcrumb: `0x50003000` (magic "HDMI").

**План интеграции:**
1. `Makefile` DBG_OBJS: добавить `hdmi.o fb.o hud.o`
2. `main_dbg.c`: `#include "hdmi.h"`, `#include "hud.h"`,
   `hdmi_init(); hud_init();` после EMAC
3. Tick path: `hud_update(frame)` на каждом тике (или в dbgmon_service)
4. **Caveat:** framebuffer at `0x4D000000` (8МБ) — FreeBSD может
   аллоцировать эту память (она в /memory range). Нужно зарезервировать
   через DTB `/reserved-memory` или уменьшить /memory reg.

## Долги (из ROADMAP.md)

| # | Что | Статус | Приоритет |
|---|---|---|---|
| DELAY() hang | Гость висит в busy-wait после device probes | **ТЕКУЩИЙ БЛОКЕР** | высокий |
| MUSB console | Второй канал консоли (USB CDC-ACM) | код есть, интеграция pending | высокий |
| HDMI/HUD | Live-дашборд на мониторе | код есть, интеграция pending | средний |
| virtio-blk rootfs | root по сети (без SD card) | virtio.c/virtio_blk.c есть, не подключены | высокий (после DELAY) |
| vconsole generalize | trap-and-log для произвольных MMIO range | только UART0 сейчас | средний |
| vgic timer | forward CNTV guest interrupt | revert (регрессия), нужен другой подход | низкий |

## Файлы — шпаргалка

```
/opt/bzdos/microkernel/
  main_dbg.c        — точка входа HV-отладчика (что инициальзируется, что вызывается)
  dbgmon.c          — сетевой отладчик (команды, диспетч, call/patch/sw/sr2)
  el2_exc.c         — EL2 trap handler (tick → gic_timer_irq → musb_poll → dbgmon → sched)
  gic_timer.c       — GIC + CNTP tick (CNTHCTL НЕ трогаем, 0x3 от ATF)
  kload.c/h         — парсинг FreeBSD ELF, placement, modinfo, enter
  stage2.c          — IPA→PA identity translation (DRAM=Normal-WB, MMIO=Device, UART trap)
  vconsole.c        — trap-and-emulate UART0 (захват гостевого консоля)
  gtrace.c          — TVM trap-and-emulate EL1 MMU regs + VBAR trampoline (ffv)
  musb.c/h          — CDC-ACM гаджет (не в DBG-сборке)
  hdmi.c/h          — HDMI 1920×1080 forced (не в DBG-сборке)
  fb.c/h            — framebuffer primitives + 8×8 font
  hud.c/h           — hypervisor dashboard compositor
  reboot.c          — usb_gadget_disconnect() + WDT reset
  onebp.c/h         — one-shot HVC breakpoint (disarmed, но linked)
  firstfault.c/h    — vector first-fault catcher (ffv)
  hwbp.c/h          — hardware breakpoints/watchpoints (PSTATE.D masked early boot)
  hvdbg.py          — ← ТЫ БУДЕШЬ ИСПОЛЬЗОВАТЬ ЭТО
  chimpd.py         — автономный supervisor
  loady_over_acm.py — USB loader (library, используется chimpd)
  PROGRESS.md       — полная хронология баг-охоты (читай!)
  ROADMAP.md        — Tier 1/2/3 план
  link.ld           — 0x42000000, _text_start/_text_end

/opt/bzdos/tftpboot/
  kernel            — FreeBSD arm64 kernel ELF (15.9MB, от 07.07)
  bananapi-min.dtb  — DTB (с /memory, 42940 байт)

/opt/bzdos/build/
  bananapi-min.dts  — DTB source (с /memory)
  freebsd-src/      — sparse checkout FreeBSD releng/15.1
```

## Breadcrumb map (DRAM, доступно через `r`/`d`/`dump`)

| Адрес | Magic | Что |
|---|---|---|
| 0x50000000 | — | MUSB progress |
| 0x50000100 | — | EMAC progress |
| 0x50000300 | MLR1 | REPL |
| 0x50000400 | EXC1 | EL2 exception record |
| 0x50000500 | — | jitter/TIMR |
| 0x50000700 | BTR1 | backtrace |
| 0x50000800 | GICT | GIC timer ticks (words: magic/ticks_lo/hi/last_iar/period/mismatches/init_done/ctl_live) |
| 0x50000a00 | — | SCHED |
| 0x50000b00 | GST1 | guest preempt counter |
| 0x50000c00 | STG2 | stage-2 (magic/vtcr/hcr/ndesc/selfcheck) |
| 0x50000d00 | KLD1 | kload (elf_valid/n_seg/kernel_end/entry_pa/kernend_va) |
| 0x50000e00 | DBG1 | main_dbg boot checkpoints |
| 0x50000f00 | UART | vconsole capture ring (magic/total_bytes/fault_count + ring data at +0x10) |
| 0x50002000 | GTRC | gtrace TVM trap ring |
| 0x50002400 | — | firstfault level-1 |
| 0x50002800 | SST1 | single-step ring |
| 0x50003000 | HDMI | HDMI bring-up |
| 0x50005800 | FF1V | vector first-fault (original guest EL1 fault: ESR/ELR/FAR/SP/SPSR + GPRs) |
| 0x50007000 | 1BP1 | onebp snapshot |

## Чек-листы

### Загрузить HV на плату (когда плата на U-Boot)
```bash
cd /opt/bzdos/microkernel
python3 chimpd.py --once --no-reset
# или вручную:
python3 dbg-boot.py   # TFTP kernel+DTB, loady+bootelf
```

### Пересобрать HV
```bash
cd /opt/bzdos/microkernel && make clean && make dbg
```

### Сбросить плату через EMAC (когда HV жив)
```python
from hvdbg import HV
hv = HV(); hv.wdt_reset()
# или: repl-cmd.py "w 0x01c20cb4 1" "w 0x01c20cb8 21" "w 0x01c20cb0 14af"
```

### Живая отладка (HV на плате)
```python
from hvdbg import HV
hv = HV()
hv.cmd('t')              # summary всех breadcrumbs
hv.cmd('gr')             # гостевые регистры
hv.cmd('sr2')            # EL2 сисреги
hv.vconsole_text(512)    # гостевый консоль
hv.cmd('ffv')            # first-fault (если был)
# копать DELAY():
hv.cmd('sr')             # CNTFRQ/CNTP_CTL/CNTV_CTL гостя
```

### Грабли (не наступать)
- **`gva` показывает IPA (stage-1 output), не финальный PA.** Stage-2 дальше
  транслирует IPA→PA. Используй `hv.walk_stage2(ipa)` для полного walk.
  VTCR.SL0=1 → walk стартует с L1 (не L0).
- **`pt <table> <va>` НЕ учитывает VTCR.SL0.** Добавляет лишний L0 уровень.
  Не используй для stage-2 (только для stage-1 гостя).
- **`call` на noreturn-функцию (kload_enter)** — eret происходит, call не
  возвращается. EMAC замолкает до следующего тика. Но если MMU гостя
  включён — eret в EL1 с VA=PA → fault. Нужен `sw sctlr1 0` перед call,
  ПЛЮС TLB flush (TLBI VMALLS12E1IS). Проще: добавить `restart`-команду
  в dbgmon (полный re-init + re-enter).
- **`/memory` узел в DTB** — если пропадёт при пересборке, ВСЁ ломается
  (panic до консоля). Проверять: `fdtget -l bananapi-min.dtb / | grep memory`.
- **Дублирующий .dts** в `/opt/bzdos/build/` может быть устаревшим
  (не совпадать с `/opt/bzdos/tftpboot/bananapi-min.dtb`). Канон — .dtb в tftpboot.
- **proxy env на хосте** — `http_proxy`/`https_proxy` мешают локальным
  запросам. Для EMAC/TFTP не критично (raw L2), но для `dnsmasq`/`curl` —
  обнулять.

## Что дал агент-research (параллельный)

- **MUSB**: DTB имеет `usb@1c19000` с `dr_mode="otg"` — FreeBSD будет
  пробовать musb_otg. Нужен stage-2 trap (invalid page 0x01c19000) или
  `status="disabled"` в DTB. MUSB+PHY на одной 4К странице (L3 idx 25 в
  том же L2 блоке что UART0).
- **HDMI/HUD**: `hdmi_init()` форсит 1920×1080 без EDID. Framebuffer
  0x4D000000 (8МБ) — FreeBSD может затереть (нужно reserved-memory в DTB).
  hud_update(frame) на тике даёт live-дашборд.
- **CODE TODO/FIXME**: нет ни одного в коде. Все долги — в ROADMAP/PROGRESS.
