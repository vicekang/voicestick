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
#include "freertos/queue.h"
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
#define OPUS_V1_BITRATE 20000
#define OPUS_V2_MIN_BITRATE 7000
#define OPUS_V2_MAX_BITRATE 12000
#define OPUS_MAX_PACKET_SIZE 220
#define OPUS_COMPLEXITY 1

#define TX_QUEUE_DEPTH 24
#define TX_RETRY_DELAY_MS 10
#define TX_MAX_RETRIES 8
#define TX_ACK_TIMEOUT_MS 100
#define TX_BUNDLE_COLLECT_TIMEOUT_MS 75
#define TX_V2_MAX_BUNDLE 4
#define TX_V2_ACK_WINDOW_BATCHES 2
#define TASK_EXIT_WAIT_MS 800

typedef struct {
    uint32_t session_id;
    uint32_t seq;
    uint8_t  flags;
    uint16_t len;
    uint8_t  data[OPUS_MAX_PACKET_SIZE];
} audio_packet_t;

typedef struct {
    uint8_t version;
    uint8_t bundle_size;
    uint16_t packet_budget;
    uint32_t opus_bitrate;
    uint32_t max_inflight_frames;
} audio_transport_profile_t;

static atomic_bool s_running;
static bool s_initialized;
static uint32_t s_session_id;
static uint32_t s_seq;
static TaskHandle_t s_audio_task;
static TaskHandle_t s_tx_task;
static QueueHandle_t s_tx_queue;
static audio_transport_profile_t s_transport_profile;
static atomic_uint_fast32_t s_overflow_drops;

/* Per-session resources: created on start, destroyed on stop */
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_codec;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static OpusEncoder *s_opus_encoder;

static bool tasks_exited(void)
{
    return s_audio_task == NULL && s_tx_task == NULL;
}

