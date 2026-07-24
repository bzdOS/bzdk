# bzdOS microkernel — board hardware facts & boot ABI

> **Что это за файл.** Справочник по железу BPI-M64 (Allwinner A64) и по
> контракту «U-Boot → payload», на который опирается текущая сборка. Обзор
> самого проекта и его текущего состояния — в [`README.md`](README.md) и
> [`ROADMAP.md`](ROADMAP.md); это уже **EL2-гипервизор**, грузящий FreeBSD как
> EL1-гость, а не «OTG-консоль» (прежняя формулировка этого файла устарела —
> см. историю git). `link.ld` ссылается сюда за картой памяти и ABI.

## Железо / карта памяти (ФАКТЫ, проверено)
- SoC: Allwinner A64, 4×Cortex-A53, ARMv8-A. 2 GiB DRAM @ 0x40000000.
- Грузит и исполняет **U-Boot** (не FEL): U-Boot работает на **EL2, MMU включён**,
  периферия плоско отображена как device-память. → прямой MMIO работает.
- MUSB (USB-OTG контроллер) MMIO: **0x01C19000**, размер 0x4000.
- CCU (clock): **0x01C20000**. BUS_CLK_GATING2 @ +0x6C, бит24 = OTG gate.
- U-Boot оставляет клок MUSB включённым и PHY сконфигуренным.
- Физического UART/HDMI для отладки нет — вся отладка по EMAC (raw Ethernet,
  ethertype 0x88B5) + USB-OTG CDC-ACM. См. README.

## Как грузим (итерация без физического передёрга)
1. Стенд ловит U-Boot `=>` на ACM.
2. `loady 0x42000000` → U-Boot шлёт 'C' (ymodem) → хост шлёт бинарь.
3. `go 0x42000000` → U-Boot **вызывает** payload как функцию на EL2, MMU on,
   на стеке U-Boot. Если payload **возвращается (ret)** — U-Boot продолжает с
   prompt → можно итерировать (loady новую версию → go) БЕЗ передёрга.

Каноничный путь загрузки автоматизирован в `reliable_load.py` (см. README);
руками `loady`/`go` не гонять.

## ABI / calling convention (КРИТИЧНО — контракт start.S ↔ U-Boot)
- Точка входа по адресу **0x42000000**, вызывается как `long entry(int argc, char **argv)`.
- Чтобы вернуться в U-Boot чисто (нужно для итерации без передёрга):
  - НЕ выключать MMU/кэши; НЕ портить память U-Boot (только своя область + MMIO).
  - Сохранить callee-saved (x19-x30) в прологе, восстановить, `ret`. Возврат в x0.
  - Образ линкуется @0x42000000 (`link.ld`); стек — вниз от вершины RAM-региона.
- Payload — сырой бинарь (`objcopy -O binary`), позиция фиксированная, без релокаций.
- **Stage-2-защита образа**: `link.ld` ASSERT'ит, что образ (text+rodata+data+bss)
  умещается в один 2 MiB L2-блок @0x42000000 — это единственная область, которую
  stage-2 вычитает у гостя (см. `stage2.c` HVIMG_L2_IDX). Вырос образ за 2 MiB —
  сборка падает, а не молча теряет границу изоляции.

## Сборка
`aarch64-linux-gnu-gcc -ffreestanding -nostdlib -mgeneral-regs-only -march=armv8-a
 -fno-stack-protector -Wall -O2 -fno-pic -fno-pie` + `link.ld` (base 0x42000000)
→ ELF → `objcopy -O binary`. Текущая целевая сборка — `make dbg` (см. Makefile).

## Драйвер MUSB — API (контракт, файл musb.h)
Ключевой урок этого драйвера: gadget-энумерация надёжна только при **непрерывном
poll-loop** (`musb_poll()` обслуживает EP0 SETUP на КАЖДОМ вызове, независимо от
TX) — штатная gadget-консоль опрашивала MUSB только внутри `printf` и не
успевала завершить USB-энумерацию. Гипервизор поднимает свою USB-ACM консоль
(`usbacm.c` поверх `musb.c`) именно так.
```c
void musb_init(void);   /* device-mode gadget: EP0 + EP1 bulk IN/OUT (CDC-ACM) */
int  musb_poll(void);   /* сервис USB: энумерация(EP0 SETUP)+RX. Звать НЕПРЕРЫВНО. */
int  musb_ready(void);  /* 1 после SET_CONFIGURATION хоста (ttyACM поднялся) */
void musb_putc(int c);  /* байт в TX (авто-flush на '\n' или заполнении) */
void musb_flush(void);  /* принудительный flush TX */
void musb_puts(const char *s);
int  musb_getc(void);   /* принятый байт 0..255, или -1 если нет */
```

## CDC-ACM дескрипторы
- Device: VID/PID **0x1d6b/0x0010** (это и есть сигнатура HV-консоли, по которой
  `reliable_load.py` отличает её от download-gadget'а U-Boot `1f3a:efe8`).
- Config: 2 интерфейса (Control CDC + Data), IAD, EP2 IN notify (0x82),
  EP1 IN bulk (0x81) = TX консоли, EP1 OUT bulk (0x01) = RX.
