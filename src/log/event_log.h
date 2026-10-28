// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

typedef struct {
    int64_t ts;
    uint8_t cause;
    uint8_t action;
    int16_t err;
    char file[40];
} ev_t;

void event_log_init(int reset);
void event_log_add(uint8_t cause, uint8_t action, int err, const char *file);
uint8_t event_log_count(void);
const ev_t *event_log_get(uint8_t i);
void event_log_drop(uint8_t n);
const char *ev_cause_str(uint8_t c);
const char *ev_action_str(uint8_t a);
