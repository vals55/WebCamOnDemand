// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "avi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#define HDR_SIZE 224
#define WBUF_SIZE (32 * 1024)

struct avi_s {
    FILE *f;
    uint16_t w, h;
    uint32_t frames, max_frames;
    uint32_t movi_bytes;
    uint32_t max_chunk;
    uint32_t *idx;
    uint8_t *wbuf;
    char path[96];
};

static void p32(uint8_t **p, uint32_t v) {
    (*p)[0] = v; (*p)[1] = v >> 8; (*p)[2] = v >> 16; (*p)[3] = v >> 24;
    *p += 4;
}

static void p16(uint8_t **p, uint16_t v) {
    (*p)[0] = v; (*p)[1] = v >> 8;
    *p += 2;
}

static void pcc(uint8_t **p, const char *s) {
    memcpy(*p, s, 4);
    *p += 4;
}

static void build_header(uint8_t *h, const avi_t *a, uint32_t usec, uint32_t rate1000) {
    uint8_t *p = h;
    uint32_t idx_bytes = a->frames * 16;

    pcc(&p, "RIFF");
    p32(&p, HDR_SIZE + a->movi_bytes + idx_bytes);
    pcc(&p, "AVI ");

    pcc(&p, "LIST"); p32(&p, 192); pcc(&p, "hdrl");

    pcc(&p, "avih"); p32(&p, 56);
    p32(&p, usec);
    p32(&p, (uint32_t)((uint64_t)a->max_chunk * rate1000 / 1000));
    p32(&p, 0);
    p32(&p, 0x10);
    p32(&p, a->frames);
    p32(&p, 0);
    p32(&p, 1);
    p32(&p, a->max_chunk);
    p32(&p, a->w); p32(&p, a->h);
    p32(&p, 0); p32(&p, 0); p32(&p, 0); p32(&p, 0);

    pcc(&p, "LIST"); p32(&p, 116); pcc(&p, "strl");

    pcc(&p, "strh"); p32(&p, 56);
    pcc(&p, "vids"); pcc(&p, "MJPG");
    p32(&p, 0);
    p16(&p, 0); p16(&p, 0);
    p32(&p, 0);
    p32(&p, 1000);
    p32(&p, rate1000);
    p32(&p, 0);
    p32(&p, a->frames);
    p32(&p, a->max_chunk);
    p32(&p, 0xFFFFFFFF);
    p32(&p, 0);
    p16(&p, 0); p16(&p, 0); p16(&p, a->w); p16(&p, a->h);

    pcc(&p, "strf"); p32(&p, 40);
    p32(&p, 40);
    p32(&p, a->w); p32(&p, a->h);
    p16(&p, 1); p16(&p, 24);
    pcc(&p, "MJPG");
    p32(&p, (uint32_t)a->w * a->h * 3);
    p32(&p, 0); p32(&p, 0); p32(&p, 0); p32(&p, 0);

    pcc(&p, "LIST"); p32(&p, 4 + a->movi_bytes); pcc(&p, "movi");
}

avi_t *avi_open(const char *path, uint16_t w, uint16_t h, uint32_t max_frames) {
    uint8_t zero[HDR_SIZE] = {0};
    avi_t *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->idx = heap_caps_malloc((size_t)max_frames * 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a->idx) {
        a->idx = malloc((size_t)max_frames * 8);
    }
    a->wbuf = heap_caps_malloc(WBUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a->f = fopen(path, "wb");
    if (a->f && a->wbuf) {
        setvbuf(a->f, (char *)a->wbuf, _IOFBF, WBUF_SIZE);
    }
    strlcpy(a->path, path, sizeof(a->path));
    if (!a->idx || !a->f || fwrite(zero, 1, HDR_SIZE, a->f) != HDR_SIZE) {
        if (a->f) {
            fclose(a->f);
            remove(path);
        }
        free(a->wbuf);
        free(a->idx);
        free(a);
        return NULL;
    }
    a->w = w;
    a->h = h;
    a->max_frames = max_frames;
    return a;
}

esp_err_t avi_add_frame(avi_t *a, const uint8_t *data, size_t len) {
    static const uint8_t pad = 0;
    uint8_t hdr[8];
    uint8_t *p = hdr;
    uint32_t pad_len = len & 1;

    if (a->frames >= a->max_frames) {
        return ESP_ERR_INVALID_SIZE;    /* лимит кадров: не сбой, файл валиден */
    }
    pcc(&p, "00dc");
    p32(&p, (uint32_t)len);

    if (fwrite(hdr, 1, 8, a->f) != 8 ||
        fwrite(data, 1, len, a->f) != len ||
        (pad_len && fwrite(&pad, 1, 1, a->f) != 1)) {
        return ESP_FAIL;
    }
    a->idx[a->frames * 2] = 4 + a->movi_bytes;
    a->idx[a->frames * 2 + 1] = (uint32_t)len;
    a->frames++;
    a->movi_bytes += 8 + (uint32_t)len + pad_len;
    if (len > a->max_chunk) {
        a->max_chunk = (uint32_t)len;
    }
    return ESP_OK;
}

esp_err_t avi_close(avi_t *a, int64_t elapsed_us) {
    uint8_t hdr[HDR_SIZE];
    uint8_t buf[16 * 64];
    uint8_t ih[8];
    uint8_t *p;
    uint32_t i = 0, n;
    uint32_t usec = 100000, rate1000 = 10000;
    esp_err_t err = ESP_OK;

    if (a->frames == 0 || elapsed_us <= 0) {
        err = ESP_FAIL;
    } else {
        usec = (uint32_t)(elapsed_us / a->frames);
        rate1000 = (uint32_t)((uint64_t)a->frames * 1000000000ULL / (uint64_t)elapsed_us);
    }

    p = ih;
    pcc(&p, "idx1");
    p32(&p, a->frames * 16);
    if (fwrite(ih, 1, 8, a->f) != 8) {
        err = ESP_FAIL;
    }
    while (i < a->frames && err == ESP_OK) {
        n = a->frames - i;
        if (n > 64) {
            n = 64;
        }
        p = buf;
        for (uint32_t k = 0; k < n; k++) {
            pcc(&p, "00dc");
            p32(&p, 0x10);
            p32(&p, a->idx[(i + k) * 2]);
            p32(&p, a->idx[(i + k) * 2 + 1]);
        }
        if (fwrite(buf, 1, n * 16, a->f) != n * 16) {
            err = ESP_FAIL;
        }
        i += n;
    }

    build_header(hdr, a, usec, rate1000);
    if (fseek(a->f, 0, SEEK_SET) != 0 || fwrite(hdr, 1, HDR_SIZE, a->f) != HDR_SIZE) {
        err = ESP_FAIL;
    }
    if (fclose(a->f) != 0) {
        err = ESP_FAIL;
    }
    if (a->frames == 0) {
        remove(a->path);
    }
    free(a->wbuf);
    free(a->idx);
    free(a);
    return err;
}
