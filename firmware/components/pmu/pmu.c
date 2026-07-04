#include "pmu.h"
#include "esp_log.h"

// AXP2101 registers (from XPowersLib AXP2101Constants.h).
#define AXP2101_ADDR            0x34
#define AXP2101_REG_STATUS1     0x00   // bit5 = VBUS good
#define AXP2101_REG_STATUS2     0x01   // bits[6:5] = battery current direction
#define AXP2101_REG_ADC_CH_CTRL 0x30   // bit0 = VBAT ADC enable
#define AXP2101_REG_VBAT_H      0x34   // ADC result0 high (bits[5:0])
#define AXP2101_REG_VBAT_L      0x35   // ADC result0 low
#define AXP2101_REG_BAT_PERCENT 0xA4   // fuel gauge, 0..100

static const char *TAG = "pmu";
static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof b, 100);
}

esp_err_t pmu_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    // Enable the VBAT ADC channel (bit0) so battery-voltage reads are valid.
    uint8_t ch = 0;
    if (rd(AXP2101_REG_ADC_CH_CTRL, &ch) == ESP_OK) {
        wr(AXP2101_REG_ADC_CH_CTRL, (uint8_t)(ch | 0x01));
    }
    ESP_LOGI(TAG, "AXP2101 up");
    return ESP_OK;
}

esp_err_t pmu_read(pmu_status_t *out)
{
    if (out == NULL || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Fail the whole read if ANY register NAKs (plausible on the shared bus):
    // returning ESP_OK with a byte left at its pre-zeroed value would report a
    // fabricated 0% / 0 mV as if it were a genuine reading (0 <= 100 passes the
    // batt_pct guard below, so it looks like a real dead battery, not "unknown"),
    // corrupting both the live UI and the epoch CSV's batt_pct/vbat_mv columns.
    // Callers treat a non-ESP_OK return as "no update", so bailing here is safe.
    uint8_t s1 = 0, s2 = 0, pct = 0, vh = 0, vl = 0;
    esp_err_t err;
    if ((err = rd(AXP2101_REG_STATUS1, &s1))      != ESP_OK) return err;
    if ((err = rd(AXP2101_REG_STATUS2, &s2))      != ESP_OK) return err;
    if ((err = rd(AXP2101_REG_BAT_PERCENT, &pct)) != ESP_OK) return err;
    if ((err = rd(AXP2101_REG_VBAT_H, &vh))       != ESP_OK) return err;
    if ((err = rd(AXP2101_REG_VBAT_L, &vl))       != ESP_OK) return err;

    out->vbus_present = (s1 & 0x20) != 0;                  // STATUS1 bit5
    out->charging     = ((s2 >> 5) & 0x03) == 0x01;        // STATUS2 [6:5] == 01
    out->batt_pct     = (pct <= 100) ? (int)pct : -1;
    out->vbat_mv      = (uint16_t)(((vh & 0x3F) << 8) | vl);
    return ESP_OK;
}
