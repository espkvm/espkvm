# Board pin map, hardware constants, and where the pins physically are.
idf_component_register(SRCS "board_header.c" "board_reserved.c" "board_gpio.c" INCLUDE_DIRS "include"
                       REQUIRES esp_common PRIV_REQUIRES esp_driver_gpio)
