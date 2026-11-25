idf_component_register(
    SRCS "usb_video.c" "usb_video_h264.c" "usb_video_info.c" "usb_video_transport.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_tinyusb esp_hw_support esp_timer heap nvs_flash mbedtls kvm_config espressif__cjson)
