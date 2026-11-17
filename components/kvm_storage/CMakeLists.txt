idf_component_register(
    SRCS "kvm_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES
        kvm_media
    PRIV_REQUIRES
        kvm_board
        kvm_config
        esp_driver_sdmmc
        fatfs
        esp_driver_gpio
        esp_timer
)
