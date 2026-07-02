#include "bsp.h"
#include "esp_log.h"

static const char *TAG = "bsp";
static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t bsp_init(void)
{
    ESP_LOGI(TAG, "init: shared I2C bus (SDA=%d SCL=%d @ %d Hz)",
             BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO, BSP_I2C_FREQ_HZ);

    // TODO(phase0): the AXP2101 must enable the display/audio/sensor rails
    // before those chips respond. Do that here once pins are confirmed.
    // TODO(phase0): bring up the SH8601 QSPI display panel + FT3168 touch here
    // (board-specific); the `ui` component then attaches LVGL to them.

    const i2c_master_bus_config_t cfg = {
        .i2c_port                     = BSP_I2C_PORT,
        .sda_io_num                   = BSP_I2C_SDA_GPIO,
        .scl_io_num                   = BSP_I2C_SCL_GPIO,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

esp_err_t bsp_i2c_get_bus(i2c_master_bus_handle_t *out_bus)
{
    if (out_bus == NULL || s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_bus = s_i2c_bus;
    return ESP_OK;
}

esp_err_t bsp_sdcard_mount(const char *mount_point)
{
    // TODO(phase2): wire SDMMC/SPI from the schematic and mount FAT here.
    ESP_LOGW(TAG, "sdcard_mount('%s'): not yet implemented", mount_point);
    return ESP_OK;
}
