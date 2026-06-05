#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sticks3_tcp_server_start(uint16_t port);
esp_err_t sticks3_tcp_server_stop(void);
bool      sticks3_tcp_server_is_client_connected(void);

// Stream buffers shared with uart_bridge
extern StreamBufferHandle_t tcp_to_uart_buf;
extern StreamBufferHandle_t uart_to_tcp_buf;

#ifdef __cplusplus
}
#endif
