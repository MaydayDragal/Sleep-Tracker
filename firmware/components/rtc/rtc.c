#include "pcf85063.h"
#include "esp_log.h"

// PCF85063A register map (BCD). Seconds bit7 is the oscillator-stopped flag.
#define PCF85063_ADDR        0x51
#define PCF85063_REG_CTRL1   0x00
#define PCF85063_REG_SEC     0x04   // 0x04..0x0A: sec,min,hour,day,dotw,month,year

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    // CTRL1 = 0x00: normal mode, oscillator enabled (clears STOP if set).
    uint8_t ctrl1[2] = { PCF85063_REG_CTRL1, 0x00 };
    return i2c_master_transmit(s_dev, ctrl1, sizeof ctrl1, 100);
}

esp_err_t pcf85063_get(pcf85063_datetime_t *out)
{
    if (out == NULL || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t reg = PCF85063_REG_SEC, b[7];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, 100);
    if (err != ESP_OK) {
        return err;
    }
    out->sec   = bcd2dec(b[0] & 0x7F);
    out->min   = bcd2dec(b[1] & 0x7F);
    out->hour  = bcd2dec(b[2] & 0x3F);
    out->day   = bcd2dec(b[3] & 0x3F);
    out->dotw  = (uint8_t)(b[4] & 0x07);
    out->month = bcd2dec(b[5] & 0x1F);
    out->year  = (uint16_t)(2000 + bcd2dec(b[6]));
    return ESP_OK;
}

esp_err_t pcf85063_set(const pcf85063_datetime_t *in)
{
    if (in == NULL || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[8] = {
        PCF85063_REG_SEC,
        dec2bcd(in->sec),
        dec2bcd(in->min),
        dec2bcd(in->hour),
        dec2bcd(in->day),
        (uint8_t)(in->dotw & 0x07),
        dec2bcd(in->month),
        dec2bcd((uint8_t)(in->year % 100)),
    };
    return i2c_master_transmit(s_dev, b, sizeof b, 100);
}

bool pcf85063_time_valid(const pcf85063_datetime_t *t)
{
    return t != NULL &&
           t->year >= 2024 && t->year <= 2069 &&
           t->month >= 1 && t->month <= 12 &&
           t->day >= 1 && t->day <= 31 &&
           t->dotw <= 6 && t->hour <= 23 && t->min <= 59 && t->sec <= 59;
}
