#include "actigraphy.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

// QMI8658 6-axis IMU (wrist), accel-only low-power. Register map from the
// Waveshare SensorLib QMI8658 constants; config mirrors their ESP-IDF example
// (±4g, 250 Hz). Only the accelerometer is enabled (gyro off) for actigraphy.

#define QMI8658_ADDR          0x6B   // SA0 high on this board (0x6A is the alt)
#define QMI8658_REG_WHOAMI    0x00   // == 0x05
#define QMI8658_REG_CTRL1     0x02
#define QMI8658_REG_CTRL2     0x03
#define QMI8658_REG_CTRL3     0x04
#define QMI8658_REG_CTRL5     0x06
#define QMI8658_REG_CTRL7     0x08
#define QMI8658_REG_RESET     0x60
#define QMI8658_REG_AX_L      0x35   // 0x35..0x3A: AX,AY,AZ (int16 LE)
#define QMI8658_WHOAMI_VAL    0x05
#define QMI8658_LSB_PER_G     8192.0f   // ±4g full scale

static const char *TAG = "actigraphy";
static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof b, 100);
}

esp_err_t actigraphy_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = QMI8658_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    wr(QMI8658_REG_RESET, 0xB0);             // soft reset
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t who = 0;
    if (rd(QMI8658_REG_WHOAMI, &who, 1) != ESP_OK || who != QMI8658_WHOAMI_VAL) {
        ESP_LOGE(TAG, "QMI8658 WHO_AM_I=0x%02x (expected 0x05)", who);
        return ESP_ERR_NOT_FOUND;
    }

    wr(QMI8658_REG_CTRL1, 0x40);   // serial-interface address auto-increment
    wr(QMI8658_REG_CTRL7, 0x00);   // sensors off while configuring
    wr(QMI8658_REG_CTRL2, 0x15);   // accel: aFS=±4g (001) | aODR=250Hz (0x5)
    wr(QMI8658_REG_CTRL3, 0x00);   // gyro off
    wr(QMI8658_REG_CTRL5, 0x00);   // no low-pass filter
    err = wr(QMI8658_REG_CTRL7, 0x01);   // enable accelerometer
    ESP_LOGI(TAG, "QMI8658 up (WHO_AM_I=0x05, accel +/-4g @250Hz)");
    return err;
}

esp_err_t actigraphy_read_accel_g(float *ax, float *ay, float *az)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[6];
    esp_err_t err = rd(QMI8658_REG_AX_L, b, sizeof b);
    if (err != ESP_OK) {
        return err;
    }
    int16_t rx = (int16_t)((b[1] << 8) | b[0]);
    int16_t ry = (int16_t)((b[3] << 8) | b[2]);
    int16_t rz = (int16_t)((b[5] << 8) | b[4]);
    if (ax) *ax = rx / QMI8658_LSB_PER_G;
    if (ay) *ay = ry / QMI8658_LSB_PER_G;
    if (az) *az = rz / QMI8658_LSB_PER_G;
    return ESP_OK;
}

esp_err_t actigraphy_read_activity(float *out_count)
{
    float x, y, z;
    esp_err_t err = actigraphy_read_accel_g(&x, &y, &z);
    if (err != ESP_OK) {
        if (out_count) *out_count = 0.0f;
        return err;
    }
    // Simple activity proxy: magnitude's deviation from 1 g at rest.
    float mag = sqrtf(x * x + y * y + z * z);
    if (out_count) *out_count = fabsf(mag - 1.0f);
    return ESP_OK;
}
