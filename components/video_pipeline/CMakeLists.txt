idf_component_register(
    SRCS
        "capture.c"
        "capture_pixfmt.c"
        "capture_hw.c"
        "capture_loop.c"
        "capture_mjpeg.c"
        "capture_h264.c"
        "capture_screentext.c"
        "capture_flat.c"
        "capture_flat_scan.c"
        "h264_probe.c"
        "video_frame.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        kvm_bridge
        kvm_config
    PRIV_REQUIRES
        # The capture path never names a bridge driver - it asks kvm_bridge which
        # one answered - but a driver still has to be in the image to register
        # itself, so the ones this firmware ships are required here.
        tc358743
        kvm_board
        kvm_screentext
        espressif__esp_h264
        esp_driver_ppa
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_cam
        esp_driver_isp
        esp_driver_jpeg
        esp_hw_support
        esp_psram
        esp_mm
        esp_timer
        nvs_flash
)
