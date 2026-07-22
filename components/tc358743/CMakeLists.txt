idf_component_register(
    SRCS "tc358743.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c
    PRIV_REQUIRES
        kvm_board
)
