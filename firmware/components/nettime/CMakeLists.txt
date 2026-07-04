idf_component_register(
    SRCS "nettime.c"
    INCLUDE_DIRS "include"
    REQUIRES rtc esp_wifi esp_netif esp_event nvs_flash
)
# esp_netif_sntp (esp_netif_sntp.h) + the SNTP client come with esp_netif/lwip.
# WiFi credentials: copy wifi_config.example.h -> wifi_config.h (gitignored).
