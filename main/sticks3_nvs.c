#include "sticks3_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "sticks3_nvs";
static const char *NAMESPACE = "bridge";
static const char *KEY_BAUD = "baud";
static const uint32_t DEFAULT_BAUD = 1000000;

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
