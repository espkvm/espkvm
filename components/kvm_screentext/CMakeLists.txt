idf_component_register(
    SRCS "screentext.c" "screentext_store.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_timer
)
