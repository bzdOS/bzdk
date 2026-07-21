/* main_diag.c — диагностика v2: watchdog-страховка + ограничение по РЕАЛЬНОМУ
 * времени (ARM generic timer), чтобы гарантированно вернуться в U-Boot и дать
 * прочитать "хлебные крошки" @0x42010000. Мы заводим железо вслепую.
 *
 * Стратегия:
 *   - Армируем watchdog A64 (@0x01C20CA0) на ~16с ПЕРВЫМ делом: если что-то
 *     жёстко зависнет/фолтнет и мы не вернёмся — SoC сам ресетнётся в ждущий
 *     U-Boot (bootdelay=-1 уже сохранён).
 *   - Цикл опроса MUSB ограничен ~8с по CNTPCT_EL0 (не по числу итераций —
 *     MMIO медленный, 12M итераций занимали >120с).
 *   - Перед возвратом watchdog ВЫКЛЮЧАЕМ → U-Boot живёт → хост читает крошки.
 *
 * Крошки (u32 @0x42010000): [0]=0xB2D0D1A6 [1]=прогресс(0x11 вошли;0x22 init ок;
 *   0x44 ready;0x55 без ready;0x99 конец) [2]=pre DEVCTL/POWER [3]=post
 *   [6]=итер до ready [7]=финал DEVCTL/POWER [8]=musb_ready [10]=число итераций
 *   [11]=прошло тиков таймера [12]=CNTFRQ.
 */
#include <stdint.h>
#include "musb.h"

#define BCB  ((volatile uint32_t *)0x42010000UL)
#define MUSB 0x01c19000UL
static inline uint8_t r8(uint32_t o){ return *(volatile uint8_t *)(MUSB + o); }
#define REG_POWER  0x01
#define REG_DEVCTL 0x60
#define SNAP() (((uint32_t)r8(REG_DEVCTL) << 8) | r8(REG_POWER))

/* --- ARM generic timer (доступен на EL2 без настройки) --- */
static inline uint64_t cntpct(void){ uint64_t v; __asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(v)); return v; }
static inline uint64_t cntfrq(void){ uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0":"=r"(v)); return v; }

/* --- A64 watchdog @0x01C20CA0 --- */
#define WDT_BASE  0x01c20ca0UL
#define WDT_CTRL  (*(volatile uint32_t *)(WDT_BASE + 0x10))
#define WDT_CFG   (*(volatile uint32_t *)(WDT_BASE + 0x14))
#define WDT_MODE  (*(volatile uint32_t *)(WDT_BASE + 0x18))
static void wdt_arm_16s(void){
    WDT_CFG  = 1;              /* при срабатывании — сброс всей системы */
    WDT_MODE = (11u << 4) | 1; /* интервал idx11=16с, enable */
    WDT_CTRL = (0x0A57u << 1) | 1; /* key + restart (pet) */
}
static void wdt_disable(void){ WDT_MODE = 0; }

long main(void)
{
    wdt_arm_16s();                 /* страховка ПЕРВЫМ делом */

    for (int i = 0; i < 16; i++) BCB[i] = 0;
    BCB[0] = 0xB2D0D1A6;
    BCB[1] = 0x11;
    BCB[2] = SNAP();
    BCB[12] = (uint32_t)cntfrq();

    musb_init();

    BCB[1] = 0x22;
    BCB[3] = SNAP();

    uint64_t f = cntfrq(); if (f < 1000000 || f > 100000000) f = 24000000;
    uint64_t t0 = cntpct();
    uint64_t tend = t0 + f * 8;     /* ~8 секунд опроса */
    int ready = 0; uint32_t iters = 0; uint32_t rdy_iter = 0;
    while (cntpct() < tend) {
        int rdy = musb_poll();
        if (rdy && !ready) { ready = 1; rdy_iter = iters; }
        iters++;
        BCB[10] = iters;
    }
    BCB[11] = (uint32_t)(cntpct() - t0);

    BCB[1] = ready ? 0x44 : 0x55;
    BCB[6] = rdy_iter;
    BCB[7] = SNAP();
    BCB[8] = (uint32_t)musb_ready();

    musb_puts("BZDOS-MK-ALIVE 1\r\n");
    musb_flush();

    BCB[1] = 0x99;                  /* дошли до конца */
    wdt_disable();                  /* чистый выход → U-Boot не ресетить */
    return 0;
}
