idf_component_register(
    SRCS "ethernet.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        kvm_config
        esp_eth
        esp_netif
        esp_event
        espressif__mdns
)
