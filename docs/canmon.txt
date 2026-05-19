#include "ui.h"
#include "services/display.h"
#include "services/network.h"
#include "services/settings.h"
#include "services/can_monitor.h"
#include "drivers/encoder.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef enum {
    SCREEN_MAIN,
    SCREEN_MENU,
    SCREEN_SETTINGS,
    SCREEN_UPDATE_RATE,
    SCREEN_NTP_UPDATE_RATE,
    SCREEN_DST,
    SCREEN_TIMEZONE,
    SCREEN_BRIGHTNESS,
    SCREEN_NETWORK,
    SCREEN_NTP,
    SCREEN_IP_EDIT,
    SCREEN_CAN_MONITOR,
} ui_screen_t;
static ui_screen_t s_current_screen = SCREEN_MAIN;

typedef enum { IP_FIELD_IP, IP_FIELD_MASK, IP_FIELD_GW, IP_FIELD_NTP } ip_field_t;

static lv_indev_t *s_enc_indev       = NULL;
static lv_group_t *s_menu_group      = NULL;
static lv_group_t *s_settings_group  = NULL;
static lv_group_t *s_network_group   = NULL;
static lv_group_t *s_empty_group     = NULL;

// Main screen widgets
static lv_obj_t *s_main_scr          =  NULL;
static lv_obj_t *s_ip_label          =  NULL;
static lv_obj_t *s_time_label        =  NULL;
static lv_obj_t *s_date_label        =  NULL;
static lv_obj_t *s_connected_label   =  NULL;
static lv_obj_t *s_last_sync_label   =  NULL;

static ui_connection_state_t s_connection_state = UI_CONN_DISCONNECTED;
static bool                  s_ntp_error         = false;

// Menu screen widgets
static lv_obj_t *s_menu_scr       = NULL;
static lv_obj_t *s_status_label   = NULL;
static lv_obj_t *s_first_menu_btn = NULL;

// Settings screen widgets
static lv_obj_t *s_settings_scr       = NULL;
static lv_obj_t *s_first_settings_btn = NULL;

// Update-rate picker widgets + data
static lv_obj_t *s_update_rate_scr    = NULL;
static lv_obj_t *s_update_rate_roller = NULL;

static const uint32_t s_rate_values[] = {
    1, 60, 120, 300, 600, 1800, 3600, 14400, 43200, 86400,
};
#define RATE_COUNT  ((int)(sizeof(s_rate_values) / sizeof(s_rate_values[0])))
static int s_rate_idx = 1;

// NTP update-rate picker widgets + data
static lv_obj_t *s_ntp_update_rate_scr    = NULL;
static lv_obj_t *s_ntp_update_rate_roller = NULL;

static const uint32_t s_ntp_rate_values[] = {
    300, 1800, 7200, 43200, 86400,
};
#define NTP_RATE_COUNT  ((int)(sizeof(s_ntp_rate_values) / sizeof(s_ntp_rate_values[0])))
static int s_ntp_rate_idx = 0;

// DST picker widgets + data  (0=Winter, 1=Summer, 2=Auto)
static lv_obj_t *s_dst_scr    = NULL;
static lv_obj_t *s_dst_roller = NULL;
static int       s_dst_idx    = 0;

// Timezone picker widgets + data  (idx 0=UTC-12 … idx 12=UTC … idx 24=UTC+12)
static lv_obj_t *s_tz_scr   = NULL;
static lv_obj_t *s_tz_table = NULL;
static int       s_tz_idx   = 12;

static const char *const s_tz_names[25] = {
    "UTC-12", "UTC-11", "UTC-10", "UTC-9",  "UTC-8",  "UTC-7",  "UTC-6",
    "UTC-5",  "UTC-4",  "UTC-3",  "UTC-2",  "UTC-1",  "UTC",    "UTC+1 (CET)",
    "UTC+2",  "UTC+3",  "UTC+4",  "UTC+5",  "UTC+6",  "UTC+7",  "UTC+8",
    "UTC+9",  "UTC+10", "UTC+11", "UTC+12",
};
#define TZ_ROW_H  34  // montserrat_20 (~25px) + top/bottom cell padding

// Brightness screen widgets
static lv_obj_t *s_brightness_scr   = NULL;
static lv_obj_t *s_brightness_label = NULL;
static lv_obj_t *s_brightness_bar   = NULL;
static int       s_brightness_pct   = 100;

// Network screen widgets
static lv_obj_t *s_network_scr    = NULL;
static lv_obj_t *s_net_mode_btn   = NULL;
static lv_obj_t *s_net_mode_label = NULL;
static lv_obj_t *s_net_ip_btn     = NULL;
static lv_obj_t *s_net_ip_label   = NULL;
static lv_obj_t *s_net_mask_btn   = NULL;
static lv_obj_t *s_net_mask_label = NULL;
static lv_obj_t *s_net_gw_btn     = NULL;
static lv_obj_t *s_net_gw_label   = NULL;
static lv_obj_t *s_net_ntp_btn    = NULL;
static lv_obj_t *s_net_ntp_label  = NULL;

// Network config state
static bool    s_net_dhcp    = true;
static uint8_t s_net_ip[4]   = {192, 168,   1, 100};
static uint8_t s_net_mask[4] = {255, 255, 255,   0};
static uint8_t s_net_gw[4]   = {192, 168,   1,   1};
static bool    s_net_applied = false;

// Entry snapshots — saved on screen open, restored on Back / long-press cancel
static bool    s_net_dhcp_saved;
static uint8_t s_net_ip_saved[4];
static uint8_t s_net_mask_saved[4];
static uint8_t s_net_gw_saved[4];
static int     s_ntp_idx_saved;
static uint8_t s_ntp_ip_saved[4];
static int     s_brightness_saved;
static int     s_rate_idx_saved;
static int     s_ntp_rate_idx_saved;
static int     s_dst_idx_saved;
static int     s_tz_idx_saved;

// NTP picker screen widgets + data
static lv_obj_t *s_ntp_scr        = NULL;
static lv_obj_t *s_ntp_roller     = NULL;
static lv_obj_t *s_ntp_hint_label = NULL;

