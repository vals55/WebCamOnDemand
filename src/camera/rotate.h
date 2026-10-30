// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_camera.h"

/* Поворот на 90° программный: JPEG -> RGB565 -> поворот -> JPEG. Нужна PSRAM.
   Кадр при декодировании уменьшается в 2/4/8 раз, пока RGB565-буфер больше лимита. */
#define ROT_PHOTO_MAX  (1000 * 1024)   /* фото: 1600x1200 -> 800x600 */
#define ROT_VIDEO_MAX  (800 * 1024)    /* видео и стрим: 1024x768 -> 512x384 */

/* Рабочие буферы выделяются один раз и живут до app_rot_release()
   (её вызывает app_cam_deinit). Вызовы потокобезопасны. */
void app_rot_init(void);
void app_rot_release(void);

/* Размер кадра после поворота (rot: 0 нет, 1 = 90° по часовой, 2 = 90° против). */
void app_rot_dims(uint16_t w, uint16_t h, uint8_t rot, size_t max_bytes,
                  uint16_t *ow, uint16_t *oh);

/* Повёрнутая копия кадра (malloc, освобождать free) или NULL, если rot == 0 или ошибка.
   quality: 4..63, как в настройках. */
uint8_t *app_rot_try(const camera_fb_t *fb, uint8_t rot, uint8_t quality,
                     size_t max_bytes, size_t *olen);
