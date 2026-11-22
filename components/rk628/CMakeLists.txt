# SPDX-FileCopyrightText: 2026 Evgenij Spitsyn and ESP-KVM contributors
# SPDX-License-Identifier: Apache-2.0
#
# The RK628F/H bridge driver (Waveshare HDMI to CSI Adapter (B), Radxa 4K HDMI
# IN, WarpKVM boards). Not run on hardware yet: see README.md.
idf_component_register(
    SRCS "rk628.c"
    REQUIRES
        esp_driver_i2c
        kvm_bridge
        esp_timer
)
# Without this the linker drops an object nobody references (kvm_bridge.h).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u kvm_bridge_reg_rk628")
