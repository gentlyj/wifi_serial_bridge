#include "sticks3_ui.h"
#include "sticks3_power.h"
#include "sticks3_display.h"
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
static lv_timer_t *s_battery_timer;

// State-specific labels
static lv_obj_t *s_label_status;
static lv_obj_t *s_label_ip;
static lv_obj_t *s_label_baud;
static lv_obj_t *s_label_ws;
static lv_obj_t *s_label_rx;
static lv_obj_t *s_label_tx;

// Cached values
static char s_ip_str[32] = "0.0.0.0";
static uint32_t s_baud = 1500000;

#define SCREEN_W 240
#define SCREEN_H 135
#define HEADER_H  24
#define CONTENT_Y 28
#define CONTENT_H 74
#define ACTION_Y  108
#define ACTION_H  22
#define LEFT_X    8
#define LEFT_W    86
#define RIGHT_X   100
#define RIGHT_W   132
#define ROW_STEP  15

#define FONT_SM     &lv_font_montserrat_14
#define FONT_MD     &lv_font_montserrat_20
#define FONT_LG     &lv_font_montserrat_28

#define COLOR_BG        0x070A0F
#define COLOR_PANEL     0x111823
#define COLOR_PANEL_2   0x172230
#define COLOR_BORDER    0x2A3646
#define COLOR_TEXT      0xF2F6FB
#define COLOR_MUTED     0x8FA0B3
#define COLOR_DIM       0x526273
#define COLOR_GREEN     0x41D982
#define COLOR_YELLOW    0xF4C95D
#define COLOR_CYAN      0x4CC9F0
#define COLOR_RED       0xF06A6A
#define COLOR_BTN_B     0x526273

static uint8_t voltage_to_percent(uint16_t mv) {
    if (mv >= 4150) return 100;
    if (mv <= 3300) return 0;
    return (uint8_t)((mv - 3300) * 100 / (4150 - 3300));
}

static void style_screen(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x101621), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
}

static lv_obj_t *create_panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_pad_all(obj, 6, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              uint32_t color, int w, lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, w);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static void format_baud(char *buf, size_t size, uint32_t baud) {
    if (baud >= 1000000 && baud % 1000000 == 0) {
        snprintf(buf, size, "%uM", (unsigned)(baud / 1000000));
    } else if (baud >= 1000000 && baud % 100000 == 0) {
        snprintf(buf, size, "%u.%uM", (unsigned)(baud / 1000000), (unsigned)((baud / 100000) % 10));
    } else if (baud >= 1000 && baud % 1000 == 0) {
        snprintf(buf, size, "%uk", (unsigned)(baud / 1000));
    } else {
        snprintf(buf, size, "%u", (unsigned)baud);
    }
}

static void format_count(char *buf, size_t size, uint64_t value) {
    if (value >= 1000000ULL) {
        snprintf(buf, size, "%lluM", (unsigned long long)(value / 1000000ULL));
    } else if (value >= 1000ULL) {
        snprintf(buf, size, "%lluK", (unsigned long long)(value / 1000ULL));
    } else {
        snprintf(buf, size, "%llu", (unsigned long long)value);
    }
}

static void battery_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!s_label_battery) return;

    uint16_t mv = 0;
    bool charging = false;
    if (sticks3_power_get_battery(&mv) != ESP_OK) {
        lv_label_set_text(s_label_battery, "BAT ERR");
        lv_obj_set_style_text_color(s_label_battery, lv_color_hex(COLOR_RED), 0);
        return;
    }

    sticks3_power_is_charging(&charging);
    uint8_t pct = voltage_to_percent(mv);
    lv_label_set_text_fmt(s_label_battery, "%u%%", pct);
    lv_obj_set_style_text_color(s_label_battery,
        lv_color_hex(charging ? COLOR_GREEN : (pct <= 15 ? COLOR_RED : COLOR_TEXT)), 0);
}

