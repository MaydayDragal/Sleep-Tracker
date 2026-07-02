#include "board.h"
#include "bsp/esp-bsp.h"   // managed Waveshare BSP (waveshare/esp32_s3_touch_amoled_1_8)
#include "esp_lvgl_port.h"
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

esp_err_t board_display_start(void)
{
    // NOTE: we deliberately do NOT call the vendor bsp_display_start(). That path
    // hard-asserts (ESP_ERROR_CHECK) on touch init, and BSP v2.0.3 never releases
    // the FT3168 touch reset via the TCA9554 expander (both LCD_RST and TOUCH_RST
    // are NC), so the touch chip stays mute at 0x38 and the whole board panics in
    // a boot loop. The CO5300 display itself comes up fine over QSPI, so we bring
    // up display + LVGL directly here and defer touch (Phase-0 TODO: drive the
    // expander reset pin once its mapping is confirmed from the schematic).
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
    ESP_LOGW(TAG, "touch deferred (vendor BSP doesn't release FT3168 reset via TCA9554) — Phase-0 TODO");
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
