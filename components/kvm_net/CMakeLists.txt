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
        kvm_storage
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

if(CONFIG_KVM_WIFI)
    # Block esp-hosted's constructor from eagerly bringing the C6 SDIO transport up
    # at boot (it claims the P4's single SD host controller, which the microSD also
    # needs, and cannot be safely torn down). wifi.c provides __wrap_esp_hosted_init
    # and brings the co-processor up itself only in a WiFi mode. See wifi.c.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_hosted_init")
endif()
