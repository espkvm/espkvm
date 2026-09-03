# Which HDMI-to-CSI bridge is attached, and how to drive it. Drivers register
# themselves; this component knows none of them by name.
idf_component_register(
    SRCS "kvm_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
