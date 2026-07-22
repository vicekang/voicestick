#include "audio_feedback.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "stick_s3_board.h"

#define FEEDBACK_SAMPLE_RATE 16000
#define FEEDBACK_CHUNK_SAMPLES 192
#define ROLL_QUEUE_DEPTH 1

static const char *TAG = "audio_feedback";

typedef enum {
    EFFECT_START,
    EFFECT_END,
    EFFECT_ROLL,
} effect_t;

typedef struct {
    i2s_chan_handle_t tx_handle;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_data_if_t *data_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_if_t *codec_if;
    esp_codec_dev_handle_t codec;
} speaker_session_t;

static QueueHandle_t s_roll_queue;
static SemaphoreHandle_t s_playback_mutex;
static volatile bool s_rolls_suspended;
static bool s_initialized;
static uint32_t s_noise_state = 0x8d2b79f5;

static float clampf(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static float next_noise(void)
{
    s_noise_state = s_noise_state * 1664525u + 1013904223u;
    return ((s_noise_state >> 8) / 8388607.5f) - 1.0f;
}

static void close_speaker_session(speaker_session_t *session)
{
    (void)stick_s3_board_speaker_enable(false);
    if (session->codec) {
        (void)esp_codec_dev_set_out_mute(session->codec, true);
        (void)esp_codec_dev_close(session->codec);
        esp_codec_dev_delete(session->codec);
        session->codec = NULL;
    }
    if (session->codec_if) {
        audio_codec_delete_codec_if(session->codec_if);
        session->codec_if = NULL;
    }
    if (session->data_if) {
        audio_codec_delete_data_if(session->data_if);
        session->data_if = NULL;
    }
    if (session->gpio_if) {
        audio_codec_delete_gpio_if(session->gpio_if);
        session->gpio_if = NULL;
    }
    if (session->ctrl_if) {
        audio_codec_delete_ctrl_if(session->ctrl_if);
        session->ctrl_if = NULL;
    }
    if (session->tx_handle) {
        (void)i2s_del_channel(session->tx_handle);
        session->tx_handle = NULL;
    }
}

static esp_err_t open_speaker_session(speaker_session_t *session, int volume)
{
    memset(session, 0, sizeof(*session));

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &session->tx_handle, NULL),
                        TAG, "create speaker I2S");

    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(FEEDBACK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = STICK_S3_PIN_ES8311_MCLK,
            .bclk = STICK_S3_PIN_ES8311_BCLK,
            .ws = STICK_S3_PIN_ES8311_LRCK,
            .dout = STICK_S3_PIN_ES8311_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    esp_err_t ret = i2s_channel_init_std_mode(session->tx_handle, &standard);
    if (ret != ESP_OK) {
        close_speaker_session(session);
        return ret;
    }
    ret = i2s_channel_enable(session->tx_handle);
    if (ret != ESP_OK) {
        close_speaker_session(session);
        return ret;
    }

    audio_codec_i2c_cfg_t i2c = {
        .port = I2C_NUM_1,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = stick_s3_board_i2c_bus(),
    };
    session->ctrl_if = audio_codec_new_i2c_ctrl(&i2c);
    ESP_GOTO_ON_FALSE(session->ctrl_if != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "create codec control");

    audio_codec_i2s_cfg_t i2s = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = session->tx_handle,
    };
    session->data_if = audio_codec_new_i2s_data(&i2s);
    ESP_GOTO_ON_FALSE(session->data_if != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "create codec data");
    session->gpio_if = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(session->gpio_if != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "create codec GPIO");

    es8311_codec_cfg_t codec_config = {
        .ctrl_if = session->ctrl_if,
        .gpio_if = session->gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    session->codec_if = es8311_codec_new(&codec_config);
    ESP_GOTO_ON_FALSE(session->codec_if != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "create ES8311 output");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = session->codec_if,
        .data_if = session->data_if,
    };
    session->codec = esp_codec_dev_new(&device_config);
    ESP_GOTO_ON_FALSE(session->codec != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "create speaker device");

    esp_codec_dev_sample_info_t sample_config = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = I2S_STD_SLOT_LEFT,
        .sample_rate = FEEDBACK_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_GOTO_ON_FALSE(esp_codec_dev_open(session->codec, &sample_config) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "open speaker codec");
    ESP_GOTO_ON_FALSE(esp_codec_dev_set_out_vol(session->codec, volume) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "set speaker volume");
    ESP_GOTO_ON_FALSE(esp_codec_dev_set_out_mute(session->codec, false) == ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "unmute speaker");
    ESP_GOTO_ON_ERROR(stick_s3_board_speaker_enable(true), fail, TAG,
                      "enable speaker amplifier");
    /* The external amplifier needs time to leave shutdown. The previous 6 ms
     * delay clipped almost the entire short dice transient. */
    vTaskDelay(pdMS_TO_TICKS(22));
    int16_t silence[128] = {0};
    ESP_GOTO_ON_FALSE(esp_codec_dev_write(session->codec, silence, sizeof(silence)) ==
                          ESP_CODEC_DEV_OK,
                      ESP_FAIL, fail, TAG, "prime speaker path");
    return ESP_OK;

fail:
    close_speaker_session(session);
    return ret;
}

