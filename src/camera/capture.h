// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

typedef struct {
    char file[40];
    uint8_t *jpeg;
    size_t jpeg_len;
    int sd_free_mb;
} cap_result_t;

esp_err_t capture_run(const app_config_t *c, cap_action_t act, bool keep_jpeg,
                      cap_result_t *r);
void capture_free(cap_result_t *r);
