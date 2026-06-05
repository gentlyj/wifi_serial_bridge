#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sticks3_provision_start(void);
esp_err_t sticks3_provision_stop(void);
bool      sticks3_provision_is_active(void);

#ifdef __cplusplus
}
#endif
