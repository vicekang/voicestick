idf_component_register(
    SRCS "voice_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update audio_transport bt nvs_flash
)
