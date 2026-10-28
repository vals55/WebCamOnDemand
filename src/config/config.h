// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { ACT_NONE = 0, ACT_PHOTO, ACT_VIDEO, ACT_BOTH } cap_action_t;
typedef enum { MQTT_OFF = 0, MQTT_EACH, MQTT_BATCH, MQTT_SERVER_ONLY } mqtt_policy_t;

typedef struct {
    uint16_t version;
    uint16_t size;

    char wifi_ssid[33];
    char wifi_pass[65];
    char ntp_server[48];
    char tz[48];
    uint8_t wifi_timeout_s;

    bool timer_enabled;
    uint32_t timer_interval_s;
    uint8_t timer_action;
    bool pir_enabled;
    uint8_t pir_action;
    uint16_t pir_dead_s;
    bool btn_enabled;

    uint8_t framesize;
    uint8_t jpeg_quality;
    uint16_t video_len_s;
    uint8_t video_fps;

    uint16_t srv_idle_s;
    bool srv_periodic;
    uint32_t srv_period_s;

    uint8_t mqtt_policy;
    char mqtt_host[64];
    uint16_t mqtt_port;
    bool mqtt_tls;
    char mqtt_user[32];
    char mqtt_pass[64];
    char mqtt_prefix[48];
    char device_id[32];
    bool ha_discovery;
    char ha_prefix[32];
    bool mqtt_snapshot;
    uint8_t mqtt_snap_quality;
    uint8_t mqtt_batch_n;
    uint8_t mqtt_timeout_s;

    /* v2: поля добавляются только в конец (миграция из v1 в config.c) */
    uint8_t wifi_bssid[6];
    bool wifi_bssid_set;          /* false = любой роутер с этим SSID */
    uint8_t stream_framesize;
    uint8_t stream_quality;

    /* v3 */
    bool sd_overwrite;            /* true = циклическая перезапись SD */
} app_config_t;

void config_defaults(app_config_t *cfg);
esp_err_t config_load(app_config_t *cfg);
esp_err_t config_save(const app_config_t *cfg);
bool config_has_wifi(const app_config_t *cfg);
