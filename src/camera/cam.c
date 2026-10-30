// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "cam.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "sensor.h"
#include "app_config.h"
#include "cam_tune.h"
#include "rotate.h"

#if CAM_OV5640_AF_ENABLE && defined(__has_include)
#if __has_include("esp_camera_af.h")
#include "esp_camera_af.h"
#define HAVE_AF 1
#endif
#endif

static const char *TAG = "cam";

#define PIN_PWDN    CAM_PIN_PWDN
#define FB_MIN      (128 * 1024)
#define FB_MAX      (1024 * 1024)
#define FB_DIV      4             /* буфер = W*H/4: запас для OV5640 при q>=10 */
#define DROP_SWITCH 3             /* кадров выбросить после смены разрешения */

static bool s_ready;
static bool s_stream;
static bool s_photo_mode;
static framesize_t s_photo_fs;
static framesize_t s_stream_fs;
static uint8_t s_photo_q;
static uint8_t s_stream_q;
static bool s_hflip, s_vflip;
static SemaphoreHandle_t s_lock;
static char s_name[16] = "none";

static size_t fb_bytes(framesize_t fs) {
    size_t n = (size_t)resolution[fs].width * resolution[fs].height / FB_DIV;

    if (n < FB_MIN) n = FB_MIN;
    if (n > FB_MAX) n = FB_MAX;
    return (n + 4095) & ~(size_t)4095;
}

typedef struct {
    int8_t brightness, contrast, saturation, sharpness;
    uint8_t denoise;
    bool lenc;
    uint8_t warmup_photo, warmup_stream;
} cam_tune_t;

static const cam_tune_t T_OV3660 = {
    CAM_OV3660_BRIGHTNESS, CAM_OV3660_CONTRAST, CAM_OV3660_SATURATION, CAM_OV3660_SHARPNESS,
    CAM_OV3660_DENOISE, CAM_OV3660_LENC, CAM_OV3660_WARMUP_PHOTO, CAM_OV3660_WARMUP_STREAM,
};
static const cam_tune_t T_OV5640 = {
    CAM_OV5640_BRIGHTNESS, CAM_OV5640_CONTRAST, CAM_OV5640_SATURATION, CAM_OV5640_SHARPNESS,
    CAM_OV5640_DENOISE, CAM_OV5640_LENC, CAM_OV5640_WARMUP_PHOTO, CAM_OV5640_WARMUP_STREAM,
};

static const cam_tune_t *tune_for(const sensor_t *s) {
    if (s->id.PID == OV3660_PID) return &T_OV3660;
    if (s->id.PID == OV5640_PID) return &T_OV5640;
    return NULL;
}

