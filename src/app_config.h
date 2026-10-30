// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"

/* Выбор платы по целевому чипу: ESP32 = AI-Thinker ESP32-CAM, ESP32-S3 = Freenove ESP32-S3-WROOM CAM */
#if CONFIG_IDF_TARGET_ESP32S3

#define BOARD_NAME       "ESP32-S3-CAM"
#define PIN_BTN          GPIO_NUM_0
#define PIN_PIR          GPIO_NUM_14        /* RTC GPIO 0-21; 13 занят PCLK камеры */
#define PIR_GPIO_STR     "14"
#define PIN_LED          GPIO_NUM_2         /* синий LED на IO2, горит уровнем 1 (WS2812 на IO48 не используется) */
#define HAS_LED          1
#define LED_ON_LEVEL     1

#define CAM_PIN_PWDN     (-1)               /* PWDN не выведен */
#define CAM_PIN_XCLK     15
#define CAM_PIN_SDA      4
#define CAM_PIN_SCL      5
#define CAM_PIN_D0       11
#define CAM_PIN_D1       9
#define CAM_PIN_D2       8
#define CAM_PIN_D3       10
#define CAM_PIN_D4       12
#define CAM_PIN_D5       18
#define CAM_PIN_D6       17
#define CAM_PIN_D7       16
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK     13

/* SD_MMC, 1-bit: на S3 пины задаются явно */
#define SD_PIN_CLK       GPIO_NUM_39
#define SD_PIN_CMD       GPIO_NUM_38
#define SD_PIN_D0        GPIO_NUM_40

#else  /* ESP32 AI-Thinker */

#define BOARD_NAME       "AI-Thinker ESP32-CAM"
#define PIN_BTN          GPIO_NUM_0
#define PIN_PIR          GPIO_NUM_13
#define PIR_GPIO_STR     "13"
#define PIN_LED          GPIO_NUM_33        /* красный светодиод на плате, активный 0 */
#define HAS_LED          1
#define LED_ON_LEVEL     0

#define CAM_PIN_PWDN     32
#define CAM_PIN_XCLK     0
#define CAM_PIN_SDA      26
#define CAM_PIN_SCL      27
#define CAM_PIN_D0       5
#define CAM_PIN_D1       18
#define CAM_PIN_D2       19
#define CAM_PIN_D3       21
#define CAM_PIN_D4       36
#define CAM_PIN_D5       39
#define CAM_PIN_D6       34
#define CAM_PIN_D7       35
#define CAM_PIN_VSYNC    25
#define CAM_PIN_HREF     23
#define CAM_PIN_PCLK     22

#endif

#define CFG_NVS_NS       "cfg"
#define CFG_NVS_KEY      "blob"

#define PIR_LOW_WAIT_MS  3000     /* сколько ждать спада PIR перед сном */
#define PIR_BACKOFF_MIN  15       /* первая перепроверка залипшего PIR, с */
#define PIR_BACKOFF_MAX  600      /* потолок перепроверки, с */
#define BTN_LONG_MS      3000     /* удержание IO0 от момента пробуждения -> портал */

/* Индикация светодиодом */
#ifndef LED_SRV_ON_MS
#define LED_SRV_ON_MS      10      /* серверный режим: длительность вспышки */
#endif
#ifndef LED_SRV_PERIOD_MS
#define LED_SRV_PERIOD_MS  1000    /* серверный режим: период вспышек */
#endif
#ifndef LED_PIR_MS
#define LED_PIR_MS         1000    /* срабатывание PIR: LED горит столько мс */
#endif
