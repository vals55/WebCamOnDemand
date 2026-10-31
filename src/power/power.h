// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

typedef enum {
    WAKE_FIRST_BOOT = 0,
    WAKE_TIMER,
    WAKE_PIR,
    WAKE_BUTTON,
    WAKE_RESUME,      /* проснулись по таймеру, но плановых задач нет (перепроверка PIR) */
    WAKE_UNKNOWN,
} wake_cause_t;

wake_cause_t power_wake_cause(void);
uint32_t power_boot_count(void);

/* Причина последнего сброса, не считая пробуждений из глубокого сна (переживает сон):
   poweron, ext, sw, panic, int_wdt, task_wdt, wdt, brownout, ... */
const char *power_reset_reason(void);
/* true, если текущий запуск - авария: panic, watchdog, brownout, lockup */
bool power_reset_abnormal(void);

/* Монотонное время RTC в мс: идёт и в глубоком сне, сбрасывается только по питанию/RST */
uint64_t power_now_ms(void);

/* true, если системное время установлено (SNTP); иначе файлы на SD не пишем */
bool power_time_valid(void);

/* true, если IO0 удерживается BTN_LONG_MS с момента пробуждения.
   Пока кнопка зажата, LED мигает; при срабатывании горит постоянно. */
bool power_button_long_press(uint32_t ms);
void power_led(bool on);

/* Серверный режим: короткая вспышка LED_SRV_ON_MS раз в LED_SRV_PERIOD_MS (esp_timer, без отдельной задачи) */
void power_led_blink_start(uint32_t on_ms, uint32_t period_ms);
void power_led_blink_stop(void);   /* останавливает мигание и гасит LED */

/* Одиночная вспышка: LED горит ms миллисекунд и гаснет сам (PIR) */
void power_led_pulse(uint32_t ms);
void power_led_wait_idle(void);    /* дождаться конца вспышки перед сном */

/* Ждать спада PIR не дольше timeout_ms. false = линия так и осталась в 1 */
bool power_pir_wait_low(uint32_t timeout_ms);

esp_err_t power_sleep(const app_config_t *cfg, uint32_t sleep_s, bool arm_pir);
