set(src_dirs
            LED
            KEY
            EXIT
            UART
            MYUART
            SPIFFS
            WIFISTA
            WIFIAP
            WIFIPROV
            MYNTC
            # TM-Y05 BLE simulator and protocol sources
            MYBLE
            )

set(include_dirs
            LED
            KEY
            EXIT
            UART
            MYUART
            SPIFFS
            WIFISTA
            WIFIAP
            WIFIPROV
            MYNTC
            MYBLE
            )


set(requires
            esp_wifi
            esp_http_server
            esp_netif
            esp_event
            esp_app_format
            nvs_flash
            driver
            spiffs
            bt)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options (-ffast-math -O3 -Wno-error=format=-Wno-format)
