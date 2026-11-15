set(srcs)
if(CONFIG_KVM_ENABLE_WEBRTC)
    list(APPEND srcs "kvm_webrtc.c" "wr_cert.c" "wr_crypto_psa.c" "stun.c" "srtp.c"
         "rtp_h264.c" "sdp.c" "sps_patch.c" "sctp.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        mbedtls
        lwip
        esp_timer
        kvm_config
        video_pipeline
        kvm_audio
        kvm_timing
)
