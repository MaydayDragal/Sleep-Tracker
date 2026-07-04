// =============================================================================
//  MAX3010x driver implementation. See max30102.h for the API and rationale.
//
//  Register addresses / bit positions follow the MAX30102 and MAX30101
//  datasheets (datasheets/). The two parts are register-compatible; the only
//  physical difference this driver cares about is the MAX30101's extra GREEN
//  LED (LED3), reached through Multi-LED mode.
//
//  LED ↔ colour mapping:
//    MAX30102 — LED1 (0x0C) = RED, LED2 (0x0D) = IR.  (verified on this board)
//    MAX30101 — assumes LED1 = RED, LED2 = IR, LED3 (0x0E) = GREEN.
//      TODO(max30101): confirm the LED1/2/3 ↔ colour assignment against the
//      MAX30101 datasheet when the part arrives. The MAX30102 in hand has no
//      green channel, so this assumption is inert until then.
// =============================================================================

#include "max30102.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <string.h>

// --- Register map ------------------------------------------------------------
#define REG_INT_STATUS_1    0x00
#define REG_INT_STATUS_2    0x01
#define REG_INT_ENABLE_1    0x02
#define REG_INT_ENABLE_2    0x03
#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C   // RED
#define REG_LED2_PA         0x0D   // IR
#define REG_LED3_PA         0x0E   // GREEN (MAX30101)
#define REG_PILOT_PA        0x10   // proximity-mode pilot LED current
#define REG_MULTI_LED_CTRL1 0x11   // SLOT2[6:4] | SLOT1[2:0]
#define REG_MULTI_LED_CTRL2 0x12   // SLOT4[6:4] | SLOT3[2:0]
#define REG_DIE_TEMP_INT    0x1F   // signed °C
#define REG_DIE_TEMP_FRAC   0x20   // [3:0], 0.0625 °C/LSB
#define REG_DIE_TEMP_CONFIG 0x21   // TEMP_EN[0]
#define REG_PROX_INT_THRESH 0x30
#define REG_REV_ID          0xFE
#define REG_PART_ID         0xFF

// --- Register bits -----------------------------------------------------------
#define MODE_SHDN           0x80
#define MODE_RESET          0x40
#define FIFO_ROLLOVER_EN    0x10
#define TEMP_EN             0x01
#define ST1_DIE_TEMP_RDY    0x02   // in INT_STATUS_2

#define PART_ID_MAX3010X    0x15
#define FIFO_DEPTH          32
#define I2C_ADDR            0x57
#define ADC_MASK            0x3FFFFu   // 18-bit sample

static const char *TAG = "max3010x";

// --- Driver state ------------------------------------------------------------
static struct {
    i2c_master_dev_handle_t dev;
    max30102_variant_t      variant;
    max30102_mode_t         mode;
    max30102_adc_range_t    adc_range;
    max30102_pulse_width_t  pw;
    max30102_sample_avg_t   smp_ave;
    uint16_t                fs;
    uint8_t                 a_full_code;      // FIFO_CONFIG[3:0] (empty spaces at A_FULL)
    uint8_t                 led_red, led_ir, led_green;
    uint8_t                 channels;         // 3-byte channels per FIFO sample (1..3)
    max30102_slot_t         chan_slot[4];     // demux: channel index -> which LED colour
    int                     int_gpio;
    bool                    isr_installed;
    max30102_isr_cb_t       cb;
    void                   *cb_arg;
} S;

// --- Low-level I2C -----------------------------------------------------------
static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(S.dev, &reg, 1, buf, n, 200);
}
static esp_err_t rd8(uint8_t reg, uint8_t *v) { return rd(reg, v, 1); }
static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(S.dev, b, sizeof b, 100);
}

