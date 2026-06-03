#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "sticks3_power.h"
#include "sticks3_display.h"
#include "sticks3_button.h"
#include "sticks3_audio.h"

static const char *TAG = "MAIN";

static int counter_a = 0;
static int counter_b = 0;
static lv_obj_t *label_a = NULL;
static lv_obj_t *label_b = NULL;
static lv_obj_t *label_battery = NULL;

static void btn_a_click_cb(lv_event_t *e) {
    counter_a++;
    lv_label_set_text_fmt(label_a, "BtnA: %d", counter_a);
    sticks3_audio_play_tone(500, 200, 30);  // 500Hz, 200ms, 30% volume
}

static void btn_b_click_cb(lv_event_t *e) {
    counter_b++;
    lv_label_set_text_fmt(label_b, "BtnB: %d", counter_b);
    sticks3_audio_play_tone(1000, 200, 30);  // 1000Hz, 200ms, 30% volume
}

static uint8_t voltage_to_percent(uint16_t mv) {
    if (mv >= 4150) return 100;
    if (mv <= 3300) return 0;
    return (uint8_t)((mv - 3300) * 100 / (4150 - 3300));
}

static void battery_timer_cb(lv_timer_t *timer) {
    uint16_t mv = 0;
    bool charging = false;

    if (sticks3_power_get_battery(&mv) != ESP_OK) {
        lv_label_set_text(label_battery, "BAT:ERR");
        return;
    }

    sticks3_power_is_charging(&charging);
    uint8_t pct = voltage_to_percent(mv);

    lv_label_set_text_fmt(label_battery, "BAT:%u%%", pct);

    if (charging) {
        lv_obj_set_style_text_color(label_battery, lv_color_hex(0x00FF00), 0);
    } else {
        lv_obj_set_style_text_color(label_battery, lv_color_hex(0xFF0000), 0);
    }

}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "StickS3 starting...");

    ESP_ERROR_CHECK(sticks3_power_init());
    ESP_ERROR_CHECK(sticks3_power_lcd_enable());
    ESP_ERROR_CHECK(sticks3_display_init());
    ESP_ERROR_CHECK(sticks3_button_init());
    ESP_ERROR_CHECK(sticks3_audio_speaker_init());

    lvgl_port_lock(0);

    // Black background
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    // [保留参考] Title label
    // label_title = lv_label_create(scr);
    // lv_label_set_text(label_title, "StickS3");
    // lv_obj_set_style_text_color(label_title, lv_color_hex(0xFF0000), 0);
    // lv_obj_set_style_text_font(label_title, &lv_font_montserrat_14, 0);
    // lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 10);

    // Battery label - top right corner
    label_battery = lv_label_create(scr);
    lv_label_set_text(label_battery, "BAT:---");
    lv_obj_set_style_text_color(label_battery, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(label_battery, &lv_font_montserrat_14, 0);
    lv_obj_align(label_battery, LV_ALIGN_TOP_RIGHT, -5, 10);

    // BtnA clickable area
    lv_obj_t *btn_a_area = lv_obj_create(scr);
    lv_obj_set_size(btn_a_area, 125, 75);
    lv_obj_set_pos(btn_a_area, 5, 50);
    lv_obj_set_style_bg_color(btn_a_area, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_bg_opa(btn_a_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_a_area, lv_color_hex(0x4444FF), 0);
    lv_obj_set_style_border_width(btn_a_area, 2, 0);
    lv_obj_set_style_radius(btn_a_area, 8, 0);
    lv_obj_add_event_cb(btn_a_area, btn_a_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_a_area, LV_OBJ_FLAG_SCROLLABLE);

    label_a = lv_label_create(btn_a_area);
    lv_label_set_text(label_a, "BtnA: 0");
    lv_obj_set_style_text_color(label_a, lv_color_hex(0xAAAAFF), 0);
    lv_obj_set_style_text_font(label_a, &lv_font_montserrat_14, 0);
    lv_obj_center(label_a);

    // BtnB clickable area
    lv_obj_t *btn_b_area = lv_obj_create(scr);
    lv_obj_set_size(btn_b_area, 125, 75);
    lv_obj_set_pos(btn_b_area, 5, 145);
    lv_obj_set_style_bg_color(btn_b_area, lv_color_hex(0x1A2E1A), 0);
    lv_obj_set_style_bg_opa(btn_b_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_b_area, lv_color_hex(0x44FF44), 0);
    lv_obj_set_style_border_width(btn_b_area, 2, 0);
    lv_obj_set_style_radius(btn_b_area, 8, 0);
    lv_obj_add_event_cb(btn_b_area, btn_b_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_b_area, LV_OBJ_FLAG_SCROLLABLE);

    label_b = lv_label_create(btn_b_area);
    lv_label_set_text(label_b, "BtnB: 0");
    lv_obj_set_style_text_color(label_b, lv_color_hex(0xAAFFAA), 0);
    lv_obj_set_style_text_font(label_b, &lv_font_montserrat_14, 0);
    lv_obj_center(label_b);

    // Battery timer - read every 1 second
    lv_timer_create(battery_timer_cb, 1000, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI ready. Press buttons to interact.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
