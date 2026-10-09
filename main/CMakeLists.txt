idf_component_register(
    SRCS "main.c" "tmy05_console.c" "ble_profile_switch.c" "ota_service.c"
         "mqtt_service.c"
    INCLUDE_DIRS "."
    REQUIRES BSP nvs_flash console esp_http_client esp_https_ota app_update
             json mbedtls spi_flash mqtt
)
