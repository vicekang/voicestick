#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VOICE_BLE_DEVICE_NAME_PREFIX "VS"

#define VOICE_BLE_FLAG_START 0x01
#define VOICE_BLE_FLAG_END   0x02

typedef void (*voice_ble_connection_cb_t)(bool connected);

esp_err_t voice_ble_init(void);
void voice_ble_set_connection_callback(voice_ble_connection_cb_t callback);
const char *voice_ble_device_id(void);
const char *voice_ble_device_name(void);
bool voice_ble_is_connected(void);
esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                               const uint8_t *opus_payload, size_t len);
esp_err_t voice_ble_send_press_start(uint32_t session_id);
esp_err_t voice_ble_send_press_end(uint32_t session_id);
esp_err_t voice_ble_send_cancel(void);