// --- Encoding helpers --------------------------------------------------------
static int sr_code(uint16_t hz)
{
    switch (hz) {
        case 50:   return 0;  case 100:  return 1;  case 200:  return 2;
        case 400:  return 3;  case 800:  return 4;  case 1000: return 5;
        case 1600: return 6;  case 3200: return 7;  default:   return -1;
    }
}

static uint8_t adc_bits(max30102_adc_range_t r)
{
    switch (r) {
        case MAX30102_ADC_2048NA:  return 0;
        case MAX30102_ADC_8192NA:  return 2;
        case MAX30102_ADC_16384NA: return 3;
        case MAX30102_ADC_4096NA:
        case MAX30102_ADC_DEFAULT:
        default:                   return 1;   // 4096 nA
    }
}

// Widest pulse width (=> best SNR) that still fits `channels` LED slots at `fs`.
// Table mirrors the datasheet's proven rate/pulse-width combinations for two
// LEDs (verified on hardware); a third slot (green) narrows one step.
static uint8_t auto_pw_code(uint16_t fs, uint8_t channels)
{
    uint8_t code;
    if      (fs <= 100) code = 3;   // 411 us / 18-bit
    else if (fs <= 200) code = 2;   // 215 us / 17-bit
    else if (fs <= 400) code = 1;   // 118 us / 16-bit
    else                code = 0;   //  69 us / 15-bit
    if (channels >= 3 && code > 0) code--;   // green adds a slot; leave headroom
    return code;
}

static uint8_t pw_code(void)
{
    switch (S.pw) {
        case MAX30102_PW_69US_15BIT:  return 0;
        case MAX30102_PW_118US_16BIT: return 1;
        case MAX30102_PW_215US_17BIT: return 2;
        case MAX30102_PW_411US_18BIT: return 3;
        case MAX30102_PW_AUTO:
        default:                      return auto_pw_code(S.fs, S.channels);
    }
}

// --- Register writers grouped by function ------------------------------------
static esp_err_t write_fifo_config(void)
{
    return wr(REG_FIFO_CONFIG,
              ((uint8_t)S.smp_ave << 5) | FIFO_ROLLOVER_EN | (S.a_full_code & 0x0F));
}

static esp_err_t write_spo2_config(void)
{
    const int src = sr_code(S.fs);
    const uint8_t v = (uint8_t)((adc_bits(S.adc_range) << 5) |
                                (((src < 0 ? 1 : src) & 0x7) << 2) |
                                (pw_code() & 0x3));
    return wr(REG_SPO2_CONFIG, v);
}

static esp_err_t write_led_currents(void)
{
    esp_err_t e = wr(REG_LED1_PA, S.led_red);
    if (e == ESP_OK) e = wr(REG_LED2_PA, S.led_ir);
    if (e == ESP_OK && S.variant == MAX3010X_MAX30101) e = wr(REG_LED3_PA, S.led_green);
    return e;
}

// Apply a mode: set MODE_CONFIG, compute the channel count + FIFO demux map, and
// (for MULTI) program the slot registers.
static esp_err_t apply_mode(max30102_mode_t mode, const max30102_slot_t slots[4])
{
    S.mode = mode;
    switch (mode) {
        case MAX30102_MODE_HR:
            S.channels = 1;
            S.chan_slot[0] = MAX30102_SLOT_RED;   // HR mode drives a single LED (LED1)
            break;
        case MAX30102_MODE_MULTI: {
            uint8_t n = 0;
            max30102_slot_t s0 = MAX30102_SLOT_NONE, s1 = MAX30102_SLOT_NONE;
            max30102_slot_t s2 = MAX30102_SLOT_NONE, s3 = MAX30102_SLOT_NONE;
            if (slots) { s0 = slots[0]; s1 = slots[1]; s2 = slots[2]; s3 = slots[3]; }
            const max30102_slot_t raw[4] = { s0, s1, s2, s3 };
            for (int i = 0; i < 4; i++) {
                if (raw[i] != MAX30102_SLOT_NONE) S.chan_slot[n++] = raw[i];
            }
            S.channels = (n == 0) ? 1 : n;
            wr(REG_MULTI_LED_CTRL1, (uint8_t)((s1 << 4) | s0));
            wr(REG_MULTI_LED_CTRL2, (uint8_t)((s3 << 4) | s2));
            break;
        }
        case MAX30102_MODE_SPO2:
        default:
            S.mode = MAX30102_MODE_SPO2;
            S.channels = 2;
            S.chan_slot[0] = MAX30102_SLOT_RED;   // SpO2 FIFO order: RED then IR
            S.chan_slot[1] = MAX30102_SLOT_IR;
            break;
    }
    return wr(REG_MODE_CONFIG, (uint8_t)S.mode);   // SHDN cleared => running
}

