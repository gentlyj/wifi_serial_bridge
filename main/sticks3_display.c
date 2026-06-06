#include "sticks3_display.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "sticks3_display";

// StickS3 display pin configuration (from M5GFX autodetect)
#define LCD_HOST           SPI3_HOST
#define LCD_MOSI_PIN       39
#define LCD_SCLK_PIN       40
#define LCD_DC_PIN         45
#define LCD_CS_PIN         41
#define LCD_RST_PIN        21
#define LCD_BACKLIGHT_PIN  38

// ST7789 panel parameters
#define LCD_H_RES          135
#define LCD_V_RES          240
#define LCD_OFFSET_X       52
#define LCD_OFFSET_Y       40

// SPI transfer buffer size (10 rows worth of data)
#define LCD_BUF_LINES      10

// Backlight PWM config
#define LCD_PWM_TIMER      LEDC_TIMER_0
#define LCD_PWM_CHANNEL    LEDC_CHANNEL_7
#define LCD_PWM_SPEED      LEDC_LOW_SPEED_MODE
#define LCD_PWM_FREQ_HZ    256
#define LCD_PWM_RESOLUTION LEDC_TIMER_8_BIT

static esp_lcd_panel_handle_t panel_handle = NULL;

static void backlight_init(void) {
    // Configure LEDC timer for backlight PWM
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LCD_PWM_SPEED,
        .duty_resolution = LCD_PWM_RESOLUTION,
        .timer_num       = LCD_PWM_TIMER,
        .freq_hz         = LCD_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    // Configure LEDC channel for backlight
    const ledc_channel_config_t channel_cfg = {
        .gpio_num   = LCD_BACKLIGHT_PIN,
        .speed_mode = LCD_PWM_SPEED,
        .channel    = LCD_PWM_CHANNEL,
        .timer_sel  = LCD_PWM_TIMER,
        .duty       = 0,  // Start with backlight off
        .hpoint     = 0,
    };
    ledc_channel_config(&channel_cfg);
}

void sticks3_display_set_brightness(uint8_t brightness) {
    // brightness 0-255 maps to duty 0-255 (8-bit resolution)
    ledc_set_duty(LCD_PWM_SPEED, LCD_PWM_CHANNEL, brightness);
    ledc_update_duty(LCD_PWM_SPEED, LCD_PWM_CHANNEL);
}

void sticks3_display_sleep(void) {
    sticks3_display_set_brightness(0);
    ESP_LOGI(TAG, "Display sleep (backlight off)");
}

void sticks3_display_wake(void) {
    sticks3_display_set_brightness(200);
    ESP_LOGI(TAG, "Display wake (backlight restored)");
}

esp_err_t sticks3_display_init(void) {
    ESP_LOGI(TAG, "Initializing ST7789 display...");

    // 1. Initialize backlight (off until display is ready)
    backlight_init();

    // 2. Initialize SPI bus
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num     = LCD_SCLK_PIN,
        .mosi_io_num     = LCD_MOSI_PIN,
        .miso_io_num     = -1,  // No MISO
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Create SPI panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num        = LCD_DC_PIN,
        .cs_gpio_num        = LCD_CS_PIN,
        .pclk_hz            = 40 * 1000 * 1000,  // 40 MHz
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 4. Create ST7789 panel
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,  // RGB565
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 5. Reset and initialize panel
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    // 6. Set orientation and offsets for StickS3
    esp_lcd_panel_invert_color(panel_handle, true);  // ST7789 needs color inversion
    esp_lcd_panel_set_gap(panel_handle, LCD_OFFSET_X, LCD_OFFSET_Y);
    esp_lcd_panel_swap_xy(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, false, false);

    // 7. Turn on display
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // 8. Initialize LVGL
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 9. Add display to LVGL
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = io_handle,
        .panel_handle   = panel_handle,
        .buffer_size    = LCD_H_RES * LCD_BUF_LINES,
        .double_buffer  = true,
        .hres           = LCD_H_RES,
        .vres           = LCD_V_RES,
        .monochrome     = false,
        .color_format   = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy    = false,
            .mirror_x   = false,
            .mirror_y   = false,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL add display failed");
        return ESP_FAIL;
    }

    // 10. Turn on backlight
    sticks3_display_set_brightness(200);  // ~80% brightness

    ESP_LOGI(TAG, "Display initialized: %dx%d, LVGL ready", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}
