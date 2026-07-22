#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef struct {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_rad_s;
    float gyro_y_rad_s;
    float gyro_z_rad_s;
} stick_s3_imu_sample_t;

#define STICK_S3_PIN_BUTTON_FRONT 11
#define STICK_S3_PIN_BUTTON_SIDE  12
#define STICK_S3_PIN_PMIC_IRQ     13

#define STICK_S3_PIN_I2C_SCL 48
#define STICK_S3_PIN_I2C_SDA 47

#define STICK_S3_PIN_ES8311_MCLK 18
// Pin names follow the codec's perspective:
//   ES8311_DIN  = codec serial data input  (DSDIN, MCU -> codec, speaker path) = GPIO14
//   ES8311_DOUT = codec serial data output (ASDOUT, codec -> MCU, mic path)   = GPIO16
#define STICK_S3_PIN_ES8311_BCLK 17
#define STICK_S3_PIN_ES8311_LRCK 15
#define STICK_S3_PIN_ES8311_DIN  14
#define STICK_S3_PIN_ES8311_DOUT 16

#define STICK_S3_PIN_LCD_MOSI 39
#define STICK_S3_PIN_LCD_SCK  40
#define STICK_S3_PIN_LCD_DC   45
#define STICK_S3_PIN_LCD_CS   41
#define STICK_S3_PIN_LCD_RST  21
#define STICK_S3_PIN_LCD_BL   38

esp_err_t stick_s3_board_init(void);
i2c_master_bus_handle_t stick_s3_board_i2c_bus(void);
esp_err_t stick_s3_board_battery_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_vbus_voltage_mv(int *voltage_mv);
esp_err_t stick_s3_board_battery_level(int *level_percent);
esp_err_t stick_s3_board_battery_charging(bool *charging);
esp_err_t stick_s3_board_usb_powered(bool *usb_powered);
esp_err_t stick_s3_board_clear_power_irqs(uint8_t *sys_status);
esp_err_t stick_s3_board_speaker_enable(bool enabled);
void stick_s3_board_prepare_deep_sleep(void);
bool stick_s3_front_button_pressed(void);
bool stick_s3_side_button_pressed(void);
esp_err_t stick_s3_imu_init(void);
bool stick_s3_imu_is_ready(void);
esp_err_t stick_s3_imu_read(stick_s3_imu_sample_t *sample);
