#include "board.h"
#include "bsp/esp-bsp.h"   // managed Waveshare BSP (waveshare/esp32_s3_touch_amoled_1_8)
#include "bsp/touch.h"     // bsp_touch_new()
#include "esp_lcd_touch.h" // esp_lcd_touch_handle_t (also enables esp_lvgl_port's touch API)
#include "esp_lvgl_port.h"
#include "esp_io_expander.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

esp_err_t board_init(void)
{
    // The managed BSP installs the shared I2C master bus (port 1, SDA=15/SCL=14).
    // It is idempotent, so it's safe if the touch bring-up later re-inits it.
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");
    board_i2c_scan();
    return ESP_OK;
}

esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t *out_bus)
{
    if (out_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;   // board_init() not called yet
    }
    *out_bus = bus;
    return ESP_OK;
}

void board_i2c_scan(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "i2c scan skipped: bus not initialized");
        return;
    }
    ESP_LOGI(TAG, "I2C scan on the shared bus (SDA=15 SCL=14):");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50 /*ms*/) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X  ACK", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan done: %d device(s) responded", found);
}

esp_err_t board_sdcard_mount(void)
{
    // Mount point is fixed by the BSP's Kconfig (CONFIG_BSP_SD_MOUNT_POINT,
    // default "/sdcard"). Returns an error (harmlessly) if no card is inserted.
    return bsp_sdcard_mount();
}

// The FT3168 touch and the CO5300 reset both hang off the TCA9554 IO-expander,
// which the managed BSP creates but never drives — so the touch chip stays in
// reset and mute at 0x38. Waveshare's own FT3168 example releases them by
// pulsing expander pins 0/1/2 low->high before init; we replicate that here.
static void board_release_peripheral_resets(void)
{
    esp_io_expander_handle_t exp = bsp_io_expander_init();   // TCA9554 @ 0x20
    if (exp == NULL) {
        ESP_LOGW(TAG, "IO expander init failed; LCD/touch resets not released");
        return;
    }
    const uint32_t rst_pins = (1U << 0) | (1U << 1) | (1U << 2);   // EXIO0/1/2
    esp_io_expander_set_dir(exp, rst_pins, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(exp, rst_pins, 0);   // assert (active-low)
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_io_expander_set_level(exp, rst_pins, 1);   // release
    vTaskDelay(pdMS_TO_TICKS(120));                // FT3168 boot time
}

esp_err_t board_display_start(void)
{
    // We bring up display + LVGL directly rather than via the vendor
    // bsp_display_start(), which ESP_ERROR_CHECKs touch and would panic-loop if
    // touch weren't found. First release the LCD/touch resets on the TCA9554,
    // then bring up the CO5300 panel, LVGL, and (non-fatally) touch.
    board_release_peripheral_resets();

    bsp_display_config_t disp_hw = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * (int)sizeof(uint16_t),
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(bsp_display_new(&disp_hw, &panel, &io), TAG, "bsp_display_new failed");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = BSP_LCD_H_RES * 100,   // partial buffer, in PSRAM
        .double_buffer = true,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags         = { .buff_dma = false, .buff_spiram = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    // Touch — non-fatal. The FT3168 answers at 0x38 (FT5x06-compatible driver in
    // the BSP) once its reset is released above. If it still doesn't probe, keep
    // the display usable rather than aborting.
    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK && tp != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
        if (lvgl_port_add_touch(&touch_cfg) != NULL) {
            ESP_LOGI(TAG, "touch up (FT3168 @ 0x38)");
        } else {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed");
        }
    } else {
        ESP_LOGW(TAG, "touch still not found at 0x38 after expander reset");
    }
    return ESP_OK;
}

bool board_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void board_display_unlock(void)
{
    lvgl_port_unlock();
}
