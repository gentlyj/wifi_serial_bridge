#include "sticks3_power.h"
#include "M5PM1.h"
#include "esp_log.h"

static const char *TAG = "sticks3_power";

static M5PM1 pm1;
static i2c_port_t s_i2c_port = I2C_NUM_1;

esp_err_t sticks3_power_init(void) {
    // Initialize M5PM1 using legacy I2C driver on port 1 (SDA=47, SCL=48)
    m5pm1_err_t err = pm1.begin(s_i2c_port, M5PM1_DEFAULT_ADDR, 47, 48, M5PM1_I2C_FREQ_100K);
    if (err != M5PM1_OK) {
        ESP_LOGE(TAG, "M5PM1 begin failed: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "M5PM1 initialized");
    return ESP_OK;
}

esp_err_t sticks3_power_lcd_enable(void) {
    // Configure PM1 GPIO2 as push-pull output, set HIGH to enable LCD power
    // This matches the M5GFX autodetect logic for StickS3
    m5pm1_err_t err;

    // Set GPIO2 function to GPIO (register 0x16 bit 2 = 0)
    err = pm1.gpioSetFunc(M5PM1_GPIO_NUM_2, M5PM1_GPIO_FUNC_GPIO);
    if (err != M5PM1_OK) {
        ESP_LOGE(TAG, "gpioSetFunc failed: %d", err);
        return ESP_FAIL;
    }

    // Set GPIO2 as output, push-pull, value=HIGH
    err = pm1.gpioSet(M5PM1_GPIO_NUM_2, M5PM1_GPIO_MODE_OUTPUT, 1,
                      M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL);
    if (err != M5PM1_OK) {
        ESP_LOGE(TAG, "gpioSet failed: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LCD power enabled (PM1 GPIO2 HIGH)");
    return ESP_OK;
}

esp_err_t sticks3_power_get_battery(uint16_t *mv) {
    if (!mv) return ESP_ERR_INVALID_ARG;
    m5pm1_err_t err = pm1.readVbat(mv);
    if (err != M5PM1_OK) {
        ESP_LOGE(TAG, "readVbat failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sticks3_power_is_charging(bool *charging) {
    if (!charging) return ESP_ERR_INVALID_ARG;

    // Detect USB power by reading VIN voltage (USB/DC input)
    // If VIN > 4000mV, USB is connected and charging
    uint16_t vin_mv = 0;
    m5pm1_err_t err = pm1.readVin(&vin_mv);
    if (err != M5PM1_OK) {
        ESP_LOGE(TAG, "readVin failed: %d", err);
        return ESP_FAIL;
    }
    *charging = (vin_mv > 4000);

    ESP_LOGD(TAG, "VIN: %u mV, charging: %d", vin_mv, *charging);
    return ESP_OK;
}

i2c_port_t sticks3_power_get_i2c_port(void) {
    return s_i2c_port;
}

esp_err_t sticks3_power_speaker_pa_enable(void) {
    // Enable speaker PA via M5PM1 GPIO3 (output HIGH)
    // Matches m5unified: gpioSet handles func select + mode + output
    m5pm1_err_t err = pm1.gpioSet(M5PM1_GPIO_NUM_3, M5PM1_GPIO_MODE_OUTPUT, 1,
                                  M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL);
    if (err != M5PM1_OK) {
        ESP_LOGE(TAG, "PA enable failed: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Speaker PA enabled (PM1 GPIO3 HIGH)");
    return ESP_OK;
}

esp_err_t sticks3_power_speaker_pa_disable(void) {
    m5pm1_err_t err = pm1.gpioSetOutput(M5PM1_GPIO_NUM_3, 0);
    if (err != M5PM1_OK) {
        ESP_LOGE(TAG, "PA disable failed: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Speaker PA disabled (PM1 GPIO3 LOW)");
    return ESP_OK;
}
