// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "power.h"
#include <time.h>
#include "app_config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

static RTC_DATA_ATTR uint32_t s_boot_count;
#if HAS_LED
static bool s_led_init;
#endif

#define PIR_MASK  (1ULL << PIN_PIR)
#define BLINK_MS  100

static void release_pins(void) {
    if (rtc_gpio_is_valid_gpio(PIN_BTN)) {
        rtc_gpio_deinit(PIN_BTN);
    }
    if (rtc_gpio_is_valid_gpio(PIN_PIR)) {
        rtc_gpio_deinit(PIN_PIR);
    }
}

wake_cause_t power_wake_cause(void) {
    wake_cause_t cause;

    s_boot_count++;

    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER:
        cause = WAKE_TIMER;
        break;
    case ESP_SLEEP_WAKEUP_EXT0:
        cause = WAKE_BUTTON;
        break;
    case ESP_SLEEP_WAKEUP_EXT1:
        cause = (esp_sleep_get_ext1_wakeup_status() & PIR_MASK) ? WAKE_PIR : WAKE_UNKNOWN;
        break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        cause = WAKE_FIRST_BOOT;
        break;
    default:
        cause = WAKE_UNKNOWN;
        break;
    }

    release_pins();
    ESP_LOGI(TAG, "wake cause=%d boot=%u", cause, (unsigned)s_boot_count);
    return cause;
}

uint32_t power_boot_count(void) {
    return s_boot_count;
}

uint64_t power_now_ms(void) {
    return esp_clk_rtc_time() / 1000ULL;
}

bool power_time_valid(void) {
    return time(NULL) > 1700000000;
}

void power_led(bool on) {
#if HAS_LED
    if (!s_led_init) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << PIN_LED,
            .mode = GPIO_MODE_OUTPUT,
        };

        gpio_config(&io);
        s_led_init = true;
    }
    gpio_set_level(PIN_LED, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
#else
    (void)on;
#endif
}

typedef enum { LED_IDLE = 0, LED_BLINK, LED_PULSE } led_mode_t;

static esp_timer_handle_t s_led_tmr;
static volatile led_mode_t s_led_mode = LED_IDLE;
static bool s_blink_on;
static uint32_t s_on_ms, s_off_ms;

static void led_timer_cb(void *arg) {
    (void)arg;
    if (s_led_mode == LED_BLINK) {
        s_blink_on = !s_blink_on;
        power_led(s_blink_on);
        esp_timer_start_once(s_led_tmr, (uint64_t)(s_blink_on ? s_on_ms : s_off_ms) * 1000ULL);
    } else if (s_led_mode == LED_PULSE) {
        s_led_mode = LED_IDLE;
        power_led(false);
    }
}

static bool led_timer_ready(void) {
    if (!s_led_tmr) {
        const esp_timer_create_args_t a = {
            .callback = led_timer_cb,
            .name = "led",
        };

        if (esp_timer_create(&a, &s_led_tmr) != ESP_OK) {
            s_led_tmr = NULL;
            return false;
        }
    }
    return true;
}

static void led_timer_halt(void) {
    s_led_mode = LED_IDLE;
    if (s_led_tmr) {
        esp_timer_stop(s_led_tmr);   /* ошибка, если не запущен, - это нормально */
    }
}

void power_led_blink_start(uint32_t on_ms, uint32_t period_ms) {
    if (!led_timer_ready()) {
        return;
    }
    led_timer_halt();
    if (on_ms == 0 || period_ms <= on_ms) {
        power_led(true);
        return;
    }
    s_on_ms = on_ms;
    s_off_ms = period_ms - on_ms;
    s_blink_on = true;
    s_led_mode = LED_BLINK;
    power_led(true);
    esp_timer_start_once(s_led_tmr, (uint64_t)s_on_ms * 1000ULL);
}

void power_led_blink_stop(void) {
    led_timer_halt();
    power_led(false);
}

void power_led_pulse(uint32_t ms) {
    if (!led_timer_ready()) {
        return;
    }
    led_timer_halt();
    s_led_mode = LED_PULSE;
    power_led(true);
    esp_timer_start_once(s_led_tmr, (uint64_t)ms * 1000ULL);
}

void power_led_wait_idle(void) {
    while (s_led_mode == LED_PULSE) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool power_button_long_press(uint32_t ms) {
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    bool blink = false;

    if (gpio_config(&io) != ESP_OK) {
        return false;
    }
    /* esp_timer считает от старта приложения, т.е. почти от нажатия */
    while (gpio_get_level(PIN_BTN) == 0) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);

        if (t >= ms) {
            power_led(true);
            ESP_LOGI(TAG, "long press %u ms", (unsigned)t);
            return true;
        }
        blink = !blink;
        power_led(blink);
        vTaskDelay(pdMS_TO_TICKS(BLINK_MS));
    }
    power_led(false);
    ESP_LOGI(TAG, "short press %u ms", (unsigned)(esp_timer_get_time() / 1000));
    return false;
}

static bool pir_is_high(void) {
    const gpio_config_t io = {
        .pin_bit_mask = PIR_MASK,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };

    if (gpio_config(&io) != ESP_OK) {
        return false;
    }
    return gpio_get_level(PIN_PIR) != 0;
}

bool power_pir_wait_low(uint32_t timeout_ms) {
    uint32_t t = 0;

    while (pir_is_high()) {
        if (t >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        t += 50;
    }
    return true;
}

esp_err_t power_sleep(const app_config_t *cfg, uint32_t sleep_s, bool arm_pir) {
    esp_err_t err;

    led_timer_halt();
#if HAS_LED
    if (s_led_init) {
        power_led(false);
        gpio_reset_pin(PIN_LED);
    }
#endif
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    if (sleep_s > 0) {
        err = esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * 1000000ULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "timer wake: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (cfg->btn_enabled) {
        err = esp_sleep_enable_ext0_wakeup(PIN_BTN, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ext0: %s", esp_err_to_name(err));
            return err;
        }
        rtc_gpio_pulldown_dis(PIN_BTN);
        rtc_gpio_pullup_en(PIN_BTN);
    }

    if (arm_pir) {
        err = esp_sleep_enable_ext1_wakeup(PIR_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ext1: %s", esp_err_to_name(err));
            return err;
        }
        rtc_gpio_pullup_dis(PIN_PIR);
        rtc_gpio_pulldown_en(PIN_PIR);
    }

    ESP_LOGI(TAG, "sleep timer=%us btn=%d pir=%d", (unsigned)sleep_s, cfg->btn_enabled, arm_pir);
    esp_deep_sleep_start();
    return ESP_FAIL;
}
