# bzdOS microkernel — надёжная OTG-консоль для Allwinner A64 (BPI-M64 "Chimp")

## Зачем
FreeBSD-ядро на A64 заводится **вслепую** (нет UART/HDMI). Штатная gadget-консоль
не энумерируется, потому что опрашивает MUSB только внутри `printf` → хост не
успевает завершить USB-энумерацию. **Микроядро** — крошечный bare-metal AArch64
payload с **непрерывным poll-loop**: хост энумерует гарантированно, консоль стабильна.
Отладив MUSB-инициализацию тут, мы портируем её во FreeBSD (патчим свою ОС).

## Железо / карта памяти (ФАКТЫ, проверено)
- SoC: Allwinner A64, 4×Cortex-A53, ARMv8-A. 2 GiB DRAM @ 0x40000000.
- Грузит и исполняет **U-Boot** (не FEL): U-Boot работает на **EL2, MMU включён**,
  периферия плоско отображена как device-память. → прямой MMIO работает, MMU НЕ трогаем.
- MUSB (USB-OTG контроллер) MMIO: **0x01C19000**, размер 0x4000.
- CCU (clock): **0x01C20000**. BUS_CLK_GATING2 @ +0x6C, бит24 = OTG gate.
- U-Boot оставляет клок MUSB включённым и PHY сконфигуренным (можно не трогать PHY).

## Как грузим (без передёргов между итерациями)
1. Стенд ловит U-Boot `=>` на ACM.
2. `loady 0x42000000` → U-Boot шлёт 'C' (ymodem) → хост `sb` шлёт `microkernel.bin`.
3. `go 0x42000000` → U-Boot **вызывает** payload как функцию на EL2, MMU on,
   на стеке U-Boot. Если payload **возвращается (ret)** — U-Boot продолжает с prompt.
   → можно итерировать (loady новую версию → go) БЕЗ передёрга.

## ABI / calling convention (КРИТИЧНО)
- Точка входа по адресу **0x42000000**, вызывается как `long entry(int argc, char **argv)`.
- Чтобы вернуться в U-Boot чисто:
  - НЕ выключать MMU/кэши; НЕ портить память U-Boot (только своя область + MMIO).
  - Сохранить callee-saved (x19-x30) в прологе, восстановить, `ret`. Возврат в x0.
  - Своя область: код+данные линкуются @0x42000000; свой стек — вниз от 0x42400000
    (4 MiB запас), BSS чистим сами. Всё в пределах 0x42000000..0x42800000.
- Payload — сырой бинарь (`objcopy -O binary`), позиция фиксированная (link @0x42000000),
  без релокаций.

## Сборка
`aarch64-linux-gnu-gcc -ffreestanding -nostdlib -mgeneral-regs-only -march=armv8-a
 -fno-stack-protector -Wall -O2` + линк-скрипт (base 0x42000000) → ELF → `objcopy -O binary`.

## Драйвер MUSB — API (контракт, файл musb.h)
```c
void musb_init(void);   /* device-mode gadget: EP0 + EP1 bulk IN/OUT (CDC-ACM) */
int  musb_poll(void);   /* СЕРВИС USB: энумерация(EP0 SETUP)+RX. Звать НЕПРЕРЫВНО.
                           returns musb_ready(). Должен обрабатывать EP0 на КАЖДОМ
                           вызове вне зависимости от TX — это и есть фикс энумерации. */
int  musb_ready(void);  /* 1 после SET_CONFIGURATION хоста (ttyACM поднялся) */
void musb_putc(int c);  /* байт в TX (авто-flush на '\n' или заполнении) */
void musb_flush(void);  /* принудительный flush TX */
void musb_puts(const char *s);
int  musb_getc(void);   /* принятый байт 0..255, или -1 если нет */
```

## CDC-ACM дескрипторы (минимум, из рабочего usbgadget_console.c)
- Device: VID/PID 0x1d6b/0x0010 (Linux Foundation, произвольный), class=2 (CDC).
- Config: 2 интерфейса (Control CDC + Data), IAD, EP2 IN notify (0x82),
  EP1 IN bulk (0x81) = TX консоли, EP1 OUT bulk (0x01) = RX.
- Регистры MUSB и логика EP0/enumeration — см. эталон
  /opt/bzdos/qemu-test/usbgadget_console.c (портировать в freestanding, БЕЗ pmap:
  адреса физические напрямую, volatile-указатели на 0x01C19000/0x01C20000).

## Дорожки агентов (без коллизий, каждый пишет свои файлы)
- **A (scaffold+build)**: start.S, link.ld, Makefile, main_stage0.c (проверка
  load/exec/return: пишет сигнатуру в 0x42010000, возвращается), main.c (интеграция:
  зовёт musb_* — консольный цикл со стадиями/echo/peek/'q'-возврат).
- **B (usb driver)**: musb.c реализует musb.h (непрерывный poll-loop enumeration).
- **C (host tooling)**: dev-stand recipe "microkernel" (в supervisor.py, отдельная
  ветка) + host-загрузчик loady_over_acm.py (catch U-Boot → loady → sb → go → capture).

## Критерий успеха Stage-1
После `go` в течение ~2с на хосте появляется /dev/ttyACM0, и стенд читает с него
повторяющийся маркер `BZDOS-MK-ALIVE <счётчик>\r\n`. Это доказывает надёжную
энумерацию + консоль по OTG — то, чего FreeBSD-ядро не смогло.
