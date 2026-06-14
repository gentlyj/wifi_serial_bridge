#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_pm.h"
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
#include "sticks3_http_server.h"
#include "sticks3_uart_bridge.h"
#include "sticks3_ui.h"

static const char *TAG = "MAIN";

#define UART_TX_PIN  GPIO_NUM_4
#define UART_RX_PIN  GPIO_NUM_5
#define TCP_PORT     8080

static QueueHandle_t s_app_event_queue;
static ui_state_t s_current_state = UI_STATE_WIFI_DISCONNECTED;
static uint32_t s_baud = 1500000;

// Power management state
static bool s_screen_asleep = false;
static bool s_wake_suppress_next_click = false;
static TickType_t s_last_activity_tick = 0;
static TickType_t s_last_fwd_change_tick = 0;
static uint64_t s_last_rx_bytes = 0;
static uint64_t s_last_tx_bytes = 0;
static bool s_charge_state_known = false;
static bool s_last_charging = false;

#define IDLE_SHUTDOWN_TICKS  pdMS_TO_TICKS(5UL * 60 * 1000)  // 5 minutes
#define SCREEN_OFF_TICKS     pdMS_TO_TICKS(20UL * 1000)       // 20 seconds
#define LOW_BATTERY_MV       3400

static const uint32_t BAUD_RATES[] = {
    115200, 230400, 460800, 921600, 1000000, 1500000, 2000000
};
static const int BAUD_COUNT = sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]);

// ISR callback: send wake event from GPIO interrupt
static void IRAM_ATTR button_wake_isr(void *arg) {
    app_event_t ev = APP_EVENT_WAKE_SCREEN;
    xQueueSendFromISR(s_app_event_queue, &ev, NULL);
}

static void reset_idle_timers(TickType_t now) {
    s_last_activity_tick = now;
    s_last_fwd_change_tick = now;
    sticks3_uart_bridge_get_stats(&s_last_rx_bytes, &s_last_tx_bytes);
}

static void reset_shutdown_timer(TickType_t now) {
    s_last_fwd_change_tick = now;
    sticks3_uart_bridge_get_stats(&s_last_rx_bytes, &s_last_tx_bytes);
}

/*
 * State ownership:
 * DISCONNECTED: no runtime HTTP/WS, no provisioning portal, STA stopped.
 * PROVISIONING: provisioning HTTP/DNS portal only; runtime HTTP/WS/UART stopped.
 * WIFI_WAITING: STA connected, runtime HTTP + WS listening, UART bridge stopped.
 * BRIDGE_ACTIVE: WIFI_WAITING services plus UART bridge forwarding.
 */
static void stop_runtime_services(void) {
    sticks3_uart_bridge_stop();
    sticks3_tcp_server_stop();
    sticks3_http_server_stop();
    sticks3_http_server_set_ws_connected(false);
    sticks3_ui_update_ws_connected(false);
}

static void enter_disconnected_state(bool changed) {
    stop_runtime_services();
    sticks3_provision_stop();
    sticks3_wifi_disconnect();
    sticks3_ui_set_state(UI_STATE_WIFI_DISCONNECTED);
    if (changed) sticks3_audio_play_tone(200, 300, 30);
}

static void enter_provisioning_state(bool changed) {
    stop_runtime_services();
    sticks3_provision_start();
    sticks3_ui_set_state(UI_STATE_PROVISIONING);
    if (changed) sticks3_audio_play_tone(300, 200, 30);
}

static void enter_waiting_state(bool changed) {
    sticks3_provision_stop();
    sticks3_uart_bridge_stop();

    char ip[32];
    if (sticks3_wifi_get_ip(ip, sizeof(ip)) == ESP_OK) {
        sticks3_ui_update_ip(ip);
    }
    sticks3_ui_set_state(UI_STATE_WIFI_WAITING);
    sticks3_ui_update_baud(s_baud);
    sticks3_ui_update_ws_connected(false);
    sticks3_http_server_set_ws_connected(false);
    sticks3_tcp_server_start(TCP_PORT);
    sticks3_http_server_start();
    reset_idle_timers(xTaskGetTickCount());
    if (changed) sticks3_audio_play_tone(400, 200, 30);
}

static void enter_active_state(bool changed) {
    sticks3_provision_stop();
    sticks3_tcp_server_start(TCP_PORT);
    sticks3_http_server_start();
    sticks3_uart_bridge_init(UART_TX_PIN, UART_RX_PIN);
    if (sticks3_uart_bridge_set_baud(s_baud) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply baud on bridge start: %u", (unsigned)s_baud);
    }
    sticks3_uart_bridge_start();
    sticks3_ui_set_state(UI_STATE_BRIDGE_ACTIVE);
    sticks3_ui_update_baud(s_baud);
    reset_idle_timers(xTaskGetTickCount());
    if (changed) sticks3_audio_play_tone(500, 150, 30);
}

