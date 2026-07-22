#pragma once

#include "esp_err.h"

esp_err_t audio_feedback_init(void);
void audio_feedback_suspend_rolls(void);
void audio_feedback_resume_rolls(void);
void audio_feedback_play_roll(float intensity);
esp_err_t audio_feedback_play_start(void);
esp_err_t audio_feedback_play_end(void);
