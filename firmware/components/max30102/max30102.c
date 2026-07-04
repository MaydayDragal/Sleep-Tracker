#include "max30102.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// MAX3010x PPG driver (external, I2C @ 0x57). Register map per the Analog Devices
// MAX30102 / MAX30101 datasheets (they are register-compatible; both PART_ID
// 0x15). RED|IR uses SpO2 mode (0x03); adding GREEN uses Multi-LED mode (0x07)
// with one time-slot per channel. INT line unused on this board — we poll the
// FIFO write/read pointers (PLAN.md §2.2 notes INT as a power win TODO).

#define MAX30102_ADDR       0x57
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
#define REG_LED1_PA         0x0C   // red
#define REG_LED2_PA         0x0D   // ir
#define REG_LED3_PA         0x0E   // green (MAX30101)
#define REG_MULTI_LED_1     0x11   // SLOT2<<4 | SLOT1
#define REG_MULTI_LED_2     0x12   // SLOT4<<4 | SLOT3
#define REG_DIE_TINT        0x1F
#define REG_DIE_TFRAC       0x20
#define REG_DIE_TEMP_CFG    0x21
#define REG_PART_ID         0xFF
#define MAX3010X_PART_ID    0x15

#define MODE_HR             0x02   // red only
#define MODE_SPO2           0x03   // red + ir
#define MODE_MULTI_LED      0x07   // slot-programmable (needed for green)

// Multi-LED SLOTx codes (datasheet Table 9): 1=LED1(red), 2=LED2(ir), 3=LED3(green).
#define SLOT_RED            0x1
#define SLOT_IR             0x2
#define SLOT_GREEN          0x3

#define FIFO_DEPTH          32
#define BYTES_PER_CHAN      3
#define MAX_CHANS           3
#define MAX_BYTES_PER_SAMP  (MAX_CHANS * BYTES_PER_CHAN)   // 9 (green + red + ir)

static const char *TAG = "max3010x";
static i2c_master_dev_handle_t s_dev;

// Active channels, in FIFO slot order, cached from the last (re)configure so the
// FIFO unpacker knows the sample width and which field each triplet maps to.
static max3010x_channel_t s_slot[MAX_CHANS];
static int      s_nchan;                 // active channel count (=> bytes/sample)
static uint8_t  s_mode = MODE_SPO2;      // current mode (restored after shutdown)
static uint16_t s_rate_hz = 100;         // configured per-channel rate
static uint8_t  s_smp_ave = 1;           // 1/2/4/8/16/32
static uint8_t  s_ovf;                   // OVF_COUNTER from the last read_fifo()

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 200);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof b, 100);
}

// SMP_AVE[2:0] code: 0=1,1=2,2=4,3=8,4=16,5=32 (datasheet Table 3).
static uint8_t smp_ave_code(uint8_t ave)
{
    switch (ave) {
        case 2:  return 1;
        case 4:  return 2;
        case 8:  return 3;
        case 16: return 4;
        case 32: return 5;
        default: return 0;   // 1 or unknown => no averaging
    }
}

// SPO2_CONFIG (0x0A): [6:5]=ADC range (01 => 4096 nA), [4:2]=sample-rate code,
// [1:0]=LED pulse-width/resolution. Higher sample rates need a shorter pulse so
// all active LED slots fit within the sample period (datasheet Tables 11/15).
static uint8_t spo2_config(uint16_t sr_hz)
{
    uint8_t sr, pw;      // sample-rate code / pulse-width code
    switch (sr_hz) {
        case 50:  sr = 0; pw = 3; break;   // 411 us, 18-bit
        case 100: sr = 1; pw = 3; break;   // 411 us, 18-bit
        case 200: sr = 2; pw = 2; break;   // 215 us, 17-bit
        case 400: sr = 3; pw = 1; break;   // 118 us, 16-bit
        case 800: sr = 4; pw = 0; break;   //  69 us, 15-bit (shortest pulse fits 3 LEDs)
        default:  sr = 1; pw = 3; break;   // fall back to 100 Hz
    }
    return (uint8_t)((1u << 5) | (sr << 2) | pw);
}

