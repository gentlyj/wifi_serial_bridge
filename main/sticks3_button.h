#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize button input (GPIO11 + GPIO12)
 *        Registers LVGL input device for button events
 */
esp_err_t sticks3_button_init(void);

/**
 * @brief Enable GPIO interrupt on button pins for wake detection.
 *        Works even when LVGL is stopped.
 * @param callback Function called from ISR when any button is pressed (active-low falling edge)
 */
typedef void (*button_wake_cb_t)(void *arg);
esp_err_t sticks3_button_enable_wake(button_wake_cb_t callback, void *arg);

/**
 * @brief Disable button wake interrupt (call after screen wakes)
 */
void sticks3_button_disable_wake(void);

#ifdef __cplusplus
}
#endif
