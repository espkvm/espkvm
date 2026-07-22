idf_component_register(
    SRCS "usb_hid.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        kvm_board
        kvm_config
        esp_driver_gpio
        espressif__esp_tinyusb
)
