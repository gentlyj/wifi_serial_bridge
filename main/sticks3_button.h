#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize button input (GPIO11 + GPIO12)
 *        Registers LVGL input device for button events
 */
esp_err_t sticks3_button_init(void);

#ifdef __cplusplus
}
#endif