static const char *const s_ntp_server_names[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
    "time.windows.com",
    "Manual NTP IP",
};
#define NTP_COUNT  5
static int     s_ntp_idx  = 0;
static uint8_t s_ntp_ip[4] = {216, 239, 35, 0};  // default manual: time1.google.com

// IP edit screen widgets
static lv_obj_t *s_ip_edit_scr    = NULL;
static lv_obj_t *s_ip_title_label = NULL;
static lv_obj_t *s_ip_value_label = NULL;

// CAN monitor screen widgets and state
static lv_obj_t *s_can_scr  = NULL;
static lv_obj_t *s_can_cont = NULL;
static lv_obj_t *s_can_log  = NULL;
static char      s_can_log_text[CAN_MON_LOG_SIZE * CAN_MON_LINE_MAX];

// IP edit state
static ip_field_t s_ip_field = IP_FIELD_IP;
static int        s_ip_octet = 0;

// Single rotation accumulator: 2 raw CW/CCW events = 1 detent = 1 step.
// Reset on every screen transition so there is never leftover state.
static int s_enc_accum = 0;

// ---------------------------------------------------------------------------
// Encoder accumulator + navigation helpers
// (all called from ui_encoder_event, which runs in the LVGL task)
// ---------------------------------------------------------------------------

static int accum_step(encoder_event_t event)
{
    if (event != ENCODER_CW && event != ENCODER_CCW) return 0;
    s_enc_accum += (event == ENCODER_CW) ? 1 : -1;
    if (s_enc_accum >= 2)  { s_enc_accum = 0; return  1; }
    if (s_enc_accum <= -2) { s_enc_accum = 0; return -1; }
    return 0;
}

static void group_nav(lv_group_t *g, int step)
{
    if (step > 0) lv_group_focus_next(g);
    else          lv_group_focus_prev(g);
}

static void group_click(lv_group_t *g)
{
    lv_obj_t *focused = lv_group_get_focused(g);
    if (focused) lv_obj_send_event(focused, LV_EVENT_CLICKED, NULL);
}

// ---------------------------------------------------------------------------
// Value-change helpers (run in LVGL task — LVGL calls are safe)
// ---------------------------------------------------------------------------

static void brightness_refresh(void)
{
    lv_bar_set_value(s_brightness_bar, s_brightness_pct, LV_ANIM_OFF);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_brightness_pct);
    lv_label_set_text(s_brightness_label, buf);
    display_set_brightness((uint8_t)s_brightness_pct);
}

static void brightness_step(int dir)
{
    s_brightness_pct += dir * 5;
    if (s_brightness_pct < 20)  s_brightness_pct = 20;
    if (s_brightness_pct > 100) s_brightness_pct = 100;
    brightness_refresh();
}

static void update_rate_step(int dir)
{
    s_rate_idx += dir;
    if (s_rate_idx < 0)          s_rate_idx = 0;
    if (s_rate_idx >= RATE_COUNT) s_rate_idx = RATE_COUNT - 1;
    lv_roller_set_selected(s_update_rate_roller, (uint32_t)s_rate_idx, LV_ANIM_OFF);
}

static void ntp_update_rate_step(int dir)
{
    s_ntp_rate_idx += dir;
    if (s_ntp_rate_idx < 0)               s_ntp_rate_idx = 0;
    if (s_ntp_rate_idx >= NTP_RATE_COUNT) s_ntp_rate_idx = NTP_RATE_COUNT - 1;
    lv_roller_set_selected(s_ntp_update_rate_roller, (uint32_t)s_ntp_rate_idx, LV_ANIM_OFF);
}

static void dst_step(int dir)
{
    s_dst_idx += dir;
    if (s_dst_idx < 0) s_dst_idx = 0;
    if (s_dst_idx > 2) s_dst_idx = 2;
    lv_roller_set_selected(s_dst_roller, (uint32_t)s_dst_idx, LV_ANIM_OFF);
}

static void tz_table_draw_cb(lv_event_t *e)
{
    lv_draw_task_t   *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (base->part != LV_PART_ITEMS) return;
    if ((int)base->id1 != s_tz_idx) return;

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    if (fill) {
        fill->color = lv_palette_main(LV_PALETTE_BLUE);
        fill->opa   = LV_OPA_60;
    }
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(task);
    if (label) label->color = lv_color_black();
}

static void tz_table_sync(void)
{
    lv_obj_invalidate(s_tz_table);
    lv_obj_scroll_to_y(s_tz_table,
                       s_tz_idx * TZ_ROW_H - TZ_ROW_H,
                       LV_ANIM_OFF);
}

static void tz_step(int dir)
{
    s_tz_idx += dir;
    if (s_tz_idx < 0)  s_tz_idx = 0;
    if (s_tz_idx > 24) s_tz_idx = 24;
    tz_table_sync();
}

// ---------------------------------------------------------------------------
static void save_settings(void);
static void can_monitor_refresh(void);

// Forward declarations for screen transitions used by callbacks / helpers
// ---------------------------------------------------------------------------

static void ui_open_settings(void);
static void ui_show_settings(void);
static void ui_show_update_rate(void);
static void ui_show_ntp_update_rate(void);
static void ui_show_dst(void);
static void ui_show_timezone(void);
static void ui_show_brightness(void);
static void ui_open_network(void);
static void ui_show_network(void);
static void ui_open_ntp(void);
static void ui_open_ip_edit(ip_field_t field);
static void ui_open_can_monitor(void);

// ---------------------------------------------------------------------------
// Network screen helpers
// ---------------------------------------------------------------------------

