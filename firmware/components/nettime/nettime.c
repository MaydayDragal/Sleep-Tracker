#include "nettime.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

// Transient WiFi + SNTP time fetch. See nettime.h. WiFi is torn down before this
// returns; it is never left running. Deliberately does NO I2C (returns the time
// as a value; the caller writes the RTC later) so WiFi never overlaps the shared
// I2C bus — concurrent WiFi + I2C wedged the bus and froze the display/touch.

// Network credentials live in a gitignored header next to this file (copy
// wifi_config.example.h -> wifi_config.h). Absent => the defaults below make
// nettime_sync() a no-op, so the repo still builds without secrets checked in.
// NOTE: after creating wifi_config.h, force a rebuild of this file (it was first
// compiled without it, so the build's dep tracking won't notice) — `pio run -t
// clean` or delete .pio/build, else NTP silently stays skipped.
#if defined(__has_include)
#  if __has_include("wifi_config.h")
#    include "wifi_config.h"
#  endif
#endif
#ifndef SLEEPTRK_WIFI_SSID
#  define SLEEPTRK_WIFI_SSID ""
#endif
#ifndef SLEEPTRK_WIFI_PASS
#  define SLEEPTRK_WIFI_PASS ""
#endif
#ifndef SLEEPTRK_UTC_OFFSET_MIN
#  define SLEEPTRK_UTC_OFFSET_MIN 0     // local wall clock = UTC + this (minutes)
#endif
#ifndef SLEEPTRK_NTP_SERVER
#  define SLEEPTRK_NTP_SERVER "pool.ntp.org"
#endif

static const char *TAG = "nettime";

#define GOT_IP_BIT      BIT0
#define FAIL_BIT        BIT1
#define MAX_RETRY       8
#define CONNECT_MS      15000
#define SNTP_MS         10000

static EventGroupHandle_t s_evt;
static int                s_retry;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry++ < MAX_RETRY) {
            esp_wifi_connect();
        } else if (s_evt) {
            xEventGroupSetBits(s_evt, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        if (s_evt) xEventGroupSetBits(s_evt, GOT_IP_BIT);
    }
}

static esp_err_t ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

// Bring WiFi STA up and block until we have an IP (or time out). Returns true on
// connect; *sta/*hw/*hi hold handles to tear down afterward either way.
static bool wifi_up(esp_netif_t **sta, esp_event_handler_instance_t *hw,
                    esp_event_handler_instance_t *hi)
{
    if (ensure_nvs() != ESP_OK) {
        ESP_LOGW(TAG, "nvs init failed");
        return false;
    }
    esp_netif_init();                                  // idempotent
    esp_err_t e = esp_event_loop_create_default();     // may already exist
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(e));
        return false;
    }
    *sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    s_evt = xEventGroupCreate();
    s_retry = 0;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, hw);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, hi);

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, SLEEPTRK_WIFI_SSID, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, SLEEPTRK_WIFI_PASS, sizeof wc.sta.password - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (esp_wifi_start() != ESP_OK) {
        return false;
    }

    ESP_LOGI(TAG, "connecting to \"%s\"...", SLEEPTRK_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_evt, GOT_IP_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_MS));
    return (bits & GOT_IP_BIT) != 0;
}

static void wifi_down(esp_netif_t *sta, esp_event_handler_instance_t hw,
                      esp_event_handler_instance_t hi)
{
    esp_wifi_stop();
    if (hi) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, hi);
    if (hw) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, hw);
    esp_wifi_deinit();
    if (sta) esp_netif_destroy_default_wifi(sta);
    if (s_evt) { vEventGroupDelete(s_evt); s_evt = NULL; }
    ESP_LOGI(TAG, "WiFi off");
}

time_t nettime_fetch(void)
{
    if (strlen(SLEEPTRK_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no WiFi SSID set (copy components/nettime/wifi_config.example.h "
                      "-> wifi_config.h) — skipping NTP");
        return 0;
    }

    esp_netif_t *sta = NULL;
    esp_event_handler_instance_t hw = NULL, hi = NULL;
    time_t local = 0;   // local wall clock as a time_t (UTC + offset), 0 => failed

    if (wifi_up(&sta, &hw, &hi)) {
        ESP_LOGI(TAG, "connected — SNTP from %s", SLEEPTRK_NTP_SERVER);
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(SLEEPTRK_NTP_SERVER);
        if (esp_netif_sntp_init(&sc) == ESP_OK) {
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_MS)) == ESP_OK) {
                local = time(NULL) + (time_t)(SLEEPTRK_UTC_OFFSET_MIN) * 60;
                struct tm tmv;
                gmtime_r(&local, &tmv);
                ESP_LOGI(TAG, "NTP time: %04d-%02d-%02d %02d:%02d:%02d (UTC%+dmin)",
                         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                         tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(SLEEPTRK_UTC_OFFSET_MIN));
            } else {
                ESP_LOGW(TAG, "SNTP sync timed out");
            }
            esp_netif_sntp_deinit();
        }
    } else {
        ESP_LOGW(TAG, "WiFi connect failed/timed out");
    }

    wifi_down(sta, hw, hi);
    return local;
}
