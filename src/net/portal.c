// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#include "portal.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_camera.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_sta.h"
#include "portal_page.h"
#include "app_config.h"

static const char *TAG = "portal";

#define PORTAL_IDLE_S   300
#define SCAN_MAX        20
#define MAX_BODY        4096

typedef enum { T_HDR, T_STR, T_PASS, T_MAC, T_U8, T_U16, T_U32, T_BOOL } ftype_t;
typedef struct { int v; const char *label; } opt_t;
typedef struct {
    const char *name;
    const char *label;
    ftype_t type;
    uint16_t off;
    uint16_t size;
    const opt_t *opts;
    const char *attrs;   /* доп. атрибуты input: min/max/placeholder/required */
    const char *hint;    /* подсказка под полем */
} field_t;

static const opt_t O_ACT[] = {{0, "Нет"}, {1, "Фото"}, {2, "Видео"}, {3, "Фото + видео"}, {-1, NULL}};
static const opt_t O_MQ[] = {{0, "Выключен"}, {1, "Каждое пробуждение"}, {2, "Пакетами"},
                             {3, "Только в серверном режиме"}, {-1, NULL}};
static const opt_t O_FS[] = {{FRAMESIZE_QQVGA, "160x120"}, {FRAMESIZE_QVGA, "320x240"},
                             {FRAMESIZE_CIF, "400x296"}, {FRAMESIZE_VGA, "640x480"},
                             {FRAMESIZE_SVGA, "800x600"}, {FRAMESIZE_XGA, "1024x768"},
                             {FRAMESIZE_HD, "1280x720"}, {FRAMESIZE_SXGA, "1280x1024"},
                             {FRAMESIZE_UXGA, "1600x1200"}, {FRAMESIZE_FHD, "1920x1080 (OV5640)"},
                             {FRAMESIZE_QXGA, "2048x1536 (OV5640)"}, {-1, NULL}};
static const opt_t O_SFS[] = {{FRAMESIZE_QVGA, "320x240"}, {FRAMESIZE_VGA, "640x480"},
                              {FRAMESIZE_SVGA, "800x600"}, {FRAMESIZE_XGA, "1024x768"},
                              {FRAMESIZE_HD, "1280x720"}, {-1, NULL}};

#define SZ(m) sizeof(((app_config_t *)0)->m)
#define HDR(l)              {"", l, T_HDR, 0, 0, NULL, NULL, NULL}
#define FS(m, l, a, h)      {#m, l, T_STR, offsetof(app_config_t, m), SZ(m), NULL, a, h}
#define FP(m, l, h)         {#m, l, T_PASS, offsetof(app_config_t, m), SZ(m), NULL, NULL, h}
#define F8(m, l, o, a, h)   {#m, l, T_U8, offsetof(app_config_t, m), 1, o, a, h}
#define F16(m, l, a, h)     {#m, l, T_U16, offsetof(app_config_t, m), 2, NULL, a, h}
#define F32(m, l, a, h)     {#m, l, T_U32, offsetof(app_config_t, m), 4, NULL, a, h}
#define FM(m, l, h)         {#m, l, T_MAC, offsetof(app_config_t, m), 6, NULL, \
                             "placeholder='AA:BB:CC:DD:EE:FF' maxlength='17'", h}
#define FB(m, l, h)         {#m, l, T_BOOL, offsetof(app_config_t, m), 1, NULL, NULL, h}

#define PASS_HINT "Пусто = не менять, «-» = очистить"

