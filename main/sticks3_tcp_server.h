#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Control command callback: receives command string (without \x01 prefix)
// Should write response (without \x01 prefix) into resp_buf.
// Return true if a response was written.
typedef bool (*ctrl_cmd_cb_t)(const char *cmd, char *resp_buf, size_t resp_size);

esp_err_t sticks3_tcp_server_start(uint16_t port);
esp_err_t sticks3_tcp_server_stop(void);
bool      sticks3_tcp_server_is_client_connected(void);

// Register control command callback (called by main)
void      sticks3_tcp_server_set_ctrl_cb(ctrl_cmd_cb_t cb);

// Diagnostic mode: echo test without UART
void      sticks3_tcp_server_set_diag(bool enabled);

// Stream buffers shared with uart_bridge
extern StreamBufferHandle_t tcp_to_uart_buf;
extern StreamBufferHandle_t uart_to_tcp_buf;

#ifdef __cplusplus
}
#endif