// Build the slot table from the requested channel mask (RED, then IR, then GREEN
// — a fixed, stable order) and program the mode + multi-LED slot registers.
static void configure_channels(uint8_t channels)
{
    if (channels == 0) {
        channels = MAX3010X_CH_RED | MAX3010X_CH_IR;   // legacy default
    }

    s_nchan = 0;
    if (channels & MAX3010X_CH_RED)   s_slot[s_nchan++] = MAX3010X_CH_RED;
    if (channels & MAX3010X_CH_IR)    s_slot[s_nchan++] = MAX3010X_CH_IR;
    if (channels & MAX3010X_CH_GREEN) s_slot[s_nchan++] = MAX3010X_CH_GREEN;

    const bool green = (channels & MAX3010X_CH_GREEN) != 0;
    if (!green && channels == (MAX3010X_CH_RED | MAX3010X_CH_IR)) {
        // Exactly RED+IR: SpO2 mode is the register-preset equivalent of a
        // 2-slot multi-LED config — keep it, so this path is bit-identical to
        // the legacy driver on the current board.
        s_mode = MODE_SPO2;
        wr(REG_MODE_CONFIG, s_mode);
    } else {
        // Any set involving green (or a custom set) => Multi-LED with explicit slots.
        uint8_t code[MAX_CHANS] = {0};
        for (int i = 0; i < s_nchan; i++) {
            code[i] = (s_slot[i] == MAX3010X_CH_RED)   ? SLOT_RED
                    : (s_slot[i] == MAX3010X_CH_IR)    ? SLOT_IR
                    : /* GREEN */                        SLOT_GREEN;
        }
        s_mode = MODE_MULTI_LED;
        wr(REG_MODE_CONFIG, s_mode);
        wr(REG_MULTI_LED_1, (uint8_t)((code[1] << 4) | code[0]));  // SLOT2|SLOT1
        wr(REG_MULTI_LED_2, code[2]);                              // SLOT4=0 | SLOT3
                                                                   // (we cap at 3 channels)
    }
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
    if (rd(REG_PART_ID, &part, 1) != ESP_OK || part != MAX3010X_PART_ID) {
        ESP_LOGE(TAG, "PART_ID=0x%02x (expected 0x15) — sensor not on the bus at 0x57", part);
        return ESP_ERR_NOT_FOUND;
    }

    wr(REG_MODE_CONFIG, 0x40);   // soft reset
    vTaskDelay(pdMS_TO_TICKS(10));

    // Clear FIFO pointers (recommended on entering a conversion mode).
    wr(REG_FIFO_WR_PTR, 0x00);
    wr(REG_OVF_COUNTER, 0x00);
    wr(REG_FIFO_RD_PTR, 0x00);

    // FIFO config: SMP_AVE[7:5] | ROLLOVER_EN(bit4) | A_FULL[3:0].
    s_smp_ave = (cfg && cfg->smp_ave) ? cfg->smp_ave : 1;
    wr(REG_FIFO_CONFIG, (uint8_t)((smp_ave_code(s_smp_ave) << 5) | 0x10));

    const uint8_t channels = (cfg ? cfg->channels : 0);
    configure_channels(channels);   // sets mode + slots + s_slot/s_nchan

    s_rate_hz = (cfg && cfg->sample_rate_hz) ? cfg->sample_rate_hz : 100;
    wr(REG_SPO2_CONFIG, spo2_config(s_rate_hz));   // ADC 4096 nA + rate + pulse width

    const uint8_t red = (cfg && cfg->led_current_red) ? cfg->led_current_red : 0x24;
    const uint8_t ir  = (cfg && cfg->led_current_ir)  ? cfg->led_current_ir  : 0x24;
    wr(REG_LED1_PA, red);
    wr(REG_LED2_PA, ir);
    bool green = false;
    for (int i = 0; i < s_nchan; i++) {
        if (s_slot[i] == MAX3010X_CH_GREEN) green = true;
    }
    if (green) {
        const uint8_t grn = (cfg && cfg->led_current_green) ? cfg->led_current_green : 0x24;
        wr(REG_LED3_PA, grn);
    }

    ESP_LOGI(TAG, "up (PART 0x15, mode=0x%02x, %d ch, %uHz/ave%u => %uHz out, LED r/ir/g=%u/%u/%u)",
             s_mode, s_nchan, (unsigned)s_rate_hz, (unsigned)s_smp_ave,
             (unsigned)max30102_output_rate_hz(), red, ir,
             (cfg ? cfg->led_current_green : 0));
    return ESP_OK;
}

