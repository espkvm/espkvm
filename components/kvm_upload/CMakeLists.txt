idf_component_register(
    SRCS "kvm_upload.c" "upload_dav.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        esp_http_client
        esp-tls
        esp_timer
        mbedtls
        kvm_config
        kvm_storage
        video_pipeline
)
