idf_component_register(
    SRCS "kvm_atx.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_driver_gpio
        kvm_config
)
