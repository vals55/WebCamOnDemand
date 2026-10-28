# Changelog / Журнал изменений

Формат: [Keep a Changelog](https://keepachangelog.com/), версионирование: SemVer.

## [0.2.0] - 2026-10-29
### Added / Добавлено
- Циклическая перезапись SD (`sd_overwrite`): удаление старейших папок/файлов при нехватке места. / Circular SD recording.
- Файлы в папках по датам `/sdcard/YYYYMMDD/IMG_HHMMSS.jpg`. / Date-based folders.
- Запись на SD только при установленном времени; разовая синхронизация по Wi-Fi с экспоненциальным повтором (5 мин - 1 ч). / SD writes only with valid time; one-shot SNTP sync with backoff.
- Ротация `log.txt` (256 КБ -> `log.old`). / Log rotation.
- `device_id` из двух последних октетов MAC: `webcam_XXXX`. / Short device ID.
- Конфиг v3, миграции v1 -> v3 и v2 -> v3. / Config v3 with migrations.

### Fixed / Исправлено
- Часовой пояс (TZ) теперь устанавливается при каждом пробуждении (после глубокого сна `localtime()` шла по UTC). / TZ restored after deep sleep.
- Предупреждение `-Wshift-count-negative` в `power_led()` на ESP32-S3. / Compiler warning on S3.
- Поле `file` в журнале событий увеличено до 40 байт. / Event log path length.

## [0.1.0]
- Первая версия: глубокий сон, фото/видео по таймеру и PIR, серверный режим, портал настроек, MQTT + Home Assistant. / Initial version.
