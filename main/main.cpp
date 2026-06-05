#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "sticks3_power.h"
#include "sticks3_display.h"
#include "sticks3_button.h"
#include "sticks3_audio.h"
#include "sticks3_nvs.h"
#include "sticks3_wifi.h"
#include "sticks3_provision.h"
#include "sticks3_tcp_server.h"
#include "sticks3_uart_bridge.h"
#include "sticks3_ui.h"

static const char *TAG = "MAIN";

#define UART_TX_PIN  GPIO_NUM_4
#define UART_RX_PIN  GPIO_NUM_5
#define TCP_PORT     8080

static QueueHandle_t s_app_event_queue;
static ui_state_t s_current_state = UI_STATE_WIFI_DISCONNECTED;
static uint32_t s_baud = 1000000;

static const uint32_t BAUD_RATES[] = { 1000000, 1500000 };
static const int BAUD_COUNT = sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]);

static void transition_to(ui_state_t new_state) {
    if (new_state == s_current_state) return;

    ESP_LOGI(TAG, "State %d -> %d", s_current_state, new_state);

    // Exit old state
    switch (s_current_state) {
        case UI_STATE_WIFI_DISCONNECTED:
            break;
        case UI_STATE_PROVISIONING:
            sticks3_provision_stop();
            break;
        case UI_STATE_WIFI_WAITING:
            // Only stop TCP server when going to DISCONNECTED or PROVISIONING
            if (new_state == UI_STATE_WIFI_DISCONNECTED || new_state == UI_STATE_PROVISIONING) {
                sticks3_tcp_server_stop();
            }
            break;
        case UI_STATE_BRIDGE_ACTIVE:
            sticks3_uart_bridge_stop();
            // Only stop TCP server when going to DISCONNECTED or PROVISIONING
            if (new_state == UI_STATE_WIFI_DISCONNECTED || new_state == UI_STATE_PROVISIONING) {
                sticks3_tcp_server_stop();
            }
            break;
    }

    // Enter new state
    switch (new_state) {
        case UI_STATE_WIFI_DISCONNECTED:
            sticks3_wifi_disconnect();
            sticks3_ui_set_state(UI_STATE_WIFI_DISCONNECTED);
            sticks3_audio_play_tone(200, 300, 30);
            break;

        case UI_STATE_PROVISIONING:
            sticks3_provision_start();
            sticks3_ui_set_state(UI_STATE_PROVISIONING);
            sticks3_audio_play_tone(500, 200, 30);
            break;

        case UI_STATE_WIFI_WAITING: {
            char ip[32];
            if (sticks3_wifi_get_ip(ip, sizeof(ip)) == ESP_OK) {
                sticks3_ui_update_ip(ip);
            }
            sticks3_ui_set_state(UI_STATE_WIFI_WAITING);
            sticks3_ui_update_baud(s_baud);
            sticks3_tcp_server_start(TCP_PORT);
            sticks3_audio_play_tone(1000, 200, 30);
            break;
        }

        case UI_STATE_BRIDGE_ACTIVE:
            sticks3_uart_bridge_init(UART_TX_PIN, UART_RX_PIN);
            sticks3_uart_bridge_set_baud(s_baud);
            sticks3_uart_bridge_start();
            sticks3_ui_set_state(UI_STATE_BRIDGE_ACTIVE);
            sticks3_ui_update_baud(s_baud);
            sticks3_audio_play_tone(1500, 150, 30);
            break;
    }

    s_current_state = new_state;
}

static void cycle_baud_rate(void) {
    int idx = 0;
    for (int i = 0; i < BAUD_COUNT; i++) {
        if (BAUD_RATES[i] == s_baud) {
            idx = (i + 1) % BAUD_COUNT;
            break;
        }
    }
    s_baud = BAUD_RATES[idx];
    sticks3_uart_bridge_set_baud(s_baud);
    sticks3_nvs_save_baud(s_baud);
    sticks3_ui_update_baud(s_baud);
    ESP_LOGI(TAG, "Baud rate -> %u", (unsigned)s_baud);
    sticks3_audio_play_tone(800, 100, 20);
}

