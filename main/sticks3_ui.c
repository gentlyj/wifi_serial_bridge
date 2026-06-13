#include "sticks3_ui.h"
#include "sticks3_power.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "sticks3_ui";

static QueueHandle_t s_event_queue;
static ui_state_t s_current_state;
static volatile bool s_screen_asleep = false;

static lv_obj_t *s_label_battery;

// State-specific labels
static lv_obj_t *s_label_status;
static lv_obj_t *s_label_ip;
static lv_obj_t *s_label_baud;
static lv_obj_t *s_dot_indicator;

// Cached values
static char s_ip_str[32] = "0.0.0.0";
static uint32_t s_baud = 1500000;

#define FONT  &lv_font_montserrat_14

#define COLOR_WHITE   0xFFFFFF
#define COLOR_GREEN   0x00FF00
#define COLOR_YELLOW  0xFFFF00
#define COLOR_CYAN    0x00FFFF
#define COLOR_GRAY    0x888888
#define COLOR_BG      0x000000

static uint8_t voltage_to_percent(uint16_t mv) {
    if (mv >= 4150) return 100;
    if (mv <= 3300) return 0;
    return (uint8_t)((mv - 3300) * 100 / (4150 - 3300));
}

static void battery_timer_cb(lv_timer_t *timer) {
    uint16_t mv = 0;
    bool charging = false;
    if (sticks3_power_get_battery(&mv) != ESP_OK) {
        lv_label_set_text(s_label_battery, "BAT:ERR");
        return;
    }
    sticks3_power_is_charging(&charging);
    uint8_t pct = voltage_to_percent(mv);
    lv_label_set_text_fmt(s_label_battery, "BAT:%u%%", pct);
    lv_obj_set_style_text_color(s_label_battery,
        lv_color_hex(charging ? COLOR_GREEN : 0xFF0000), 0);
}

