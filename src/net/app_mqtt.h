// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

typedef struct {
    bool server_mode;
    bool snapshot;
    uint32_t interval_s;
    uint8_t hflip;      /* 0 = нет команды, 1 = ON, 2 = OFF */
    uint8_t vflip;      /* 0 = нет команды, 1 = ON, 2 = OFF */
    uint8_t rotate;     /* 0 = нет команды, 1 = 0°, 2 = 90°, 3 = 270° */
} mqtt_cmds_t;

esp_err_t mqtt_open(const app_config_t *c);
void mqtt_close(const app_config_t *c);
esp_err_t mqtt_publish_raw(const char *topic, const char *data, int len, int qos, bool retain);
esp_err_t mqtt_publish_snapshot(const app_config_t *c, const uint8_t *jpg, size_t len);
esp_err_t mqtt_publish_motion(const app_config_t *c);
esp_err_t mqtt_publish_events(const app_config_t *c);
esp_err_t mqtt_publish_state(const app_config_t *c, const char *wake, int rssi,
                             const char *ip, int sd_free_mb);
void mqtt_poll_cmds(const app_config_t *c, mqtt_cmds_t *out, uint32_t wait_ms);
void mqtt_clear_cmds(const app_config_t *c, const mqtt_cmds_t *cm);
