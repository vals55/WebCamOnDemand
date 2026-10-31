// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "config.h"
#include "power.h"
#include "capture.h"
#include "cam.h"
#include "event_log.h"
#include "wifi_sta.h"
#include "portal.h"
#include "server.h"
#include "app_mqtt.h"
#include "ha_discovery.h"

static const char *TAG = "main";

/* Расписание в монотонном времени RTC (мс), переживает глубокий сон */
static RTC_DATA_ATTR bool s_sched_ok;
static RTC_DATA_ATTR uint64_t s_next_timer_ms;
static RTC_DATA_ATTR uint64_t s_next_srv_ms;
static RTC_DATA_ATTR uint64_t s_pir_until_ms;
static RTC_DATA_ATTR uint32_t s_pir_backoff_s;

#define DUE_SLACK_MS 1000

/* Повтор синхронизации времени при отсутствии сети: 5 мин -> x2 -> 1 ч */
static RTC_DATA_ATTR uint64_t s_time_retry_ms;
static RTC_DATA_ATTR uint32_t s_time_backoff_s;
#define TIME_BACKOFF_MIN 300
#define TIME_BACKOFF_MAX 3600
#define TIME_SYNC_MS     8000

static void sched_reset(const app_config_t *c, uint64_t now) {
    s_next_timer_ms = now + (uint64_t)c->timer_interval_s * 1000;
    s_next_srv_ms = now + (uint64_t)c->srv_period_s * 1000;
    s_pir_until_ms = 0;
    s_pir_backoff_s = 0;
    s_sched_ok = true;
}

/* Наступил ли срок; если да - сдвинуть на следующий период */
static bool sched_due(uint64_t *next, uint32_t period_s, uint64_t now) {
    uint64_t per = (uint64_t)period_s * 1000;

    if (*next > now + per) {
        *next = now + per;          /* период уменьшили или RTC сбился */
    }
    if (now + DUE_SLACK_MS < *next) {
        return false;
    }
    *next += per;
    if (*next <= now) {
        *next = now + per;          /* пропустили несколько периодов */
    }
    return true;
}

/* Время нужно для имён папок/файлов. Нет времени -> разовое подключение к сети.
   Нет сети -> false (файлы не пишем), следующая попытка не раньше backoff. */
static bool ensure_time(const app_config_t *c) {
    int rssi = 0;

    if (power_time_valid()) {
        return true;
    }
    if (!config_has_wifi(c) || power_now_ms() < s_time_retry_ms) {
        return false;
    }
    if (net_init() == ESP_OK && wifi_sta_connect(c, &rssi) == ESP_OK) {
        net_time_sync(c, TIME_SYNC_MS);
        wifi_sta_stop();
    }
    if (power_time_valid()) {
        s_time_backoff_s = 0;
        return true;
    }
    s_time_backoff_s = s_time_backoff_s ? s_time_backoff_s * 2 : TIME_BACKOFF_MIN;
    if (s_time_backoff_s > TIME_BACKOFF_MAX) {
        s_time_backoff_s = TIME_BACKOFF_MAX;
    }
    s_time_retry_ms = power_now_ms() + (uint64_t)s_time_backoff_s * 1000;
    ESP_LOGW(TAG, "time sync failed, retry in %us", (unsigned)s_time_backoff_s);
    return false;
}

