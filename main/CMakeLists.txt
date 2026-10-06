idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        video_pipeline
        kvm_net
        kvm_web
        kvm_rtc
        kvm_hid
        kvm_runbook
        kvm_sched
        kvm_notify
        kvm_cec
        kvm_config
        kvm_log
        kvm_atx
        kvm_serial
        kvm_netlog
        kvm_mqtt
        nvs_flash
        kvm_storage
        kvm_record
        kvm_display
        app_update
        esp_timer
        espcoredump
)
