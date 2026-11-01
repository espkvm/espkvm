idf_component_register(
    SRCS "kvm_vnc.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        lwip
        kvm_config
        kvm_net
        kvm_web
        kvm_hid
        video_pipeline
        esp_timer
        esp_system
        esp_hw_support
)
