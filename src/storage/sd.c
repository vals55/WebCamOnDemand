// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "sd.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "power.h"
#include "app_config.h"

static const char *TAG = "sd";
static sdmmc_card_t *s_card;

#define NOSYNC_DIR     "nosync"
#define LOG_MAX_BYTES  (256 * 1024)
#define CLEAN_MAX_ITER 4000

esp_err_t sd_mount(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err;

    host.flags = SDMMC_HOST_FLAG_1BIT;
    slot.width = 1;
#if CONFIG_IDF_TARGET_ESP32S3
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0 = SD_PIN_D0;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount: %s", esp_err_to_name(err));
        s_card = NULL;
    }
    return err;
}

void sd_unmount(void) {
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
        s_card = NULL;
    }
}

int sd_free_mb(void) {
    uint64_t total = 0, free_b = 0;

    if (!s_card || esp_vfs_fat_info(SD_MOUNT, &total, &free_b) != ESP_OK) {
        return -1;
    }
    return (int)(free_b / (1024 * 1024));
}

/* ---------- Циклическая перезапись ---------- */

static bool is_date_dir(const char *n) {
    if (strlen(n) != 8) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (!isdigit((unsigned char)n[i])) {
            return false;
        }
    }
    return true;
}

static bool is_media(const char *n) {
    return !strncmp(n, "IMG_", 4) || !strncmp(n, "VID_", 4);
}

static void rm_dir_all(const char *dir) {
    char p[300];
    struct dirent *e;
    DIR *d = opendir(dir);

    if (!d) {
        return;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

/* Удаляет самый старый (по имени) файл каталога; пустой каталог удаляет. true = есть прогресс. */
static bool rm_oldest_file(const char *dir) {
    char best[64] = "";
    char p[300];
    struct dirent *e;
    DIR *d = opendir(dir);

    if (!d) {
        return false;
    }
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_type == DT_DIR) {
            continue;
        }
        if (!best[0] || strcmp(e->d_name, best) < 0) {
            strlcpy(best, e->d_name, sizeof(best));
        }
    }
    closedir(d);
    if (!best[0]) {
        return rmdir(dir) == 0;
    }
    snprintf(p, sizeof(p), "%s/%s", dir, best);
    ESP_LOGW(TAG, "recycle %s", p);
    return unlink(p) == 0;
}

static bool delete_oldest(void) {
    char legacy[64] = "";
    char oldest[16] = "";
    char p[300];
    bool has_nosync = false;
    int ndirs = 0;
    struct dirent *e;
    DIR *d = opendir(SD_MOUNT);

    if (!d) {
        return false;
    }
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR) {
            if (!strcmp(e->d_name, NOSYNC_DIR)) {
                has_nosync = true;
                ndirs++;
            } else if (is_date_dir(e->d_name)) {
                ndirs++;
                if (!oldest[0] || strcmp(e->d_name, oldest) < 0) {
                    strlcpy(oldest, e->d_name, sizeof(oldest));
                }
            }
        } else if (is_media(e->d_name)) {
            if (!legacy[0] || strcmp(e->d_name, legacy) < 0) {
                strlcpy(legacy, e->d_name, sizeof(legacy));
            }
        }
    }
    closedir(d);

    if (legacy[0]) {
        snprintf(p, sizeof(p), "%s/%s", SD_MOUNT, legacy);
        ESP_LOGW(TAG, "recycle %s", p);
        return unlink(p) == 0;
    }
    if (has_nosync || oldest[0]) {
        snprintf(p, sizeof(p), "%s/%s", SD_MOUNT, has_nosync ? NOSYNC_DIR : oldest);
        if (ndirs > 1) {
            ESP_LOGW(TAG, "recycle dir %s", p);
            rm_dir_all(p);
            return true;
        }
        return rm_oldest_file(p);
    }
    return false;
}

int sd_free_space(int target_mb) {
    int f = sd_free_mb();

    for (int i = 0; f >= 0 && f < target_mb && i < CLEAN_MAX_ITER; i++) {
        if (!delete_oldest()) {
            break;
        }
        f = sd_free_mb();
    }
    ESP_LOGI(TAG, "free after cleanup: %d MB (target %d)", f, target_mb);
    return f;
}

/* ---------- Имена файлов ---------- */

void sd_make_name(char *out, size_t n, const char *prefix, const char *ext) {
    time_t t = time(NULL);
    struct tm tm;
    char dir[40];

    if (t > 1700000000) {
        localtime_r(&t, &tm);
        snprintf(dir, sizeof(dir), SD_MOUNT "/%04d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        mkdir(dir, 0775);
        snprintf(out, n, "%s/%s_%02d%02d%02d.%s", dir, prefix,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ext);
    } else {
        snprintf(dir, sizeof(dir), SD_MOUNT "/" NOSYNC_DIR);
        mkdir(dir, 0775);
        snprintf(out, n, "%s/%s_%06u.%s", dir, prefix,
                 (unsigned)power_boot_count(), ext);
    }
    {
        struct stat st;
        char base[96];
        char *dot;

        if (stat(out, &st) == 0) {
            strlcpy(base, out, sizeof(base));
            dot = strrchr(base, '.');
            if (dot) {
                *dot = '\0';
            }
            for (int i = 1; i < 100; i++) {
                snprintf(out, n, "%s_%d.%s", base, i, ext);
                if (stat(out, &st) != 0) {
                    break;
                }
            }
        }
    }
}

esp_err_t sd_save(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    size_t w;

    if (!f) {
        return ESP_FAIL;
    }
    w = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || w != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void sd_log(const char *fmt, ...) {
    char msg[120];
    char line[160];
    va_list ap;
    FILE *f;
    struct stat st;
    time_t t = time(NULL);

    if (!s_card) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(line, sizeof(line), "%lld %s\n", (long long)t, msg);

    if (stat(SD_MOUNT "/log.txt", &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        unlink(SD_MOUNT "/log.old");
        rename(SD_MOUNT "/log.txt", SD_MOUNT "/log.old");
    }
    f = fopen(SD_MOUNT "/log.txt", "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}
