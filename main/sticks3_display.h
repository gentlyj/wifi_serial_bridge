#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ST7789 display with LVGL
 *        - Configures SPI bus (SPI3_HOST)
 *        - Creates ST7789 panel (135x240, offsets x=52 y=40)
 *        - Initializes LVGL and registers display
 *        - Sets up backlight PWM on GPIO38
 */
esp_err_t sticks3_display_init(void);

/**
 * @brief Set backlight brightness
 * @param brightness Brightness level 0-255
 */
void sticks3_display_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
