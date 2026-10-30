// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_camera.h"
#include "app_config.h"

static const char *TAG = "config";

void config_defaults(app_config_t *c) {
    uint8_t mac[6] = {0};
    memset(c, 0, sizeof(*c));

    strlcpy(c->ntp_server, "ru.pool.ntp.org", sizeof(c->ntp_server));
    strlcpy(c->tz, "MSK-3", sizeof(c->tz));
    c->wifi_timeout_s = 15;

    c->timer_enabled = true;
    c->timer_interval_s = 600;
    c->timer_action = ACT_PHOTO;
    c->pir_enabled = true;
    c->pir_action = ACT_PHOTO;
    c->pir_dead_s = 30;
    c->btn_enabled = true;

    c->framesize = FRAMESIZE_UXGA;
    c->jpeg_quality = 12;
    c->stream_framesize = FRAMESIZE_XGA;
    c->stream_quality = 15;
    c->video_len_s = 10;
    c->video_fps = 10;
    c->sd_overwrite = true;

    c->srv_idle_s = 60;
    c->srv_periodic = false;
    c->srv_period_s = 3600;

    c->mqtt_policy = MQTT_OFF;
    c->mqtt_port = 1883;
    strlcpy(c->ha_prefix, "homeassistant", sizeof(c->ha_prefix));
    c->ha_discovery = true;
    c->mqtt_snap_quality = 15;
    c->mqtt_batch_n = 6;
    c->mqtt_timeout_s = 10;

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(c->device_id, sizeof(c->device_id), "webcam_%02X%02X",
             mac[4], mac[5]);
    snprintf(c->mqtt_prefix, sizeof(c->mqtt_prefix), "webcam/%s", c->device_id);
}

#define CLAMP(f, lo, hi) \
    do { if ((f) < (lo)) (f) = (lo); else if ((f) > (hi)) (f) = (hi); } while (0)
#define TERM(f) ((f)[sizeof(f) - 1] = '\0')

static void sanitize(app_config_t *c) {
    TERM(c->wifi_ssid);  TERM(c->wifi_pass);  TERM(c->ntp_server); TERM(c->tz);
    TERM(c->mqtt_host);  TERM(c->mqtt_user);  TERM(c->mqtt_pass);
    TERM(c->mqtt_prefix); TERM(c->device_id); TERM(c->ha_prefix);

    CLAMP(c->timer_interval_s, 10, 7UL * 24 * 3600);
    CLAMP(c->srv_period_s, 60, 7UL * 24 * 3600);
    CLAMP(c->jpeg_quality, 4, 63);
    CLAMP(c->stream_quality, 4, 63);
    CLAMP(c->video_fps, 1, 30);
    CLAMP(c->video_len_s, 1, 300);
    CLAMP(c->srv_idle_s, 10, 3600);
    CLAMP(c->wifi_timeout_s, 5, 60);
    CLAMP(c->mqtt_timeout_s, 3, 60);
    CLAMP(c->mqtt_batch_n, 1, 100);
    if (c->framesize >= FRAMESIZE_INVALID) c->framesize = FRAMESIZE_UXGA;
    if (c->stream_framesize > FRAMESIZE_SXGA) c->stream_framesize = FRAMESIZE_XGA;
    if (c->timer_action > ACT_BOTH) c->timer_action = ACT_PHOTO;
    if (c->pir_action > ACT_BOTH) c->pir_action = ACT_PHOTO;
    if (c->mqtt_policy > MQTT_SERVER_ONLY) c->mqtt_policy = MQTT_OFF;
    if (c->mqtt_port == 0) c->mqtt_port = c->mqtt_tls ? 8883 : 1883;
    if (c->rotate > 2) c->rotate = 0;
}

/* Конфигурация хранится одним blob. Размер не совпал (изменилась структура) -> значения по умолчанию. */
esp_err_t config_load(app_config_t *cfg) {
    nvs_handle_t h;
    size_t len = sizeof(*cfg);
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READONLY, &h);

    if (err == ESP_OK) {
        err = nvs_get_blob(h, CFG_NVS_KEY, cfg, &len);
        nvs_close(h);
        if (err == ESP_OK && len != sizeof(*cfg)) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "using defaults: %s", esp_err_to_name(err));
        config_defaults(cfg);
        return err;
    }
    sanitize(cfg);
    return ESP_OK;
}

esp_err_t config_save(const app_config_t *cfg) {
    app_config_t tmp = *cfg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    sanitize(&tmp);
    err = nvs_set_blob(h, CFG_NVS_KEY, &tmp, sizeof(tmp));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return err;
}

bool config_has_wifi(const app_config_t *cfg) {
    return cfg->wifi_ssid[0] != '\0';
}
