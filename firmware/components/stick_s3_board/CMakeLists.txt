idf_component_register(
    SRCS "stick_s3_board.c" "stick_s3_imu.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c
)
