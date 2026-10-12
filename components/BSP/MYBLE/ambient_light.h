#ifndef TMY06_AMBIENT_LIGHT_H
#define TMY06_AMBIENT_LIGHT_H

#include "esp_err.h"
#include "tmy06_protocol.h"

esp_err_t ambient_light_load(tmy06_ambient_settings_t *settings);
esp_err_t ambient_light_save(const tmy06_ambient_settings_t *settings);
esp_err_t ambient_light_start(void);

#endif
