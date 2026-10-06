idf_component_register(
    SRCS "kvm_netlog.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        lwip
        kvm_config
        kvm_notify
        esp_timer
)