static void network_screen_refresh(void)
{
    lv_label_set_text(s_net_mode_label,
                      s_net_dhcp ? "Mode: DHCP" : "Mode: Static");

    char buf[32];
    snprintf(buf, sizeof(buf), "IP  : %d.%d.%d.%d",
             s_net_ip[0], s_net_ip[1], s_net_ip[2], s_net_ip[3]);
    lv_label_set_text(s_net_ip_label, buf);

    snprintf(buf, sizeof(buf), "Mask: %d.%d.%d.%d",
             s_net_mask[0], s_net_mask[1], s_net_mask[2], s_net_mask[3]);
    lv_label_set_text(s_net_mask_label, buf);

    snprintf(buf, sizeof(buf), "GW  : %d.%d.%d.%d",
             s_net_gw[0], s_net_gw[1], s_net_gw[2], s_net_gw[3]);
    lv_label_set_text(s_net_gw_label, buf);

    if (s_net_dhcp) {
        lv_obj_add_state(s_net_ip_btn,   LV_STATE_DISABLED);
        lv_obj_add_state(s_net_mask_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_net_gw_btn,   LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_net_ip_btn,   LV_STATE_DISABLED);
        lv_obj_remove_state(s_net_mask_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(s_net_gw_btn,   LV_STATE_DISABLED);
    }

    if (s_ntp_idx < NTP_COUNT - 1) {
        snprintf(buf, sizeof(buf), "NTP: %s", s_ntp_server_names[s_ntp_idx]);
    } else {
        snprintf(buf, sizeof(buf), "NTP: %d.%d.%d.%d",
                 s_ntp_ip[0], s_ntp_ip[1], s_ntp_ip[2], s_ntp_ip[3]);
    }
    lv_label_set_text(s_net_ntp_label, buf);
}

// ---------------------------------------------------------------------------
// IP editor helpers
// ---------------------------------------------------------------------------

static void ip_edit_refresh(void)
{
    static const char *const field_names[] = {
        "IP Address", "Subnet Mask", "Gateway", "NTP Server IP"
    };
    const uint8_t *oct = (s_ip_field == IP_FIELD_IP)   ? s_net_ip   :
                         (s_ip_field == IP_FIELD_MASK)  ? s_net_mask :
                         (s_ip_field == IP_FIELD_GW)    ? s_net_gw   : s_ntp_ip;

    char title[32];
    snprintf(title, sizeof(title), "%s  [%d/4]",
             field_names[s_ip_field], s_ip_octet + 1);
    lv_label_set_text(s_ip_title_label, title);

    char val[32];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) val[n++] = '.';
        if (i == s_ip_octet) val[n++] = '[';
        n += snprintf(val + n, (int)sizeof(val) - n, "%d", oct[i]);
        if (i == s_ip_octet) val[n++] = ']';
    }
    val[n] = '\0';
    lv_label_set_text(s_ip_value_label, val);
}

static void ip_octet_step(int dir)
{
    uint8_t *oct = (s_ip_field == IP_FIELD_IP)   ? s_net_ip   :
                   (s_ip_field == IP_FIELD_MASK)  ? s_net_mask :
                   (s_ip_field == IP_FIELD_GW)    ? s_net_gw   : s_ntp_ip;
    int val = (int)oct[s_ip_octet] + dir;
    if (val < 0)   val = 255;
    if (val > 255) val = 0;
    oct[s_ip_octet] = (uint8_t)val;
    ip_edit_refresh();
}

static void ip_octet_next(void)
{
    if (s_ip_octet < 3) {
        s_ip_octet++;
        ip_edit_refresh();
    } else {
        ui_show_network();
    }
}

// ---------------------------------------------------------------------------
// NTP picker helpers
// ---------------------------------------------------------------------------

static void ntp_refresh(void)
{
    lv_roller_set_selected(s_ntp_roller, (uint32_t)s_ntp_idx, LV_ANIM_OFF);

    lv_label_set_text(s_ntp_hint_label,
                      (s_ntp_idx == NTP_COUNT - 1) ? "Click: edit IP  Hold: cancel"
                                                   : "Click: save  Hold: cancel");
}

static void ntp_step(int dir)
{
    s_ntp_idx += dir;
    if (s_ntp_idx < 0)          s_ntp_idx = 0;
    if (s_ntp_idx >= NTP_COUNT) s_ntp_idx = NTP_COUNT - 1;
    ntp_refresh();
}

static void ntp_confirm(void)
{
    if (s_ntp_idx == NTP_COUNT - 1) {
        ui_open_ip_edit(IP_FIELD_NTP);
    } else {
        ui_show_network();
    }
}

// ---------------------------------------------------------------------------
// Persist all current settings to NVS
// ---------------------------------------------------------------------------

static void save_settings(void)
{
    settings_t s = {
        .net_dhcp       = s_net_dhcp,
        .ntp_idx        = s_ntp_idx,
        .brightness_pct = s_brightness_pct,
        .rate_idx       = s_rate_idx,
        .ntp_rate_idx   = s_ntp_rate_idx,
        .dst_idx        = s_dst_idx,
        .tz_idx         = s_tz_idx,
    };
    memcpy(s.net_ip,   s_net_ip,   4);
    memcpy(s.net_mask, s_net_mask, 4);
    memcpy(s.net_gw,   s_net_gw,   4);
    memcpy(s.ntp_ip,   s_ntp_ip,   4);
    settings_save(&s);
}

// ---------------------------------------------------------------------------
// LVGL event callbacks (fired by group_click → lv_obj_send_event)
// ---------------------------------------------------------------------------

static void on_menu_exit(lv_event_t *e)     { ui_show_main(); }
static void on_settings_back(lv_event_t *e) { ui_show_menu(); }
static void on_net_apply(lv_event_t *e)
{
    s_net_applied = true;
    network_ip_cfg_t net_cfg = { .dhcp = s_net_dhcp };
    memcpy(net_cfg.ip,   s_net_ip,   4);
    memcpy(net_cfg.mask, s_net_mask, 4);
    memcpy(net_cfg.gw,   s_net_gw,   4);
    network_apply_ip(&net_cfg);
    save_settings();
    ui_show_menu();
}
static void on_net_back(lv_event_t *e)
{
    s_net_dhcp = s_net_dhcp_saved;
    memcpy(s_net_ip,   s_net_ip_saved,   4);
    memcpy(s_net_mask, s_net_mask_saved, 4);
    memcpy(s_net_gw,   s_net_gw_saved,   4);
    s_ntp_idx  = s_ntp_idx_saved;
    memcpy(s_ntp_ip,   s_ntp_ip_saved,   4);
    ui_show_menu();
}
static void on_net_ip_btn(lv_event_t *e)    { ui_open_ip_edit(IP_FIELD_IP); }
static void on_net_mask_btn(lv_event_t *e)  { ui_open_ip_edit(IP_FIELD_MASK); }
static void on_net_gw_btn(lv_event_t *e)    { ui_open_ip_edit(IP_FIELD_GW); }
static void on_net_ntp_btn(lv_event_t *e)   { ui_open_ntp(); }