static const field_t FIELDS[] = {
    HDR("Wi-Fi"),
    FS(wifi_ssid, "SSID", "required", NULL),
    FP(wifi_pass, "Пароль", PASS_HINT),
    FM(wifi_bssid, "BSSID", "Пусто = любой роутер с этим SSID. Заполняется при выборе сети из списка"),
    F8(wifi_timeout_s, "Таймаут подключения, с", NULL, "min='5' max='60'", "5–60"),

    HDR("Время"),
    FS(ntp_server, "NTP-сервер", "placeholder='ru.pool.ntp.org'", NULL),
    FS(tz, "Часовой пояс (POSIX TZ)", "placeholder='MSK-3'", "Например, MSK-3 для Москвы"),

    HDR("Съёмка по таймеру"),
    FB(timer_enabled, "Включить таймер", NULL),
    F32(timer_interval_s, "Интервал, с", "min='10'", "Не меньше 10 с. 600 = раз в 10 минут"),
    F8(timer_action, "Действие", O_ACT, NULL, NULL),

    HDR("Датчик движения и кнопка"),
    FB(pir_enabled, "Включить PIR (GPIO" PIR_GPIO_STR ")", NULL),
    F8(pir_action, "Действие по PIR", O_ACT, NULL, NULL),
    F16(pir_dead_s, "Мёртвое время PIR, с", "min='0' max='3600'",
        "Повторные срабатывания в этот период игнорируются"),
    FB(btn_enabled, "Включить кнопку IO0",
       "Короткое нажатие: серверный режим. Удержание 3 с: этот портал"),

    HDR("Камера"),
    F8(framesize, "Разрешение фото и видео", O_FS, NULL,
       "Больше 1600x1200 только у OV5640. Для видео реальный FPS падает с ростом разрешения"),
    F8(jpeg_quality, "Качество JPEG фото и видео", NULL, "min='4' max='63'",
       "4–63, меньше = лучше и крупнее файл. Для OV5640 рекомендуется 10–15"),
    F8(stream_framesize, "Разрешение стрима", O_SFS, NULL, "1024x768 — около 8–12 fps по Wi-Fi"),
    F8(stream_quality, "Качество JPEG стрима", NULL, "min='4' max='63'", "Рекомендуется 12–20"),
    F16(video_len_s, "Длина видео, с", "min='1' max='300'", "1–300"),
    F8(video_fps, "FPS видео", NULL, "min='1' max='30'", "1–30, реальный FPS зависит от разрешения"),
    FB(sd_overwrite, "Циклическая перезапись SD (удалять самые старые файлы при нехватке места)", NULL),

    HDR("Серверный режим"),
    F16(srv_idle_s, "Простой до сна, с", "min='10' max='3600'",
        "Сколько ждать без клиентов, прежде чем уснуть"),
    FB(srv_periodic, "Периодически просыпаться в серверный режим", NULL),
    F32(srv_period_s, "Период серверного режима, с", "min='60'", "3600 = раз в час"),

    HDR("MQTT и Home Assistant"),
    F8(mqtt_policy, "Политика отправки", O_MQ, NULL, NULL),
    FS(mqtt_host, "Адрес брокера", "placeholder='192.168.1.10'", NULL),
    F16(mqtt_port, "Порт", "min='0' max='65535'", "0 = по умолчанию (1883, с TLS 8883)"),
    FB(mqtt_tls, "TLS", NULL),
    FS(mqtt_user, "Логин", NULL, NULL),
    FP(mqtt_pass, "Пароль", PASS_HINT),
    FS(mqtt_prefix, "Префикс топиков", NULL, NULL),
    FS(device_id, "Device ID", "required", "Используется в топиках и имени точки доступа"),
    FB(ha_discovery, "Автообнаружение Home Assistant", NULL),
    FS(ha_prefix, "Discovery-префикс", "placeholder='homeassistant'", NULL),
    FB(mqtt_snapshot, "Слать снимок при событии", NULL),
    F8(mqtt_snap_quality, "Качество снимка для MQTT", NULL, "min='4' max='63'", "4–63, меньше = лучше"),
    F8(mqtt_batch_n, "Событий в пакете", NULL, "min='1' max='100'", "Для политики «Пакетами», 1–100"),
    F8(mqtt_timeout_s, "Таймаут подключения к брокеру, с", NULL, "min='3' max='60'", "3–60"),
};
#define NFIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

