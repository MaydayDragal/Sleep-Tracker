#include "ui.h"
#include "board.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>

// Phase 0/1 bring-up screen: a live diagnostic readout (RTC time, IMU accel
// magnitude, battery) plus a tap-test button that verifies the touch path.
// LVGL is serviced by the esp_lvgl_port task in the BSP, so ui_tick() is a no-op
// and cross-task updates go through ui_set_status() under the LVGL lock.
// TODO(phase4): real watch face / tracking / morning-report / settings screens.

static const char *TAG = "ui";

static lv_obj_t *s_time_lbl;
static lv_obj_t *s_accel_lbl;
static lv_obj_t *s_batt_lbl;

static void tap_btn_cb(lv_event_t *e)
{
    lv_obj_t *counter = (lv_obj_t *)lv_event_get_user_data(e);
    static int taps = 0;
    lv_label_set_text_fmt(counter, "Taps: %d", ++taps);
}

static lv_obj_t *make_label(lv_obj_t *scr, const char *text, int y)
{
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
    return lbl;
}

esp_err_t ui_init(void)
{
    esp_err_t err = board_display_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display start failed: %s", esp_err_to_name(err));
        return err;
    }

    if (board_display_lock(1000)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

        make_label(scr, "Sleep Tracker", 24);
        s_time_lbl  = make_label(scr, "--:--:--", 70);
        s_accel_lbl = make_label(scr, "IMU: -- g", 110);
        s_batt_lbl  = make_label(scr, "Batt: --", 150);

        // Tap-test: button + counter — verifies the touch path end to end.
        lv_obj_t *counter = lv_label_create(scr);
        lv_label_set_text(counter, "Taps: 0");
        lv_obj_set_style_text_color(counter, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(counter, LV_ALIGN_BOTTOM_MID, 0, -20);

        lv_obj_t *btn = lv_button_create(scr);
        lv_obj_set_size(btn, 180, 80);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
        lv_obj_add_event_cb(btn, tap_btn_cb, LV_EVENT_CLICKED, counter);
        lv_obj_t *btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Tap me");
        lv_obj_center(btn_lbl);

        board_display_unlock();
        ESP_LOGI(TAG, "UI built (live readout + tap-test button)");
    } else {
        ESP_LOGW(TAG, "could not take LVGL lock");
    }
    return ESP_OK;
}

void ui_set_status(const ui_status_t *s)
{
    if (s == NULL || s_time_lbl == NULL) {
        return;   // screen not built yet
    }
    if (!board_display_lock(100)) {
        return;
    }

    lv_label_set_text(s_time_lbl, s->time_str ? s->time_str : "--:--:--");

    char buf[64];
    int g100 = (int)(s->accel_g * 100.0f + 0.5f);   // integer format — avoid %f
    snprintf(buf, sizeof buf, "IMU: %d.%02d g", g100 / 100, g100 % 100);
    lv_label_set_text(s_accel_lbl, buf);

    const char *src = s->charging ? "CHG" : (s->vbus ? "USB" : "BAT");
    if (s->batt_pct >= 0) {
        snprintf(buf, sizeof buf, "Batt: %d%%  %dmV  %s", s->batt_pct, s->vbat_mv, src);
    } else {
        snprintf(buf, sizeof buf, "Batt: --  %dmV  %s", s->vbat_mv, src);
    }
    lv_label_set_text(s_batt_lbl, buf);

    board_display_unlock();
}

void ui_tick(void)
{
    // No-op: LVGL is driven by the esp_lvgl_port task started in the BSP.
}
