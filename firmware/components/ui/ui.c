#include "ui.h"
#include "board.h"
#include "lvgl.h"
#include "esp_log.h"

// Phase 0: bring up the display + touch + LVGL via the board seam (which
// delegates to the managed Waveshare BSP) and draw a "hello world" so we can
// confirm pixels on the AMOLED. LVGL itself is serviced by the esp_lvgl_port
// task inside the BSP, so ui_tick() must NOT call lv_timer_handler().
// TODO(phase4): watch face / tracking / morning-report / settings screens.

static const char *TAG = "ui";

static void tap_btn_cb(lv_event_t *e)
{
    lv_obj_t *counter = (lv_obj_t *)lv_event_get_user_data(e);
    static int taps = 0;
    lv_label_set_text_fmt(counter, "Taps: %d", ++taps);
}

esp_err_t ui_init(void)
{
    esp_err_t err = board_display_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Any lv_* call must happen under the LVGL lock.
    if (board_display_lock(1000)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

        lv_obj_t *title = lv_label_create(scr);
        lv_label_set_text(title, "Sleep Tracker\nPhase 0");
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

        // Tap-test: a button that increments an on-screen counter — verifies the
        // full touch path (FT3168 -> esp_lcd_touch -> LVGL input) end to end.
        lv_obj_t *counter = lv_label_create(scr);
        lv_label_set_text(counter, "Taps: 0");
        lv_obj_set_style_text_color(counter, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(counter, LV_ALIGN_CENTER, 0, 70);

        lv_obj_t *btn = lv_button_create(scr);
        lv_obj_set_size(btn, 180, 80);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, -10);
        lv_obj_add_event_cb(btn, tap_btn_cb, LV_EVENT_CLICKED, counter);
        lv_obj_t *btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, "Tap me");
        lv_obj_center(btn_lbl);

        board_display_unlock();
        ESP_LOGI(TAG, "UI built (title + tap-test button)");
    } else {
        ESP_LOGW(TAG, "could not take LVGL lock");
    }
    return ESP_OK;
}

void ui_tick(void)
{
    // No-op: LVGL is driven by the esp_lvgl_port task started in the BSP.
}
