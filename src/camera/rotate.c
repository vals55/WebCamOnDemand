// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "rotate.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "img_converters.h"
#include "jpeg_decoder.h"

static const char *TAG = "rotate";

/* fmt2jpg ждёт RGB565 со старшим байтом первым. Если после поворота
   красный и синий поменялись местами или цвета "каша" - поставьте 0. */
#define ROT_SWAP_BYTES 1

static SemaphoreHandle_t s_mtx;
static uint16_t *s_src, *s_dst;    /* рабочие буферы одинакового размера s_cap */
static size_t s_cap;

void app_rot_init(void) {
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
}

static void free_bufs(void) {
    free(s_src);
    free(s_dst);
    s_src = s_dst = NULL;
    s_cap = 0;
}

void app_rot_release(void) {
    if (!s_mtx) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    free_bufs();
    xSemaphoreGive(s_mtx);
}

static bool ensure_bufs(size_t bytes) {
    if (bytes <= s_cap) {
        return true;
    }
    free_bufs();
    s_src = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dst = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_src || !s_dst) {
        free_bufs();
        return false;
    }
    s_cap = bytes;
    return true;
}

/* Степень уменьшения при декодировании: 0 = 1/1, 1 = 1/2, 2 = 1/4, 3 = 1/8 */
static int pick_shift(uint16_t w, uint16_t h, size_t max_bytes) {
    int s = 0;

    while (s < 3 && ((size_t)(w >> s) * (h >> s) * 2) > max_bytes) {
        s++;
    }
    return s;
}

void app_rot_dims(uint16_t w, uint16_t h, uint8_t rot, size_t max_bytes,
                  uint16_t *ow, uint16_t *oh) {
    int s = rot ? pick_shift(w, h, max_bytes) : 0;

    *ow = rot ? h >> s : w;
    *oh = rot ? w >> s : h;
}

static uint8_t *rotate_locked(const camera_fb_t *fb, uint8_t rot, uint8_t quality,
                              size_t max_bytes, size_t *olen) {
    int s = pick_shift(fb->width, fb->height, max_bytes);
    uint16_t sw = fb->width >> s, sh = fb->height >> s;
    size_t bytes = (size_t)sw * sh * 2;
    uint8_t *out = NULL;
    esp_jpeg_image_output_t img;
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)fb->buf,
        .indata_size = fb->len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = s,                 /* JPEG_IMAGE_SCALE_0 / 1_2 / 1_4 / 1_8 */
        .flags = {.swap_color_bytes = ROT_SWAP_BYTES},
    };

    if (!ensure_bufs(bytes)) {
        ESP_LOGW(TAG, "no memory for %ux%u", sw, sh);
        return NULL;
    }
    cfg.outbuf = (uint8_t *)s_src;
    cfg.outbuf_size = bytes;
    if (esp_jpeg_decode(&cfg, &img) != ESP_OK || img.width != sw || img.height != sh) {
        ESP_LOGW(TAG, "decode failed");
        return NULL;
    }

    /* вход sw x sh, выход sh x sw */
    for (uint16_t y = 0; y < sh; y++) {
        const uint16_t *in = s_src + (size_t)y * sw;

        for (uint16_t x = 0; x < sw; x++) {
            size_t col = (rot == 1) ? (size_t)(sh - 1 - y) : y;
            size_t row = (rot == 1) ? x : (size_t)(sw - 1 - x);

            s_dst[row * sh + col] = in[x];
        }
    }

    /* качество 4..63 (меньше = лучше) -> проценты для fmt2jpg */
    int pct = 100 - (int)quality * 100 / 63;

    if (!fmt2jpg((uint8_t *)s_dst, bytes, sh, sw, PIXFORMAT_RGB565,
                 pct < 10 ? 10 : pct, &out, olen)) {
        ESP_LOGW(TAG, "encode failed");
        return NULL;
    }
    return out;
}

uint8_t *app_rot_try(const camera_fb_t *fb, uint8_t rot, uint8_t quality,
                     size_t max_bytes, size_t *olen) {
    uint8_t *out;

    if (!rot || !s_mtx || !esp_psram_is_initialized()) {
        return NULL;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    out = rotate_locked(fb, rot, quality, max_bytes, olen);
    xSemaphoreGive(s_mtx);
    return out;
}