// --- INT-line ISR ------------------------------------------------------------
static void IRAM_ATTR max30102_gpio_isr(void *arg)
{
    (void)arg;
    if (S.cb) S.cb(S.cb_arg);
}

// =============================================================================
//  Public API
// =============================================================================

void max30102_default_config(max30102_config_t *cfg, max30102_variant_t variant)
{
    if (!cfg) return;
    *cfg = (max30102_config_t){
        .sample_rate_hz    = 100,
        .led_current_red   = 0x24,          // ~7 mA
        .led_current_ir    = 0x24,
        .int_gpio          = -1,
        .variant           = variant,
        .mode              = MAX30102_MODE_SPO2,
        .led_current_green = (variant == MAX3010X_MAX30101) ? 0x24 : 0,
        .adc_range         = MAX30102_ADC_DEFAULT,
        .pulse_width       = MAX30102_PW_AUTO,
        .sample_avg        = MAX30102_AVG_1,
        .int_a_full_filled = 24,
    };
}

esp_err_t max30102_init(i2c_master_bus_handle_t bus, const max30102_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &S.dev);
    if (err != ESP_OK) return err;

    uint8_t part = 0;
    if (rd8(REG_PART_ID, &part) != ESP_OK || part != PART_ID_MAX3010X) {
        ESP_LOGE(TAG, "PART_ID=0x%02x (want 0x15) — no MAX3010x at 0x57", part);
        i2c_master_bus_rm_device(S.dev);
        S.dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    // Resolve config (0 => default) into driver state.
    S.variant     = cfg->variant;
    S.adc_range   = cfg->adc_range;
    S.pw          = cfg->pulse_width;
    S.smp_ave     = cfg->sample_avg;
    S.fs          = (sr_code(cfg->sample_rate_hz) >= 0) ? cfg->sample_rate_hz : 100;
    S.led_red     = cfg->led_current_red   ? cfg->led_current_red   : 0x24;
    S.led_ir      = cfg->led_current_ir    ? cfg->led_current_ir    : 0x24;
    S.led_green   = cfg->led_current_green;
    S.int_gpio    = cfg->int_gpio;
    uint8_t filled = cfg->int_a_full_filled ? cfg->int_a_full_filled : 24;
    if (filled < 17) filled = 17;
    if (filled > 32) filled = 32;
    S.a_full_code = (uint8_t)(32 - filled);

    // POR reset, then apply configuration.
    max30102_reset();
    max30102_flush_fifo();
    write_fifo_config();
    apply_mode(cfg->mode ? cfg->mode : MAX30102_MODE_SPO2,
               (cfg->mode == MAX30102_MODE_MULTI) ? cfg->slots : NULL);
    write_spo2_config();
    write_led_currents();

    // Interrupt-driven reads: arm A_FULL and install the GPIO ISR.
    if (S.int_gpio >= 0) {
        max30102_config_interrupts(MAX30102_INT_A_FULL);
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << S.int_gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,       // INT is open-drain, active low
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io);
        gpio_install_isr_service(0);                  // ESP_ERR_INVALID_STATE if already up — fine
        if (gpio_isr_handler_add(S.int_gpio, max30102_gpio_isr, NULL) == ESP_OK) {
            S.isr_installed = true;
        }
    }

    ESP_LOGI(TAG, "%s up: %s, %u ch, %u Hz, LED r/ir/g=%u/%u/%u, INT=%d",
             (S.variant == MAX3010X_MAX30101) ? "MAX30101" : "MAX30102",
             (S.mode == MAX30102_MODE_MULTI) ? "MULTI" :
             (S.mode == MAX30102_MODE_HR)    ? "HR" : "SpO2",
             S.channels, (unsigned)S.fs, S.led_red, S.led_ir, S.led_green, S.int_gpio);
    return ESP_OK;
}

