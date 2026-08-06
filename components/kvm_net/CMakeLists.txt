# WiFi (via an ESP32-C6 co-processor) is compiled only where a board carries one.
# wifi.c has an #else stub so it always builds; the esp_wifi_remote / esp_wifi
# requirement is added only when CONFIG_KVM_WIFI, so the Waveshare build neither
# links nor needs those components.
set(wifi_reqs "")
if(CONFIG_KVM_WIFI)
    list(APPEND wifi_reqs espressif__esp_wifi_remote espressif__esp_hosted esp_wifi)
endif()

idf_component_register(
    SRCS "ethernet.c"
         "kvm_ts.c"
         "kvm_wg.c"
         "wifi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        kvm_config
        esp_eth
        esp_netif
        esp_event
        espressif__mdns
        microlink
        wireguard_lwip
        mbedtls
        lwip
        ${wifi_reqs}
)
