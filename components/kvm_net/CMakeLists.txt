idf_component_register(
    SRCS "ethernet.c"
         "kvm_ts.c"
         "kvm_wg.c"
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
)