static esp_err_t wait_for_tasks_to_exit(TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;
    while (!tasks_exited()) {
        if (xTaskGetTickCount() >= deadline) {
            ESP_LOGW(TAG, "tasks still exiting: audio=%p tx=%p",
                     s_audio_task, s_tx_task);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

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
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2c ctrl");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_1,
        .rx_handle = s_rx_handle,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec i2s data");

    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec gpio");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
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
    s_codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create es8311");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
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

static void configure_transport_profile(void)
{
    s_transport_profile = (audio_transport_profile_t) {
        .version = AUDIO_TRANSPORT_V1,
        .bundle_size = 1,
        .packet_budget = OPUS_MAX_PACKET_SIZE,
        .opus_bitrate = OPUS_V1_BITRATE,
        .max_inflight_frames = 1,
    };

    if (voice_ble_audio_transport_version() < AUDIO_TRANSPORT_V2) {
        return;
    }

    const uint16_t mtu = voice_ble_att_mtu();
    for (uint8_t bundle = TX_V2_MAX_BUNDLE; bundle >= 2; bundle--) {
        const size_t budget = audio_transport_v2_packet_budget(mtu, bundle);
        if (budget < 2 || budget > UINT8_MAX) {
            continue;
        }
        const uint32_t bitrate = (uint32_t)(budget - 1) * 8 * 1000 / AUDIO_FRAME_MS;
        if (bitrate < OPUS_V2_MIN_BITRATE) {
            continue;
        }

        s_transport_profile.version = AUDIO_TRANSPORT_V2;
        s_transport_profile.bundle_size = bundle;
        s_transport_profile.packet_budget = (uint16_t)budget;
        s_transport_profile.opus_bitrate = bitrate < OPUS_V2_MAX_BITRATE
            ? bitrate
            : OPUS_V2_MAX_BITRATE;
        s_transport_profile.max_inflight_frames = bundle * TX_V2_ACK_WINDOW_BATCHES;
        break;
    }

    if (s_transport_profile.version != AUDIO_TRANSPORT_V2) {
        ESP_LOGW(TAG, "att mtu=%u cannot carry v2 speech at >=%d bps; falling back to v1",
                 mtu, OPUS_V2_MIN_BITRATE);
        voice_ble_set_audio_transport_version(AUDIO_TRANSPORT_V1);
    }
}

static esp_err_t init_opus(void)
{
    int error = OPUS_OK;
    s_opus_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS,
                                         OPUS_APPLICATION_VOIP, &error);
    ESP_RETURN_ON_FALSE(s_opus_encoder != NULL && error == OPUS_OK,
                        ESP_FAIL, TAG, "create opus encoder error=%d", error);
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_BITRATE(s_transport_profile.opus_bitrate));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    ESP_LOGI(TAG, "transport v%u mtu=%u bundle=%u packet_budget=%u bitrate=%" PRIu32,
             s_transport_profile.version, voice_ble_att_mtu(),
             s_transport_profile.bundle_size, s_transport_profile.packet_budget,
             s_transport_profile.opus_bitrate);
    return ESP_OK;
}

static void deinit_opus(void)
{
    if (s_opus_encoder) {
        opus_encoder_destroy(s_opus_encoder);
        s_opus_encoder = NULL;
    }
}

static void deinit_codec(void)
{
    if (s_codec) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
}

static void deinit_i2s(void)
{
    if (s_rx_handle) {
        /* Channel is already disabled by codec close; just release it. */
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
}

static void deinit_session_resources(void)
{
    deinit_opus();
    deinit_codec();
    deinit_i2s();
    ESP_LOGI(TAG, "session resources released");
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t mono[AUDIO_FRAME_SAMPLES];
    uint8_t opus_buf[OPUS_MAX_PACKET_SIZE];
    uint32_t enqueued = 0;
    uint32_t dropped = 0;

    while (atomic_load(&s_running)) {
        esp_err_t err = esp_codec_dev_read(s_codec, mono, sizeof(mono));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec read failed: %s", esp_err_to_name(err));
            continue;
        }

        opus_int32 encoded = opus_encode(s_opus_encoder, mono, AUDIO_FRAME_SAMPLES,
                                         opus_buf, s_transport_profile.packet_budget);
        if (encoded < 0) {
            ESP_LOGE(TAG, "opus encode failed: %d", (int)encoded);
            continue;
        }

        audio_packet_t pkt = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = (s_seq == 0) ? VOICE_BLE_FLAG_START : 0x00,
            .len = (uint16_t)encoded,
        };
        memcpy(pkt.data, opus_buf, encoded);

        if (xQueueSend(s_tx_queue, &pkt, 0) == pdTRUE) {
            s_seq++;
            enqueued++;
        } else {
            /* Queue full: drop oldest packet to make room */
            audio_packet_t discard;
            xQueueReceive(s_tx_queue, &discard, 0);
            xQueueSend(s_tx_queue, &pkt, 0);
            s_seq++;
            enqueued++;
            dropped++;
            atomic_store(&s_overflow_drops, dropped);
            if (dropped == 1 || (dropped % 20) == 0) {
                ESP_LOGW(TAG, "tx queue overflow, dropped oldest (total=%" PRIu32 ")", dropped);
            }
        }
    }

    audio_packet_t sentinel = {
        .session_id = s_session_id,
        .seq = s_seq,
        .flags = VOICE_BLE_FLAG_END,
        .len = 0,
    };
    xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);

    ESP_LOGI(TAG, "audio task exit: enqueued=%" PRIu32 " overflow_drops=%" PRIu32,
             enqueued, dropped);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t send_audio_bundle(const audio_packet_t *bundle, uint8_t count)
{
    if (!bundle || count == 0 || count > TX_V2_MAX_BUNDLE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_transport_profile.version == AUDIO_TRANSPORT_V1) {
        return voice_ble_send_audio(bundle[0].session_id, bundle[0].seq,
                                    bundle[0].flags, bundle[0].data, bundle[0].len);
    }

    audio_transport_packet_t packets[TX_V2_MAX_BUNDLE];
    for (uint8_t index = 0; index < count; index++) {
        packets[index] = (audio_transport_packet_t) {
            .seq = bundle[index].seq,
            .flags = bundle[index].flags,
            .payload = bundle[index].data,
            .len = bundle[index].len,
        };
    }

    const uint32_t next_seq = bundle[count - 1].seq + 1;
    ESP_RETURN_ON_ERROR(
        voice_ble_wait_for_audio_window(bundle[0].session_id, next_seq,
                                        s_transport_profile.max_inflight_frames,
                                        TX_ACK_TIMEOUT_MS),
        TAG, "audio ack window");
    return voice_ble_send_audio_packets(bundle[0].session_id, bundle[0].seq,
                                        0, packets, count);
}