static void handle_event(app_event_t event) {
    switch (event) {
        case APP_EVENT_BTN_A_CLICK:
            ESP_LOGI(TAG, "BtnA click, state=%d", s_current_state);
            if (s_current_state == UI_STATE_WIFI_WAITING ||
                s_current_state == UI_STATE_BRIDGE_ACTIVE) {
                cycle_baud_rate();
            }
            break;

        case APP_EVENT_BTN_A_LONG_PRESS:
            ESP_LOGI(TAG, "BtnA long press, state=%d", s_current_state);
            if (s_current_state == UI_STATE_BRIDGE_ACTIVE) {
                transition_to(UI_STATE_WIFI_WAITING);
            }
            break;

        case APP_EVENT_BTN_B_LONG_PRESS:
            ESP_LOGI(TAG, "BtnB long press, state=%d", s_current_state);
            if (s_current_state == UI_STATE_WIFI_DISCONNECTED) {
                transition_to(UI_STATE_PROVISIONING);
            } else if (s_current_state == UI_STATE_WIFI_WAITING ||
                       s_current_state == UI_STATE_BRIDGE_ACTIVE) {
                transition_to(UI_STATE_WIFI_DISCONNECTED);
            }
            break;

        case APP_EVENT_BTN_B_CLICK:
            // Side button click — unused
            break;

        case APP_EVENT_WIFI_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected");
            transition_to(UI_STATE_WIFI_WAITING);
            break;

        case APP_EVENT_WIFI_FAILED:
            ESP_LOGW(TAG, "WiFi connection failed");
            sticks3_ui_update_status("Connect failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            transition_to(UI_STATE_WIFI_DISCONNECTED);
            break;

        case APP_EVENT_TCP_CLIENT_CONNECTED:
            ESP_LOGI(TAG, "TCP client connected");
            transition_to(UI_STATE_BRIDGE_ACTIVE);
            sticks3_ui_update_ws_connected(true);
            break;

        case APP_EVENT_TCP_CLIENT_DISCONNECTED:
            ESP_LOGI(TAG, "TCP client disconnected");
            sticks3_ui_update_ws_connected(false);
            transition_to(UI_STATE_WIFI_WAITING);
            break;
    }
}

// Task to monitor WiFi connection result during provisioning
static void wifi_monitor_task(void *arg) {
    EventGroupHandle_t eg = sticks3_wifi_get_event_group();
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(eg, BIT0 | BIT1,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & BIT0) {
            app_event_t ev = APP_EVENT_WIFI_CONNECTED;
            xQueueSend(s_app_event_queue, &ev, 0);
        } else if (bits & BIT1) {
            app_event_t ev = APP_EVENT_WIFI_FAILED;
            xQueueSend(s_app_event_queue, &ev, 0);
        }
    }
}

// Task to monitor TCP client connection
static void tcp_monitor_task(void *arg) {
    bool was_connected = false;
    while (true) {
        bool connected = sticks3_tcp_server_is_client_connected();
        if (connected && !was_connected) {
            app_event_t ev = APP_EVENT_TCP_CLIENT_CONNECTED;
            xQueueSend(s_app_event_queue, &ev, 0);
        } else if (!connected && was_connected) {
            app_event_t ev = APP_EVENT_TCP_CLIENT_DISCONNECTED;
            xQueueSend(s_app_event_queue, &ev, 0);
        }
        was_connected = connected;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "StickS3 WiFi Bridge starting...");

    // Hardware init
    ESP_ERROR_CHECK(sticks3_power_init());
    ESP_ERROR_CHECK(sticks3_power_lcd_enable());
    ESP_ERROR_CHECK(sticks3_display_init());
    ESP_ERROR_CHECK(sticks3_button_init());
    ESP_ERROR_CHECK(sticks3_audio_speaker_init());

    // NVS init
    ESP_ERROR_CHECK(sticks3_nvs_init());
    sticks3_nvs_load_baud(&s_baud);
    ESP_LOGI(TAG, "Loaded baud: %u", (unsigned)s_baud);

    // WiFi init
    ESP_ERROR_CHECK(sticks3_wifi_init());

    // Event queue
    s_app_event_queue = xQueueCreate(16, sizeof(app_event_t));

    // UI init
    sticks3_ui_init(s_app_event_queue);

    // Check if there are saved WiFi credentials
    wifi_config_t saved_cfg = {0};
    esp_wifi_get_config(WIFI_IF_STA, &saved_cfg);
    if (strlen((char *)saved_cfg.sta.ssid) > 0) {
        ESP_LOGI(TAG, "Found saved WiFi: %s, auto-connecting...", saved_cfg.sta.ssid);
        esp_wifi_start();
        EventGroupHandle_t eg = sticks3_wifi_get_event_group();
        EventBits_t bits = xEventGroupWaitBits(eg,
            BIT0 | BIT1, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
        if (bits & BIT0) {
            ESP_LOGI(TAG, "Auto-connected");
            transition_to(UI_STATE_WIFI_WAITING);
        } else {
            ESP_LOGI(TAG, "Auto-connect failed, waiting for provisioning");
            esp_wifi_stop();
        }
    } else {
        ESP_LOGI(TAG, "No saved WiFi credentials");
    }

    // Monitor tasks
    xTaskCreate(wifi_monitor_task, "wifi_mon", 4096, NULL, 4, NULL);
    xTaskCreate(tcp_monitor_task, "tcp_mon", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "System ready. Entering main loop.");

    // Main event loop
    app_event_t event;
    while (true) {
        if (xQueueReceive(s_app_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle_event(event);
        }
    }
}