static void transition_to(ui_state_t new_state) {
    bool changed = new_state != s_current_state;
    if (changed) {
        ESP_LOGI(TAG, "State %d -> %d", s_current_state, new_state);
    } else {
        ESP_LOGI(TAG, "State %d refresh", new_state);
    }

    switch (new_state) {
        case UI_STATE_WIFI_DISCONNECTED:
            enter_disconnected_state(changed);
            break;

        case UI_STATE_PROVISIONING:
            enter_provisioning_state(changed);
            break;

        case UI_STATE_WIFI_WAITING:
            enter_waiting_state(changed);
            break;

        case UI_STATE_BRIDGE_ACTIVE:
            enter_active_state(changed);
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
    uint32_t next_baud = BAUD_RATES[idx];
    if (sticks3_uart_bridge_set_baud(next_baud) != ESP_OK) {
        ESP_LOGW(TAG, "Baud change failed: %u", (unsigned)next_baud);
        return;
    }
    s_baud = next_baud;
    sticks3_nvs_save_baud(s_baud);
    sticks3_ui_update_baud(s_baud);
    ESP_LOGI(TAG, "Baud rate -> %u", (unsigned)s_baud);
    sticks3_audio_play_tone(350, 100, 20);
}

static bool is_supported_baud(uint32_t baud) {
    for (int i = 0; i < BAUD_COUNT; i++) {
        if (BAUD_RATES[i] == baud) return true;
    }
    return false;
}

static uint32_t http_get_baud(void) {
    return s_baud;
}

static bool http_set_baud(uint32_t baud) {
    if (!is_supported_baud(baud)) {
        ESP_LOGW(TAG, "HTTP rejected unsupported baud: %u", (unsigned)baud);
        return false;
    }

    if (sticks3_uart_bridge_set_baud(baud) != ESP_OK) {
        ESP_LOGW(TAG, "HTTP baud change failed: %u", (unsigned)baud);
        return false;
    }
    s_baud = baud;
    sticks3_nvs_save_baud(s_baud);
    sticks3_ui_update_baud(s_baud);
    ESP_LOGI(TAG, "HTTP baud rate -> %u", (unsigned)s_baud);
    return true;
}

// Forward declaration
static void screen_wake(void);
static void prepare_auto_shutdown(const char *reason);

// Deferred reboot task (allows WS response to be sent first)
static void deferred_reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Control command handler (called from WS server for \x01-prefixed frames)
static bool handle_ctrl_cmd(const char *cmd, char *resp, size_t resp_size) {
    ESP_LOGI(TAG, "Control command: %s", cmd);
    if (strcmp(cmd, "REBOOT") == 0) {
        snprintf(resp, resp_size, "REBOOT");
        xTaskCreate(deferred_reboot_task, "reboot", 2048, NULL, 5, NULL);
        return true;
    } else if (strcmp(cmd, "SCR_OFF") == 0) {
        if (!s_screen_asleep) {
            s_screen_asleep = true;
            sticks3_display_sleep();
            sticks3_ui_pause();
            sticks3_button_enable_wake(button_wake_isr, NULL);
            esp_pm_config_t pm_cfg = {
                .max_freq_mhz = 80,
                .min_freq_mhz = 80,
                .light_sleep_enable = false,
            };
            esp_pm_configure(&pm_cfg);
        }
        snprintf(resp, resp_size, "SCR_OFF OK");
        return true;
    } else if (strcmp(cmd, "SCR_ON") == 0) {
        screen_wake();
        snprintf(resp, resp_size, "SCR_ON OK");
        return true;
    } else if (strcmp(cmd, "BATT") == 0) {
        uint16_t mv = 0;
        if (sticks3_power_get_battery(&mv) == ESP_OK) {
            snprintf(resp, resp_size, "BATT %u", (unsigned)mv);
        } else {
            snprintf(resp, resp_size, "BATT ERR");
        }
        return true;
    } else if (strcmp(cmd, "DIAG_ON") == 0) {
        sticks3_tcp_server_set_diag(true);
        snprintf(resp, resp_size, "DIAG ON");
        return true;
    } else if (strcmp(cmd, "DIAG_OFF") == 0) {
        sticks3_tcp_server_set_diag(false);
        snprintf(resp, resp_size, "DIAG OFF");
        return true;
    }
    snprintf(resp, resp_size, "ERR unknown cmd");
    return true;
}

static void screen_wake(void) {
    if (!s_screen_asleep) return;
    ESP_LOGI(TAG, "Screen wake");
    sticks3_button_disable_wake();
    sticks3_display_wake();
    sticks3_ui_resume();
    s_screen_asleep = false;
    s_wake_suppress_next_click = true;
    reset_idle_timers(xTaskGetTickCount());

    // Restore CPU to full speed
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm_cfg);
}

