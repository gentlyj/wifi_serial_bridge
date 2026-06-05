#include "sticks3_wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "sticks3_wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static int s_retry_count = 0;
static bool s_initialized = false;
static bool s_connecting = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connecting && s_retry_count < MAX_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "Retry %d/%d", s_retry_count, MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connecting = false;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t sticks3_wifi_init(void) {
    if (s_initialized) return ESP_OK;

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

esp_err_t sticks3_wifi_connect(const char *ssid, const char *password) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    s_connecting = true;
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // esp_wifi_start() is idempotent if already started (e.g. after provisioning)
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to %s...", ssid);
    return ESP_OK;
}

esp_err_t sticks3_wifi_disconnect(void) {
    s_connecting = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi disconnected");
    return ESP_OK;
}

esp_err_t sticks3_wifi_get_ip(char *ip_str, size_t max_len) {
    esp_netif_ip_info_t ip_info;
    if (s_sta_netif == NULL) return ESP_ERR_INVALID_STATE;
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) return ESP_FAIL;
    snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

bool sticks3_wifi_is_connected(void) {
    if (!s_initialized) return false;
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

EventGroupHandle_t sticks3_wifi_get_event_group(void) {
    return s_wifi_event_group;
}
