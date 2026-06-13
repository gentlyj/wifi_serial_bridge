#include "sticks3_uart_bridge.h"
#include "sticks3_tcp_server.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "sticks3_uart";

#define UART_NUM        UART_NUM_1
#define RX_BUF_SIZE     4096
#define TX_BUF_SIZE     4096
#define TASK_STACK      4096

static TaskHandle_t s_rx_task = NULL;
static TaskHandle_t s_tx_task = NULL;
static SemaphoreHandle_t s_uart_mutex = NULL;
static bool s_running = false;
static bool s_initialized = false;
static uint32_t s_current_baud = 1500000;
static gpio_num_t s_tx_pin, s_rx_pin;
static uint64_t s_rx_bytes = 0;
static uint64_t s_tx_bytes = 0;

static void uart_rx_task(void *arg) {
    uint8_t buf[512];
    ESP_LOGI(TAG, "UART RX task started");

    while (s_running) {
        int len = uart_read_bytes(UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0 && uart_to_tcp_buf != NULL) {
            size_t sent = xStreamBufferSend(uart_to_tcp_buf, buf, len, pdMS_TO_TICKS(100));
            s_rx_bytes += sent;
            if (sent < len) {
                ESP_LOGW(TAG, "uart_to_tcp_buf overflow");
            }
        }
    }

    ESP_LOGI(TAG, "UART RX task exiting");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

static void uart_tx_task(void *arg) {
    uint8_t buf[512];
    ESP_LOGI(TAG, "UART TX task started");

    while (s_running) {
        if (tcp_to_uart_buf == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t len = xStreamBufferReceive(tcp_to_uart_buf, buf, sizeof(buf),
                                          pdMS_TO_TICKS(100));
        if (len > 0) {
            int written = -1;
            if (s_uart_mutex && xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                written = uart_write_bytes(UART_NUM, buf, len);
                xSemaphoreGive(s_uart_mutex);
            }
            if (written > 0) {
                s_tx_bytes += written;
            }
        }
    }

    ESP_LOGI(TAG, "UART TX task exiting");
    s_tx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sticks3_uart_bridge_init(gpio_num_t tx_pin, gpio_num_t rx_pin) {
    if (s_initialized) return ESP_OK;

    if (!s_uart_mutex) {
        s_uart_mutex = xSemaphoreCreateMutex();
        if (!s_uart_mutex) {
            ESP_LOGE(TAG, "Create UART mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_tx_pin = tx_pin;
    s_rx_pin = rx_pin;

    uart_config_t uart_config = {
        .baud_rate = s_current_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, tx_pin, rx_pin,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0));

    s_initialized = true;
    ESP_LOGI(TAG, "UART1 initialized: TX=GPIO%d RX=GPIO%d baud=%u",
             tx_pin, rx_pin, (unsigned)s_current_baud);
    return ESP_OK;
}

esp_err_t sticks3_uart_bridge_set_baud(uint32_t baud_rate) {
    if (!s_initialized) {
        s_current_baud = baud_rate;
        ESP_LOGI(TAG, "Baud rate pending init: %u", (unsigned)baud_rate);
        return ESP_OK;
    }

    if (!s_uart_mutex || xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Set baudrate failed: UART busy");
        return ESP_ERR_TIMEOUT;
    }

    if (s_running) {
        if (tcp_to_uart_buf) xStreamBufferReset(tcp_to_uart_buf);
        if (uart_to_tcp_buf) xStreamBufferReset(uart_to_tcp_buf);
    }

    uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(100));
    uart_flush_input(UART_NUM);

    esp_err_t ret = uart_set_baudrate(UART_NUM, baud_rate);
    if (ret == ESP_OK) {
        s_current_baud = baud_rate;
        uart_flush_input(UART_NUM);
    }

    xSemaphoreGive(s_uart_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set baudrate failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Baud rate set to %u", (unsigned)baud_rate);
    return ESP_OK;
}

esp_err_t sticks3_uart_bridge_start(void) {
    if (s_running) return ESP_OK;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    s_running = true;
    s_rx_bytes = 0;
    s_tx_bytes = 0;

    // Pin to core 1 to avoid contention with WiFi (core 0)
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx", TASK_STACK, NULL, 7, &s_rx_task, 1);
    xTaskCreatePinnedToCore(uart_tx_task, "uart_tx", TASK_STACK, NULL, 7, &s_tx_task, 1);

    ESP_LOGI(TAG, "UART bridge started");
    return ESP_OK;
}

esp_err_t sticks3_uart_bridge_stop(void) {
    if (!s_running) return ESP_OK;

    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(300));

    s_rx_task = NULL;
    s_tx_task = NULL;

    ESP_LOGI(TAG, "UART bridge stopped");
    return ESP_OK;
}

uint32_t sticks3_uart_bridge_get_baud(void) {
    return s_current_baud;
}

void sticks3_uart_bridge_get_stats(uint64_t *rx_bytes, uint64_t *tx_bytes) {
    if (rx_bytes) *rx_bytes = s_rx_bytes;
    if (tx_bytes) *tx_bytes = s_tx_bytes;
}
