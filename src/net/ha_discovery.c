// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ha_discovery.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "app_mqtt.h"

static const char *TAG = "ha";
#define HA_BUF 1100

/* Discovery публикуется заново после каждой перепрошивки (id зависит от времени сборки) */
static const char BUILD[] = __DATE__ " " __TIME__;
static RTC_DATA_ATTR uint32_t s_done_id;

static uint32_t build_id(void) {
    uint32_t h = 5381;

    for (const char *p = BUILD; *p; p++) {
        h = h * 33 + (uint8_t)*p;
    }
    return h ? h : 1;
}

void ha_discovery_invalidate(void) {
    s_done_id = 0;
}

static esp_err_t pub(const app_config_t *c, const char *comp, const char *obj,
                     const char *name, const char *extra) {
    char topic[160];
    char *buf = malloc(HA_BUF);
    esp_err_t err;
    int l;

    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", c->ha_prefix, comp, c->device_id, obj);
    l = snprintf(buf, HA_BUF,
                 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\","
                 "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"WebCam %s\","
                 "\"manufacturer\":\"by Vals\",\"model\":\"ESP32 CAM on demand\"},%s}",
                 name, c->device_id, obj, c->device_id, c->device_id, extra);
    err = (l > 0 && l < HA_BUF) ? mqtt_publish_raw(topic, buf, l, 1, true) : ESP_ERR_INVALID_SIZE;
    free(buf);
    return err;
}

static esp_err_t pub_switch(const app_config_t *c, const char *obj, const char *name, const char *icon) {
    char x[420];

    snprintf(x, sizeof(x), "\"command_topic\":\"%s/cmd/%s\",\"state_topic\":\"%s/state\","
             "\"value_template\":\"{{ value_json.%s }}\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
             "\"state_on\":\"ON\",\"state_off\":\"OFF\",\"optimistic\":true,"
             "\"icon\":\"mdi:%s\",\"retain\":true",
             c->mqtt_prefix, obj, c->mqtt_prefix, obj, icon);
    return pub(c, "switch", obj, name, x);
}

void ha_discovery_publish(const app_config_t *c) {
    char x[480];
    const char *p = c->mqtt_prefix;
    esp_err_t err = ESP_OK;

    if (s_done_id == build_id()) {
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
             "\"step\":10,\"unit_of_measurement\":\"s\",\"mode\":\"box\",\"optimistic\":true,\"retain\":true", p, p);
    err |= pub(c, "number", "interval", "Timer interval", x);

    err |= pub_switch(c, "hflip", "Flip horizontal", "flip-horizontal");
    err |= pub_switch(c, "vflip", "Flip vertical", "flip-vertical");

    snprintf(x, sizeof(x), "\"command_topic\":\"%s/cmd/rotate\",\"state_topic\":\"%s/state\","
             "\"value_template\":\"{{ value_json.rotate }}\",\"options\":[\"0\",\"90\",\"270\"],"
             "\"optimistic\":true,\"icon\":\"mdi:rotate-right\",\"retain\":true", p, p);
    err |= pub(c, "select", "rotate", "Rotate", x);

    if (err == ESP_OK) {
        s_done_id = build_id();
    } else {
        ESP_LOGW(TAG, "discovery incomplete");
    }
}
