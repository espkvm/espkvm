idf_component_register(
    SRCS "sched_cron.c" "kvm_sched.c" "button.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        nvs_flash
        espressif__cjson
        esp_timer
        esp_netif
        lwip
        kvm_config
        kvm_atx
        kvm_net
        kvm_runbook
        kvm_record
        esp_driver_gpio
)
