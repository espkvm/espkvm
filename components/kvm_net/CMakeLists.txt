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
         "ipv6.c"
         "kvm_ts.c"
         "kvm_wg.c"
         "wifi.c"
    INCLUDE_DIRS "include"
    # esp_netif is public rather than private: kvm_ipv6.h names esp_netif_t, so
    # anything including it needs esp_netif's headers too.
    REQUIRES esp_netif
    PRIV_REQUIRES
        kvm_config
        kvm_storage
        esp_eth
        esp_event
        espressif__mdns
        microlink
        wireguard_lwip
        mbedtls
        lwip
        ${wifi_reqs}
)

# esp-hosted's eager boot-time init (which would claim the P4's single SD host
# controller, shared with the microSD) is disabled per board via
# CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n; wifi.c brings the co-processor
# up itself only in a WiFi mode. No linker wrap is needed. See wifi.c.
