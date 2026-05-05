#include "audio_pipeline.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/i2s_types.h"
#include "opus.h"

#include "stick_s3_board.h"
#include "voice_ble.h"

static const char *TAG = "audio_pipeline";

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS 1
#define AUDIO_FRAME_MS 60
#define AUDIO_FRAME_SAMPLES ((AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS) / 1000)
#define AUDIO_STEREO_SAMPLES (AUDIO_FRAME_SAMPLES * 2)
#define OPUS_BITRATE 16000
#define OPUS_MAX_PACKET_SIZE 220
#define OPUS_COMPLEXITY 4

static atomic_bool s_running;
static bool s_initialized;
static uint32_t s_session_id;
static uint32_t s_seq;
static TaskHandle_t s_audio_task;
static i2s_chan_handle_t s_rx_handle;
static i2s_chan_handle_t s_tx_handle;
static esp_codec_dev_handle_t s_codec;
static OpusEncoder *s_opus_encoder;

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle),
                        TAG, "create i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = STICK_S3_PIN_ES8311_MCLK,
            .bclk = STICK_S3_PIN_ES8311_BCLK,
            .ws = STICK_S3_PIN_ES8311_LRCK,
            .dout = STICK_S3_PIN_ES8311_DIN,
            .din = STICK_S3_PIN_ES8311_DOUT,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg),
                        TAG, "init i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx");
    return ESP_OK;
}

static esp_err_t init_codec(void)
{
    i2c_master_bus_handle_t i2c_bus = stick_s3_board_i2c_bus();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "i2c bus unavailable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2c ctrl");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_1,
        .rx_handle = s_rx_handle,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2s data");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec gpio");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
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
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "create codec dev");

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = I2S_STD_SLOT_LEFT,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_cfg) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "open codec");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, 36.0) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set mic gain");
    return ESP_OK;
}

static esp_err_t init_opus(void)
{
    int error = OPUS_OK;
    s_opus_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &error);
    ESP_RETURN_ON_FALSE(s_opus_encoder != NULL && error == OPUS_OK,
                        ESP_FAIL, TAG, "create opus encoder error=%d", error);
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    return ESP_OK;
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t mono[AUDIO_FRAME_SAMPLES];
    uint8_t opus_packet[OPUS_MAX_PACKET_SIZE];
    uint32_t sent = 0;

    while (atomic_load(&s_running)) {
        esp_err_t err = esp_codec_dev_read(s_codec, mono, sizeof(mono));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read failed: %s", esp_err_to_name(err));
            continue;
        }

        int32_t peak = 0;
        for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
            int32_t sample = mono[i];
            if (sample < 0) {
                sample = -sample;
            }
            if (sample > peak) {
                peak = sample;
            }
        }

        opus_int32 encoded = opus_encode(s_opus_encoder, mono, AUDIO_FRAME_SAMPLES,
                                         opus_packet, sizeof(opus_packet));
        if (encoded < 0) {
            ESP_LOGE(TAG, "opus encode failed: %d", (int)encoded);
            continue;
        }

        uint8_t flags = s_seq == 0 ? 0x01 : 0x00;
        err = voice_ble_send_audio(s_session_id, s_seq++, flags, opus_packet, encoded);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "send audio failed: %s", esp_err_to_name(err));
            continue;
        }

        sent++;
        if ((sent % 20) == 1) {
            ESP_LOGD(TAG, "audio frame seq=%" PRIu32 " opus=%d bytes=%u peak=%" PRId32,
                     s_seq - 1, (int)encoded, (unsigned)sizeof(mono), peak);
        }
    }

    ESP_LOGI(TAG, "audio task exit");
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_pipeline_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s init");
    ESP_RETURN_ON_ERROR(init_codec(), TAG, "codec init");
    ESP_RETURN_ON_ERROR(init_opus(), TAG, "opus init");

    s_initialized = true;
    ESP_LOGI(TAG, "audio pipeline ready: es8311 -> opus -> ble");
    return ESP_OK;
}

esp_err_t audio_pipeline_start(uint32_t session_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (atomic_load(&s_running)) {
        return ESP_OK;
    }

    s_session_id = session_id;
    s_seq = 0;
    opus_encoder_ctl(s_opus_encoder, OPUS_RESET_STATE);
    atomic_store(&s_running, true);
    ESP_LOGI(TAG, "start session %" PRIu32, session_id);

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio_pipeline", 32768,
                                            NULL, 5, &s_audio_task, 1);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_audio_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!atomic_load(&s_running)) {
        return ESP_OK;
    }
    atomic_store(&s_running, false);
    ESP_LOGI(TAG, "stop session %" PRIu32, s_session_id);
    voice_ble_send_audio(s_session_id, s_seq++, 0x02, NULL, 0);
    return ESP_OK;
}

uint32_t audio_pipeline_session_id(void)
{
    return s_session_id;
}
