# WiFi (via an ESP32-C6 co-processor) is compiled only where a board carries one.
# wifi.c has an #else stub so it always builds; the esp_wifi_remote / esp_wifi
# requirement is added only when CONFIG_KVM_WIFI, so the Waveshare build neither
# links nor needs those components.
set(wifi_reqs "")
if(CONFIG_KVM_WIFI)
    list(APPEND wifi_reqs espressif__esp_wifi_remote espressif__esp_hosted esp_wifi)
endif()

# The co-processor image the console can install (tools/build-coproc.sh). One
# per chip; the board overlay says which chip it carries.
set(coproc_embed "")
set(coproc_version "")
if(CONFIG_KVM_WIFI_COPROC_UPDATE)
    if(CONFIG_SLAVE_IDF_TARGET_ESP32C5)
        set(coproc_chip esp32c5)
    else()
        set(coproc_chip esp32c6)
    endif()
    set(coproc_bin "${CMAKE_CURRENT_LIST_DIR}/coproc/${coproc_chip}.bin")
    if(EXISTS "${coproc_bin}")
        set(coproc_embed "coproc/${coproc_chip}.bin")
        file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/coproc/${coproc_chip}.version" coproc_version LIMIT_COUNT 1)
        # The host side: warn when the image was built from another esp_hosted.
        set(hosted_yml "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_hosted/idf_component.yml")
        if(EXISTS "${hosted_yml}")
            file(STRINGS "${hosted_yml}" hosted_line REGEX "^version:" LIMIT_COUNT 1)
            string(REGEX REPLACE "^version:[ '\"]*([0-9.]+).*" "\\1" hosted_version "${hosted_line}")
            if(NOT hosted_version STREQUAL coproc_version)
                message(WARNING "coproc/${coproc_chip}.bin is esp_hosted ${coproc_version}, the host is "
                                "${hosted_version}: run tools/build-coproc.sh ${coproc_chip}")
            endif()
        endif()
    else()
        message(WARNING "No ${coproc_bin}: the Wi-Fi chip update is left out")
    endif()
endif()

idf_component_register(
    SRCS "ethernet.c"
         "ipv6.c"
         "kvm_ts.c"
         "kvm_wg.c"
         "wifi.c"
         "coproc.c"
         "ecc_wait.c"
    INCLUDE_DIRS "include"
    # esp_netif is public rather than private: kvm_ipv6.h names esp_netif_t, so
    # anything including it needs esp_netif's headers too.
    REQUIRES esp_netif
    PRIV_REQUIRES
        kvm_board
        kvm_config
        kvm_notify
        kvm_storage
        esp_eth
        esp_event
        esp_app_format
        espressif__mdns
        microlink
        wireguard_lwip
        mbedtls
        esp_security
        esp_hal_security
        esp_timer
        lwip
        ${wifi_reqs}
    EMBED_FILES ${coproc_embed}
)

# esp-hosted's eager boot-time init (which would claim the P4's single SD host
# controller, shared with the microSD) is disabled per board via
# CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n; wifi.c brings the co-processor
# up itself only in a WiFi mode. No linker wrap is needed. See wifi.c.

# ethernet.c resets the EMAC on the way into every restart (esp-idf #19085).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart_noos")

# ecc_wait.c puts a time limit on IDF's wait for the ECC accelerator.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ecc_point_multiply"
                                                 "-Wl,--wrap=esp_ecc_point_verify"
                                                 "-u __wrap_esp_ecc_point_multiply")

# wifi.c moves esp-hosted's SDIO packet buffers out of internal RAM, and makes
# its long waits for the co-processor sleep instead of spin.
if(CONFIG_KVM_WIFI)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=eh_host_port_dma_alloc"
                                                     "-Wl,--wrap=eh_host_port_task_delay_us")
endif()

if(coproc_embed)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        KVM_COPROC_VERSION="${coproc_version}" KVM_COPROC_CHIP="${coproc_chip}")
endif()
