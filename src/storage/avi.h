// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct avi_s avi_t;

avi_t *avi_open(const char *path, uint16_t w, uint16_t h, uint32_t max_frames);
/* ESP_ERR_INVALID_SIZE: достигнут лимит кадров, файл остаётся корректным */
esp_err_t avi_add_frame(avi_t *a, const uint8_t *data, size_t len);
esp_err_t avi_close(avi_t *a, int64_t elapsed_us);
