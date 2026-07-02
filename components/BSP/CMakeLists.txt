set(src_dirs
            LED
            KEY
            EXIT
            UART
            MYUART
            SPIFFS
            WIFISTA
            WIFIAP
            MYNTC
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
            MYNTC
            MYBLE
            )


set(requires
            esp_wifi
            driver
            spiffs
            bt)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
component_compile_options (-ffast-math -O3 -Wno-error=format=-Wno-format)
