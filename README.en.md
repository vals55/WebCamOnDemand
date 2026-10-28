# WebCam on Demand (ESP32-CAM)

**English** | [Русский](README.md)

A battery-powered camera built on ESP32-CAM / ESP32-S3-CAM. Most of the time the device is in deep sleep; it periodically wakes up, saves a photo and/or short video to an SD card, then sleeps again. On demand (button, timer, MQTT command) it starts a Wi-Fi server with snapshot and MJPEG stream, and goes back to sleep after the client disconnects (or an idle timeout).

## Features

- Deep sleep with wake-up by timer, PIR sensor (ext1) and button IO0 (ext0).
- Photos (JPEG) and videos (MJPEG AVI) on SD; timer and PIR actions are configurable: none / photo / video / both.
- PIR dead time after a trigger and protection against a stuck sensor (15 - 600 s backoff).
- Server mode: control page, `/capture`, `/status`, `/sleep`, MJPEG stream on port 81, mDNS `http://<device-id>.local`.
- Setup portal (`WebCam-XXXX` access point, captive DNS, network scan, BSSID pinning).
- MQTT and Home Assistant auto-discovery; publish policy: off / each event / batched / server mode only.
- Circular SD recording: the oldest data is removed when space runs low.
- SNTP time sync; nothing is written to SD until the time is valid.
- The schedule is kept in RTC memory and survives deep sleep.
- Versioned NVS configuration with automatic migration (v1, v2 -> v3).

## Supported boards

| Board | Chip | Flash | PSRAM | PlatformIO env |
|---|---|---|---|---|
| AI-Thinker ESP32-CAM | ESP32 | 4 MB | 4 MB (quad) | `esp32cam_debug` / `esp32cam_release` |
| Freenove ESP32-S3-WROOM CAM | ESP32-S3 | 16 MB | 8 MB (octal) | `esp32s3cam_debug` / `esp32s3cam_release` |

Pins (button, PIR, LED, camera, SD) are defined in `src/app_config.h` for both boards. PIR is GPIO13 on ESP32 and GPIO14 on ESP32-S3. SD runs in SDMMC 1-bit mode.

## Operating modes

| Wake-up cause | Behaviour |
|---|---|
| First boot | Portal if Wi-Fi is not set up, otherwise server mode |
| Timer (`timer_interval_s`) | `timer_action`, then sleep |
| PIR | `pir_action`, event publish, dead time |
| Button IO0, short press | Server mode |
| Button IO0, hold 3 s | Setup portal (LED on) |
| Periodic (`srv_period_s`) | Server mode, if enabled |

## Project layout

```
src/
  main.c               wake-up and sleep logic
  app_config.h         board pins, config version
  camera/              camera driver, photo/video capture
  config/              settings struct, NVS, migrations
  log/                 event log kept in RTC memory
  net/                 Wi-Fi STA, portal, HTTP server, MQTT, Home Assistant
  power/               wake causes, sleep, button, LED
  storage/             SD card, AVI, circular recording
platformio.ini         environments for both boards
sdkconfig.*            ESP-IDF settings (common and per-board)
partitions_*.csv       partition tables
```

## Build and flash

Requires [PlatformIO](https://platformio.org/) (ESP-IDF framework).

```bash
pio run -e esp32s3cam_release            # build
pio run -e esp32s3cam_release -t upload  # flash
pio device monitor                       # serial monitor
```

Use `esp32cam_release` for the AI-Thinker board. Without a USB programmer, put the board into download mode (IO0 to GND).

## First-time setup

1. Power on: the camera creates an open access point `WebCam-XXXX` (XXXX is the last 4 characters of `device_id`).
2. Connect to it and open `http://192.168.4.1` (it opens automatically as a captive portal).
3. Scan for networks, enter the password, optionally MQTT and capture settings, then save.
4. The device reboots into normal operation.

To reopen the portal later, hold IO0 for 3 seconds at wake-up.

## SD storage

```
/sdcard/YYYYMMDD/IMG_HHMMSS.jpg
/sdcard/YYYYMMDD/VID_HHMMSS.avi
/sdcard/log.txt  (rotated at 256 KB -> log.old)
```

With `sd_overwrite` enabled and low space, the oldest date folders are removed first. The write threshold is 20 MB free; cleanup frees an extra 50 MB of headroom, and for video a size estimate is added. Legacy files in the root (`IMG_*`, `VID_*`) are deleted first.

## MQTT

Default prefix: `webcam/<device_id>`.

| Topic | Direction | Description |
|---|---|---|
| `<prefix>/status` | device -> broker | `awake` / `asleep` (LWT, retained) |
| `<prefix>/state` | device -> broker | JSON: wake cause, RSSI, IP, SD free space, interval |
| `<prefix>/event` | device -> broker | JSON events from the log |
| `<prefix>/motion` | device -> broker | `ON` on PIR trigger |
| `<prefix>/snapshot` | device -> broker | JPEG snapshot |
| `<prefix>/cmd/server_mode` | broker -> device | `1` - enter server mode |
| `<prefix>/cmd/snapshot` | broker -> device | `1` - take a snapshot |
| `<prefix>/cmd/set_interval` | broker -> device | timer interval in seconds (10 - 604800) |

Commands are processed at the next wake-up that has a network session (the camera sleeps in between). Home Assistant entities are created automatically (`homeassistant/.../config`).

## Known limitations

- MQTT commands are not instant; they are delivered at the next wake-up.
- The portal access point is open and the Wi-Fi password is sent over HTTP. Use the portal in a trusted environment.
- For high-resolution streaming the bottleneck is Wi-Fi, not PSRAM.

## License

MIT, see [LICENSE](LICENSE). Set the author name in the file before publishing.
