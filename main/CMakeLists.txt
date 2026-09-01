idf_component_register(
    SRCS "main.c" "tmy05_console.c"
    INCLUDE_DIRS "."
    REQUIRES BSP nvs_flash console
)
