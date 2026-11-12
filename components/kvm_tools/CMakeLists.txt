idf_component_register(
    SRCS "kvm_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES
        espressif__cjson
        kvm_config
        kvm_hid
)
