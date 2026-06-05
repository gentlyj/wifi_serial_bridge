#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sticks3_nvs_init(void);
esp_err_t sticks3_nvs_save_baud(uint32_t baud);
esp_err_t sticks3_nvs_load_baud(uint32_t *baud);

#ifdef __cplusplus
}
#endif