static void tx_task(void *arg)
{
    (void)arg;
    audio_packet_t bundle[TX_V2_MAX_BUNDLE];
    uint32_t sent_frames = 0;
    uint32_t sent_notifications = 0;
    uint32_t tx_dropped = 0;
    uint32_t end_seq = 0;
    bool stopping = false;

    while (!stopping) {
        if (xQueueReceive(s_tx_queue, &bundle[0], portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (bundle[0].flags == VOICE_BLE_FLAG_END && bundle[0].len == 0) {
            end_seq = bundle[0].seq;
            break;
        }

        uint8_t count = 1;
        while (count < s_transport_profile.bundle_size) {
            audio_packet_t next;
            if (xQueueReceive(s_tx_queue, &next,
                              pdMS_TO_TICKS(TX_BUNDLE_COLLECT_TIMEOUT_MS)) != pdTRUE) {
                break;
            }
            if (next.flags == VOICE_BLE_FLAG_END && next.len == 0) {
                end_seq = next.seq;
                stopping = true;
                break;
            }
            bundle[count++] = next;
        }

        int retries = 0;
        while (send_audio_bundle(bundle, count) != ESP_OK) {
            if (++retries >= TX_MAX_RETRIES) {
                tx_dropped += count;
                ESP_LOGW(TAG, "drop audio bundle seq=%" PRIu32
                              " count=%u after %d retries",
                         bundle[0].seq, count, retries);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
        }
        if (retries < TX_MAX_RETRIES) {
            sent_frames += count;
            sent_notifications++;
        }
    }

    int end_retries = 0;
    while (voice_ble_send_audio(s_session_id, end_seq, VOICE_BLE_FLAG_END, NULL, 0) != ESP_OK &&
           ++end_retries < TX_MAX_RETRIES) {
        vTaskDelay(pdMS_TO_TICKS(TX_RETRY_DELAY_MS));
    }

    ESP_LOGI(TAG, "tx task exit: transport=v%u frames=%" PRIu32
                  " notifications=%" PRIu32 " tx_dropped=%" PRIu32
                  " overflow_dropped=%" PRIuFAST32,
             s_transport_profile.version, sent_frames, sent_notifications,
             tx_dropped, atomic_load(&s_overflow_drops));

    while (s_audio_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    deinit_session_resources();
    s_tx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_pipeline_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(audio_packet_t));
    ESP_RETURN_ON_FALSE(s_tx_queue != NULL, ESP_ERR_NO_MEM, TAG, "create tx queue");

    s_initialized = true;
    ESP_LOGI(TAG, "audio pipeline ready (resources allocated on demand)");
    return ESP_OK;
}

esp_err_t audio_pipeline_start(uint32_t session_id)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (atomic_load(&s_running)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS)),
                        TAG, "wait previous session exit");

    configure_transport_profile();
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "i2s init");
    esp_err_t err = init_codec();
    if (err != ESP_OK) {
        deinit_i2s();
        ESP_LOGE(TAG, "codec init: %s", esp_err_to_name(err));
        return err;
    }
    err = init_opus();
    if (err != ESP_OK) {
        deinit_codec();
        deinit_i2s();
        ESP_LOGE(TAG, "opus init: %s", esp_err_to_name(err));
        return err;
    }

    voice_ble_request_fast_interval();

    xQueueReset(s_tx_queue);
    s_session_id = session_id;
    s_seq = 0;
    atomic_store(&s_overflow_drops, 0);
    voice_ble_begin_audio_session(session_id);
    opus_encoder_ctl(s_opus_encoder, OPUS_RESET_STATE);
    atomic_store(&s_running, true);

    BaseType_t ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 4096,
                                            NULL, 6, &s_tx_task, 0);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_tx_task = NULL;
        deinit_session_resources();
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreatePinnedToCore(audio_task, "audio_pipeline", 32768,
                                 NULL, 5, &s_audio_task, 1);
    if (ok != pdPASS) {
        atomic_store(&s_running, false);
        s_audio_task = NULL;
        audio_packet_t sentinel = {
            .session_id = s_session_id,
            .seq = s_seq,
            .flags = VOICE_BLE_FLAG_END,
            .len = 0,
        };
        xQueueSend(s_tx_queue, &sentinel, portMAX_DELAY);
        (void)wait_for_tasks_to_exit(pdMS_TO_TICKS(TASK_EXIT_WAIT_MS));
        /* tx_task cleans up session resources on exit */
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "start session %" PRIu32, session_id);
    return ESP_OK;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!atomic_load(&s_running)) {
        return ESP_OK;
    }
    atomic_store(&s_running, false);
    ESP_LOGI(TAG, "stop session %" PRIu32, s_session_id);

    return ESP_OK;
}

esp_err_t audio_pipeline_wait_stopped(uint32_t timeout_ms)
{
    return wait_for_tasks_to_exit(pdMS_TO_TICKS(timeout_ms));
}

uint32_t audio_pipeline_session_id(void)
{
    return s_session_id;
}
