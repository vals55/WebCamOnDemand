// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "wifi_sta.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

#define BIT_OK   BIT0
#define BIT_FAIL BIT1
#define MAX_RETRY 3

static EventGroupHandle_t s_eg;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static bool s_inited;
static volatile bool s_sta_enabled;
static int s_retry;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && s_sta_enabled) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && s_sta_enabled) {
        if (s_retry++ < MAX_RETRY) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_eg, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_eg, BIT_OK);
    }
}

esp_err_t net_init(void) {
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    if (s_inited) {
        return ESP_OK;
    }
    if (!s_eg) {
        s_eg = xEventGroupCreate();
        if (!s_eg) {
            return ESP_ERR_NO_MEM;
        }
    }
    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    /* повторный вызов после ошибки не должен плодить интерфейсы */
    if (!s_sta) s_sta = esp_netif_create_default_wifi_sta();
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();

    err = esp_wifi_init(&wc);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_sta_connect(const app_config_t *c, int *rssi) {
    wifi_config_t wc = {0};
    EventBits_t bits;
    wifi_ap_record_t ap;
    char host[32];
    esp_err_t err;

    strlcpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = c->wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (c->wifi_bssid_set) {
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, c->wifi_bssid, 6);
        ESP_LOGI(TAG, "connect: ssid='%s' bssid=%02X:%02X:%02X:%02X:%02X:%02X", c->wifi_ssid,
                 c->wifi_bssid[0], c->wifi_bssid[1], c->wifi_bssid[2],
                 c->wifi_bssid[3], c->wifi_bssid[4], c->wifi_bssid[5]);
    } else {
        ESP_LOGI(TAG, "connect: ssid='%s' bssid=any", c->wifi_ssid);
    }

    strlcpy(host, c->device_id, sizeof(host));
    for (char *p = host; *p; p++) {
        if (*p == '_') {
            *p = '-';
        }
    }
    esp_netif_set_hostname(s_sta, host);

    s_retry = 0;
    xEventGroupClearBits(s_eg, BIT_OK | BIT_FAIL);
    s_sta_enabled = true;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: %s", esp_err_to_name(err));
        s_sta_enabled = false;
        return err;
    }

    bits = xEventGroupWaitBits(s_eg, BIT_OK | BIT_FAIL, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS((uint32_t)c->wifi_timeout_s * 1000));
    if (!(bits & BIT_OK)) {
        ESP_LOGW(TAG, "connect failed");
        wifi_sta_stop();
        return ESP_ERR_TIMEOUT;
    }
    if (rssi) {
        *rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    }
    return ESP_OK;
}

void wifi_sta_ip(char *out, size_t n) {
    esp_netif_ip_info_t ip;

    out[0] = '\0';
    if (s_sta && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    }
}

void wifi_sta_stop(void) {
    s_sta_enabled = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
}

bool net_time_sync(const app_config_t *c, uint32_t timeout_ms) {
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(c->ntp_server);
    bool ok;

    setenv("TZ", c->tz, 1);
    tzset();
    if (esp_netif_sntp_init(&sc) != ESP_OK) {
        return false;
    }
    ok = (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK);
    esp_netif_sntp_deinit();
    return ok;
}
