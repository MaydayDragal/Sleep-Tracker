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

        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label, "Sleep Tracker\nPhase 0");
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(label);

        board_display_unlock();
        ESP_LOGI(TAG, "LVGL hello-world drawn");
    } else {
        ESP_LOGW(TAG, "could not take LVGL lock");
    }
    return ESP_OK;
}

void ui_tick(void)
{
    // No-op: LVGL is driven by the esp_lvgl_port task started in the BSP.
}
