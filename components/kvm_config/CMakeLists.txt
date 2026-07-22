idf_component_register(
    SRCS
        "kvm_caps.c"
        "kvm_thermal.c"
        "kvm_settings.c"
        "kvm_settings_table.c"
    INCLUDE_DIRS "include"
    REQUIRES
        espressif__cjson
    PRIV_REQUIRES
        esp_driver_tsens
        nvs_flash
)