static float sample_for_effect(effect_t effect, float t, float duration,
                               float *phase, float intensity)
{
    const float normalized = clampf(t / duration, 0, 1);
    const float attack = clampf(t * 95.0f, 0, 1);
    const float release = clampf((duration - t) * 36.0f, 0, 1);
    const float envelope = attack * release;
    float frequency = 700;
    float tone = 0;
    float noise = next_noise();

    if (effect == EFFECT_START) {
        if (t < 0.032f) {
            tone = noise * (1.0f - t / 0.032f) * 0.62f;
            frequency = 640;
        } else {
            const float chirp = (t - 0.032f) / (duration - 0.032f);
            frequency = 690.0f + chirp * 720.0f;
            tone = sinf(*phase) * 0.82f + noise * 0.11f;
        }
    } else if (effect == EFFECT_END) {
        frequency = 1280.0f - normalized * 660.0f;
        tone = sinf(*phase) * 0.72f + sinf(*phase * 0.51f) * 0.18f;
        if (normalized > 0.72f) {
            tone += noise * (normalized - 0.72f) * 0.85f;
        }
    } else {
        /* A hard plastic die is a very short broadband click followed by
         * several quickly decaying body resonances. Unlike the radio cues,
         * the collision starts at full amplitude with no fade-in. */
        const float hard_click = noise * expf(-t * 115.0f) * 0.72f;
        const float body = sinf(2.0f * (float)M_PI * 520.0f * t) *
                           expf(-t * 34.0f) * 0.50f;
        const float edge = sinf(2.0f * (float)M_PI * 1280.0f * t + 0.7f) *
                           expf(-t * 49.0f) * 0.31f;
        const float sparkle = sinf(2.0f * (float)M_PI * 2380.0f * t + 1.2f) *
                              expf(-t * 72.0f) * 0.15f;
        return (hard_click + body + edge + sparkle) * intensity;
    }

    *phase += 2.0f * (float)M_PI * frequency / FEEDBACK_SAMPLE_RATE;
    if (*phase > 2.0f * (float)M_PI) {
        *phase = fmodf(*phase, 2.0f * (float)M_PI);
    }
    return tone * envelope * intensity;
}

static esp_err_t play_effect_locked(effect_t effect, float intensity)
{
    speaker_session_t session;
    const int volume = effect == EFFECT_ROLL ? 74 : 60;
    ESP_RETURN_ON_ERROR(open_speaker_session(&session, volume), TAG,
                        "open feedback speaker");

    const float duration = effect == EFFECT_START ? 0.145f :
                           effect == EFFECT_END ? 0.125f :
                           0.078f + clampf(intensity, 0, 1) * 0.030f;
    const int total_samples = (int)(duration * FEEDBACK_SAMPLE_RATE);
    int16_t pcm[FEEDBACK_CHUNK_SAMPLES];
    float phase = 0;
    int written_samples = 0;
    esp_err_t result = ESP_OK;

    while (written_samples < total_samples) {
        const int count = total_samples - written_samples < FEEDBACK_CHUNK_SAMPLES
                              ? total_samples - written_samples
                              : FEEDBACK_CHUNK_SAMPLES;
        for (int i = 0; i < count; ++i) {
            const float t = (written_samples + i) / (float)FEEDBACK_SAMPLE_RATE;
            const float value = sample_for_effect(effect, t, duration, &phase,
                                                  clampf(intensity, 0.2f, 1.0f));
            const float gain = effect == EFFECT_ROLL ? 22500.0f : 15000.0f;
            pcm[i] = (int16_t)clampf(value * gain, -28000, 28000);
        }
        if (esp_codec_dev_write(session.codec, pcm, count * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            result = ESP_FAIL;
            break;
        }
        written_samples += count;
    }

    memset(pcm, 0, sizeof(pcm));
    (void)esp_codec_dev_write(session.codec, pcm, 64 * sizeof(int16_t));
    vTaskDelay(pdMS_TO_TICKS(8));
    close_speaker_session(&session);
    return result;
}

static void roll_task(void *arg)
{
    (void)arg;
    float intensity;
    while (true) {
        if (xQueueReceive(s_roll_queue, &intensity, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_rolls_suspended) {
            continue;
        }
        if (xSemaphoreTake(s_playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!s_rolls_suspended) {
                const esp_err_t err = play_effect_locked(EFFECT_ROLL, intensity);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "dice collision playback failed: %s",
                             esp_err_to_name(err));
                }
            }
            xSemaphoreGive(s_playback_mutex);
        }
    }
}

esp_err_t audio_feedback_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_roll_queue = xQueueCreate(ROLL_QUEUE_DEPTH, sizeof(float));
    s_playback_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_roll_queue && s_playback_mutex, ESP_ERR_NO_MEM, TAG,
                        "allocate feedback controls");
    BaseType_t task_ok = xTaskCreatePinnedToCore(roll_task, "dice_sfx", 4096,
                                                 NULL, 3, NULL, 0);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create feedback task");
    s_initialized = true;
    ESP_LOGI(TAG, "speaker feedback ready");
    return ESP_OK;
}

void audio_feedback_suspend_rolls(void)
{
    s_rolls_suspended = true;
    if (s_roll_queue) {
        xQueueReset(s_roll_queue);
    }
}

void audio_feedback_resume_rolls(void)
{
    s_rolls_suspended = false;
}

void audio_feedback_play_roll(float intensity)
{
    if (!s_initialized || s_rolls_suspended || intensity < 0.20f) {
        return;
    }
    intensity = clampf(intensity, 0.20f, 1.0f);
    (void)xQueueOverwrite(s_roll_queue, &intensity);
}

static esp_err_t play_blocking(effect_t effect)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "feedback not initialized");
    if (xSemaphoreTake(s_playback_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = play_effect_locked(effect, 1.0f);
    xSemaphoreGive(s_playback_mutex);
    return result;
}

esp_err_t audio_feedback_play_start(void)
{
    audio_feedback_suspend_rolls();
    return play_blocking(EFFECT_START);
}

esp_err_t audio_feedback_play_end(void)
{
    return play_blocking(EFFECT_END);
}
