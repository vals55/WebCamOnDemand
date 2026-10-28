// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SD_MOUNT "/sdcard"

esp_err_t sd_mount(void);
void sd_unmount(void);
int sd_free_mb(void);

/* Имя файла: /sdcard/YYYYMMDD/PFX_HHMMSS.ext или /sdcard/nosync/PFX_<boot>.ext (время не синхронизировано).
   Нужные каталоги создаются. */
void sd_make_name(char *out, size_t n, const char *prefix, const char *ext);

/* Циклическая перезапись: удаляет самые старые данные, пока свободно < target_mb.
   Порядок: старые файлы в корне -> nosync -> каталоги дат по возрастанию.
   Возвращает свободное место (МБ) после очистки или -1. */
int sd_free_space(int target_mb);

esp_err_t sd_save(const char *path, const uint8_t *data, size_t len);
void sd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
