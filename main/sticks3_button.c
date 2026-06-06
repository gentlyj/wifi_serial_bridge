#include "sticks3_button.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lvgl.h"

static const char *TAG = "sticks3_button";

#define BTN_A_PIN  11
#define BTN_B_PIN  12
#define BTN_COUNT  2

// BtnA: front strip at bottom-left of screen
// BtnB: side button → mapped to bottom-right area for LVGL event delivery
static const lv_point_t btn_points[BTN_COUNT] = {
    {33, 230},   // BtnA — front button, bottom-left
    {100, 230},  // BtnB — side button, bottom-right
};

static lv_indev_t *btn_indev = NULL;

// Wake interrupt state
static button_wake_cb_t s_wake_cb = NULL;
static void *s_wake_arg = NULL;
static bool s_wake_irq_enabled = false;

static void IRAM_ATTR button_isr_handler(void *arg) {
    if (s_wake_cb) {
        s_wake_cb(s_wake_arg);
    }
}

static void button_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    bool btn_a = !gpio_get_level(BTN_A_PIN);
    bool btn_b = !gpio_get_level(BTN_B_PIN);

    if (btn_a) {
        data->btn_id = 0;
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

    // Install GPIO ISR service (shared, once)
    gpio_install_isr_service(0);

    ESP_LOGI(TAG, "Buttons initialized: GPIO%d (BtnA/front), GPIO%d (BtnB/side)",
             BTN_A_PIN, BTN_B_PIN);
    return ESP_OK;
}

esp_err_t sticks3_button_enable_wake(button_wake_cb_t callback, void *arg) {
    if (s_wake_irq_enabled) return ESP_OK;

    s_wake_cb = callback;
    s_wake_arg = arg;

    // Reconfigure both button pins to trigger on falling edge (active-low press)
    gpio_set_intr_type(BTN_A_PIN, GPIO_INTR_NEGEDGE);
    gpio_set_intr_type(BTN_B_PIN, GPIO_INTR_NEGEDGE);

    gpio_isr_handler_add(BTN_A_PIN, button_isr_handler, (void *)(intptr_t)BTN_A_PIN);
    gpio_isr_handler_add(BTN_B_PIN, button_isr_handler, (void *)(intptr_t)BTN_B_PIN);

    s_wake_irq_enabled = true;
    ESP_LOGI(TAG, "Button wake IRQ enabled");
    return ESP_OK;
}

void sticks3_button_disable_wake(void) {
    if (!s_wake_irq_enabled) return;

    gpio_isr_handler_remove(BTN_A_PIN);
    gpio_isr_handler_remove(BTN_B_PIN);

    // Restore to no interrupt (LVGL polling will handle input)
    gpio_set_intr_type(BTN_A_PIN, GPIO_INTR_DISABLE);
    gpio_set_intr_type(BTN_B_PIN, GPIO_INTR_DISABLE);

    s_wake_irq_enabled = false;
    s_wake_cb = NULL;
    s_wake_arg = NULL;
    ESP_LOGI(TAG, "Button wake IRQ disabled");
}
