// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "capture.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cam.h"
#include "sd.h"
#include "power.h"
#include "avi.h"

static const char *TAG = "capture";

#define MIN_FREE_MB      20   /* ниже - запись запрещена (если перезапись выключена) */
#define CLEAN_MARGIN_MB  50   /* при очистке освобождаем на столько больше порога */
#define VIDEO_KB_PER_FR  40   /* оценка размера кадра видео */

static void set_file(cap_result_t *r, const char *path)
{
    size_t pre = strlen(SD_MOUNT) + 1;
    const char *rel = strlen(path) > pre ? path + pre : path;
    strlcpy(r->file, rel, sizeof(r->file));
}

static int video_need_mb(const app_config_t *c)
{
    uint32_t mb = ((uint32_t)c->video_len_s * c->video_fps * VIDEO_KB_PER_FR) / 1024 + 1;
    return mb > 2000 ? 2000 : (int)mb;
}

static esp_err_t do_photo(bool keep, bool sd_ok, cap_result_t *r)
{
    char path[96];
    camera_fb_t *fb = app_cam_photo_begin();
    esp_err_t err = ESP_OK;
    if (!fb) return ESP_FAIL;
    if (keep) {
        r->jpeg = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!r->jpeg) r->jpeg = malloc(fb->len);
        if (r->jpeg) {
            memcpy(r->jpeg, fb->buf, fb->len);
            r->jpeg_len = fb->len;
        }
    }
    if (sd_ok) {
        sd_make_name(path, sizeof(path), "IMG", "jpg");
        err = sd_save(path, fb->buf, fb->len);
        if (err == ESP_OK) {
            set_file(r, path);
            sd_log("photo %s", r->file);
        } else {
            sd_log("photo error %d", err);
        }
    } else if (!r->jpeg) {
        err = ESP_ERR_NOT_FOUND;
    }
    app_cam_photo_end(fb);
    return err;
}

static esp_err_t do_video(const app_config_t *c, cap_result_t *r)
{
    char path[96];
    camera_fb_t *fb;
    avi_t *a;
    int64_t period = 1000000LL / c->video_fps;
    int64_t start, next, now, total = (int64_t)c->video_len_s * 1000000LL;
    uint32_t max_frames = (uint32_t)c->video_len_s * c->video_fps + 8;
    uint32_t frames = 0, w, h;
    uint64_t bytes = 0;
    esp_err_t err = ESP_OK, ce;

    fb = app_cam_capture();
    if (!fb) return ESP_FAIL;
    sd_make_name(path, sizeof(path), "VID", "avi");
    w = fb->width;
    h = fb->height;
    a = avi_open(path, w, h, max_frames);
    app_cam_release(fb);
    if (!a) return ESP_ERR_NO_MEM;

    start = esp_timer_get_time();
    next = start;
    while ((now = esp_timer_get_time()) - start < total) {
        fb = app_cam_capture();
        if (!fb) {
            err = ESP_FAIL;
            break;
        }
        err = avi_add_frame(a, fb->buf, fb->len);
        bytes += fb->len;
        app_cam_release(fb);
        if (err == ESP_ERR_INVALID_SIZE) {
            err = ESP_OK;
            break;
        }
        if (err != ESP_OK) break;
        frames++;
        next += period;
        now = esp_timer_get_time();
        if (next > now) vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        else next = now;
    }
    now = esp_timer_get_time() - start;
    ce = avi_close(a, now);
    ESP_LOGI(TAG, "video %ux%u %u frames %.1f fps avg %u KB", (unsigned)w, (unsigned)h, (unsigned)frames,
             now > 0 ? frames * 1e6 / (double)now : 0.0, frames ? (unsigned)(bytes / frames / 1024) : 0);
    if (err == ESP_OK) err = ce;
    if (err == ESP_OK) {
        set_file(r, path);
        sd_log("video %s", r->file);
    } else {
        sd_log("video error %d", err);
    }
    return err;
}

esp_err_t capture_run(const app_config_t *c, cap_action_t act, bool keep_jpeg, cap_result_t *r)
{
    esp_err_t err = ESP_OK, e;
    bool mounted, writable;
    bool want_video = (act == ACT_VIDEO || act == ACT_BOTH);

    memset(r, 0, sizeof(*r));
    r->sd_free_mb = -1;
    if (!power_time_valid()) {
        ESP_LOGW(TAG, "time not set: SD writes disabled");
        err = ESP_ERR_INVALID_STATE;
        mounted = false;
    } else {
        mounted = (sd_mount() == ESP_OK);
    }
    writable = mounted;
    if (mounted) {
        r->sd_free_mb = sd_free_mb();
        if (c->sd_overwrite) {
            int need = MIN_FREE_MB + (want_video ? video_need_mb(c) : 0);
            if (r->sd_free_mb >= 0 && r->sd_free_mb <= need)
                r->sd_free_mb = sd_free_space(need + CLEAN_MARGIN_MB);
        }
        if (r->sd_free_mb >= 0 && r->sd_free_mb <= MIN_FREE_MB) {
            ESP_LOGW(TAG, "SD almost full: %d MB", r->sd_free_mb);
            writable = false;
            err = ESP_ERR_NO_MEM;
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (!writable && !keep_jpeg) {
        goto out;               /* снимать некуда: камеру не включаем */
    }

    e = app_cam_init(c, false);
    if (e != ESP_OK) {
        err = e;
        goto out;
    }
    if (act == ACT_PHOTO || act == ACT_BOTH) {
        e = do_photo(keep_jpeg, writable, r);
        if (e != ESP_OK) err = e;
    }
    if (want_video && writable) {
        e = do_video(c, r);
        if (e != ESP_OK) err = e;
    }
    app_cam_deinit();
out:
    if (mounted) {
        r->sd_free_mb = sd_free_mb();
        sd_unmount();
    }
    return err;
}

void capture_free(cap_result_t *r)
{
    free(r->jpeg);
    r->jpeg = NULL;
    r->jpeg_len = 0;
}
