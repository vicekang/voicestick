#include "stick_s3_board.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BMI270_I2C_FREQ_HZ 400000
#define BMI270_CHIP_ID 0x24

#define BMI270_REG_CHIP_ID 0x00
#define BMI270_REG_ACC_DATA 0x0c
#define BMI270_REG_ACC_CONF 0x40
#define BMI270_REG_ACC_RANGE 0x41
#define BMI270_REG_GYR_CONF 0x42
#define BMI270_REG_GYR_RANGE 0x43
#define BMI270_REG_INIT_CTRL 0x59
#define BMI270_REG_INIT_ADDR_0 0x5b
#define BMI270_REG_INIT_DATA 0x5e
#define BMI270_REG_INTERNAL_STATUS 0x21
#define BMI270_REG_PWR_CONF 0x7c
#define BMI270_REG_PWR_CTRL 0x7d
#define BMI270_REG_CMD 0x7e

#define BMI270_CMD_SOFT_RESET 0xb6

static const char *TAG = "stick_s3_imu";
static i2c_master_dev_handle_t s_imu_dev;
static bool s_imu_ready;

#include "bmi270_config.inc"

static esp_err_t imu_read(uint8_t reg, void *data, size_t len)
{
    return i2c_master_transmit_receive(s_imu_dev, &reg, 1, data, len, 200);
}

static esp_err_t imu_write(uint8_t reg, const void *data, size_t len)
{
    uint8_t stack_buf[65];
    if (len > sizeof(stack_buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    stack_buf[0] = reg;
    memcpy(stack_buf + 1, data, len);
    return i2c_master_transmit(s_imu_dev, stack_buf, len + 1, 500);
}

static esp_err_t imu_write_u8(uint8_t reg, uint8_t value)
{
    return imu_write(reg, &value, 1);
}

static esp_err_t add_imu_device(uint8_t address)
{
    i2c_master_bus_handle_t bus = stick_s3_board_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus unavailable");

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = BMI270_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(bus, &config, &s_imu_dev);
}

static esp_err_t probe_imu(void)
{
    const uint8_t candidates[] = {0x68, 0x69};
    for (size_t i = 0; i < sizeof(candidates); ++i) {
        if (s_imu_dev) {
            (void)i2c_master_bus_rm_device(s_imu_dev);
            s_imu_dev = NULL;
        }

        esp_err_t err = add_imu_device(candidates[i]);
        if (err != ESP_OK) {
            continue;
        }

        uint8_t chip_id = 0;
        err = imu_read(BMI270_REG_CHIP_ID, &chip_id, 1);
        if (err == ESP_OK && chip_id == BMI270_CHIP_ID) {
            ESP_LOGI(TAG, "BMI270 found at 0x%02x", candidates[i]);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t upload_config(void)
{
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_INIT_CTRL, 0x00), TAG,
                        "disable config load");

    const size_t chunk_size = 32;
    for (size_t offset = 0; offset < sizeof(bmi270_config_file); offset += chunk_size) {
        const size_t remaining = sizeof(bmi270_config_file) - offset;
        const size_t count = remaining < chunk_size ? remaining : chunk_size;
        const uint8_t init_addr[2] = {
            (uint8_t)((offset >> 1) & 0x0f),
            (uint8_t)(offset >> 5),
        };
        ESP_RETURN_ON_ERROR(imu_write(BMI270_REG_INIT_ADDR_0, init_addr,
                                      sizeof(init_addr)),
                            TAG, "set config address");
        ESP_RETURN_ON_ERROR(imu_write(BMI270_REG_INIT_DATA,
                                      bmi270_config_file + offset, count),
                            TAG, "upload config");
    }

    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_INIT_CTRL, 0x01), TAG,
                        "enable config load");
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t status = 0;
    for (int retry = 0; retry < 40; ++retry) {
        ESP_RETURN_ON_ERROR(imu_read(BMI270_REG_INTERNAL_STATUS, &status, 1), TAG,
                            "read init status");
        if ((status & 0x0f) == 0x01) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGE(TAG, "BMI270 config rejected, status=0x%02x", status);
    return ESP_FAIL;
}

esp_err_t stick_s3_imu_init(void)
{
    if (s_imu_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(probe_imu(), TAG, "probe BMI270");
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_CMD, BMI270_CMD_SOFT_RESET), TAG,
                        "reset BMI270");
    vTaskDelay(pdMS_TO_TICKS(3));
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_PWR_CONF, 0x00), TAG,
                        "disable power save");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(upload_config(), TAG, "load BMI270 config");

    /* 100 Hz, performance mode, +/-8 g and +/-2000 dps. */
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_ACC_CONF, 0xa8), TAG,
                        "configure accelerometer");
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_ACC_RANGE, 0x02), TAG,
                        "configure accelerometer range");
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_GYR_CONF, 0xa9), TAG,
                        "configure gyroscope");
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_GYR_RANGE, 0x00), TAG,
                        "configure gyroscope range");
    ESP_RETURN_ON_ERROR(imu_write_u8(BMI270_REG_PWR_CTRL, 0x0e), TAG,
                        "enable accelerometer and gyroscope");
    vTaskDelay(pdMS_TO_TICKS(5));

    s_imu_ready = true;
    ESP_LOGI(TAG, "BMI270 ready at 100 Hz");
    return ESP_OK;
}

bool stick_s3_imu_is_ready(void)
{
    return s_imu_ready;
}

esp_err_t stick_s3_imu_read(stick_s3_imu_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG, "sample is null");
    ESP_RETURN_ON_FALSE(s_imu_ready, ESP_ERR_INVALID_STATE, TAG, "IMU not ready");

    uint8_t raw[12];
    ESP_RETURN_ON_ERROR(imu_read(BMI270_REG_ACC_DATA, raw, sizeof(raw)), TAG,
                        "read IMU sample");

    int16_t values[6];
    for (size_t i = 0; i < 6; ++i) {
        values[i] = (int16_t)((uint16_t)raw[i * 2] |
                              ((uint16_t)raw[i * 2 + 1] << 8));
    }

    const float accel_scale = 8.0f / 32768.0f;
    const float gyro_scale = (2000.0f / 32768.0f) * ((float)M_PI / 180.0f);
    sample->accel_x_g = values[0] * accel_scale;
    sample->accel_y_g = values[1] * accel_scale;
    sample->accel_z_g = values[2] * accel_scale;
    sample->gyro_x_rad_s = values[3] * gyro_scale;
    sample->gyro_y_rad_s = values[4] * gyro_scale;
    sample->gyro_z_rad_s = values[5] * gyro_scale;
    return ESP_OK;
}
