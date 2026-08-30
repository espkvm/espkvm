idf_component_register(
    SRCS
        "kvm_display.c"
        "kvm_panels.c"
        "drivers/mono_oled.c"
        "drivers/ssd1306/ssd1306.c"
        "drivers/ssd1315/ssd1315.c"
        "drivers/sh1106/sh1106.c"
        "drivers/gc9a01/gc9a01.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "drivers"
    PRIV_REQUIRES
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_mm
        esp_lcd
        video_pipeline
        kvm_net
        kvm_config
        esp_netif
        esp_timer
        esp_app_format
)
