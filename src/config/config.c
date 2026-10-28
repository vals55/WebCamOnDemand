// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "config.h"
#include <string.h>
#include <stddef.h>
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
    c->version = CFG_VERSION;
    c->size = sizeof(*c);

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

/* Конец полей версии 1: всё, что после, добавлено в v2 */
#define CFG_V1_END offsetof(app_config_t, wifi_bssid)
/* Конец полей версии 2: sd_overwrite добавлено в v3 */
#define CFG_V2_END offsetof(app_config_t, sd_overwrite)

static void clamp_u32(uint32_t *v, uint32_t lo, uint32_t hi) {
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
}

static void sanitize(app_config_t *c) {
    uint32_t t;

    c->wifi_ssid[sizeof(c->wifi_ssid) - 1] = '\0';
    c->wifi_pass[sizeof(c->wifi_pass) - 1] = '\0';
    c->mqtt_host[sizeof(c->mqtt_host) - 1] = '\0';
    c->mqtt_user[sizeof(c->mqtt_user) - 1] = '\0';
    c->mqtt_pass[sizeof(c->mqtt_pass) - 1] = '\0';
    c->mqtt_prefix[sizeof(c->mqtt_prefix) - 1] = '\0';
    c->device_id[sizeof(c->device_id) - 1] = '\0';
    c->ha_prefix[sizeof(c->ha_prefix) - 1] = '\0';
    c->ntp_server[sizeof(c->ntp_server) - 1] = '\0';
    c->tz[sizeof(c->tz) - 1] = '\0';

    clamp_u32(&c->timer_interval_s, 10, 7UL * 24 * 3600);
    clamp_u32(&c->srv_period_s, 60, 7UL * 24 * 3600);
    t = c->jpeg_quality;   clamp_u32(&t, 4, 63);    c->jpeg_quality = t;
    t = c->stream_quality; clamp_u32(&t, 4, 63);    c->stream_quality = t;
    if (c->framesize >= FRAMESIZE_INVALID) c->framesize = FRAMESIZE_UXGA;
    if (c->stream_framesize > FRAMESIZE_SXGA) c->stream_framesize = FRAMESIZE_XGA;
    t = c->video_fps;      clamp_u32(&t, 1, 30);    c->video_fps = t;
    t = c->video_len_s;    clamp_u32(&t, 1, 300);   c->video_len_s = t;
    t = c->srv_idle_s;     clamp_u32(&t, 10, 3600); c->srv_idle_s = t;
    t = c->wifi_timeout_s; clamp_u32(&t, 5, 60);    c->wifi_timeout_s = t;
    t = c->mqtt_timeout_s; clamp_u32(&t, 3, 60);    c->mqtt_timeout_s = t;
    t = c->mqtt_batch_n;   clamp_u32(&t, 1, 100);   c->mqtt_batch_n = t;
    if (c->timer_action > ACT_BOTH) c->timer_action = ACT_PHOTO;
    if (c->pir_action > ACT_BOTH) c->pir_action = ACT_PHOTO;
    if (c->mqtt_policy > MQTT_SERVER_ONLY) c->mqtt_policy = MQTT_OFF;
    if (c->mqtt_port == 0) c->mqtt_port = c->mqtt_tls ? 8883 : 1883;
}

esp_err_t config_load(app_config_t *cfg) {
    nvs_handle_t h;
    size_t len = sizeof(*cfg);
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READONLY, &h);

    if (err == ESP_OK) {
        err = nvs_get_blob(h, CFG_NVS_KEY, cfg, &len);
        nvs_close(h);
        if (err == ESP_OK && cfg->version == 1 && len == cfg->size && len >= CFG_V1_END) {
            app_config_t old = *cfg;

            config_defaults(cfg);
            memcpy(cfg, &old, CFG_V1_END);
            cfg->version = CFG_VERSION;
            cfg->size = sizeof(*cfg);
            sanitize(cfg);
            ESP_LOGW(TAG, "migrated v1 -> v%d", CFG_VERSION);
            config_save(cfg);
            return ESP_OK;
        }
        if (err == ESP_OK && cfg->version == 2 && len == cfg->size && len >= CFG_V2_END) {
            app_config_t old = *cfg;

            config_defaults(cfg);
            memcpy(cfg, &old, CFG_V2_END);
            cfg->version = CFG_VERSION;
            cfg->size = sizeof(*cfg);
            sanitize(cfg);
            ESP_LOGW(TAG, "migrated v2 -> v%d", CFG_VERSION);
            config_save(cfg);
            return ESP_OK;
        }
        if (err == ESP_OK &&
            (len != sizeof(*cfg) || cfg->version != CFG_VERSION ||
             cfg->size != sizeof(*cfg))) {
            err = ESP_ERR_INVALID_VERSION;
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
    esp_err_t err;

    tmp.version = CFG_VERSION;
    tmp.size = sizeof(tmp);
    sanitize(&tmp);

    err = nvs_open(CFG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
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
