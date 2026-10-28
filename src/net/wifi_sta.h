// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

esp_err_t net_init(void);
esp_err_t wifi_sta_connect(const app_config_t *c, int *rssi);
void wifi_sta_ip(char *out, size_t n);
void wifi_sta_stop(void);
bool net_time_sync(const app_config_t *c, uint32_t timeout_ms);
