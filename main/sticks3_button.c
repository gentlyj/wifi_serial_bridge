#include "sticks3_button.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lvgl.h"

static const char *TAG = "sticks3_button";

#define BTN_A_PIN  11
#define BTN_B_PIN  12
#define BTN_COUNT  2

// Button point coordinates - must land inside the corresponding LVGL object areas
// BtnA area: x=5~130, y=50~125 → center (67, 87)
// BtnB area: x=5~130, y=145~220 → center (67, 182)
static const lv_point_t btn_points[BTN_COUNT] = {
    {67, 87},   // BtnA
    {67, 182},  // BtnB
};

static lv_indev_t *btn_indev = NULL;

static void button_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    bool btn_a = !gpio_get_level(BTN_A_PIN);
    bool btn_b = !gpio_get_level(BTN_B_PIN);

    if (btn_a) {
        data->btn_id = 0;  // index into btn_points
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (btn_b) {
        data->btn_id = 1;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t sticks3_button_init(void) {
    ESP_LOGI(TAG, "Initializing buttons...");

    const gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BTN_A_PIN) | (1ULL << BTN_B_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&btn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    btn_indev = lv_indev_create();
    if (btn_indev == NULL) {
        ESP_LOGE(TAG, "LV indev create failed");
        return ESP_FAIL;
    }
    lv_indev_set_type(btn_indev, LV_INDEV_TYPE_BUTTON);
    lv_indev_set_read_cb(btn_indev, button_read_cb);
    lv_indev_set_button_points(btn_indev, btn_points);

    ESP_LOGI(TAG, "Buttons initialized: GPIO%d (BtnA), GPIO%d (BtnB)", BTN_A_PIN, BTN_B_PIN);
    return ESP_OK;
}
