idf_component_register(
    SRCS "usb_hid.c"
        "xinput.c"
        "mmc.c"
        "jiggler.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        kvm_board
        kvm_config
        kvm_media
        esp_driver_gpio
        esp_tinyusb
        esp_timer
        kvm_timing
)