esp_err_t max30102_deinit(void)
{
    if (S.isr_installed && S.int_gpio >= 0) {
        gpio_isr_handler_remove(S.int_gpio);
    }
    if (S.dev) {
        i2c_master_bus_rm_device(S.dev);
    }
    memset(&S, 0, sizeof S);
    S.int_gpio = -1;
    return ESP_OK;
}

esp_err_t max30102_reset(void)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    esp_err_t e = wr(REG_MODE_CONFIG, MODE_RESET);
    vTaskDelay(pdMS_TO_TICKS(10));       // RESET self-clears within a few ms
    return e;
}

esp_err_t max30102_read_id(uint8_t *part_id, uint8_t *rev_id)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    if (part_id) { esp_err_t e = rd8(REG_PART_ID, part_id); if (e != ESP_OK) return e; }
    if (rev_id)  { esp_err_t e = rd8(REG_REV_ID,  rev_id);  if (e != ESP_OK) return e; }
    return ESP_OK;
}

esp_err_t max30102_read_fifo_ex(max30102_sample_t *out, size_t max,
                                size_t *out_count, uint32_t *dropped)
{
    if (out_count) *out_count = 0;
    if (dropped)   *dropped   = 0;
    if (!S.dev || !out) return ESP_ERR_INVALID_STATE;

    uint8_t p[3];   // WR_PTR, OVF_COUNTER, RD_PTR (contiguous)
    if (rd(REG_FIFO_WR_PTR, p, 3) != ESP_OK) return ESP_FAIL;
    const uint8_t wp = p[0] & 0x1F, ovf = p[1], rp = p[2] & 0x1F;

    int avail = ((int)wp - (int)rp) & (FIFO_DEPTH - 1);
    if (ovf > 0) {                       // overflowed => FIFO is full; wp==rp reads as 0
        avail = FIFO_DEPTH;
        if (dropped) *dropped = ovf;
    }
    if (avail <= 0) return ESP_OK;
    if ((size_t)avail > max) avail = (int)max;

    const uint8_t bps = (uint8_t)(S.channels * 3);
    uint8_t buf[FIFO_DEPTH * 9];         // 32 samples × up to 3 channels × 3 bytes
    uint8_t reg = REG_FIFO_DATA;
    if (i2c_master_transmit_receive(S.dev, &reg, 1, buf,
                                    (size_t)avail * bps, 300) != ESP_OK) {
        return ESP_FAIL;
    }

    for (int i = 0; i < avail; i++) {
        max30102_sample_t s = { 0, 0, 0 };
        const uint8_t *base = &buf[i * bps];
        for (int c = 0; c < S.channels; c++) {
            const uint8_t *q = &base[c * 3];
            const uint32_t v = (((uint32_t)q[0] << 16) |
                                ((uint32_t)q[1] << 8)  | q[2]) & ADC_MASK;
            switch (S.chan_slot[c]) {
                case MAX30102_SLOT_RED:   s.red   = v; break;
                case MAX30102_SLOT_IR:    s.ir    = v; break;
                case MAX30102_SLOT_GREEN: s.green = v; break;
                default: break;
            }
        }
        out[i] = s;
    }
    if (ovf > 0) wr(REG_OVF_COUNTER, 0);   // clear the overflow latch after recovering
    if (out_count) *out_count = (size_t)avail;
    return ESP_OK;
}

