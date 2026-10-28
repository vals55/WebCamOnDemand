// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "server.h"
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "cam.h"

static const char *TAG = "server";

static volatile int s_streams;
static volatile TickType_t s_last;
static volatile bool s_quit;

static void touch(void) { s_last = xTaskGetTickCount(); }

static esp_err_t index_get(httpd_req_t *req) {
    static const char page[] =
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>WebCam</title><body style='font-family:sans-serif;text-align:center'>"
        "<h3>WebCam</h3><img id=v style='max-width:100%'><p>"
        "<button onclick='go()'>Стрим</button> "
        "<a href=/capture target=_blank><button>Снимок</button></a> "
        "<a href=/sleep><button>Спать</button></a></p>"
        "<script>function go(){v.src=location.protocol+'//'+location.hostname+':81/stream'}</script>";

    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_get(httpd_req_t *req) {
    camera_fb_t *fb;
    esp_err_t err;

    touch();
    fb = app_cam_photo_begin();
    if (!fb) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    app_cam_photo_end(fb);
    touch();
    return err;
}

static esp_err_t status_get(httpd_req_t *req) {
    char b[96];

    touch();
    snprintf(b, sizeof(b), "{\"streams\":%d,\"sensor\":\"%s\"}", s_streams, app_cam_sensor_name());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
}

static esp_err_t sleep_get(httpd_req_t *req) {
    s_quit = true;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, "<meta charset=utf-8>Засыпаю…");
}

#define STREAM_MISS_MAX 10       /* подряд пропущенных кадров до обрыва */
#define STREAM_STAT_US  5000000

static esp_err_t stream_get(httpd_req_t *req) {
    esp_err_t err = ESP_OK;
    int miss = 0;
    uint32_t n = 0, kb = 0, w = 0, h = 0;
    int64_t t0 = esp_timer_get_time();

    s_streams++;
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (!s_quit) {
        char hdr[96];
        camera_fb_t *fb = app_cam_capture();
        int64_t now;
        int hl;

        if (!fb) {
            if (++miss >= STREAM_MISS_MAX) {
                ESP_LOGW(TAG, "stream: no frames, closing");
                err = ESP_FAIL;
                break;
            }
            continue;
        }
        miss = 0;
        w = fb->width;
        h = fb->height;
        hl = snprintf(hdr, sizeof(hdr),
                      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                      (unsigned)fb->len);
        err = httpd_resp_send_chunk(req, hdr, hl);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, "\r\n", 2);
        kb += fb->len / 1024;
        app_cam_release(fb);
        if (err != ESP_OK) {
            break;
        }
        touch();
        n++;
        now = esp_timer_get_time();
        if (now - t0 >= STREAM_STAT_US) {
            ESP_LOGI(TAG, "stream %ux%u %.1f fps avg %u KB",
                     (unsigned)w, (unsigned)h, n * 1e6 / (double)(now - t0), (unsigned)(kb / n));
            n = kb = 0;
            t0 = now;
        }
    }
    s_streams--;
    touch();
    return err;
}

static void start_mdns(const app_config_t *c) {
    char host[32];

    strlcpy(host, c->device_id, sizeof(host));
    for (char *p = host; *p; p++) {
        if (*p == '_') *p = '-';
    }
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set("ESP32-CAM");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

void server_run(const app_config_t *cfg) {
    httpd_handle_t web = NULL, strm = NULL;
    httpd_config_t wc = HTTPD_DEFAULT_CONFIG();
    httpd_config_t sc = HTTPD_DEFAULT_CONFIG();
    const httpd_uri_t u_idx = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    const httpd_uri_t u_cap = {.uri = "/capture", .method = HTTP_GET, .handler = capture_get};
    const httpd_uri_t u_st = {.uri = "/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t u_sl = {.uri = "/sleep", .method = HTTP_GET, .handler = sleep_get};
    const httpd_uri_t u_str = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
    TickType_t idle = pdMS_TO_TICKS((uint32_t)cfg->srv_idle_s * 1000);

    if (app_cam_init(cfg, true) != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed");
        return;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    start_mdns(cfg);

    s_streams = 0;
    s_quit = false;
    touch();

    wc.stack_size = 6144;
    wc.max_uri_handlers = 6;
    sc.server_port = 81;
    sc.ctrl_port = wc.ctrl_port + 1;
    sc.stack_size = 6144;
    sc.max_uri_handlers = 2;

    if (httpd_start(&web, &wc) == ESP_OK) {
        httpd_register_uri_handler(web, &u_idx);
        httpd_register_uri_handler(web, &u_cap);
        httpd_register_uri_handler(web, &u_st);
        httpd_register_uri_handler(web, &u_sl);
    }
    if (httpd_start(&strm, &sc) == ESP_OK) {
        httpd_register_uri_handler(strm, &u_str);
    }
    if (!web) {
        ESP_LOGE(TAG, "http start failed");
    }

    while (web && !s_quit) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_streams == 0 && (xTaskGetTickCount() - s_last) > idle) {
            ESP_LOGI(TAG, "idle timeout");
            break;
        }
    }

    s_quit = true;
    vTaskDelay(pdMS_TO_TICKS(300));
    if (strm) httpd_stop(strm);
    if (web) httpd_stop(web);
    mdns_free();
    app_cam_deinit();
}
