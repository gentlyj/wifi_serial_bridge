#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sticks3_nvs_init(void);
esp_err_t sticks3_nvs_save_baud(uint32_t baud);
esp_err_t sticks3_nvs_load_baud(uint32_t *baud);
esp_err_t sticks3_nvs_save_macros(const char *json);
esp_err_t sticks3_nvs_load_macros(char *json, size_t json_size);

#ifdef __cplusplus
}
#endif
