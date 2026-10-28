// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_camera.h"
#include "config.h"

/* stream=false: фото/видео в разрешении из настроек.
   stream=true: стрим в stream_framesize, снимок через app_cam_photo_begin/end. */
esp_err_t app_cam_init(const app_config_t *c, bool stream);
void app_cam_deinit(void);
camera_fb_t *app_cam_capture(void);
void app_cam_release(camera_fb_t *fb);

/* Снимок в полном разрешении во время стрима. Между begin и end стрим ждёт. */
camera_fb_t *app_cam_photo_begin(void);
void app_cam_photo_end(camera_fb_t *fb);

const char *app_cam_sensor_name(void);
