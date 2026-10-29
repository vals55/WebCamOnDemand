// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ha_discovery.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "app_mqtt.h"

static const char *TAG = "ha";
static RTC_DATA_ATTR bool s_done;

void ha_discovery_invalidate(void) {
    s_done = false;
}

static esp_err_t pub(const app_config_t *c, const char *comp, const char *obj,
                     const char *name, const char *extra) {
    char topic[160];
    char *buf = malloc(1100);
    esp_err_t err;
    int l;

    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", c->ha_prefix, comp, c->device_id, obj);
    l = snprintf(buf, 1100,
                 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
                 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"WebCam %s\","
                 "\"manufacturer\":\"vals\",\"model\":\"ESP32 CAM on demand\"},%s}",
                 name, c->device_id, obj, c->device_id, c->device_id, extra);
    err = mqtt_publish_raw(topic, buf, l, 1, true);
    free(buf);
    return err;
}

void ha_discovery_publish(const app_config_t *c) {
    char x[480];
    const char *p = c->mqtt_prefix;
    esp_err_t err = ESP_OK;

    if (s_done) {
        return;
    }

    snprintf(x, sizeof(x), "\"state_topic\":\"%s/motion\",\"device_class\":\"motion\","
             "\"off_delay\":30", p);
    err |= pub(c, "binary_sensor", "motion", "Motion", x);

    snprintf(x, sizeof(x), "\"topic\":\"%s/snapshot\"", p);
    err |= pub(c, "camera", "snapshot", "Snapshot", x);

    snprintf(x, sizeof(x), "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.wake }}\","
             "\"entity_category\":\"diagnostic\"", p);
    err |= pub(c, "sensor", "wake", "Wake cause", x);

    snprintf(x, sizeof(x), "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.rssi }}\","
             "\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\","
             "\"entity_category\":\"diagnostic\"", p);
    err |= pub(c, "sensor", "rssi", "RSSI", x);

    snprintf(x, sizeof(x), "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.sd_free_mb }}\","
             "\"unit_of_measurement\":\"MB\",\"entity_category\":\"diagnostic\"", p);
    err |= pub(c, "sensor", "sd_free", "SD free", x);

    snprintf(x, sizeof(x), "\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.ip }}\","
             "\"entity_category\":\"diagnostic\"", p);
    err |= pub(c, "sensor", "ip", "IP", x);

    snprintf(x, sizeof(x), "\"state_topic\":\"%s/event\",\"value_template\":\"{{ value_json.action }} {{ value_json.file }}\"", p);
    err |= pub(c, "sensor", "last_event", "Last event", x);

    snprintf(x, sizeof(x), "\"command_topic\":\"%s/cmd/server_mode\",\"payload_press\":\"1\",\"retain\":true", p);
    err |= pub(c, "button", "server_mode", "Server mode", x);

    snprintf(x, sizeof(x), "\"command_topic\":\"%s/cmd/snapshot\",\"payload_press\":\"1\",\"retain\":true", p);
    err |= pub(c, "button", "take_snapshot", "Take snapshot", x);

    snprintf(x, sizeof(x), "\"command_topic\":\"%s/cmd/set_interval\",\"state_topic\":\"%s/state\","
             "\"value_template\":\"{{ value_json.timer_interval }}\",\"min\":10,\"max\":86400,"
             "\"step\":10,\"unit_of_measurement\":\"s\",\"mode\":\"box\",\"retain\":true", p, p);
    err |= pub(c, "number", "interval", "Timer interval", x);

    if (err == ESP_OK) {
        s_done = true;
    } else {
        ESP_LOGW(TAG, "discovery incomplete");
    }
}
