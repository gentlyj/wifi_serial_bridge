#pragma once

#include "esp_err.h"
#include "hal/gpio_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sticks3_uart_bridge_init(gpio_num_t tx_pin, gpio_num_t rx_pin);
esp_err_t sticks3_uart_bridge_set_baud(uint32_t baud_rate);
esp_err_t sticks3_uart_bridge_start(void);
esp_err_t sticks3_uart_bridge_stop(void);
uint32_t  sticks3_uart_bridge_get_baud(void);
void      sticks3_uart_bridge_get_stats(uint64_t *rx_bytes, uint64_t *tx_bytes);

#ifdef __cplusplus
}
#endif
