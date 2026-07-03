#include "max30102.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// MAX30102 PPG driver (external, I2C @ 0x57). SpO2 mode = red + IR, both logged.
// Register map per the Analog Devices datasheet. INT line unused for now — we
// poll the FIFO write/read pointers (PLAN.md §2.2 notes INT as a power win TODO).

#define MAX30102_ADDR       0x57
#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C   // red
#define REG_LED2_PA         0x0D   // IR
#define REG_PART_ID         0xFF
#define MAX30102_PART_ID    0x15

#define FIFO_DEPTH          32
#define BYTES_PER_SAMPLE    6      // SpO2 mode: RED[3] + IR[3]

static const char *TAG = "max30102";
static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 200);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof b, 100);
}

esp_err_t max30102_init(i2c_master_bus_handle_t bus, const max30102_config_t *cfg)
{
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MAX30102_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t part = 0;
    if (rd(REG_PART_ID, &part, 1) != ESP_OK || part != MAX30102_PART_ID) {
        ESP_LOGE(TAG, "PART_ID=0x%02x (expected 0x15) — sensor not on the bus at 0x57", part);
        return ESP_ERR_NOT_FOUND;
    }

    wr(REG_MODE_CONFIG, 0x40);   // soft reset
    vTaskDelay(pdMS_TO_TICKS(10));

    // Clear FIFO pointers.
    wr(REG_FIFO_WR_PTR, 0x00);
    wr(REG_OVF_COUNTER, 0x00);
    wr(REG_FIFO_RD_PTR, 0x00);

    wr(REG_FIFO_CONFIG, 0x10);   // no sample-averaging, FIFO rollover enabled
    wr(REG_MODE_CONFIG, 0x03);   // SpO2 mode (red + IR)
    wr(REG_SPO2_CONFIG, 0x27);   // ADC 4096 nA, 100 Hz, 411 us pulse (18-bit)

    uint8_t red = (cfg && cfg->led_current_red) ? cfg->led_current_red : 0x24;
    uint8_t ir  = (cfg && cfg->led_current_ir)  ? cfg->led_current_ir  : 0x24;
    wr(REG_LED1_PA, red);
    wr(REG_LED2_PA, ir);

    ESP_LOGI(TAG, "MAX30102 up (PART_ID=0x15, SpO2 100Hz, LED red/ir=%u/%u)", red, ir);
    return ESP_OK;
}

esp_err_t max30102_read_fifo(max30102_sample_t *out, size_t max, size_t *out_count)
{
    if (out_count) *out_count = 0;
    if (s_dev == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t wr_ptr = 0, rd_ptr = 0;
    if (rd(REG_FIFO_WR_PTR, &wr_ptr, 1) != ESP_OK ||
        rd(REG_FIFO_RD_PTR, &rd_ptr, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    int avail = ((int)wr_ptr - (int)rd_ptr) & (FIFO_DEPTH - 1);
    if (avail <= 0) {
        return ESP_OK;
    }
    if ((size_t)avail > max) {
        avail = (int)max;
    }

    uint8_t buf[FIFO_DEPTH * BYTES_PER_SAMPLE];
    uint8_t reg = REG_FIFO_DATA;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf,
                                                (size_t)avail * BYTES_PER_SAMPLE, 300);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < avail; i++) {
        const uint8_t *p = &buf[i * BYTES_PER_SAMPLE];
        out[i].red = (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) & 0x3FFFF;
        out[i].ir  = (((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 8) | p[5]) & 0x3FFFF;
    }
    if (out_count) *out_count = (size_t)avail;
    return ESP_OK;
}

esp_err_t max30102_shutdown(bool enable)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return wr(REG_MODE_CONFIG, enable ? 0x80 : 0x03);   // SHDN bit / back to SpO2
}
