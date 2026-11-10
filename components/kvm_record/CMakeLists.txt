idf_component_register(
    SRCS "kvm_record.c" "dashcam.c" "frame_ring.c" "ts_mux.c" "keylog.c" "keymap_layouts.c" "ts_to_mp4.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        kvm_regions
        video_pipeline
        kvm_hid
        kvm_config
        kvm_screentext
        kvm_atx
        kvm_notify
        kvm_storage
        kvm_upload
        kvm_audio
        esp_ringbuf
        esp_timer
        esp_psram
        fatfs
)
