#include "ui.h"
#include "ppg.h"
#include "board.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdint.h>

// Phase 0/1 bring-up + PPG-debug screen: a live diagnostic readout (RTC time,
// IMU accel magnitude, HR/SpO2, battery) plus a scrolling graph of the
// band-passed PPG waveform for debugging the heart-rate signal. LVGL is serviced
// by the esp_lvgl_port task in the BSP, so ui_tick() is a no-op and cross-task
// updates go through ui_set_status() / ui_update_waveform() under the LVGL lock.
// TODO(phase4): real watch face / tracking / morning-report / settings screens.

static const char *TAG = "ui";

static lv_obj_t *s_time_lbl;
static lv_obj_t *s_accel_lbl;
static lv_obj_t *s_hr_lbl;
static lv_obj_t *s_batt_lbl;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ppg_ser;

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

        make_label(scr, "Sleep Tracker - PPG debug", 8);
        s_time_lbl  = make_label(scr, "--:--:--", 36);
        s_accel_lbl = make_label(scr, "IMU: -- g", 62);
        s_hr_lbl    = make_label(scr, "HR: -- (no finger)", 88);
        s_batt_lbl  = make_label(scr, "Batt: --", 114);

        // Live PPG waveform graph — band-passed IR, auto-scaled per frame.
        s_chart = lv_chart_create(scr);
        lv_obj_set_size(s_chart, 350, 280);
        lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(s_chart, PPG_WAVE_N);
        lv_chart_set_div_line_count(s_chart, 5, 6);
        lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x101010), LV_PART_MAIN);
        lv_obj_set_style_border_color(s_chart, lv_color_hex(0x303030), LV_PART_MAIN);
        // Hide the per-point round markers for a clean continuous trace.
        lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
        lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
        s_ppg_ser = lv_chart_add_series(s_chart, lv_palette_main(LV_PALETTE_GREEN),
                                        LV_CHART_AXIS_PRIMARY_Y);

        board_display_unlock();
        ESP_LOGI(TAG, "UI built (readout + live PPG graph)");
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

    if (!s->finger) {
        lv_label_set_text(s_hr_lbl, "HR: -- (no finger)");
    } else if (s->hr_bpm > 0) {
        snprintf(buf, sizeof buf, "HR: %d bpm   SpO2: %d%%", s->hr_bpm, s->spo2);
        lv_label_set_text(s_hr_lbl, buf);
    } else {
        lv_label_set_text(s_hr_lbl, "HR: -- (reading...)");
    }

    const char *src = s->charging ? "CHG" : (s->vbus ? "USB" : "BAT");
    if (s->batt_pct >= 0) {
        snprintf(buf, sizeof buf, "Batt: %d%%  %dmV  %s", s->batt_pct, s->vbat_mv, src);
    } else {
        snprintf(buf, sizeof buf, "Batt: --  %dmV  %s", s->vbat_mv, src);
    }
    lv_label_set_text(s_batt_lbl, buf);

    board_display_unlock();
}

void ui_update_waveform(void)
{
    if (s_chart == NULL || s_ppg_ser == NULL) {
        return;   // screen not built yet
    }
    int32_t buf[PPG_WAVE_N];
    size_t n = ppg_copy_waveform(buf, PPG_WAVE_N);
    if (n == 0) {
        return;
    }
    if (!board_display_lock(20)) {
        return;
    }

    // Auto-scale Y to the window so the pulse fills the graph; clamp a minimum
    // span so a flat (no-finger) trace isn't amplified into meaningless noise.
    int32_t mn = buf[0], mx = buf[0];
    for (size_t i = 1; i < n; i++) {
        if (buf[i] < mn) mn = buf[i];
        if (buf[i] > mx) mx = buf[i];
    }
    if (mx - mn < 200) {
        const int32_t c = (mn + mx) / 2;
        mn = c - 100;
        mx = c + 100;
    }
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, mn, mx);

    int32_t *ys = lv_chart_get_series_y_array(s_chart, s_ppg_ser);
    for (size_t i = 0; i < n; i++) {
        ys[i] = buf[i];
    }
    lv_chart_refresh(s_chart);
    board_display_unlock();
}

void ui_tick(void)
{
    // No-op: LVGL is driven by the esp_lvgl_port task started in the BSP.
}
