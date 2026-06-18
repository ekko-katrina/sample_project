#include "ffs.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"

void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t spiffs_structure = {
        .base_path = "/storage",
        .format_if_mount_failed = true,
        .max_files = 2,
        .partition_label = "storage",
    };
    esp_vfs_spiffs_register(&spiffs_structure);
}