/* Значки "показать/скрыть пароль" (как в eMeter32) */
#define SVG_OPEN "<svg viewBox='0 0 24 24' width='22' height='22' fill='none' stroke='currentColor' " \
                 "stroke-width='1' stroke-linecap='round' stroke-linejoin='round'"
#define SVG_EYE  "<path d='M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z'/><circle cx='12' cy='12' r='3'/>"
static const char EYE_ICONS[] =
    SVG_OPEN " class='on'>" SVG_EYE "</svg>"
    SVG_OPEN " class='off'>" SVG_EYE "<path d='M3 3l18 18'/></svg>";

static const char SCAN_BLOCK[] =
    "<button type='button' id='sb' onclick='scan()'>Сканировать сети</button>"
    "<div class='dd' id='dd'>"
    "<button type='button' class='ddb' id='ddb' onclick='tgl()'>Выберите сеть</button>"
    "<div class='ddl' id='nets'></div></div>";

static app_config_t *s_cfg;
static volatile TickType_t s_last;
static volatile bool s_saved;
static volatile bool s_dns_run;
static volatile bool s_dns_done;
static volatile bool s_scanning;

static void touch(void) { s_last = xTaskGetTickCount(); }

static void dns_task(void *arg) {
    static const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in a = {0};
    struct timeval tv = {.tv_sec = 1};

    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "dns bind failed");
        if (s >= 0) close(s);
        s_dns_done = true;
        vTaskDelete(NULL);
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (s_dns_run) {
        uint8_t rx[256], tx[300];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, rx, sizeof(rx), 0, (struct sockaddr *)&from, &fl);
        int i = 12, qend, isA, tl;

        if (n < 17) {
            continue;
        }
        while (i < n && rx[i]) {
            i += rx[i] + 1;
        }
        if (i + 5 > n) {
            continue;
        }
        isA = (rx[i + 1] == 0 && rx[i + 2] == 1);
        qend = i + 5;
        memcpy(tx, rx, qend);
        tx[2] = 0x81; tx[3] = 0x80;
        tx[6] = 0; tx[7] = isA ? 1 : 0;
        tx[8] = tx[9] = tx[10] = tx[11] = 0;
        tl = qend;
        if (isA) {
            memcpy(tx + tl, ans, sizeof(ans));
            tl += sizeof(ans);
        }
        sendto(s, tx, tl, 0, (struct sockaddr *)&from, fl);
    }
    close(s);
    s_dns_done = true;
    vTaskDelete(NULL);
}

/* ---------- Вспомогательные функции ---------- */

static void html_esc(char *dst, size_t n, const char *src) {
    size_t o = 0;

    for (; *src && o + 7 < n; src++) {
        switch (*src) {
        case '&': o += snprintf(dst + o, n - o, "&amp;"); break;
        case '<': o += snprintf(dst + o, n - o, "&lt;"); break;
        case '>': o += snprintf(dst + o, n - o, "&gt;"); break;
        case '"': o += snprintf(dst + o, n - o, "&quot;"); break;
        case '\'': o += snprintf(dst + o, n - o, "&#39;"); break;
        default: dst[o++] = *src;
        }
    }
    dst[o] = '\0';
}

static void json_esc(char *dst, size_t n, const char *src) {
    size_t o = 0;

    for (; *src && o + 7 < n; src++) {
        unsigned char ch = (unsigned char)*src;

        if (ch == '"' || ch == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += snprintf(dst + o, n - o, "\\u%04x", ch);
        } else {
            dst[o++] = (char)ch;
        }
    }
    dst[o] = '\0';
}

static void url_decode(char *s) {
    char *w = s;

    while (*s) {
        if (*s == '+') {
            *w++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *w++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *w++ = *s++;
        }
    }
    *w = '\0';
}