// BtnA callbacks (front button, point 0)
static void btn_a_click_cb(lv_event_t *e) {
    (void)e;
    if (s_screen_asleep) {
        app_event_t ev = APP_EVENT_WAKE_SCREEN;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    app_event_t ev = APP_EVENT_BTN_A_CLICK;
    xQueueSend(s_event_queue, &ev, 0);
}

static void btn_a_longpress_cb(lv_event_t *e) {
    (void)e;
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
    (void)e;
    if (s_screen_asleep) {
        app_event_t ev = APP_EVENT_WAKE_SCREEN;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    app_event_t ev = APP_EVENT_BTN_B_CLICK;
    xQueueSend(s_event_queue, &ev, 0);
}

static void btn_b_longpress_cb(lv_event_t *e) {
    (void)e;
    if (s_screen_asleep) {
        app_event_t ev = APP_EVENT_WAKE_SCREEN;
        xQueueSend(s_event_queue, &ev, 0);
        return;
    }
    app_event_t ev = APP_EVENT_BTN_B_LONG_PRESS;
    xQueueSend(s_event_queue, &ev, 0);
}

static void create_header(lv_obj_t *scr) {
    lv_obj_t *title = create_label(scr, "SERIAL BRIDGE", FONT_SM, COLOR_MUTED, 150, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(title, 8, 5);

    s_label_battery = create_label(scr, "--", FONT_SM, COLOR_TEXT, 48, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_label_battery, 184, 5);
}

static void create_btn_a_hint(lv_obj_t *scr, const char *label) {
    lv_obj_t *bar = create_panel(scr, LEFT_X, ACTION_Y, 104, ACTION_H, 0x0D131C);
    lv_obj_set_style_radius(bar, 5, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    lv_obj_t *key = create_label(bar, "A", FONT_SM, COLOR_BG, 18, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(key, 5, 3);
    lv_obj_set_style_bg_color(key, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(key, 9, 0);

    lv_obj_t *txt = create_label(bar, label, FONT_SM, COLOR_TEXT, 74, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(txt, 30, 3);
}

static void create_btn_b_hint(lv_obj_t *scr, const char *label, uint32_t accent) {
    (void)accent;
    lv_obj_t *bar = create_panel(scr, 128, ACTION_Y, 104, ACTION_H, 0x0D131C);
    lv_obj_set_style_radius(bar, 5, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    lv_obj_t *key = create_label(bar, "B", FONT_SM, COLOR_BG, 18, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(key, 5, 3);
    lv_obj_set_style_bg_color(key, lv_color_hex(COLOR_BTN_B), 0);
    lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(key, 9, 0);

    lv_obj_t *txt = create_label(bar, label, FONT_SM, COLOR_TEXT, 74, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(txt, 30, 3);
}

static void create_input_areas(lv_obj_t *scr) {
    lv_obj_t *btn_a_area = lv_obj_create(scr);
    lv_obj_set_pos(btn_a_area, LEFT_X, ACTION_Y - 2);
    lv_obj_set_size(btn_a_area, 104, ACTION_H + 4);
    lv_obj_set_style_bg_opa(btn_a_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_a_area, 0, 0);
    lv_obj_add_flag(btn_a_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_a_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_a_area, btn_a_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_a_area, btn_a_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *btn_b_area = lv_obj_create(scr);
    lv_obj_set_pos(btn_b_area, 128, ACTION_Y - 2);
    lv_obj_set_size(btn_b_area, 104, ACTION_H + 4);
    lv_obj_set_style_bg_opa(btn_b_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_b_area, 0, 0);
    lv_obj_add_flag(btn_b_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn_b_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_b_area, btn_b_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_b_area, btn_b_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
}

static void create_common_widgets(void) {
    lv_obj_t *scr = lv_screen_active();
    style_screen(scr);
    create_header(scr);
}

static void create_status_block(lv_obj_t *scr, const char *kicker, const char *title,
                                uint32_t accent, const char *detail) {
    lv_obj_t *panel = create_panel(scr, LEFT_X, CONTENT_Y, LEFT_W, CONTENT_H, COLOR_PANEL);
    lv_obj_set_style_pad_all(panel, 0, 0);

    lv_obj_t *k = create_label(panel, kicker, FONT_SM, accent, LEFT_W - 8, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(k, 4, 7);

    lv_obj_t *t = create_label(panel, title, FONT_SM, COLOR_TEXT, LEFT_W - 8, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(t, 4, 29);

    s_label_status = create_label(panel, detail, FONT_SM, COLOR_MUTED, LEFT_W - 8, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_label_status, 4, 51);
}

static void create_info_row(lv_obj_t *panel, int y, const char *key, const char *value,
                            uint32_t value_color, lv_obj_t **value_label) {
    lv_obj_t *k = create_label(panel, key, FONT_SM, COLOR_DIM, 50, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(k, 0, y);
    lv_obj_set_height(k, 15);

    lv_obj_t *v = create_label(panel, value, FONT_SM, value_color, 86, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(v, 30, y);
    lv_obj_set_height(v, 15);
    
    if (value_label) {
        *value_label = v;
    }
}

// ---- State 1: WiFi Disconnected ----
static void build_state_disconnected(void) {
    lv_obj_t *scr = lv_screen_active();

    create_status_block(scr, "WIFI", "OFFLINE", COLOR_RED, " ");

    lv_obj_t *panel = create_panel(scr, RIGHT_X, CONTENT_Y, RIGHT_W, CONTENT_H, COLOR_PANEL);
    create_info_row(panel, 0, "Mode", "Setup", COLOR_TEXT, NULL);
    create_info_row(panel, ROW_STEP, "AP", "Ready", COLOR_YELLOW, NULL);

    create_btn_b_hint(scr, "SETUP", COLOR_YELLOW);
}

// ---- State 2: Provisioning ----
static void build_state_provisioning(void) {
    lv_obj_t *scr = lv_screen_active();

    create_status_block(scr, "WIFI", "SETUP", COLOR_YELLOW, "Join AP");

    lv_obj_t *panel = create_panel(scr, RIGHT_X, CONTENT_Y, RIGHT_W, CONTENT_H, COLOR_PANEL);
    create_info_row(panel, 0, "Join", "S3-Setup", COLOR_YELLOW, NULL);
    create_info_row(panel, ROW_STEP, "Pass", "12345678", COLOR_TEXT, NULL);
    create_info_row(panel, ROW_STEP * 2, "Open", "192.168.4.1", COLOR_CYAN, NULL);
    create_info_row(panel, ROW_STEP * 3, "Set", "WiFi", COLOR_GREEN, NULL);
}

// ---- State 3: WiFi Connected, Waiting ----
static void build_state_waiting(void) {
    lv_obj_t *scr = lv_screen_active();

    create_status_block(scr, "WIFI", "READY", COLOR_GREEN, "Wait PC");

    lv_obj_t *panel = create_panel(scr, RIGHT_X, CONTENT_Y, RIGHT_W, CONTENT_H, COLOR_PANEL);
    create_info_row(panel, 0, "IP", s_ip_str, COLOR_GREEN, &s_label_ip);

    char baud[20];
    format_baud(baud, sizeof(baud), s_baud);
    create_info_row(panel, ROW_STEP, "Baud", baud, COLOR_TEXT, &s_label_baud);
    create_info_row(panel, ROW_STEP * 2, "UART", "G5/G4", COLOR_CYAN, NULL);
    create_info_row(panel, ROW_STEP * 3, "WS", "Idle", COLOR_MUTED, &s_label_ws);

    create_btn_a_hint(scr, "BAUD");
    create_btn_b_hint(scr, "SETUP", COLOR_YELLOW);
}

// ---- State 4: Bridge Active ----
static void build_state_active(void) {
    lv_obj_t *scr = lv_screen_active();

    create_status_block(scr, "SERIAL", "LIVE", COLOR_GREEN, "WS linked");

    lv_obj_t *panel = create_panel(scr, RIGHT_X, CONTENT_Y, RIGHT_W, CONTENT_H, COLOR_PANEL);
    create_info_row(panel, 0, "IP", s_ip_str, COLOR_TEXT, &s_label_ip);

    char baud[20];
    format_baud(baud, sizeof(baud), s_baud);
    create_info_row(panel, ROW_STEP, "Baud", baud, COLOR_TEXT, &s_label_baud);
    create_info_row(panel, ROW_STEP * 2, "UART", "G5/G4", COLOR_CYAN, NULL);
    create_info_row(panel, ROW_STEP * 3, "WS", "Linked", COLOR_GREEN, &s_label_ws);

    create_btn_a_hint(scr, "BAUD");
    create_btn_b_hint(scr, "DISC", COLOR_RED);
}

static void reset_state_refs(void) {
    s_label_status = NULL;
    s_label_ip = NULL;
    s_label_baud = NULL;
    s_label_ws = NULL;
    s_label_rx = NULL;
    s_label_tx = NULL;
    s_label_battery = NULL;
}

// ---- Public API ----

void sticks3_ui_init(QueueHandle_t event_queue) {
    s_event_queue = event_queue;
    s_current_state = UI_STATE_WIFI_DISCONNECTED;

    lvgl_port_lock(0);
    reset_state_refs();
    create_common_widgets();
    build_state_disconnected();
    create_input_areas(lv_screen_active());
    if (!s_battery_timer) {
        s_battery_timer = lv_timer_create(battery_timer_cb, 1000, NULL);
    }
    battery_timer_cb(NULL);
    lv_refr_now(NULL);
    lvgl_port_unlock();

    sticks3_display_show();
    ESP_LOGI(TAG, "UI initialized");
}

void sticks3_ui_set_state(ui_state_t state) {
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    reset_state_refs();
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

    create_input_areas(scr);
    battery_timer_cb(NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI state -> %d", state);
}

void sticks3_ui_update_ip(const char *ip) {
    strncpy(s_ip_str, ip, sizeof(s_ip_str) - 1);
    s_ip_str[sizeof(s_ip_str) - 1] = '\0';

    lvgl_port_lock(0);
    if (s_label_ip) lv_label_set_text(s_label_ip, s_ip_str);
    lvgl_port_unlock();
}

void sticks3_ui_update_baud(uint32_t baud) {
    s_baud = baud;
    lvgl_port_lock(0);
    char buf[20];
    format_baud(buf, sizeof(buf), baud);
    if (s_label_baud) lv_label_set_text(s_label_baud, buf);
    lvgl_port_unlock();
}

void sticks3_ui_update_stats(uint64_t rx_bytes, uint64_t tx_bytes) {
    lvgl_port_lock(0);
    char buf[20];
    if (s_label_rx) {
        format_count(buf, sizeof(buf), rx_bytes);
        lv_label_set_text(s_label_rx, buf);
    }
    if (s_label_tx) {
        format_count(buf, sizeof(buf), tx_bytes);
        lv_label_set_text(s_label_tx, buf);
    }
    lvgl_port_unlock();
}

void sticks3_ui_update_status(const char *msg) {
    lvgl_port_lock(0);
    if (s_label_status) lv_label_set_text(s_label_status, msg);
    lvgl_port_unlock();
}

void sticks3_ui_update_ws_connected(bool connected) {
    lvgl_port_lock(0);
    if (s_label_status) {
        lv_label_set_text(s_label_status, connected ? "WS linked" : "Wait PC");
    }
    if (s_label_ws) {
        lv_label_set_text(s_label_ws, connected ? "Linked" : "Idle");
        lv_obj_set_style_text_color(s_label_ws,
            lv_color_hex(connected ? COLOR_GREEN : COLOR_MUTED), 0);
    }
    lvgl_port_unlock();
}

void sticks3_ui_show_shutdown(const char *reason) {
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    reset_state_refs();
    style_screen(scr);

    lv_obj_t *kicker = create_label(scr, reason ? reason : "POWER", FONT_SM, COLOR_YELLOW, 220, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(kicker, 10, 30);

    lv_obj_t *title = create_label(scr, "Shutting down", FONT_MD, COLOR_TEXT, 220, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(title, 10, 56);

    lv_obj_t *msg = create_label(scr, "Please wait", FONT_SM, COLOR_MUTED, 220, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(msg, 10, 86);

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