static void prepare_auto_shutdown(const char *reason) {
    ESP_LOGW(TAG, "Prepare shutdown display: %s", reason ? reason : "auto");

    if (s_screen_asleep) {
        sticks3_button_disable_wake();
        sticks3_display_wake();
        sticks3_ui_resume();
        s_screen_asleep = false;
    } else {
        sticks3_display_wake();
    }

    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm_cfg);

    sticks3_ui_show_shutdown(reason);
    s_last_activity_tick = xTaskGetTickCount();
}

static void handle_event(app_event_t event) {
    switch (event) {
        case APP_EVENT_WAKE_SCREEN:
            ESP_LOGI(TAG, "Wake screen event");
            reset_idle_timers(xTaskGetTickCount());
            screen_wake();
            break;

        case APP_EVENT_BTN_A_CLICK:
            ESP_LOGI(TAG, "BtnA click, state=%d", s_current_state);
            reset_idle_timers(xTaskGetTickCount());
            if (s_wake_suppress_next_click) {
                s_wake_suppress_next_click = false;
                ESP_LOGI(TAG, "BtnA click suppressed (wake)");
                break;
            }
            if (s_current_state == UI_STATE_WIFI_WAITING ||
                s_current_state == UI_STATE_BRIDGE_ACTIVE) {
                cycle_baud_rate();
            }
            break;

        case APP_EVENT_BTN_A_LONG_PRESS:
            ESP_LOGI(TAG, "BtnA long press, state=%d", s_current_state);
            reset_idle_timers(xTaskGetTickCount());
            if (s_wake_suppress_next_click) {
                s_wake_suppress_next_click = false;
                ESP_LOGI(TAG, "BtnA long press suppressed (wake)");
                break;
            }
            if (s_current_state == UI_STATE_BRIDGE_ACTIVE) {
                transition_to(UI_STATE_WIFI_WAITING);
            }
            break;

        case APP_EVENT_BTN_B_LONG_PRESS:
            ESP_LOGI(TAG, "BtnB long press, state=%d", s_current_state);
            reset_idle_timers(xTaskGetTickCount());
            if (s_wake_suppress_next_click) {
                s_wake_suppress_next_click = false;
                ESP_LOGI(TAG, "BtnB long press suppressed (wake)");
                break;
            }
            if (s_current_state != UI_STATE_PROVISIONING) {
                transition_to(UI_STATE_PROVISIONING);
            }
            break;

        case APP_EVENT_BTN_B_CLICK:
            reset_idle_timers(xTaskGetTickCount());
            if (s_wake_suppress_next_click) {
                s_wake_suppress_next_click = false;
                break;
            }
            if (s_current_state == UI_STATE_BRIDGE_ACTIVE) {
                sticks3_tcp_server_disconnect_client();
                sticks3_ui_update_ws_connected(false);
                sticks3_http_server_set_ws_connected(false);
                transition_to(UI_STATE_WIFI_WAITING);
            }
            break;

        case APP_EVENT_WIFI_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected");
            transition_to(UI_STATE_WIFI_WAITING);
            reset_idle_timers(xTaskGetTickCount());
            break;

        case APP_EVENT_WIFI_FAILED:
            ESP_LOGW(TAG, "WiFi connection failed");
            sticks3_ui_update_status("Connect failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            transition_to(UI_STATE_WIFI_DISCONNECTED);
            break;

        case APP_EVENT_TCP_CLIENT_CONNECTED:
            ESP_LOGI(TAG, "TCP client connected");
            screen_wake();
            transition_to(UI_STATE_BRIDGE_ACTIVE);
            sticks3_ui_update_ws_connected(true);
            sticks3_http_server_set_ws_connected(true);
            break;

        case APP_EVENT_TCP_CLIENT_DISCONNECTED:
            ESP_LOGI(TAG, "TCP client disconnected");
            screen_wake();
            sticks3_ui_update_ws_connected(false);
            sticks3_http_server_set_ws_connected(false);
            transition_to(UI_STATE_WIFI_WAITING);
            break;
    }
}

// Task to monitor WiFi connection result during provisioning
static void wifi_monitor_task(void *arg) {
    EventGroupHandle_t eg = sticks3_wifi_get_event_group();
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(eg, BIT0 | BIT1,
                                               pdFALSE, pdFALSE, portMAX_DELAY);
        if (sticks3_provision_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        xEventGroupClearBits(eg, bits & (BIT0 | BIT1));
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

// Task to monitor idle state and manage power saving
static void idle_monitor_task(void *arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        TickType_t now = xTaskGetTickCount();

        // 1. Low-battery check
        uint16_t mv = 0;
        bool charging = false;
        if (sticks3_power_get_battery(&mv) == ESP_OK) {
            sticks3_power_is_charging(&charging);
            if (!s_charge_state_known || charging != s_last_charging) {
                ESP_LOGI(TAG, "Charge state changed: %d -> %d",
                         s_charge_state_known ? s_last_charging : -1, charging);
                reset_shutdown_timer(now);
                s_last_charging = charging;
                s_charge_state_known = true;
            }
            if (!charging && mv < LOW_BATTERY_MV) {
                ESP_LOGW(TAG, "Low battery shutdown: %u mV", mv);
                prepare_auto_shutdown("LOW BATTERY");
                for (int i = 0; i < 3; i++) {
                    sticks3_audio_play_tone(250, 150, 40);
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
                vTaskDelay(pdMS_TO_TICKS(300));
                sticks3_power_shutdown();
                vTaskDelay(pdMS_TO_TICKS(1000)); // Should not reach here
            }
        }

        // 2. Idle forwarding check (only when WiFi connected)
        if (charging) {
            reset_shutdown_timer(now);
        } else if (s_current_state >= UI_STATE_WIFI_WAITING) {
            uint64_t rx = 0, tx = 0;
            sticks3_uart_bridge_get_stats(&rx, &tx);
            if (rx != s_last_rx_bytes || tx != s_last_tx_bytes) {
                s_last_fwd_change_tick = now;
                s_last_rx_bytes = rx;
                s_last_tx_bytes = tx;
            } else if ((now - s_last_fwd_change_tick) > IDLE_SHUTDOWN_TICKS) {
                ESP_LOGW(TAG, "Idle shutdown: no data forwarded for 5 min");
                prepare_auto_shutdown("IDLE TIMEOUT");
                for (int i = 0; i < 3; i++) {
                    sticks3_audio_play_tone(250, 150, 40);
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
                vTaskDelay(pdMS_TO_TICKS(300));
                sticks3_power_shutdown();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        // 3. Screen-off check (only when WiFi connected and screen is on)
        if (s_current_state >= UI_STATE_WIFI_WAITING && !s_screen_asleep) {
            if ((now - s_last_activity_tick) > SCREEN_OFF_TICKS) {
                ESP_LOGI(TAG, "Screen off: idle 20s");
                s_screen_asleep = true;
                sticks3_display_sleep();
                sticks3_ui_pause();
                sticks3_button_enable_wake(button_wake_isr, NULL);

                // Reduce CPU frequency to save power
                esp_pm_config_t pm_cfg = {
                    .max_freq_mhz = 80,
                    .min_freq_mhz = 80,
                    .light_sleep_enable = false,
                };
                esp_pm_configure(&pm_cfg);
            }
        }
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

    // Set battery low-voltage protection (hardware enforced)
    sticks3_power_set_lvp(LOW_BATTERY_MV);

    // NVS init
    ESP_ERROR_CHECK(sticks3_nvs_init());
    sticks3_nvs_load_baud(&s_baud);
    ESP_LOGI(TAG, "Loaded baud: %u", (unsigned)s_baud);

    // WiFi init + modem sleep for power saving
    ESP_ERROR_CHECK(sticks3_wifi_init());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // Init power management tracking
    s_last_activity_tick = xTaskGetTickCount();
    s_last_fwd_change_tick = s_last_activity_tick;

    // Event queue
    s_app_event_queue = xQueueCreate(16, sizeof(app_event_t));

    // UI init
    sticks3_ui_init(s_app_event_queue);

    // Register control command handler for WS protocol
    sticks3_tcp_server_set_ctrl_cb(handle_ctrl_cmd);
    sticks3_http_server_set_baud_cbs(http_get_baud, http_set_baud);

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
    xTaskCreate(idle_monitor_task, "idle_mon", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "System ready. Entering main loop.");

    // Main event loop
    app_event_t event;
    while (true) {
        if (xQueueReceive(s_app_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle_event(event);
        }
    }
}
