#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(uint32_t session_id);
esp_err_t audio_pipeline_stop(void);
esp_err_t audio_pipeline_wait_stopped(uint32_t timeout_ms);
uint32_t audio_pipeline_session_id(void);
