#include "sticks3_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "sticks3_nvs";
static const char *NAMESPACE = "bridge";
static const char *KEY_BAUD = "baud";
static const char *KEY_MACROS = "macros";
static const uint32_t DEFAULT_BAUD = 1500000;
static const char *DEFAULT_MACROS =
    "[{\"name\":\"kmsg off\",\"data\":\"echo 0 > /proc/sys/kernel/printk\\\\r\"},"
    "{\"name\":\"reboot loader\",\"data\":\"reboot loader\\\\r\"},"
    "{\"name\":\"dmesg\",\"data\":\"dmesg -w\\\\r\"},"
    "{\"name\":\"ifconfig\",\"data\":\"ifconfig\\\\r\"}]";

esp_err_t sticks3_nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t sticks3_nvs_save_baud(uint32_t baud) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_u32(handle, KEY_BAUD, baud);
    if (ret == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t sticks3_nvs_load_baud(uint32_t *baud) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        *baud = DEFAULT_BAUD;
        return ESP_OK;
    }
    ret = nvs_get_u32(handle, KEY_BAUD, baud);
    if (ret != ESP_OK) {
        *baud = DEFAULT_BAUD;
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t sticks3_nvs_save_macros(const char *json) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_str(handle, KEY_MACROS, json ? json : "[]");
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

esp_err_t sticks3_nvs_load_macros(char *json, size_t json_size) {
    if (!json || json_size == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        strlcpy(json, DEFAULT_MACROS, json_size);
        return ESP_OK;
    }

    size_t required = json_size;
    ret = nvs_get_str(handle, KEY_MACROS, json, &required);
    if (ret != ESP_OK) {
        strlcpy(json, DEFAULT_MACROS, json_size);
    }
    nvs_close(handle);
    return ESP_OK;
}
