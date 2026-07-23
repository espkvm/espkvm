idf_component_register(
    SRCS "usb_hid.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        kvm_board
        kvm_config
        kvm_storage
        esp_driver_gpio
        esp_tinyusb
)
