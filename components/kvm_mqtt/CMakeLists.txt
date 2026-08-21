idf_component_register(
    SRCS "kvm_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES kvm_config
    PRIV_REQUIRES mqtt mbedtls esp_timer esp_hw_support esp_app_format esp_system
                  kvm_atx kvm_net video_pipeline kvm_hid kvm_screentext
)
