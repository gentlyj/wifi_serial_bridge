#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sticks3_wifi_init(void);
esp_err_t sticks3_wifi_connect(const char *ssid, const char *password);
esp_err_t sticks3_wifi_disconnect(void);
esp_err_t sticks3_wifi_get_ip(char *ip_str, size_t max_len);
bool      sticks3_wifi_is_connected(void);
EventGroupHandle_t sticks3_wifi_get_event_group(void);

#ifdef __cplusplus
}
#endif
