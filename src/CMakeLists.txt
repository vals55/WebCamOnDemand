idf_component_register(
    SRCS
        "main.c"
        "config/config.c"
        "power/power.c"
        "camera/cam.c"
        "camera/capture.c"
        "storage/sd.c"
        "storage/avi.c"
        "log/event_log.c"
        "net/wifi_sta.c"
        "net/portal.c"
        "net/server.c"
        "net/app_mqtt.c"
        "net/ha_discovery.c"
    INCLUDE_DIRS "." "config" "power" "camera" "storage" "log" "net"
    REQUIRES nvs_flash driver esp_hw_support esp_rom esp_timer esp_psram
             esp_wifi esp_netif esp_event esp_http_server lwip mqtt esp-tls
             fatfs sdmmc vfs mdns esp32-camera
)
