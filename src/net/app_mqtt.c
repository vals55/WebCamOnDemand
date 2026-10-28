// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "app_mqtt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "event_log.h"
#include "power.h"

static const char *TAG = "mqtt";

#define B_CONN BIT0
#define B_PUB  BIT1
#define B_SUB  BIT2

static esp_mqtt_client_handle_t s_cli;
static EventGroupHandle_t s_eg;
static char s_uri[96];
static char s_prefix[64];
static char s_lwt[96];
static volatile mqtt_cmds_t s_cmds;

static bool topic_is(const esp_mqtt_event_handle_t ev, const char *suffix) {
    char t[128];
    int n = snprintf(t, sizeof(t), "%s/cmd/%s", s_prefix, suffix);

    return ev->topic_len == n && strncmp(ev->topic, t, n) == 0;
}

static void on_data(esp_mqtt_event_handle_t ev) {
    char v[16] = {0};

    if (ev->data_len <= 0 || ev->current_data_offset != 0) {
        return;
    }
    memcpy(v, ev->data, ev->data_len < 15 ? ev->data_len : 15);

    if (topic_is(ev, "server_mode")) {
        s_cmds.server_mode = (v[0] != '0');
    } else if (topic_is(ev, "snapshot")) {
        s_cmds.snapshot = (v[0] != '0');
    } else if (topic_is(ev, "set_interval")) {
        uint32_t n = (uint32_t)strtoul(v, NULL, 10);
        if (n >= 10 && n <= 7UL * 24 * 3600) {
            s_cmds.interval_s = n;
        }
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t ev = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_eg, B_CONN);
        break;
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_eg, B_CONN);
        break;
    case MQTT_EVENT_PUBLISHED:
        xEventGroupSetBits(s_eg, B_PUB);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        xEventGroupSetBits(s_eg, B_SUB);
        break;
    case MQTT_EVENT_DATA:
        on_data(ev);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "error event");
        break;
    default:
        break;
    }
}

