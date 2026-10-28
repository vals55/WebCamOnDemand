// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "event_log.h"
#include <string.h>
#include <time.h>
#include "esp_attr.h"

#define EV_MAX 16

static RTC_DATA_ATTR ev_t s_ev[EV_MAX];
static RTC_DATA_ATTR uint8_t s_head;
static RTC_DATA_ATTR uint8_t s_count;

void event_log_init(int reset) {
    if (reset || s_head >= EV_MAX || s_count > EV_MAX) {
        s_head = 0;
        s_count = 0;
    }
}

void event_log_add(uint8_t cause, uint8_t action, int err, const char *file) {
    uint8_t idx;
    time_t t = time(NULL);
    ev_t *e;

    if (s_count < EV_MAX) {
        idx = (s_head + s_count) % EV_MAX;
        s_count++;
    } else {
        idx = s_head;
        s_head = (s_head + 1) % EV_MAX;
    }
    e = &s_ev[idx];
    memset(e, 0, sizeof(*e));
    e->ts = (t > 1700000000) ? (int64_t)t : 0;
    e->cause = cause;
    e->action = action;
    e->err = (int16_t)err;
    if (file) {
        strlcpy(e->file, file, sizeof(e->file));
    }
}

uint8_t event_log_count(void) {
    return s_count;
}

const ev_t *event_log_get(uint8_t i) {
    return (i < s_count) ? &s_ev[(s_head + i) % EV_MAX] : NULL;
}

void event_log_drop(uint8_t n) {
    if (n > s_count) {
        n = s_count;
    }
    s_head = (s_head + n) % EV_MAX;
    s_count -= n;
}

const char *ev_cause_str(uint8_t c) {
    static const char *n[] = {"first_boot", "timer", "pir", "button", "resume", "unknown"};
    return c < 6 ? n[c] : "unknown";
}

const char *ev_action_str(uint8_t a) {
    static const char *n[] = {"none", "photo", "video", "both"};
    return a < 4 ? n[a] : "none";
}