/* Вызвать функцию драйвера, если она есть у сенсора; отказ (значение вне диапазона) - в лог */
#define SET(fn, v) \
    do { if (s->fn && s->fn(s, (v)) != 0) ESP_LOGW(TAG, #fn "(%d) rejected", (int)(v)); } while (0)

static void apply_tuning(sensor_t *s, const cam_tune_t *t) {
    SET(set_brightness, t->brightness);
    SET(set_contrast, t->contrast);
    SET(set_saturation, t->saturation);
    SET(set_sharpness, t->sharpness);
    SET(set_denoise, t->denoise);
    SET(set_lenc, t->lenc);
}

static void apply_flip(sensor_t *s) {
    if (s) {
        SET(set_hmirror, s_hflip);
        SET(set_vflip, s_vflip);
    }
}

void app_cam_set_flip(bool hflip, bool vflip) {
    s_hflip = hflip;
    s_vflip = vflip;
    if (s_ready) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        apply_flip(esp_camera_sensor_get());
        xSemaphoreGive(s_lock);
    }
}

static void drop_frames(int n) {
    while (n-- > 0) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
}

static void set_mode(bool photo) {
    sensor_t *s = esp_camera_sensor_get();

    if (!s || photo == s_photo_mode) {
        return;
    }
    s->set_framesize(s, photo ? s_photo_fs : s_stream_fs);
    s->set_quality(s, photo ? s_photo_q : s_stream_q);
    apply_flip(s);                  /* смена разрешения может сбросить зеркало */
    s_photo_mode = photo;
    drop_frames(DROP_SWITCH);
}

esp_err_t app_cam_init(const app_config_t *c, bool stream) {
    bool psram = esp_psram_is_initialized();
    framesize_t pfs = (framesize_t)c->framesize;
    framesize_t sfs = (framesize_t)c->stream_framesize;
    framesize_t big;
    camera_config_t k = {
        .pin_pwdn = PIN_PWDN, .pin_reset = -1, .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SDA, .pin_sccb_scl = CAM_PIN_SCL,
        .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6, .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2, .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC, .pin_href = CAM_PIN_HREF, .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 10000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .fb_count = psram ? 2 : 1,
        .fb_location = psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = stream ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };
    sensor_t *s;
    const cam_tune_t *tune = NULL;
    camera_sensor_info_t *info;
    esp_err_t err;

    if (s_ready) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
        app_rot_init();
    }
    if (pfs >= FRAMESIZE_INVALID) pfs = FRAMESIZE_UXGA;
    if (sfs >= FRAMESIZE_INVALID) sfs = FRAMESIZE_XGA;
    if (!psram) {
        if (pfs > FRAMESIZE_QVGA) pfs = FRAMESIZE_QVGA;
        if (sfs > FRAMESIZE_QVGA) sfs = FRAMESIZE_QVGA;
    }
    big = (stream && sfs > pfs) ? sfs : pfs;

    k.frame_size = stream ? sfs : pfs;
    k.jpeg_quality = stream ? c->stream_quality : c->jpeg_quality;
    k.jpeg_buffer_size = psram ? fb_bytes(big) : 0;

#if CAM_PIN_PWDN >= 0
    rtc_gpio_hold_dis(PIN_PWDN);
    rtc_gpio_deinit(PIN_PWDN);
#endif

    err = esp_camera_init(&k);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    s_stream = stream;
    s_photo_mode = !stream;
    s_photo_q = c->jpeg_quality;
    s_stream_q = c->stream_quality;

    s = esp_camera_sensor_get();
    tune = s ? tune_for(s) : NULL;
    info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    if (info) {
        strlcpy(s_name, info->name, sizeof(s_name));
        if (pfs > info->max_size) {
            ESP_LOGW(TAG, "%s max %ux%u, photo size reduced", info->name,
                     resolution[info->max_size].width, resolution[info->max_size].height);
            pfs = info->max_size;
        }
        if (sfs > info->max_size) {
            sfs = info->max_size;
        }
    } else if (s) {
        snprintf(s_name, sizeof(s_name), "PID 0x%04X", s->id.PID);
    }
    s_hflip = c->hflip;
    s_vflip = c->vflip;
    apply_flip(s);
    if (s && tune) {
        apply_tuning(s, tune);
    }
    s_photo_fs = pfs;
    s_stream_fs = sfs;

    ESP_LOGI(TAG, "sensor %s: photo %ux%u q=%u, stream %ux%u q=%u, fb %ux%u KB",
             s_name, resolution[pfs].width, resolution[pfs].height, s_photo_q,
             resolution[sfs].width, resolution[sfs].height, s_stream_q,
             (unsigned)k.fb_count, (unsigned)(k.jpeg_buffer_size / 1024));

#ifdef HAVE_AF
    /* Прошивка автофокуса грузится до прогрева, сведение фокуса - после: сцена уже стабильна */
    bool af = (s && s->id.PID == OV5640_PID);
    if (af) {
        esp_camera_af_config_t afc = {.mode = ESP_CAMERA_AF_MODE_AUTO, .timeout_ms = CAM_OV5640_AF_TIMEOUT_MS};

        af = (esp_camera_af_init(s, &afc) == ESP_OK);
        if (!af) ESP_LOGW(TAG, "autofocus unavailable (нет AF-объектива или выключен в menuconfig)");
    }
#endif

    drop_frames(tune ? (stream ? tune->warmup_stream : tune->warmup_photo)
                     : (stream ? CAM_OTHER_WARMUP_STREAM : CAM_OTHER_WARMUP_PHOTO));

#ifdef HAVE_AF
    if (af) {
        esp_camera_af_status_t st;

        if (esp_camera_af_trigger(s) == ESP_OK) {
            esp_camera_af_wait(s, 0, &st);      /* 0 = таймаут из esp_camera_af_config_t */
        }
    }
#endif
    return ESP_OK;
}

void app_cam_deinit(void) {
    app_rot_release();
    if (s_ready) {
        esp_camera_deinit();
        s_ready = false;
    }
#if CAM_PIN_PWDN >= 0
    rtc_gpio_init(PIN_PWDN);
    rtc_gpio_set_direction(PIN_PWDN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(PIN_PWDN, 1);
    rtc_gpio_hold_en(PIN_PWDN);
#endif
}

camera_fb_t *app_cam_capture(void) {
    camera_fb_t *fb;

    if (!s_ready) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fb = esp_camera_fb_get();
    xSemaphoreGive(s_lock);
    return fb;
}

void app_cam_release(camera_fb_t *fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

camera_fb_t *app_cam_photo_begin(void) {
    camera_fb_t *fb;

    if (!s_ready) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_stream) {
        set_mode(true);
    }
    fb = esp_camera_fb_get();
    if (fb) {
        ESP_LOGI(TAG, "photo %ux%u %u KB", fb->width, fb->height, (unsigned)(fb->len / 1024));
        return fb;                  /* мьютекс держим до app_cam_photo_end */
    }
    ESP_LOGW(TAG, "photo: no frame");
    if (s_stream) {
        set_mode(false);
    }
    xSemaphoreGive(s_lock);
    return NULL;
}

void app_cam_photo_end(camera_fb_t *fb) {
    if (!fb) {
        return;
    }
    esp_camera_fb_return(fb);
    if (s_stream) {
        set_mode(false);
    }
    xSemaphoreGive(s_lock);
}

const char *app_cam_sensor_name(void) {
    return s_name;
}
