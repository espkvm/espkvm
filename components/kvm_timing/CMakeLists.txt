idf_component_register(
    SRCS "kvm_timing.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_timer
)