// BtnA callbacks (front button, point 0)
static void btn_a_click_cb(lv_event_t *e) {
    if (s_screen_asleep) {
        app_event_t ev = APP_EVENT_WAKE_SCREEN;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    app_event_t ev = APP_EVENT_BTN_A_CLICK;
    xQueueSend(s_event_queue, &ev, 0);
}

static void btn_a_longpress_cb(lv_event_t *e) {
    if (s_screen_asleep) {
        app_event_t ev = APP_EVENT_WAKE_SCREEN;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    app_event_t ev = APP_EVENT_BTN_A_LONG_PRESS;
    xQueueSend(s_event_queue, &ev, 0);
}

// BtnB callbacks (side button, point 1)
static void btn_b_click_cb(lv_event_t *e) {
    if (s_screen_asleep) {
        app_event_t ev = APP_EVENT_WAKE_SCREEN;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    app_event_t ev = APP_EVENT_BTN_B_CLICK;
    xQueueSend(s_event_queue, &ev, 0);
}

static void btn_b_longpress_cb(lv_event_t *e) {
    if (s_screen_asleep) {
        app_event_t ev = APP_EVENT_WAKE_SCREEN;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    app_event_t ev = APP_EVENT_BTN_B_LONG_PRESS;
    xQueueSend(s_event_queue, &ev, 0);
}

static void create_common_widgets(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);

    s_label_battery = lv_label_create(scr);
    lv_label_set_text(s_label_battery, "BAT:---");
    lv_obj_set_style_text_color(s_label_battery, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(s_label_battery, FONT, 0);
    lv_obj_align(s_label_battery, LV_ALIGN_TOP_RIGHT, -5, 10);

    // Invisible event area covering bottom-left strip for BtnA (front button)
    // BtnA indev point: (33, 230)
    lv_obj_t *btn_a_area = lv_obj_create(scr);
    lv_obj_set_size(btn_a_area, 68, 20);
    lv_obj_set_pos(btn_a_area, 0, 220);
    lv_obj_set_style_bg_opa(btn_a_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_a_area, 0, 0);
    lv_obj_clear_flag(btn_a_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_a_area, btn_a_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_a_area, btn_a_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);

    // Invisible event area covering bottom-right strip for BtnB (side button)
    // BtnB indev point: (100, 230)
    lv_obj_t *btn_b_area = lv_obj_create(scr);
    lv_obj_set_size(btn_b_area, 67, 20);
    lv_obj_set_pos(btn_b_area, 68, 220);
    lv_obj_set_style_bg_opa(btn_b_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_b_area, 0, 0);
    lv_obj_clear_flag(btn_b_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_b_area, btn_b_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_b_area, btn_b_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_timer_create(battery_timer_cb, 1000, NULL);
}

// ---- State 1: WiFi Disconnected ----
static void build_state_disconnected(void) {
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "WiFi Disconnected");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_text_font(lbl, FONT, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Side btn: Setup");
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_text_font(hint, FONT, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 15);
}

// ---- State 2: Provisioning ----
static void build_state_provisioning(void) {
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "WiFi Setup");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_YELLOW), 0);
    lv_obj_set_style_text_font(lbl, FONT, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 35);

    lv_obj_t *ssid_lbl = lv_label_create(scr);
    lv_label_set_text(ssid_lbl, "SSID: S3-Setup");
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_text_font(ssid_lbl, FONT, 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 65);

    lv_obj_t *pass_lbl = lv_label_create(scr);
    lv_label_set_text(pass_lbl, "Pass: 12345678");
    lv_obj_set_style_text_color(pass_lbl, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_text_font(pass_lbl, FONT, 0);
    lv_obj_align(pass_lbl, LV_ALIGN_TOP_MID, 0, 85);

    lv_obj_t *visit = lv_label_create(scr);
    lv_label_set_text(visit, "Open browser, visit");
    lv_obj_set_style_text_color(visit, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_text_font(visit, FONT, 0);
    lv_obj_align(visit, LV_ALIGN_TOP_MID, 0, 115);

    lv_obj_t *url = lv_label_create(scr);
    lv_label_set_text(url, "192.168.4.1");
    lv_obj_set_style_text_color(url, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_text_font(url, FONT, 0);
    lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 135);

    s_label_status = lv_label_create(scr);
    lv_label_set_text(s_label_status, "Waiting...");
    lv_obj_set_style_text_color(s_label_status, lv_color_hex(COLOR_YELLOW), 0);
    lv_obj_set_style_text_font(s_label_status, FONT, 0);
    lv_obj_align(s_label_status, LV_ALIGN_TOP_MID, 0, 170);
}

// ---- State 3: WiFi Connected, Waiting ----
static void build_state_waiting(void) {
    lv_obj_t *scr = lv_screen_active();

    s_label_ip = lv_label_create(scr);
    lv_label_set_text(s_label_ip, s_ip_str);
    lv_obj_set_style_text_color(s_label_ip, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_text_font(s_label_ip, FONT, 0);
    lv_obj_align(s_label_ip, LV_ALIGN_TOP_MID, 0, 40);

    s_label_baud = lv_label_create(scr);
    lv_label_set_text_fmt(s_label_baud, "Baud: %u", (unsigned)s_baud);
    lv_obj_set_style_text_color(s_label_baud, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_text_font(s_label_baud, FONT, 0);
    lv_obj_align(s_label_baud, LV_ALIGN_TOP_MID, 0, 85);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Front btn: Baud");
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_text_font(hint, FONT, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 115);

    s_label_status = lv_label_create(scr);
    lv_label_set_text(s_label_status, "Waiting for PC...");
    lv_obj_set_style_text_color(s_label_status, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_text_font(s_label_status, FONT, 0);
    lv_obj_align(s_label_status, LV_ALIGN_TOP_MID, 0, 170);
}

// ---- State 4: Bridge Active ----
static void build_state_active(void) {
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "BRIDGE");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_text_font(title, FONT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *ip_lbl = lv_label_create(scr);
    lv_label_set_text(ip_lbl, s_ip_str);
    lv_obj_set_style_text_color(ip_lbl, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_text_font(ip_lbl, FONT, 0);
    lv_obj_align(ip_lbl, LV_ALIGN_TOP_MID, 0, 80);

    s_label_baud = lv_label_create(scr);
    lv_label_set_text_fmt(s_label_baud, "Baud: %u", (unsigned)s_baud);
    lv_obj_set_style_text_color(s_label_baud, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_text_font(s_label_baud, FONT, 0);
    lv_obj_align(s_label_baud, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *pins = lv_label_create(scr);
    lv_label_set_text(pins, "RX->G5, TX->G4");
    lv_obj_set_style_text_color(pins, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_text_font(pins, FONT, 0);
    lv_obj_align(pins, LV_ALIGN_TOP_MID, 0, 125);

    s_dot_indicator = lv_obj_create(scr);
    lv_obj_set_size(s_dot_indicator, 12, 12);
    lv_obj_set_style_bg_color(s_dot_indicator, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(s_dot_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dot_indicator, 6, 0);
    lv_obj_set_style_border_width(s_dot_indicator, 0, 0);
    lv_obj_align(s_dot_indicator, LV_ALIGN_TOP_MID, 0, 150);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Front btn\nclick->Baud\nlong->Discon WS");
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_text_font(hint, FONT, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 170);
}

// ---- Public API ----

void sticks3_ui_init(QueueHandle_t event_queue) {
    s_event_queue = event_queue;
    s_current_state = UI_STATE_WIFI_DISCONNECTED;

    lvgl_port_lock(0);
    create_common_widgets();
    build_state_disconnected();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI initialized");
}

void sticks3_ui_set_state(ui_state_t state) {
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    s_label_status = NULL;
    s_label_ip = NULL;
    s_label_baud = NULL;
    s_dot_indicator = NULL;

    create_common_widgets();

    s_current_state = state;
    switch (state) {
        case UI_STATE_WIFI_DISCONNECTED:
            build_state_disconnected();
            break;
        case UI_STATE_PROVISIONING:
            build_state_provisioning();
            break;
        case UI_STATE_WIFI_WAITING:
            build_state_waiting();
            break;
        case UI_STATE_BRIDGE_ACTIVE:
            build_state_active();
            break;
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI state -> %d", state);
}

void sticks3_ui_update_ip(const char *ip) {
    strncpy(s_ip_str, ip, sizeof(s_ip_str) - 1);
    lvgl_port_lock(0);
    if (s_label_ip) lv_label_set_text(s_label_ip, s_ip_str);
    lvgl_port_unlock();
}

void sticks3_ui_update_baud(uint32_t baud) {
    s_baud = baud;
    lvgl_port_lock(0);
    if (s_label_baud) lv_label_set_text_fmt(s_label_baud, "Baud: %u", (unsigned)baud);
    lvgl_port_unlock();
}

void sticks3_ui_update_stats(uint64_t rx_bytes, uint64_t tx_bytes) {
    (void)rx_bytes; (void)tx_bytes;
}

void sticks3_ui_update_status(const char *msg) {
    lvgl_port_lock(0);
    if (s_label_status) lv_label_set_text(s_label_status, msg);
    lvgl_port_unlock();
}

void sticks3_ui_update_ws_connected(bool connected) {
    lvgl_port_lock(0);
    if (s_dot_indicator) {
        lv_obj_set_style_bg_color(s_dot_indicator,
            lv_color_hex(connected ? COLOR_GREEN : COLOR_GRAY), 0);
    }
    lvgl_port_unlock();
}

void sticks3_ui_pause(void) {
    lvgl_port_stop();
    s_screen_asleep = true;
    ESP_LOGI(TAG, "UI paused (screen asleep)");
}

void sticks3_ui_resume(void) {
    s_screen_asleep = false;
    lvgl_port_resume();
    ESP_LOGI(TAG, "UI resumed (screen awake)");
}

bool sticks3_ui_is_screen_asleep(void) {
    return s_screen_asleep;
}