static esp_err_t nvs_init(void) {
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static bool want_mqtt(const app_config_t *c, bool server) {
    switch (c->mqtt_policy) {
    case MQTT_EACH:
        return true;
    case MQTT_BATCH:
        return server || event_log_count() >= c->mqtt_batch_n;
    case MQTT_SERVER_ONLY:
        return server;
    default:
        return false;
    }
}

static void handle_cmds(app_config_t *c, wake_cause_t wc, const mqtt_cmds_t *cm, bool *server) {
    if (cm->interval_s) {
        c->timer_interval_s = cm->interval_s;
        config_save(c);
        s_next_timer_ms = power_now_ms() + (uint64_t)c->timer_interval_s * 1000;
    }
    if (cm->hflip || cm->vflip || cm->rotate) {
        if (cm->hflip) c->hflip = (cm->hflip == 1);
        if (cm->vflip) c->vflip = (cm->vflip == 1);
        if (cm->rotate) c->rotate = cm->rotate - 1;
        config_save(c);
        app_cam_set_flip(c->hflip, c->vflip);
    }
    if (cm->snapshot) {
        cap_result_t r;
        esp_err_t e = capture_run(c, ACT_PHOTO, true, &r);

        event_log_add(wc, ACT_PHOTO, e, r.file);
        if (r.jpeg) {
            mqtt_publish_snapshot(c, r.jpeg, r.jpeg_len);
        }
        capture_free(&r);
    }
    if (cm->server_mode) {
        *server = true;
    }
}

static void net_session(app_config_t *c, wake_cause_t wc, const cap_result_t *cap, bool server) {
    int rssi = 0;
    char ip[16];
    bool mq = false;
    mqtt_cmds_t cm = {0};

    if (!config_has_wifi(c) || net_init() != ESP_OK) {
        return;
    }
    if (wifi_sta_connect(c, &rssi) != ESP_OK) {
        event_log_add(wc, ACT_NONE, ESP_ERR_TIMEOUT, "wifi");
        return;
    }
    wifi_sta_ip(ip, sizeof(ip));
    net_time_sync(c, 5000);

    if (want_mqtt(c, server) && mqtt_open(c) == ESP_OK) {
        mq = true;
        if (c->ha_discovery) {
            ha_discovery_publish(c);
        }
        if (cap && cap->jpeg) {
            mqtt_publish_snapshot(c, cap->jpeg, cap->jpeg_len);
        }
        if (wc == WAKE_PIR) {
            mqtt_publish_motion(c);
        }
        mqtt_publish_events(c);

        /* Сначала команды, потом состояние: в HA сразу попадают новые значения */
        mqtt_poll_cmds(c, &cm, 1500);
        mqtt_clear_cmds(c, &cm);
        handle_cmds(c, wc, &cm, &server);
        mqtt_publish_state(c, ev_cause_str(wc), rssi, ip, cap ? cap->sd_free_mb : -1);
    }

    if (server) {
        s_next_srv_ms = power_now_ms() + (uint64_t)c->srv_period_s * 1000;
        ESP_LOGI(TAG, "server mode at %s", ip);
        power_led_blink_start(LED_SRV_ON_MS, LED_SRV_PERIOD_MS);
        server_run(c);
        power_led_blink_stop();
    }
    if (mq) {
        mqtt_publish_events(c);
        mqtt_close(c);
    }
    wifi_sta_stop();
}

static uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static void go_sleep(const app_config_t *c, bool pir_fired) {
    uint64_t now, wake = UINT64_MAX;
    uint32_t sleep_s;
    bool arm = false;

    if (c->pir_enabled) {
        now = power_now_ms();
        if (pir_fired && c->pir_dead_s > 0) {
            s_pir_until_ms = now + (uint64_t)c->pir_dead_s * 1000;
        }
        if (now < s_pir_until_ms) {
            wake = s_pir_until_ms;
            ESP_LOGI(TAG, "pir dead time %us", (unsigned)((s_pir_until_ms - now) / 1000));
        } else if (!power_pir_wait_low(PIR_LOW_WAIT_MS)) {
            s_pir_backoff_s = s_pir_backoff_s ? s_pir_backoff_s * 2 : PIR_BACKOFF_MIN;
            if (s_pir_backoff_s > PIR_BACKOFF_MAX) {
                s_pir_backoff_s = PIR_BACKOFF_MAX;
            }
            wake = power_now_ms() + (uint64_t)s_pir_backoff_s * 1000;
            ESP_LOGW(TAG, "pir stuck high, recheck in %us", (unsigned)s_pir_backoff_s);
        } else {
            s_pir_backoff_s = 0;
            arm = true;
        }
    }

    now = power_now_ms();
    if (c->timer_enabled) {
        wake = min_u64(wake, s_next_timer_ms);
    }
    if (c->srv_periodic) {
        wake = min_u64(wake, s_next_srv_ms);
    }
    if (wake == UINT64_MAX) {
        sleep_s = 0;                /* только кнопка/PIR */
    } else if (wake > now + 1000) {
        sleep_s = (uint32_t)((wake - now + 999) / 1000);
    } else {
        sleep_s = 1;
    }
    ESP_LOGI(TAG, "next: timer %ds, server %ds",
             c->timer_enabled ? (int)((int64_t)(s_next_timer_ms - now) / 1000) : -1,
             c->srv_periodic ? (int)((int64_t)(s_next_srv_ms - now) / 1000) : -1);
    power_sleep(c, sleep_s, arm);
}

void app_main(void) {
    app_config_t cfg;
    wake_cause_t wc = power_wake_cause();
    int act = ACT_NONE;
    cap_result_t cap;
    bool server = false, portal = false, have_cap = false;
    bool pir_fired = (wc == WAKE_PIR);
    uint64_t now;

    memset(&cap, 0, sizeof(cap));
    if (nvs_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
    }
    config_load(&cfg);
    if (pir_fired && cfg.pir_enabled) {
        power_led_pulse(LED_PIR_MS);
    }

    /* TZ не переживает глубокий сон: без этого localtime() работает по UTC */
    setenv("TZ", cfg.tz, 1);
    tzset();

    if (wc == WAKE_BUTTON && power_button_long_press(BTN_LONG_MS)) {
        portal = true;
    }

    event_log_init(wc == WAKE_FIRST_BOOT);
    if (power_reset_abnormal()) {
        /* panic / watchdog / brownout: попадёт в last_event и в датчик Reset reason */
        event_log_add(wc, ACT_NONE, ESP_FAIL, power_reset_reason());
    }
    now = power_now_ms();
    if (wc == WAKE_FIRST_BOOT || !s_sched_ok) {
        sched_reset(&cfg, now);
        if (wc == WAKE_FIRST_BOOT) {
            ha_discovery_invalidate();
        }
    }
    if (!config_has_wifi(&cfg) && (wc == WAKE_FIRST_BOOT || wc == WAKE_BUTTON)) {
        portal = true;
    }

    /* Плановые задачи проверяются при любом пробуждении */
    if (cfg.timer_enabled && sched_due(&s_next_timer_ms, cfg.timer_interval_s, now)) {
        act |= cfg.timer_action;
    }
    if (cfg.srv_periodic && sched_due(&s_next_srv_ms, cfg.srv_period_s, now)) {
        server = true;
    }
    switch (wc) {
    case WAKE_PIR:
        if (cfg.pir_enabled) {
            act |= cfg.pir_action;
        }
        break;
    case WAKE_BUTTON:
    case WAKE_FIRST_BOOT:
        server = true;
        break;
    case WAKE_TIMER:
        if (act == ACT_NONE && !server) {
            wc = WAKE_RESUME;
        }
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "cause=%s act=%s server=%d portal=%d",
             ev_cause_str(wc), ev_action_str(act), server, portal);

    if (portal) {
        power_led(true);
        portal_run(&cfg);
        power_led(false);
    } else {
        if (act != ACT_NONE) {
            if (ensure_time(&cfg)) {
                bool keep = cfg.mqtt_snapshot && cfg.mqtt_policy != MQTT_OFF;
                esp_err_t e = capture_run(&cfg, (cap_action_t)act, keep, &cap);

                have_cap = true;
                event_log_add(wc, act, e, cap.file);
            } else {
                event_log_add(wc, act, ESP_ERR_INVALID_STATE, "no-time");
            }
        }
        if (server || want_mqtt(&cfg, false)) {
            net_session(&cfg, wc, have_cap ? &cap : NULL, server);
        }
    }
    capture_free(&cap);

    power_led_wait_idle();
    go_sleep(&cfg, pir_fired);
}
