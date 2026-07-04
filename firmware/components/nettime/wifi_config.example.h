#pragma once
// Copy this file to wifi_config.h (same directory — gitignored) and fill in your
// network. nettime brings WiFi up briefly at boot, sets the RTC from NTP, then
// turns WiFi off. With no wifi_config.h present, NTP is simply skipped.
//
// The ESP32-S3 radio is 2.4 GHz ONLY — a 5 GHz-only SSID will not connect.

#define SLEEPTRK_WIFI_SSID       "your-2.4GHz-ssid"
#define SLEEPTRK_WIFI_PASS       "your-wifi-password"

// Local wall clock = UTC + this many minutes (the RTC is a naive clock, matching
// the log's tz_offset_min=0 convention). Examples: 0 = UTC, -300 = US Eastern
// (EST), -240 = US Eastern (EDT), 60 = Central Europe (CET), 330 = India (IST).
#define SLEEPTRK_UTC_OFFSET_MIN  0

// Optional — defaults to pool.ntp.org.
// #define SLEEPTRK_NTP_SERVER   "pool.ntp.org"
