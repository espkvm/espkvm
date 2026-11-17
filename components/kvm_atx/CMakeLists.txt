idf_component_register(
    SRCS "kvm_atx.c" "kvmsw_tesmart.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_timer
        lwip
        kvm_config
)
