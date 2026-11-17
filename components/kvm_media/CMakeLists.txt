idf_component_register(
    SRCS "kvm_media.c"
        "media_cache.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_hw_support
        heap
)
