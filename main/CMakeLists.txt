idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        video_pipeline
        kvm_net
        kvm_web
        kvm_hid
        kvm_config
        nvs_flash
        kvm_storage
        app_update
)
