#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_STATE_WIFI_DISCONNECTED = 0,
    UI_STATE_PROVISIONING,
    UI_STATE_WIFI_WAITING,
    UI_STATE_BRIDGE_ACTIVE,
} ui_state_t;

typedef enum {
    APP_EVENT_WIFI_CONNECTED = 0,
    APP_EVENT_WIFI_FAILED,
    APP_EVENT_TCP_CLIENT_CONNECTED,
    APP_EVENT_TCP_CLIENT_DISCONNECTED,
    APP_EVENT_BTN_A_CLICK,
    APP_EVENT_BTN_A_LONG_PRESS,
    APP_EVENT_BTN_B_CLICK,
    APP_EVENT_BTN_B_LONG_PRESS,
} app_event_t;

void sticks3_ui_init(QueueHandle_t event_queue);
void sticks3_ui_set_state(ui_state_t state);
void sticks3_ui_update_ip(const char *ip);
void sticks3_ui_update_baud(uint32_t baud);
void sticks3_ui_update_stats(uint64_t rx_bytes, uint64_t tx_bytes);
void sticks3_ui_update_status(const char *msg);
void sticks3_ui_update_ws_connected(bool connected);

#ifdef __cplusplus
}
#endif
