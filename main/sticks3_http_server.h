#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start HTTP server on port 80
 *        Serves pc_tool.html when no WS client is connected,
 *        or a "busy" page if WS client is already connected.
 */
esp_err_t sticks3_http_server_start(void);

/**
 * @brief Stop HTTP server
 */
esp_err_t sticks3_http_server_stop(void);

/**
 * @brief Set whether a WebSocket client is currently connected
 *        Called by main to keep HTTP server state in sync.
 */
void sticks3_http_server_set_ws_connected(bool connected);

#ifdef __cplusplus
}
#endif