esp_err_t max30102_read_fifo(max30102_sample_t *out, size_t max, size_t *out_count)
{
    return max30102_read_fifo_ex(out, max, out_count, NULL);
}

esp_err_t max30102_samples_available(size_t *count)
{
    if (count) *count = 0;
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    uint8_t p[3];
    if (rd(REG_FIFO_WR_PTR, p, 3) != ESP_OK) return ESP_FAIL;
    int avail = ((int)(p[0] & 0x1F) - (int)(p[2] & 0x1F)) & (FIFO_DEPTH - 1);
    if (p[1] > 0) avail = FIFO_DEPTH;
    if (count) *count = (size_t)avail;
    return ESP_OK;
}

esp_err_t max30102_flush_fifo(void)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    esp_err_t e = wr(REG_FIFO_WR_PTR, 0);
    if (e == ESP_OK) e = wr(REG_OVF_COUNTER, 0);
    if (e == ESP_OK) e = wr(REG_FIFO_RD_PTR, 0);
    return e;
}

esp_err_t max30102_set_sample_rate(uint16_t hz)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    S.fs = (sr_code(hz) >= 0) ? hz : 100;
    esp_err_t e = write_spo2_config();     // re-derives AUTO pulse width for the new rate
    max30102_flush_fifo();                 // don't let the consumer mix rates
    ESP_LOGI(TAG, "sample rate -> %u Hz", (unsigned)S.fs);
    return e;
}

esp_err_t max30102_set_led_current(uint8_t red, uint8_t ir, uint8_t green)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    S.led_red = red; S.led_ir = ir; S.led_green = green;
    return write_led_currents();
}

esp_err_t max30102_set_adc_range(max30102_adc_range_t range)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    S.adc_range = range;
    return write_spo2_config();
}

esp_err_t max30102_set_pulse_width(max30102_pulse_width_t pw)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    S.pw = pw;
    return write_spo2_config();
}

esp_err_t max30102_set_sample_averaging(max30102_sample_avg_t avg)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    S.smp_ave = avg;
    return write_fifo_config();
}

esp_err_t max30102_set_mode(max30102_mode_t mode, const max30102_slot_t slots[4])
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    esp_err_t e = apply_mode(mode, slots);
    if (e == ESP_OK) e = write_spo2_config();   // channel count affects AUTO pulse width
    max30102_flush_fifo();
    return e;
}

esp_err_t max30102_shutdown(bool enable)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    return wr(REG_MODE_CONFIG, (enable ? MODE_SHDN : 0) | (uint8_t)S.mode);
}

esp_err_t max30102_config_proximity(uint8_t threshold, uint8_t pilot_pa)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    esp_err_t e = wr(REG_PROX_INT_THRESH, threshold);
    if (e == ESP_OK) e = wr(REG_PILOT_PA, pilot_pa);
    if (e == ESP_OK) {
        uint8_t en1 = 0;
        rd8(REG_INT_ENABLE_1, &en1);
        if (threshold) en1 |= (uint8_t)MAX30102_INT_PROXIMITY;
        else           en1 &= (uint8_t)~MAX30102_INT_PROXIMITY;
        e = wr(REG_INT_ENABLE_1, en1);
    }
    return e;
}

esp_err_t max30102_read_temperature(float *celsius)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    if (celsius) *celsius = 0.0f;
    esp_err_t e = wr(REG_DIE_TEMP_CONFIG, TEMP_EN);   // trigger one conversion
    if (e != ESP_OK) return e;
    for (int i = 0; i < 10; i++) {                    // ~30 ms conversion
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t st2 = 0;
        if (rd8(REG_INT_STATUS_2, &st2) == ESP_OK && (st2 & ST1_DIE_TEMP_RDY)) break;
    }
    uint8_t ti = 0, tf = 0;
    if (rd8(REG_DIE_TEMP_INT, &ti) != ESP_OK) return ESP_FAIL;
    if (rd8(REG_DIE_TEMP_FRAC, &tf) != ESP_OK) return ESP_FAIL;
    if (celsius) *celsius = (float)(int8_t)ti + (float)(tf & 0x0F) * 0.0625f;
    return ESP_OK;
}

