idf_component_register(
    SRCS "ui_status.c" "ui_status_icons.c"
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_driver_ledc esp_lcd esp_timer stick_s3_board audio_feedback lvgl
)