static void on_net_mode(lv_event_t *e)
{
    s_net_dhcp = !s_net_dhcp;
    network_screen_refresh();
    lv_group_focus_obj(s_net_mode_btn);
}

static void on_menu_select(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    switch (idx) {
    case 0: ui_open_settings(); break;
    case 1: ui_show_brightness(); break;
    case 2: ui_open_network(); break;
    case 3: ui_open_can_monitor(); break;
    default: {
        const char *text = lv_list_get_button_text(lv_obj_get_parent(btn), btn);
        lv_label_set_text(s_status_label, text);
        break;
    }
    }
}

static const char *s_menu_items[] = {
    LV_SYMBOL_SETTINGS " Timeserver settings",
    LV_SYMBOL_IMAGE    " Display",
    LV_SYMBOL_WIFI     " Network",
    LV_SYMBOL_WARNING  " CAN Monitor",
};

static void on_settings_select(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx == 0) ui_show_update_rate();
    if (idx == 1) ui_show_ntp_update_rate();
    if (idx == 2) ui_show_dst();
    if (idx == 3) ui_show_timezone();
}

// ---------------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------------

static void build_main_screen(void)
{
    s_main_scr = lv_obj_create(NULL);

    s_ip_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_ip_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ip_label, "IP:1.2.3.4");
    lv_obj_align(s_ip_label, LV_ALIGN_TOP_LEFT, 4, 4);

    s_time_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_46, 0);
    lv_label_set_text(s_time_label, "12:34:56");
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -10);

    s_date_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_date_label, "2026/04/30");
    lv_obj_align(s_date_label, LV_ALIGN_CENTER, 0, 20);

    s_connected_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_connected_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_connected_label, "Disconnected");
    lv_obj_align(s_connected_label, LV_ALIGN_TOP_RIGHT, -4, 4);

    s_last_sync_label = lv_label_create(s_main_scr);
    lv_obj_set_style_text_font(s_last_sync_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_last_sync_label, "Last sync: NTP: --:--:-- CAN: --:--:--");
    lv_obj_align(s_last_sync_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

static void build_menu_screen(void)
{
    s_menu_scr = lv_obj_create(NULL);

    s_status_label = lv_label_create(s_menu_scr);
    lv_label_set_text(s_status_label, "Select an item");
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_status_label, LV_PCT(100));
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *list = lv_list_create(s_menu_scr);
    lv_obj_set_size(list, LV_PCT(100), 145);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);

    for (int i = 0; i < (int)(sizeof(s_menu_items) / sizeof(s_menu_items[0])); i++) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, s_menu_items[i]);
        lv_obj_set_height(btn, LV_PCT(30));
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
        lv_group_add_obj(s_menu_group, btn);
        lv_obj_add_event_cb(btn, on_menu_select, LV_EVENT_CLICKED, NULL);
        if (i == 0) s_first_menu_btn = btn;
    }

    lv_obj_t *exit_btn = lv_list_add_button(list, LV_SYMBOL_HOME " ", "Exit");
    lv_obj_set_height(exit_btn, LV_PCT(30));
    lv_obj_set_style_text_font(exit_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_menu_group, exit_btn);
    lv_obj_add_event_cb(exit_btn, on_menu_exit, LV_EVENT_CLICKED, NULL);
}

static void build_settings_screen(void)
{
    s_settings_scr = lv_obj_create(NULL);

    lv_obj_t *list = lv_list_create(s_settings_scr);
    lv_obj_set_size(list, LV_PCT(100), 145);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *btn = lv_list_add_button(list, LV_SYMBOL_REFRESH, "CAN Update Rate");
    lv_obj_set_height(btn, LV_PCT(30));
    lv_obj_set_user_data(btn, (void *)(intptr_t)0);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, btn);
    lv_obj_add_event_cb(btn, on_settings_select, LV_EVENT_CLICKED, NULL);
    s_first_settings_btn = btn;

    lv_obj_t *ntp_btn = lv_list_add_button(list, LV_SYMBOL_REFRESH, "NTP Update Rate");
    lv_obj_set_height(ntp_btn, LV_PCT(30));
    lv_obj_set_user_data(ntp_btn, (void *)(intptr_t)1);
    lv_obj_set_style_text_font(ntp_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, ntp_btn);
    lv_obj_add_event_cb(ntp_btn, on_settings_select, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dst_btn = lv_list_add_button(list, LV_SYMBOL_SETTINGS, "Daylight Savings");
    lv_obj_set_height(dst_btn, LV_PCT(30));
    lv_obj_set_user_data(dst_btn, (void *)(intptr_t)2);
    lv_obj_set_style_text_font(dst_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, dst_btn);
    lv_obj_add_event_cb(dst_btn, on_settings_select, LV_EVENT_CLICKED, NULL);

    lv_obj_t *tz_btn = lv_list_add_button(list, LV_SYMBOL_SETTINGS, "UTC Timezone");
    lv_obj_set_height(tz_btn, LV_PCT(30));
    lv_obj_set_user_data(tz_btn, (void *)(intptr_t)3);
    lv_obj_set_style_text_font(tz_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, tz_btn);
    lv_obj_add_event_cb(tz_btn, on_settings_select, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_btn = lv_list_add_button(list, LV_SYMBOL_LEFT, "Back");
    lv_obj_set_height(back_btn, LV_PCT(30));
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_settings_group, back_btn);
    lv_obj_add_event_cb(back_btn, on_settings_back, LV_EVENT_CLICKED, NULL);
}

