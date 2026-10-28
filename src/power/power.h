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

/* Монотонное время RTC в мс: идёт и в глубоком сне, сбрасывается только по питанию/RST */
uint64_t power_now_ms(void);

/* true, если системное время установлено (SNTP); иначе файлы на SD не пишем */
bool power_time_valid(void);

/* true, если IO0 удерживается BTN_LONG_MS с момента пробуждения.
   Пока кнопка зажата, LED мигает; при срабатывании горит постоянно. */
bool power_button_long_press(uint32_t ms);
void power_led(bool on);

/* Ждать спада PIR не дольше timeout_ms. false = линия так и осталась в 1 */
bool power_pir_wait_low(uint32_t timeout_ms);

esp_err_t power_sleep(const app_config_t *cfg, uint32_t sleep_s, bool arm_pir);
