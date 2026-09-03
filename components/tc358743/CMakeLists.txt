idf_component_register(
    SRCS "tc358743.c" "tc358743_bridge.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c
        kvm_bridge
    PRIV_REQUIRES
        kvm_board
)

# Nothing calls into this component by name any more - the capture path asks
# kvm_bridge which chip answered - so without this the linker would drop the
# object and the driver would register itself nowhere.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u kvm_bridge_reg_tc358743")