esp_err_t max30102_read_fifo(max30102_sample_t *out, size_t max, size_t *out_count)
{
    if (out_count) *out_count = 0;
    s_ovf = 0;
    if (s_dev == NULL || out == NULL || s_nchan == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // One burst read of WR_PTR, OVF_COUNTER, RD_PTR (contiguous 0x04..0x06).
    uint8_t ptrs[3] = {0};
    if (rd(REG_FIFO_WR_PTR, ptrs, 3) != ESP_OK) {
        return ESP_FAIL;
    }
    const uint8_t wr_ptr = ptrs[0];
    s_ovf                 = ptrs[1];   // samples dropped since last drain (getter exposes it)
    const uint8_t rd_ptr = ptrs[2];

    int avail = ((int)wr_ptr - (int)rd_ptr) & (FIFO_DEPTH - 1);
    // wr==rd is ambiguous on the 5-bit pointers: empty OR exactly full (32). With
    // FIFO rollover enabled, a full FIFO that has overwritten a sample bumps
    // OVF_COUNTER, so a nonzero counter disambiguates "full" from "empty" — without
    // this a whole 32-sample window (~80 ms @ 400 Hz) is misread as empty and
    // silently dropped when a poll slips past the fill time.
    if (avail == 0 && s_ovf > 0) {
        avail = FIFO_DEPTH;
        ESP_LOGW(TAG, "FIFO overflow (>=%u samples lost) — reading full FIFO", (unsigned)s_ovf);
        wr(REG_OVF_COUNTER, 0x00);   // accounted for; avoid a later stale-counter misread
    }
    if (avail <= 0) {
        return ESP_OK;
    }
    if ((size_t)avail > max) {
        avail = (int)max;
    }

    const int bps = s_nchan * BYTES_PER_CHAN;
    uint8_t buf[FIFO_DEPTH * MAX_BYTES_PER_SAMP];
    uint8_t reg = REG_FIFO_DATA;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf,
                                                (size_t)avail * bps, 300);
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < avail; i++) {
        const uint8_t *p = &buf[i * bps];
        out[i].red = out[i].ir = out[i].green = 0;
        for (int c = 0; c < s_nchan; c++, p += BYTES_PER_CHAN) {
            const uint32_t v = (((uint32_t)p[0] << 16) |
                                ((uint32_t)p[1] << 8)  | p[2]) & 0x3FFFF;
            switch (s_slot[c]) {
                case MAX3010X_CH_RED:   out[i].red   = v; break;
                case MAX3010X_CH_IR:    out[i].ir    = v; break;
                case MAX3010X_CH_GREEN: out[i].green = v; break;
            }
        }
    }
    if (out_count) *out_count = (size_t)avail;
    return ESP_OK;
}

esp_err_t max30102_set_sample_rate(uint16_t hz)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_rate_hz = hz;
    esp_err_t err = wr(REG_SPO2_CONFIG, spo2_config(hz));
    // Flush the FIFO so the consumer doesn't mix samples from the old rate.
    wr(REG_FIFO_WR_PTR, 0x00);
    wr(REG_OVF_COUNTER, 0x00);
    wr(REG_FIFO_RD_PTR, 0x00);
    ESP_LOGI(TAG, "sample rate -> %u Hz (%u Hz out)", (unsigned)hz,
             (unsigned)max30102_output_rate_hz());
    return err;
}

esp_err_t max30102_set_led_current(uint8_t red, uint8_t ir, uint8_t green)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = wr(REG_LED1_PA, red);
    if (err == ESP_OK) err = wr(REG_LED2_PA, ir);
    if (err == ESP_OK && green) err = wr(REG_LED3_PA, green);   // MAX30101 only
    return err;
}

esp_err_t max30102_set_smp_ave(uint8_t ave)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_smp_ave = ave ? ave : 1;
    // SMP_AVE[7:5] | ROLLOVER_EN(bit4) | A_FULL[3:0]=0 — matches init.
    esp_err_t err = wr(REG_FIFO_CONFIG, (uint8_t)((smp_ave_code(s_smp_ave) << 5) | 0x10));
    // Flush so the consumer doesn't mix pre-/post-average cadence.
    wr(REG_FIFO_WR_PTR, 0x00);
    wr(REG_OVF_COUNTER, 0x00);
    wr(REG_FIFO_RD_PTR, 0x00);
    return err;
}

uint16_t max30102_output_rate_hz(void)
{
    const uint8_t ave = s_smp_ave ? s_smp_ave : 1;
    return (uint16_t)(s_rate_hz / ave);
}

uint8_t max30102_last_overflow(void)
{
    return s_ovf;
}

esp_err_t max30102_read_die_temp(float *celsius)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (wr(REG_DIE_TEMP_CFG, 0x01) != ESP_OK) {   // TEMP_EN: start one conversion
        return ESP_FAIL;
    }
    // Self-clearing bit; the conversion takes ~29 ms. Poll with a margin.
    uint8_t cfg = 0x01;
    for (int i = 0; i < 10 && (cfg & 0x01); i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (rd(REG_DIE_TEMP_CFG, &cfg, 1) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (cfg & 0x01) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t tint = 0, tfrac = 0;
    if (rd(REG_DIE_TINT, &tint, 1) != ESP_OK || rd(REG_DIE_TFRAC, &tfrac, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    if (celsius) {
        // TINT is 2's-complement °C; TFRAC adds 0.0625 °C/LSB (always positive).
        *celsius = (float)(int8_t)tint + (float)(tfrac & 0x0F) * 0.0625f;
    }
    return ESP_OK;
}

esp_err_t max30102_shutdown(bool enable)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // SHDN bit set, or restore the configured conversion mode (SpO2 / Multi-LED).
    return wr(REG_MODE_CONFIG, enable ? 0x80 : s_mode);
}
