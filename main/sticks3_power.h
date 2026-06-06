#pragma once

#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize M5PM1 power management IC
 *        Sets up I2C on port 1 (SDA=47, SCL=48)
 */
esp_err_t sticks3_power_init(void);

/**
 * @brief Enable LCD power supply via M5PM1 GPIO2 (push-pull HIGH)
 */
esp_err_t sticks3_power_lcd_enable(void);

/**
 * @brief Get battery voltage in millivolts
 * @param[out] mv Pointer to store battery voltage
 */
esp_err_t sticks3_power_get_battery(uint16_t *mv);

/**
 * @brief Check if battery is charging
 * @param[out] charging Pointer to store charging status (true = charging)
 */
esp_err_t sticks3_power_is_charging(bool *charging);

/**
 * @brief Get the I2C port number (shared by M5PM1 and ES8311)
 */
i2c_port_t sticks3_power_get_i2c_port(void);

/**
 * @brief Enable speaker PA via M5PM1 GPIO3 (output HIGH)
 */
esp_err_t sticks3_power_speaker_pa_enable(void);

/**
 * @brief Disable speaker PA via M5PM1 GPIO3 (output LOW)
 */
esp_err_t sticks3_power_speaker_pa_disable(void);

/**
 * @brief Shut down the system via M5PM1 (cuts all power)
 */
esp_err_t sticks3_power_shutdown(void);

/**
 * @brief Set battery low-voltage protection threshold
 * @param mv Threshold in millivolts (2000-4000 range, hardware-enforced auto-shutdown)
 */
esp_err_t sticks3_power_set_lvp(uint16_t mv);

#ifdef __cplusplus
}
#endif