static uint32_t get_num(const field_t *f, const app_config_t *c) {
    const uint8_t *p = (const uint8_t *)c + f->off;

    switch (f->type) {
    case T_U8: return *p;
    case T_U16: return *(const uint16_t *)p;
    case T_U32: return *(const uint32_t *)p;
    default: return 0;
    }
}

static bool parse_mac(const char *t, uint8_t *mac) {
    unsigned v[6];

    if (strlen(t) != 17 ||
        sscanf(t, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

/* false = неверное значение */
static bool set_field(const field_t *f, app_config_t *c, const char *val) {
    uint8_t *p = (uint8_t *)c + f->off;
    unsigned long v;

    switch (f->type) {
    case T_MAC:
        if (val[0] == '\0') {
            memset(p, 0, 6);
            c->wifi_bssid_set = false;
        } else if (parse_mac(val, p)) {
            c->wifi_bssid_set = true;
        } else {
            return false;
        }
        break;
    case T_STR:
        strlcpy((char *)p, val, f->size);
        break;
    case T_PASS:
        if (strcmp(val, "-") == 0) {
            p[0] = '\0';
        } else if (val[0]) {
            strlcpy((char *)p, val, f->size);
        }
        break;
    case T_U8:
        v = strtoul(val, NULL, 10);
        *p = v > 255 ? 255 : (uint8_t)v;
        break;
    case T_U16:
        v = strtoul(val, NULL, 10);
        *(uint16_t *)p = v > 65535 ? 65535 : (uint16_t)v;
        break;
    case T_U32:
        *(uint32_t *)p = (uint32_t)strtoul(val, NULL, 10);
        break;
    case T_BOOL:
        *(bool *)p = true;
        break;
    default:
        break;
    }
    return true;
}

static const char *hint_html(const field_t *f, char *buf, size_t n) {
    if (!f->hint) {
        return "";
    }
    snprintf(buf, n, "<span class='h'>%s</span>", f->hint);
    return buf;
}

/* Одно поле формы в line */
static void render_field(const field_t *f, char *line, size_t ln, char *esc, size_t en) {
    const uint8_t *p = (const uint8_t *)s_cfg + f->off;
    char hb[160];
    const char *at = f->attrs ? f->attrs : "";
    const char *h = hint_html(f, hb, sizeof(hb));

    switch (f->type) {
    case T_STR:
        html_esc(esc, en, (const char *)p);
        snprintf(line, ln,
                 "<label>%s<input type='text' name='%s' id='%s' value='%s' maxlength='%u' %s>%s</label>",
                 f->label, f->name, f->name, esc, (unsigned)f->size - 1, at, h);
        break;
    case T_PASS:
        snprintf(line, ln,
                 "<label>%s<span class='pw'><input type='password' name='%s' id='%s' value='' "
                 "maxlength='%u' placeholder='%s' autocomplete='off'>"
                 "<button type='button' class='eye' title='Показать/скрыть пароль' data-f='%s' "
                 "onclick='eye(this)'>%s</button></span>%s</label>",
                 f->label, f->name, f->name, (unsigned)f->size - 1,
                 p[0] ? "(задан)" : "(не задан)", f->name, EYE_ICONS, h);
        break;
    case T_MAC:
        if (s_cfg->wifi_bssid_set) {
            snprintf(esc, en, "%02X:%02X:%02X:%02X:%02X:%02X", p[0], p[1], p[2], p[3], p[4], p[5]);
        } else {
            esc[0] = '\0';
        }
        snprintf(line, ln, "<label>%s<input type='text' name='%s' id='%s' value='%s' %s>%s</label>",
                 f->label, f->name, f->name, esc, at, h);
        break;
    case T_BOOL:
        snprintf(line, ln,
                 "<label class='cb'><input type='checkbox' name='%s' value='1'%s>%s</label>%s",
                 f->name, *(const bool *)p ? " checked" : "", f->label, h);
        break;
    default:
        if (f->opts) {
            int n = snprintf(line, ln, "<label>%s<select name='%s' id='%s'>", f->label, f->name, f->name);
            for (const opt_t *o = f->opts; o->label && n < (int)ln; o++) {
                n += snprintf(line + n, ln - n, "<option value='%d'%s>%s</option>",
                              o->v, (uint32_t)o->v == get_num(f, s_cfg) ? " selected" : "", o->label);
            }
            if (n < (int)ln) {
                snprintf(line + n, ln - n, "</select>%s</label>", h);
            }
        } else {
            snprintf(line, ln,
                     "<label>%s<input type='number' name='%s' id='%s' value='%u' %s>%s</label>",
                     f->label, f->name, f->name, (unsigned)get_num(f, s_cfg), at, h);
        }
        break;
    }
}

/* ---------- Обработчики HTTP ---------- */

static esp_err_t root_get(httpd_req_t *req) {
    char line[1400], esc[200];
    uint8_t mac[6] = {0};
    bool open = false;

    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);

    esp_wifi_get_mac(WIFI_IF_AP, mac);
    html_esc(esc, sizeof(esc), s_cfg->device_id);
    snprintf(line, sizeof(line),
             "WebCam — настройки</h1><p>ID: %s · MAC %02X:%02X:%02X:%02X:%02X:%02X</p>"
             "<form method='post' action='/save'>",
             esc, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    httpd_resp_sendstr_chunk(req, line);

    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];

        if (f->type == T_HDR) {
            snprintf(line, sizeof(line), "%s<fieldset><legend>%s</legend>",
                     open ? "</fieldset>" : "", f->label);
            open = true;
        } else {
            if (strcmp(f->name, "wifi_ssid") == 0) {
                httpd_resp_sendstr_chunk(req, SCAN_BLOCK);
            }
            render_field(f, line, sizeof(line), esc, sizeof(esc));
        }
        httpd_resp_sendstr_chunk(req, line);
    }
    if (open) {
        httpd_resp_sendstr_chunk(req, "</fieldset>");
    }
    httpd_resp_sendstr_chunk(req, "<button type='submit'>Сохранить и перезагрузить</button></form>");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t scan_get(httpd_req_t *req) {
    wifi_ap_record_t *recs;
    uint16_t n = SCAN_MAX;
    bool first = true;

    touch();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (s_scanning) {
        return httpd_resp_sendstr(req, "[]");
    }
    s_scanning = true;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        s_scanning = false;
        return httpd_resp_sendstr(req, "[]");
    }
    recs = calloc(SCAN_MAX, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        s_scanning = false;
        return httpd_resp_sendstr(req, "[]");
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        n = 0;
    }
    s_scanning = false;

    httpd_resp_sendstr_chunk(req, "[");
    for (uint16_t i = 0; i < n; i++) {
        char ss[200], line[400];

        if (recs[i].ssid[0] == 0) {
            continue;
        }
        json_esc(ss, sizeof(ss), (const char *)recs[i].ssid);
        snprintf(line, sizeof(line),
                 "%s{\"ssid\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"rssi\":%d,\"ch\":%d,\"auth\":%d}",
                 first ? "" : ",", ss, recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                 recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                 recs[i].rssi, recs[i].primary, (int)recs[i].authmode);
        httpd_resp_sendstr_chunk(req, line);
        first = false;
    }
    free(recs);
    httpd_resp_sendstr_chunk(req, "]");
    touch();
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t send_msg(httpd_req_t *req, const char *status, const char *cls,
                          const char *msg, bool back) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "WebCam</h1><fieldset><p class='");
    httpd_resp_sendstr_chunk(req, cls);
    httpd_resp_sendstr_chunk(req, "'>");
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, "</p>");
    if (back) {
        httpd_resp_sendstr_chunk(req, "<p><a href='/'>Назад к настройкам</a></p>");
    }
    httpd_resp_sendstr_chunk(req, "</fieldset></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t save_post(httpd_req_t *req) {
    app_config_t *n;
    char *body, *save = NULL, *tok;
    char msg[256], esc[100];
    const char *bad = NULL;
    int got = 0;

    touch();
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return send_msg(req, "400 Bad Request", "err", "Неверный размер запроса", true);
    }
    body = malloc(req->content_len + 1);
    n = malloc(sizeof(*n));
    if (!body || !n) {
        free(body); free(n);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(body); free(n);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    *n = *s_cfg;
    for (size_t i = 0; i < NFIELDS; i++) {
        if (FIELDS[i].type == T_BOOL) {
            *((bool *)((uint8_t *)n + FIELDS[i].off)) = false;
        }
    }
    for (tok = strtok_r(body, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        char *val = strchr(tok, '=');
        if (!val) continue;
        *val++ = '\0';
        url_decode(tok);
        url_decode(val);
        for (size_t i = 0; i < NFIELDS; i++) {
            if (FIELDS[i].type != T_HDR && strcmp(FIELDS[i].name, tok) == 0) {
                if (!set_field(&FIELDS[i], n, val) && !bad) {
                    bad = FIELDS[i].label;
                }
                break;
            }
        }
    }
    free(body);

    if (bad) {
        free(n);
        snprintf(msg, sizeof(msg), "Неверное значение поля «%s»", bad);
        return send_msg(req, "400 Bad Request", "err", msg, true);
    }
    if (!n->wifi_ssid[0]) {
        free(n);
        return send_msg(req, "400 Bad Request", "err", "Не указан SSID сети Wi-Fi", true);
    }
    if (config_save(n) != ESP_OK) {
        free(n);
        return send_msg(req, "500 Internal Server Error", "err", "Ошибка записи настроек", true);
    }
    *s_cfg = *n;
    free(n);
    ESP_LOGI(TAG, "settings saved, restarting");

    html_esc(esc, sizeof(esc), s_cfg->wifi_ssid);
    snprintf(msg, sizeof(msg),
             "Настройки сохранены. Камера перезагрузится и подключится к сети «%s».", esc);
    s_saved = true;
    return send_msg(req, "200 OK", "ok", msg, false);
}

static esp_err_t favicon_get(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* Любой неизвестный адрес (проверки Android/iOS/Windows) -> на главную */
static esp_err_t redirect_get(httpd_req_t *req) {
    touch();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

void portal_run(app_config_t *cfg) {
    wifi_config_t ap = {0};
    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    const httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t u_save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    const httpd_uri_t u_scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get};
    const httpd_uri_t u_fav = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
    const httpd_uri_t u_any = {.uri = "/*", .method = HTTP_GET, .handler = redirect_get};
    size_t idl;

    s_cfg = cfg;
    s_saved = false;
    s_scanning = false;
    if (net_init() != ESP_OK) {
        ESP_LOGE(TAG, "net init failed");
        return;
    }

    idl = strlen(cfg->device_id);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "WebCam-%s",
             cfg->device_id + (idl > 4 ? idl - 4 : 0));
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) {
        return;
    }

    s_dns_run = true;
    s_dns_done = false;
    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);

    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.stack_size = 10240;
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    if (httpd_start(&srv, &hc) == ESP_OK) {
        httpd_register_uri_handler(srv, &u_root);
        httpd_register_uri_handler(srv, &u_save);
        httpd_register_uri_handler(srv, &u_scan);
        httpd_register_uri_handler(srv, &u_fav);
        httpd_register_uri_handler(srv, &u_any);
    }
    ESP_LOGI(TAG, "portal up: SSID %s", ap.ap.ssid);

    touch();
    while (!s_saved &&
           (xTaskGetTickCount() - s_last) < pdMS_TO_TICKS(PORTAL_IDLE_S * 1000)) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (s_saved) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    s_dns_run = false;
    if (srv) {
        httpd_stop(srv);
    }
    for (int i = 0; i < 30 && !s_dns_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_wifi_stop();
    if (s_saved) {
        esp_restart();
    }
}
