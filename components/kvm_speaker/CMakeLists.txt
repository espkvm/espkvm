idf_component_register(
    SRCS "kvm_speaker.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
        video_pipeline
        kvm_config
)