esp_err_t max30102_config_interrupts(uint32_t flags)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    const uint8_t en1 = (uint8_t)(flags & (MAX30102_INT_A_FULL | MAX30102_INT_DATA_RDY |
                                           MAX30102_INT_ALC_OVF | MAX30102_INT_PROXIMITY));
    const uint8_t en2 = (flags & MAX30102_INT_DIE_TEMP_RDY) ? ST1_DIE_TEMP_RDY : 0;
    esp_err_t e = wr(REG_INT_ENABLE_1, en1);
    if (e == ESP_OK) e = wr(REG_INT_ENABLE_2, en2);
    return e;
}

esp_err_t max30102_read_interrupt_status(uint32_t *flags)
{
    if (flags) *flags = 0;
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    uint8_t st1 = 0, st2 = 0;
    if (rd8(REG_INT_STATUS_1, &st1) != ESP_OK) return ESP_FAIL;   // clear-on-read
    if (rd8(REG_INT_STATUS_2, &st2) != ESP_OK) return ESP_FAIL;
    if (flags) *flags = st1 | ((st2 & ST1_DIE_TEMP_RDY) ? MAX30102_INT_DIE_TEMP_RDY : 0);
    return ESP_OK;
}

esp_err_t max30102_set_isr_callback(max30102_isr_cb_t cb, void *arg)
{
    S.cb = cb;
    S.cb_arg = arg;
    return ESP_OK;
}

esp_err_t max30102_preset_hr_lowpower(void)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    // Single IR slot (works on both parts), on-chip averaging, low rate/current.
    const max30102_slot_t slots[4] = { MAX30102_SLOT_IR, MAX30102_SLOT_NONE,
                                       MAX30102_SLOT_NONE, MAX30102_SLOT_NONE };
    max30102_set_sample_averaging(MAX30102_AVG_4);
    max30102_set_led_current(0, 0x18, 0);      // ~5 mA IR only
    max30102_set_pulse_width(MAX30102_PW_411US_18BIT);
    esp_err_t e = max30102_set_mode(MAX30102_MODE_MULTI, slots);
    max30102_set_sample_rate(100);
    return e;
}

esp_err_t max30102_preset_spo2(void)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    max30102_set_sample_averaging(MAX30102_AVG_1);
    max30102_set_led_current(0x24, 0x24, 0);
    max30102_set_pulse_width(MAX30102_PW_411US_18BIT);
    esp_err_t e = max30102_set_mode(MAX30102_MODE_SPO2, NULL);
    max30102_set_sample_rate(100);
    return e;
}

esp_err_t max30102_preset_hrv(void)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    max30102_set_sample_averaging(MAX30102_AVG_1);   // NO averaging — beat timing
    max30102_set_led_current(0x24, 0x24, 0);
    max30102_set_pulse_width(MAX30102_PW_AUTO);
    esp_err_t e = max30102_set_mode(MAX30102_MODE_SPO2, NULL);
    max30102_set_sample_rate(400);
    return e;
}

esp_err_t max30102_read_reg(uint8_t reg, uint8_t *val)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    return rd8(reg, val);
}

esp_err_t max30102_write_reg(uint8_t reg, uint8_t val)
{
    if (!S.dev) return ESP_ERR_INVALID_STATE;
    return wr(reg, val);
}

esp_err_t max30102_get_info(max30102_variant_t *variant, uint8_t *channels)
{
    if (variant)  *variant  = S.variant;
    if (channels) *channels = S.channels;
    return ESP_OK;
}