static void build_update_rate_screen(void)
{
    s_update_rate_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_update_rate_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "CAN Update Rate");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_update_rate_roller = lv_roller_create(s_update_rate_scr);
    lv_roller_set_options(s_update_rate_roller,
        "1 sec\n1 min\n2 min\n5 min\n10 min\n30 min\n1 hour\n4 hours\n12 hours\n24 hours",
        LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_text_font(s_update_rate_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_update_rate_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_update_rate_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_update_rate_roller, 5);
    lv_roller_set_selected(s_update_rate_roller, (uint32_t)s_rate_idx, LV_ANIM_OFF);
    lv_obj_set_width(s_update_rate_roller, LV_PCT(80));
    lv_obj_align(s_update_rate_roller, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *hint = lv_label_create(s_update_rate_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_ntp_update_rate_screen(void)
{
    s_ntp_update_rate_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_ntp_update_rate_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "NTP Update Rate");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_ntp_update_rate_roller = lv_roller_create(s_ntp_update_rate_scr);
    lv_roller_set_options(s_ntp_update_rate_roller,
        "5 min\n30 min\n2 hours\n12 hours\n24 hours",
        LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_text_font(s_ntp_update_rate_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_ntp_update_rate_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_ntp_update_rate_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_ntp_update_rate_roller, 5);
    lv_roller_set_selected(s_ntp_update_rate_roller, (uint32_t)s_ntp_rate_idx, LV_ANIM_OFF);
    lv_obj_set_width(s_ntp_update_rate_roller, LV_PCT(80));
    lv_obj_align(s_ntp_update_rate_roller, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *hint = lv_label_create(s_ntp_update_rate_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_dst_screen(void)
{
    s_dst_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_dst_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "Daylight Savings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_dst_roller = lv_roller_create(s_dst_scr);
    lv_roller_set_options(s_dst_roller,
        "Winter\nSummer\nAuto",
        LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_text_font(s_dst_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_dst_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_dst_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_dst_roller, 3);
    lv_roller_set_selected(s_dst_roller, (uint32_t)s_dst_idx, LV_ANIM_OFF);
    lv_obj_set_width(s_dst_roller, LV_PCT(60));
    lv_obj_align(s_dst_roller, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *hint = lv_label_create(s_dst_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_timezone_screen(void)
{
    s_tz_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_tz_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "UTC Timezone");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_tz_table = lv_table_create(s_tz_scr);
    lv_table_set_column_count(s_tz_table, 1);
    lv_table_set_row_count(s_tz_table, 25);
    lv_table_set_col_width(s_tz_table, 0, 180);
    lv_obj_set_size(s_tz_table, 184, 112);
    lv_obj_set_style_text_font(s_tz_table, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_text_align(s_tz_table, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);
    lv_obj_align(s_tz_table, LV_ALIGN_CENTER, 0, 4);

    for (int i = 0; i < 25; i++)
        lv_table_set_cell_value(s_tz_table, i, 0, s_tz_names[i]);

    lv_obj_add_flag(s_tz_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s_tz_table, tz_table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

    lv_obj_t *hint = lv_label_create(s_tz_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_brightness_screen(void)
{
    s_brightness_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_brightness_scr);
    lv_label_set_text(title, "Brightness");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_brightness_label = lv_label_create(s_brightness_scr);
    lv_obj_set_style_text_font(s_brightness_label, &lv_font_montserrat_20, 0);
    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%d%%", s_brightness_pct);
    lv_label_set_text(s_brightness_label, pct_buf);
    lv_obj_align(s_brightness_label, LV_ALIGN_CENTER, 0, -14);

    s_brightness_bar = lv_bar_create(s_brightness_scr);
    lv_obj_set_size(s_brightness_bar, 280, 20);
    lv_bar_set_range(s_brightness_bar, 20, 100);
    lv_bar_set_value(s_brightness_bar, s_brightness_pct, LV_ANIM_OFF);
    lv_obj_align(s_brightness_bar, LV_ALIGN_CENTER, 0, 18);

    lv_obj_t *hint = lv_label_create(s_brightness_scr);
    lv_label_set_text(hint, "Click: apply  Hold: cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void build_network_screen(void)
{
    s_network_scr = lv_obj_create(NULL);

    lv_obj_t *list = lv_list_create(s_network_scr);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 0);

    // Mode toggle — label is child 0 (no icon)
    s_net_mode_btn = lv_list_add_button(list, NULL, "Mode: DHCP");
    lv_obj_set_height(s_net_mode_btn, 38);
    lv_obj_set_style_text_font(s_net_mode_btn, &lv_font_montserrat_20, 0);
    s_net_mode_label = lv_obj_get_child(s_net_mode_btn, 0);
    lv_group_add_obj(s_network_group, s_net_mode_btn);
    lv_obj_add_event_cb(s_net_mode_btn, on_net_mode, LV_EVENT_CLICKED, NULL);

    // IP address — label is child 0 (no icon)
    s_net_ip_btn = lv_list_add_button(list, NULL, "IP  : 192.168.1.100");
    lv_obj_set_height(s_net_ip_btn, 38);
    lv_obj_set_style_text_font(s_net_ip_btn, &lv_font_montserrat_20, 0);
    s_net_ip_label = lv_obj_get_child(s_net_ip_btn, 0);
    lv_group_add_obj(s_network_group, s_net_ip_btn);
    lv_obj_add_event_cb(s_net_ip_btn, on_net_ip_btn, LV_EVENT_CLICKED, NULL);

    // Subnet mask — label is child 0 (no icon)
    s_net_mask_btn = lv_list_add_button(list, NULL, "Mask: 255.255.255.0");
    lv_obj_set_height(s_net_mask_btn, 38);
    lv_obj_set_style_text_font(s_net_mask_btn, &lv_font_montserrat_20, 0);
    s_net_mask_label = lv_obj_get_child(s_net_mask_btn, 0);
    lv_group_add_obj(s_network_group, s_net_mask_btn);
    lv_obj_add_event_cb(s_net_mask_btn, on_net_mask_btn, LV_EVENT_CLICKED, NULL);

    // Gateway — label is child 0 (no icon)
    s_net_gw_btn = lv_list_add_button(list, NULL, "GW  : 192.168.1.1");
    lv_obj_set_height(s_net_gw_btn, 38);
    lv_obj_set_style_text_font(s_net_gw_btn, &lv_font_montserrat_20, 0);
    s_net_gw_label = lv_obj_get_child(s_net_gw_btn, 0);
    lv_group_add_obj(s_network_group, s_net_gw_btn);
    lv_obj_add_event_cb(s_net_gw_btn, on_net_gw_btn, LV_EVENT_CLICKED, NULL);

    // NTP server
    s_net_ntp_btn = lv_list_add_button(list, NULL, "NTP: pool.ntp.org");
    lv_obj_set_height(s_net_ntp_btn, 38);
    lv_obj_set_style_text_font(s_net_ntp_btn, &lv_font_montserrat_20, 0);
    s_net_ntp_label = lv_obj_get_child(s_net_ntp_btn, 0);
    lv_group_add_obj(s_network_group, s_net_ntp_btn);
    lv_obj_add_event_cb(s_net_ntp_btn, on_net_ntp_btn, LV_EVENT_CLICKED, NULL);

    // Apply
    lv_obj_t *apply_btn = lv_list_add_button(list, LV_SYMBOL_OK, "Apply");
    lv_obj_set_height(apply_btn, 38);
    lv_obj_set_style_text_font(apply_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_network_group, apply_btn);
    lv_obj_add_event_cb(apply_btn, on_net_apply, LV_EVENT_CLICKED, NULL);

    // Back
    lv_obj_t *back_btn = lv_list_add_button(list, LV_SYMBOL_LEFT, "Back");
    lv_obj_set_height(back_btn, 38);
    lv_obj_set_style_text_font(back_btn, &lv_font_montserrat_20, 0);
    lv_group_add_obj(s_network_group, back_btn);
    lv_obj_add_event_cb(back_btn, on_net_back, LV_EVENT_CLICKED, NULL);
}

static void build_ntp_screen(void)
{
    s_ntp_scr = lv_obj_create(NULL);

    lv_obj_t *title = lv_label_create(s_ntp_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "NTP Server");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_ntp_roller = lv_roller_create(s_ntp_scr);
    lv_roller_set_options(s_ntp_roller,
        "pool.ntp.org\ntime.google.com\ntime.cloudflare.com\ntime.windows.com\nManual NTP IP",
        LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_text_font(s_ntp_roller, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_ntp_roller, 2, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_ntp_roller, 2, LV_PART_SELECTED);
    lv_roller_set_visible_row_count(s_ntp_roller, 5);
    lv_roller_set_selected(s_ntp_roller, (uint32_t)s_ntp_idx, LV_ANIM_OFF);
    lv_obj_set_width(s_ntp_roller, LV_PCT(80));
    lv_obj_align(s_ntp_roller, LV_ALIGN_CENTER, 0, 4);

    s_ntp_hint_label = lv_label_create(s_ntp_scr);
    lv_obj_set_style_text_font(s_ntp_hint_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_ntp_hint_label, "Click: save  Hold: cancel");
    lv_obj_align(s_ntp_hint_label, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void build_ip_edit_screen(void)
{
    s_ip_edit_scr = lv_obj_create(NULL);

    s_ip_title_label = lv_label_create(s_ip_edit_scr);
    lv_obj_set_style_text_font(s_ip_title_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ip_title_label, "IP Address  [1/4]");
    lv_obj_align(s_ip_title_label, LV_ALIGN_TOP_MID, 0, 10);

    s_ip_value_label = lv_label_create(s_ip_edit_scr);
    lv_obj_set_style_text_font(s_ip_value_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_ip_value_label, "[192].168.1.100");
    lv_obj_align(s_ip_value_label, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *hint = lv_label_create(s_ip_edit_scr);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(hint, "Rotate: change  Click: next");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void can_monitor_refresh(void)
{
    if (s_current_screen != SCREEN_CAN_MONITOR) return;
    if (lv_obj_get_scroll_bottom(s_can_cont) > 0) return;

    int offset = ui_get_tz_offset_hours() + (ui_get_dst_mode() == 1 ? 1 : 0);
    can_mon_snapshot(s_can_log_text, sizeof(s_can_log_text), offset);
    lv_label_set_text_static(s_can_log,
                             s_can_log_text[0] ? s_can_log_text : "Waiting for CAN frames...");
    lv_obj_scroll_to_y(s_can_cont, 100000, LV_ANIM_OFF);
}

static void build_can_monitor_screen(void)
{
    s_can_scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(s_can_scr, 0, 0);

    // Thin header bar
    lv_obj_t *hdr = lv_label_create(s_can_scr);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_label_set_text(hdr, LV_SYMBOL_WARNING " CAN Monitor  (long-press: back)");
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 4, 2);

    // Scrollable log container below the header
    int log_h = lv_display_get_vertical_resolution(NULL) - 18;
    s_can_cont = lv_obj_create(s_can_scr);
    lv_obj_set_size(s_can_cont, LV_PCT(100), log_h);
    lv_obj_align(s_can_cont, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_pad_all(s_can_cont, 2, 0);
    lv_obj_set_scroll_dir(s_can_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_can_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(s_can_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);

    s_can_log = lv_label_create(s_can_cont);
    lv_obj_set_width(s_can_log, LV_PCT(100));
    lv_label_set_long_mode(s_can_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_can_log, &lv_font_montserrat_14, 0);
    lv_obj_align(s_can_log, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text_static(s_can_log, "Waiting for CAN frames...");
}

// ---------------------------------------------------------------------------
// Screen transitions (all run in the LVGL task)
// ---------------------------------------------------------------------------

void ui_open_menu(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_MENU;
    lv_indev_set_group(s_enc_indev, s_menu_group);
    lv_screen_load(s_menu_scr);
    lv_group_focus_obj(s_first_menu_btn);
}

void ui_show_menu(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_MENU;
    lv_indev_set_group(s_enc_indev, s_menu_group);
    lv_screen_load(s_menu_scr);
    lv_obj_t *prev = lv_group_get_focused(s_menu_group);
    if (prev) lv_group_focus_obj(prev);
}

void ui_show_main(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_MAIN;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_main_scr);
}

static void ui_open_settings(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_SETTINGS;
    lv_indev_set_group(s_enc_indev, s_settings_group);
    lv_screen_load(s_settings_scr);
    lv_group_focus_obj(s_first_settings_btn);
}

static void ui_show_settings(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_SETTINGS;
    lv_indev_set_group(s_enc_indev, s_settings_group);
    lv_screen_load(s_settings_scr);
    lv_obj_t *prev = lv_group_get_focused(s_settings_group);
    if (prev) lv_group_focus_obj(prev);
}

static void ui_show_update_rate(void)
{
    s_rate_idx_saved = s_rate_idx;
    s_enc_accum = 0;
    s_current_screen = SCREEN_UPDATE_RATE;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_update_rate_scr);
}

static void ui_show_ntp_update_rate(void)
{
    s_ntp_rate_idx_saved = s_ntp_rate_idx;
    s_enc_accum = 0;
    s_current_screen = SCREEN_NTP_UPDATE_RATE;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_ntp_update_rate_scr);
}

static void ui_show_dst(void)
{
    s_dst_idx_saved = s_dst_idx;
    s_enc_accum = 0;
    s_current_screen = SCREEN_DST;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_dst_scr);
}

static void ui_show_timezone(void)
{
    s_tz_idx_saved = s_tz_idx;
    s_enc_accum = 0;
    s_current_screen = SCREEN_TIMEZONE;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_tz_scr);
    tz_table_sync();
}

static void ui_show_brightness(void)
{
    s_brightness_saved = s_brightness_pct;
    s_enc_accum = 0;
    s_current_screen = SCREEN_BRIGHTNESS;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_brightness_scr);
}

static void ui_open_network(void)
{
    s_net_dhcp_saved = s_net_dhcp;
    memcpy(s_net_ip_saved,   s_net_ip,   4);
    memcpy(s_net_mask_saved, s_net_mask, 4);
    memcpy(s_net_gw_saved,   s_net_gw,   4);
    s_ntp_idx_saved  = s_ntp_idx;
    memcpy(s_ntp_ip_saved,   s_ntp_ip,   4);
    s_enc_accum = 0;
    s_current_screen = SCREEN_NETWORK;
    lv_indev_set_group(s_enc_indev, s_network_group);
    network_screen_refresh();
    lv_screen_load(s_network_scr);
    lv_group_focus_obj(s_net_mode_btn);
}

static void ui_show_network(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_NETWORK;
    lv_indev_set_group(s_enc_indev, s_network_group);
    network_screen_refresh();
    lv_screen_load(s_network_scr);
    lv_obj_t *prev = lv_group_get_focused(s_network_group);
    if (prev) lv_group_focus_obj(prev);
}

static void ui_open_ntp(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_NTP;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    ntp_refresh();
    lv_screen_load(s_ntp_scr);
}

static void ui_open_ip_edit(ip_field_t field)
{
    s_ip_field  = field;
    s_ip_octet  = 0;
    s_enc_accum = 0;
    s_current_screen = SCREEN_IP_EDIT;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    ip_edit_refresh();
    lv_screen_load(s_ip_edit_scr);
}

static void ui_open_can_monitor(void)
{
    s_enc_accum = 0;
    s_current_screen = SCREEN_CAN_MONITOR;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    can_monitor_refresh();
    lv_screen_load(s_can_scr);
}

// ---------------------------------------------------------------------------
// Central encoder dispatcher — runs in the LVGL task via lv_async_call
// ---------------------------------------------------------------------------

void ui_encoder_event(void *arg)
{
    encoder_event_t event = (encoder_event_t)(uintptr_t)arg;
    int step = accum_step(event);

    switch (s_current_screen) {

    case SCREEN_MAIN:
        if (event == ENCODER_LONG) ui_open_menu();
        break;

    case SCREEN_MENU:
        if      (step != 0)              group_nav(s_menu_group, step);
        else if (event == ENCODER_CLICK) group_click(s_menu_group);
        break;

    case SCREEN_SETTINGS:
        if      (step != 0)              group_nav(s_settings_group, step);
        else if (event == ENCODER_CLICK) group_click(s_settings_group);
        break;

    case SCREEN_UPDATE_RATE:
        if      (step != 0)               update_rate_step(step);
        else if (event == ENCODER_CLICK) { ui_show_settings(); save_settings(); }
        else if (event == ENCODER_LONG) {
            s_rate_idx = s_rate_idx_saved;
            lv_roller_set_selected(s_update_rate_roller, (uint32_t)s_rate_idx, LV_ANIM_OFF);
            ui_show_settings();
        }
        break;

    case SCREEN_NTP_UPDATE_RATE:
        if      (step != 0)               ntp_update_rate_step(step);
        else if (event == ENCODER_CLICK) { ui_show_settings(); save_settings(); }
        else if (event == ENCODER_LONG) {
            s_ntp_rate_idx = s_ntp_rate_idx_saved;
            lv_roller_set_selected(s_ntp_update_rate_roller, (uint32_t)s_ntp_rate_idx, LV_ANIM_OFF);
            ui_show_settings();
        }
        break;

    case SCREEN_DST:
        if      (step != 0)               dst_step(step);
        else if (event == ENCODER_CLICK) { ui_show_settings(); save_settings(); }
        else if (event == ENCODER_LONG) {
            s_dst_idx = s_dst_idx_saved;
            lv_roller_set_selected(s_dst_roller, (uint32_t)s_dst_idx, LV_ANIM_OFF);
            ui_show_settings();
        }
        break;

    case SCREEN_TIMEZONE:
        if      (step != 0)               tz_step(step);
        else if (event == ENCODER_CLICK) { ui_show_settings(); save_settings(); }
        else if (event == ENCODER_LONG) {
            s_tz_idx = s_tz_idx_saved;
            tz_table_sync();
            ui_show_settings();
        }
        break;

    case SCREEN_BRIGHTNESS:
        if      (step != 0)               brightness_step(step);
        else if (event == ENCODER_CLICK) { ui_show_menu(); save_settings(); }
        else if (event == ENCODER_LONG) {
            s_brightness_pct = s_brightness_saved;
            display_set_brightness((uint8_t)s_brightness_pct);
            ui_show_menu();
        }
        break;

    case SCREEN_NETWORK:
        if      (step != 0)              group_nav(s_network_group, step);
        else if (event == ENCODER_CLICK) group_click(s_network_group);
        break;

    case SCREEN_NTP:
        if      (step != 0)               ntp_step(step);
        else if (event == ENCODER_CLICK)  ntp_confirm();
        else if (event == ENCODER_LONG) {
            s_ntp_idx = s_ntp_idx_saved;
            memcpy(s_ntp_ip, s_ntp_ip_saved, 4);
            ui_show_network();
        }
        break;

    case SCREEN_IP_EDIT:
        if      (step != 0)               ip_octet_step(step);
        else if (event == ENCODER_CLICK)  ip_octet_next();
        else if (event == ENCODER_LONG) {
            switch (s_ip_field) {
            case IP_FIELD_IP:   memcpy(s_net_ip,   s_net_ip_saved,   4); break;
            case IP_FIELD_MASK: memcpy(s_net_mask, s_net_mask_saved, 4); break;
            case IP_FIELD_GW:   memcpy(s_net_gw,   s_net_gw_saved,   4); break;
            case IP_FIELD_NTP:  memcpy(s_ntp_ip,   s_ntp_ip_saved,   4); break;
            }
            ui_show_network();
        }
        break;

    case SCREEN_CAN_MONITOR:
        if (event == ENCODER_CW) {
            lv_obj_scroll_by(s_can_cont, 0, -8, LV_ANIM_OFF);
        } else if (event == ENCODER_CCW) {
            lv_obj_scroll_by(s_can_cont, 0, 8, LV_ANIM_OFF);
        } else if (event == ENCODER_CLICK) {
            can_mon_push_separator();
            lv_obj_scroll_to_y(s_can_cont, 100000, LV_ANIM_OFF);
            can_monitor_refresh();
        } else if (event == ENCODER_LONG) {
            ui_show_menu();
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t ui_init(lv_indev_t *encoder_indev, const settings_t *s)
{
    // Populate state variables from saved settings before building any screen,
    // so roller positions and other widget initial values are already correct.
    s_rate_idx       = s->rate_idx;
    s_ntp_rate_idx   = s->ntp_rate_idx;
    s_dst_idx        = s->dst_idx;
    s_tz_idx         = s->tz_idx;
    s_brightness_pct = s->brightness_pct;
    s_net_dhcp       = s->net_dhcp;
    memcpy(s_net_ip,   s->net_ip,   4);
    memcpy(s_net_mask, s->net_mask, 4);
    memcpy(s_net_gw,   s->net_gw,   4);
    s_ntp_idx        = s->ntp_idx;
    memcpy(s_ntp_ip,   s->ntp_ip,   4);

    // Apply brightness immediately so the screen starts at the saved level.
    display_set_brightness((uint8_t)s_brightness_pct);

    s_enc_indev      = encoder_indev;
    s_empty_group    = lv_group_create();
    s_menu_group     = lv_group_create();
    s_settings_group = lv_group_create();
    s_network_group  = lv_group_create();
    lv_group_set_wrap(s_menu_group,     false);
    lv_group_set_wrap(s_settings_group, false);
    lv_group_set_wrap(s_network_group,  false);

    build_main_screen();
    build_menu_screen();
    build_settings_screen();
    build_update_rate_screen();
    build_ntp_update_rate_screen();
    build_dst_screen();
    build_timezone_screen();
    build_brightness_screen();
    build_network_screen();
    build_ntp_screen();
    build_ip_edit_screen();
    build_can_monitor_screen();
    can_mon_set_notify(can_monitor_refresh);

    s_current_screen = SCREEN_MAIN;
    lv_indev_set_group(s_enc_indev, s_empty_group);
    lv_screen_load(s_main_scr);

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

uint32_t ui_get_update_rate_sec(void)
{
    return s_rate_values[s_rate_idx];
}

void ui_get_ntp_server_str(char *out, size_t len)
{
    if (s_ntp_idx < NTP_COUNT - 1) {
        snprintf(out, len, "%s", s_ntp_server_names[s_ntp_idx]);
    } else {
        snprintf(out, len, "%d.%d.%d.%d",
                 s_ntp_ip[0], s_ntp_ip[1], s_ntp_ip[2], s_ntp_ip[3]);
    }
}

uint32_t ui_get_ntp_update_rate_sec(void)
{
    return s_ntp_rate_values[s_ntp_rate_idx];
}

int ui_get_dst_mode(void)
{
    return s_dst_idx;  // 0=Winter, 1=Summer, 2=Auto
}

int ui_get_tz_offset_hours(void)
{
    return s_tz_idx - 12;  // idx 0 = UTC-12, idx 12 = UTC, idx 24 = UTC+12
}

void ui_get_net_config(ui_net_config_t *out)
{
    out->dhcp    = s_net_dhcp;
    memcpy(out->ip,     s_net_ip,   4);
    memcpy(out->mask,   s_net_mask, 4);
    memcpy(out->gw,     s_net_gw,   4);
    out->ntp_idx = s_ntp_idx;
    memcpy(out->ntp_ip, s_ntp_ip,   4);
    out->applied = s_net_applied;
}

void ui_set_ip(const char *ip)  { lv_label_set_text(s_ip_label,   ip); }
void ui_set_time(const char *t) { lv_label_set_text(s_time_label, t);  }
void ui_set_date(const char *d) { lv_label_set_text(s_date_label, d);  }

void ui_set_last_sync(const char *ntp_time, const char *can_time)
{
    if (!s_last_sync_label) return;
    char buf[48];
    snprintf(buf, sizeof(buf), "Last sync: NTP: %s CAN: %s", ntp_time, can_time);
    lv_label_set_text(s_last_sync_label, buf);
}

static void connection_label_refresh(void)
{
    if (!s_connected_label) return;
    char buf[48];
    switch (s_connection_state) {
    case UI_CONN_LAN:
        snprintf(buf, sizeof(buf), s_ntp_error ? "LAN | NTP ERR" : "Connected on LAN");
        break;
    case UI_CONN_WIFI:
        snprintf(buf, sizeof(buf), s_ntp_error ? "WiFi | NTP ERR" : "Connected on WiFi");
        break;
    default:
        snprintf(buf, sizeof(buf), "Disconnected");
        break;
    }
    lv_label_set_text(s_connected_label, buf);
}

void ui_set_connection_state(ui_connection_state_t state)
{
    s_connection_state = state;
    connection_label_refresh();
}

void ui_set_ntp_error(bool error)
{
    if (s_ntp_error == error) return;
    s_ntp_error = error;
    connection_label_refresh();
}