esp_err_t mqtt_open(const app_config_t *c) {
    esp_mqtt_client_config_t mc = {0};
    EventBits_t b;

    if (!c->mqtt_host[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_eg) {
        s_eg = xEventGroupCreate();
    }
    xEventGroupClearBits(s_eg, B_CONN | B_PUB | B_SUB);
    memset((void *)&s_cmds, 0, sizeof(s_cmds));

    strlcpy(s_prefix, c->mqtt_prefix, sizeof(s_prefix));
    snprintf(s_uri, sizeof(s_uri), "%s://%s:%u", c->mqtt_tls ? "mqtts" : "mqtt",
             c->mqtt_host, (unsigned)c->mqtt_port);
    snprintf(s_lwt, sizeof(s_lwt), "%s/status", s_prefix);

    mc.broker.address.uri = s_uri;
    if (c->mqtt_tls) {
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    mc.credentials.client_id = c->device_id;
    if (c->mqtt_user[0]) {
        mc.credentials.username = c->mqtt_user;
        mc.credentials.authentication.password = c->mqtt_pass;
    }
    mc.session.last_will.topic = s_lwt;
    mc.session.last_will.msg = "asleep";
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = 1;
    mc.session.keepalive = 60;
    mc.buffer.size = 2048;
    mc.buffer.out_size = 4096;
    mc.task.stack_size = 6144;
    mc.network.timeout_ms = 5000;
    mc.network.disable_auto_reconnect = true;

    s_cli = esp_mqtt_client_init(&mc);
    if (!s_cli) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, on_event, NULL);
    if (esp_mqtt_client_start(s_cli) != ESP_OK) {
        esp_mqtt_client_destroy(s_cli);
        s_cli = NULL;
        return ESP_FAIL;
    }
    b = xEventGroupWaitBits(s_eg, B_CONN, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS((uint32_t)c->mqtt_timeout_s * 1000));
    if (!(b & B_CONN)) {
        ESP_LOGW(TAG, "broker connect timeout");
        esp_mqtt_client_stop(s_cli);
        esp_mqtt_client_destroy(s_cli);
        s_cli = NULL;
        return ESP_ERR_TIMEOUT;
    }
    mqtt_publish_raw(s_lwt, "awake", 5, 1, true);
    return ESP_OK;
}

void mqtt_close(const app_config_t *c) {
    if (!s_cli) {
        return;
    }
    mqtt_publish_raw(s_lwt, "asleep", 6, 1, true);
    esp_mqtt_client_stop(s_cli);
    esp_mqtt_client_destroy(s_cli);
    s_cli = NULL;
}

esp_err_t mqtt_publish_raw(const char *topic, const char *data, int len, int qos, bool retain) {
    int id;

    if (!s_cli) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_eg, B_PUB);
    id = esp_mqtt_client_publish(s_cli, topic, data, len, qos, retain);
    if (id < 0) {
        return ESP_FAIL;
    }
    if (qos > 0) {
        EventBits_t b = xEventGroupWaitBits(s_eg, B_PUB, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
        return (b & B_PUB) ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void topic_of(char *out, size_t n, const char *leaf) {
    snprintf(out, n, "%s/%s", s_prefix, leaf);
}

esp_err_t mqtt_publish_snapshot(const app_config_t *c, const uint8_t *jpg, size_t len) {
    char t[96];

    topic_of(t, sizeof(t), "snapshot");
    return mqtt_publish_raw(t, (const char *)jpg, (int)len, 0, false);
}

esp_err_t mqtt_publish_motion(const app_config_t *c) {
    char t[96];

    topic_of(t, sizeof(t), "motion");
    return mqtt_publish_raw(t, "ON", 2, 1, false);
}

esp_err_t mqtt_publish_events(const app_config_t *c) {
    char t[96], j[256];
    uint8_t sent = 0, n = event_log_count();
    esp_err_t err = ESP_OK;

    topic_of(t, sizeof(t), "event");
    for (uint8_t i = 0; i < n; i++) {
        const ev_t *e = event_log_get(i);
        int l = snprintf(j, sizeof(j),
                         "{\"ts\":%lld,\"cause\":\"%s\",\"action\":\"%s\",\"file\":\"%s\",\"err\":%d}",
                         (long long)e->ts, ev_cause_str(e->cause), ev_action_str(e->action),
                         e->file, e->err);
        err = mqtt_publish_raw(t, j, l, 1, false);
        if (err != ESP_OK) {
            break;
        }
        sent++;
    }
    event_log_drop(sent);
    return err;
}

esp_err_t mqtt_publish_state(const app_config_t *c, const char *wake, int rssi,
                             const char *ip, int sd_free_mb) {
    char t[96], j[320];
    int l = snprintf(j, sizeof(j),
                     "{\"wake\":\"%s\",\"boot\":%u,\"rssi\":%d,\"ip\":\"%s\","
                     "\"sd_free_mb\":%d,\"timer_interval\":%u,\"fw\":\"0.1\"}",
                     wake, (unsigned)power_boot_count(), rssi, ip, sd_free_mb,
                     (unsigned)c->timer_interval_s);

    topic_of(t, sizeof(t), "state");
    return mqtt_publish_raw(t, j, l, 1, true);
}

void mqtt_poll_cmds(const app_config_t *c, mqtt_cmds_t *out, uint32_t wait_ms) {
    char t[96];

    topic_of(t, sizeof(t), "cmd/#");
    xEventGroupClearBits(s_eg, B_SUB);
    if (esp_mqtt_client_subscribe(s_cli, t, 1) >= 0) {
        xEventGroupWaitBits(s_eg, B_SUB, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
    out->server_mode = s_cmds.server_mode;
    out->snapshot = s_cmds.snapshot;
    out->interval_s = s_cmds.interval_s;
}

void mqtt_clear_cmds(const app_config_t *c, const mqtt_cmds_t *cm) {
    char t[96];

    if (cm->server_mode) {
        topic_of(t, sizeof(t), "cmd/server_mode");
        mqtt_publish_raw(t, "", 0, 1, true);
    }
    if (cm->snapshot) {
        topic_of(t, sizeof(t), "cmd/snapshot");
        mqtt_publish_raw(t, "", 0, 1, true);
    }
    if (cm->interval_s) {
        topic_of(t, sizeof(t), "cmd/set_interval");
        mqtt_publish_raw(t, "", 0, 1, true);
    }
}
