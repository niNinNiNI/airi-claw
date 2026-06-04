#include "cap_asr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_types.h"
#include "esp_log.h"
#include "voice_ws_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cap_asr";

/* --- ADC/DAC devices --- */
static esp_codec_dev_handle_t s_adc_dev;
static esp_codec_dev_handle_t s_dac_dev;

/* --- capture state --- */
static volatile bool s_recording;
static volatile bool s_stop_requested;
static volatile TaskHandle_t s_capture_task;

/* --- ASR completion --- */
static SemaphoreHandle_t s_asr_done_sem;
static char s_asr_result[512];
static volatile bool s_asr_error;
static volatile bool s_asr_active;

/* --- parameters --- */
static uint32_t s_max_duration_ms;
static uint32_t s_silence_timeout_chunks;

/* --- ASR chunk size: 20ms @ 16kHz mono 16-bit = 640 bytes --- */
#define ASR_CHUNK_BYTES       640
#define ASR_STEREO_READ_BYTES 1280 /* stereo: 2 channels */
#define ASR_RESULT_TIMEOUT_MS 15000

/* --- WebSocket handlers registered flag --- */
static bool s_asr_handlers_registered;

/* --- helpers --- */

static esp_err_t ensure_adc_dev(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };

    /* On boards with a shared ADC/DAC codec (e.g. ES8311), the DAC section
     * must be powered to provide microphone bias. Open the DAC first at the
     * same sample rate, then open the ADC. For boards with a separate ADC
     * (e.g. ES7210), the DAC handle won't exist — skip it */
    void *dac_audio_handle = NULL;
    if (esp_board_manager_get_device_handle(
            ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &dac_audio_handle) == ESP_OK
        && dac_audio_handle) {
        esp_codec_dev_handle_t dac_dev =
            *(esp_codec_dev_handle_t *)dac_audio_handle;
        if (dac_dev) {
            esp_err_t ret = esp_codec_dev_open(dac_dev, &fs);
            if (ret == ESP_CODEC_DEV_OK) {
                s_dac_dev = dac_dev;
                esp_codec_dev_set_out_mute(dac_dev, true);
                ESP_LOGI(TAG, "DAC opened (16kHz) for mic bias");
            } else {
                ESP_LOGW(TAG, "Failed to open DAC: %d, mic may not work", ret);
            }
        }
    }

    /* Open ADC */
    void *audio_handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_AUDIO_ADC, &audio_handle);
    if (ret != ESP_OK || !audio_handle) {
        ESP_LOGW(TAG, "No audio ADC device available");
        return ESP_ERR_NOT_FOUND;
    }

    esp_codec_dev_handle_t dev = *(esp_codec_dev_handle_t *)audio_handle;
    if (!dev) {
        ESP_LOGW(TAG, "Audio ADC handle is NULL");
        return ESP_ERR_NOT_FOUND;
    }

    ret = esp_codec_dev_open(dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open ADC codec: %d", ret);
        return ESP_FAIL;
    }

    s_adc_dev = dev;

    /* Set mic gain to 30dB. The ES7210 driver's es7210_open sets 30dB
     * but es7210_start later calls es7210_mic_select which overwrites
     * it with codec->gain (0dB from calloc). Do it again here so the
     * hardware register has the correct value during capture. */
    esp_codec_dev_set_in_gain(dev, 30.0);

    ESP_LOGI(TAG, "ADC opened (16kHz stereo 16-bit)");
    return ESP_OK;
}

static void release_adc_dev(void)
{
    if (s_adc_dev) {
        esp_codec_dev_close(s_adc_dev);
        s_adc_dev = NULL;
        ESP_LOGI(TAG, "ADC closed");
    }
}

static bool is_all_zero(const int16_t *buf, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    return true;
}

/* --- ASR callbacks (registered via voice_ws_common) --- */

static void asr_text_handler(const char *type, const char *json,
                             void *user_data)
{
    (void)user_data;

    if (!s_asr_active) {
        return;
    }

    if (strcmp(type, "asr_result") == 0) {
        cJSON *root = cJSON_Parse(json);
        if (!root) return;

        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *is_final = cJSON_GetObjectItem(root, "is_final");

        if (cJSON_IsString(text) && text->valuestring) {
            ESP_LOGI(TAG, "ASR result: \"%s\" (final=%d)",
                     text->valuestring,
                     cJSON_IsTrue(is_final) ? 1 : 0);

            /* Always update with latest partial result */
            strlcpy(s_asr_result, text->valuestring, sizeof(s_asr_result));

            /* On final result, signal completion */
            if (cJSON_IsTrue(is_final)) {
                s_asr_error = false;
                s_asr_active = false;
                xSemaphoreGive(s_asr_done_sem);
            }
        }
        cJSON_Delete(root);

    } else if (strcmp(type, "error") == 0 && s_asr_active) {
        cJSON *root = cJSON_Parse(json);
        if (root) {
            cJSON *msg_item = cJSON_GetObjectItem(root, "message");
            const char *err_str = cJSON_IsString(msg_item) ?
                msg_item->valuestring : "unknown error";
            snprintf(s_asr_result, sizeof(s_asr_result),
                     "Error: %s", err_str);
            cJSON_Delete(root);
        } else {
            snprintf(s_asr_result, sizeof(s_asr_result),
                     "Error: unknown error");
        }
        s_asr_error = true;
        s_asr_active = false;
        xSemaphoreGive(s_asr_done_sem);
    }
}

static void asr_state_handler(bool connected, void *user_data)
{
    (void)user_data;
    if (!connected && s_asr_active) {
        ESP_LOGW(TAG, "WebSocket disconnected during ASR");
        snprintf(s_asr_result, sizeof(s_asr_result),
                 "Error: WebSocket disconnected");
        s_asr_error = true;
        s_asr_active = false;
        xSemaphoreGive(s_asr_done_sem);
    }
}

/* --- capture task --- */

static void asr_capture_task(void *arg)
{
    (void)arg;

    /* Allocate mono buffer */
    int16_t *mono_buf = malloc(ASR_CHUNK_BYTES);
    int16_t *stereo_buf = malloc(ASR_STEREO_READ_BYTES);
    if (!mono_buf || !stereo_buf) {
        ESP_LOGE(TAG, "Failed to allocate capture buffers");
        free(mono_buf);
        free(stereo_buf);
        snprintf(s_asr_result, sizeof(s_asr_result),
                 "Error: buffer allocation failed");
        s_asr_error = true;
        xSemaphoreGive(s_asr_done_sem);
        goto cleanup;
    }

    uint32_t silence_count = 0;
    uint32_t chunk_count = 0;
    TickType_t start_ticks = xTaskGetTickCount();

    /* Send asr_start */
    esp_err_t err = voice_ws_send_text("{\"type\":\"asr_start\"}");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send asr_start");
        snprintf(s_asr_result, sizeof(s_asr_result),
                 "Error: failed to send asr_start");
        s_asr_error = true;
        xSemaphoreGive(s_asr_done_sem);
        goto cleanup;
    }
    ESP_LOGI(TAG, "ASR recording started");

    while (s_recording) {
        /* Read 1280 bytes stereo (20ms) from I2S.
         * esp_codec_dev_read returns status code (0 = OK), not byte count. */
        int ret = esp_codec_dev_read(s_adc_dev, stereo_buf,
                                      ASR_STEREO_READ_BYTES);
        if (ret < 0) {
            ESP_LOGE(TAG, "ADC read error: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        chunk_count++;

        /* De-interleave: stereo → mono (ES7210 only drives left channel) */
        size_t mono_samples = ASR_STEREO_READ_BYTES / sizeof(int16_t) / 2;
        for (size_t i = 0; i < mono_samples; i++) {
            mono_buf[i] = stereo_buf[i * 2]; /* left channel only */
        }

        /* Diagnostic: print sample values periodically (matching reference project) */
        if (chunk_count == 1 || chunk_count % 50 == 0) {
            int16_t *mono = mono_buf;
            ESP_LOGI(TAG, "Mic chunk #%u (%u bytes mono): samples=[%d %d %d %d %d %d %d %d]",
                     (unsigned)chunk_count, (unsigned)(mono_samples * sizeof(int16_t)),
                     mono[0], mono[1], mono[2], mono[3],
                     mono[4], mono[5], mono[6], mono[7]);
        }

        /* Silence detection */
        bool silent = is_all_zero(mono_buf, mono_samples);
        if (silent) {
            silence_count++;
            ESP_LOGW(TAG, "Mic chunk #%u is ALL ZERO (total silent: %u)",
                     (unsigned)chunk_count, (unsigned)silence_count);
        } else {
            silence_count = 0;
        }

        /* Send PCM chunk to server */
        voice_ws_send_binary((const uint8_t *)mono_buf,
                             mono_samples * sizeof(int16_t));

        /* Check silence timeout */
        if (s_silence_timeout_chunks > 0 &&
            silence_count >= s_silence_timeout_chunks) {
            ESP_LOGI(TAG, "Silence timeout (%u chunks), stopping recording",
                     (unsigned)silence_count);
            break;
        }

        /* Check max duration */
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
        if (pdTICKS_TO_MS(elapsed_ticks) >= s_max_duration_ms) {
            ESP_LOGI(TAG, "Max duration reached (%ums), stopping recording",
                     (unsigned)s_max_duration_ms);
            break;
        }
    }

    /* Send asr_stop */
    voice_ws_send_text("{\"type\":\"asr_stop\"}");
    ESP_LOGI(TAG, "ASR recording stopped, waiting for result...");

    /* Wait for ASR result */
    TickType_t wait_ticks = pdMS_TO_TICKS(ASR_RESULT_TIMEOUT_MS);
    if (xSemaphoreTake(s_asr_done_sem, wait_ticks) != pdTRUE) {
        ESP_LOGE(TAG, "ASR result timeout");
        snprintf(s_asr_result, sizeof(s_asr_result),
                 "Error: ASR result timeout");
        s_asr_error = true;
    }

cleanup:
    free(mono_buf);
    free(stereo_buf);
    s_recording = false;
    s_capture_task = NULL;
    vTaskDelete(NULL);
}

/* --- cleanup --- */

static void asr_cleanup(void)
{
    s_recording = false;

    /* Wait for capture task to exit */
    if (s_capture_task != NULL) {
        int patience = 50; /* 50 * 10ms = 500ms max */
        while (s_capture_task != NULL && patience-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_capture_task != NULL) {
            ESP_LOGW(TAG, "Capture task did not exit cleanly, forcing deletion");
            vTaskDelete(s_capture_task);
        }
        s_capture_task = NULL;
    }

    /* Close ADC */
    release_adc_dev();

    /* Delete semaphore */
    if (s_asr_done_sem) {
        vSemaphoreDelete(s_asr_done_sem);
        s_asr_done_sem = NULL;
    }

    s_asr_active = false;
    ESP_LOGI(TAG, "ASR resources released");
}

void cap_asr_stop(void)
{
    s_stop_requested = true;
    s_recording = false;
}

/* --- execute callback --- */

static esp_err_t cap_asr_execute(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char *output, size_t output_size)
{
    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!voice_ws_is_connected()) {
        esp_err_t ret = voice_ws_connect(10000);
        if (ret != ESP_OK) {
            snprintf(output, output_size,
                     "Error: voice server not connected");
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Parse input parameters */
    s_max_duration_ms = 10000;
    s_silence_timeout_chunks = 50; /* 50 * 20ms = 1000ms */

    if (input_json) {
        cJSON *input = cJSON_Parse(input_json);
        if (input) {
            cJSON *dur = cJSON_GetObjectItem(input, "max_duration_ms");
            if (cJSON_IsNumber(dur) && dur->valueint > 0) {
                s_max_duration_ms = (uint32_t)dur->valueint;
                if (s_max_duration_ms > 30000) s_max_duration_ms = 30000;
            }

            cJSON *sil = cJSON_GetObjectItem(input, "silence_timeout_ms");
            if (cJSON_IsNumber(sil)) {
                if (sil->valueint == 0) {
                    s_silence_timeout_chunks = 0; /* disabled */
                } else if (sil->valueint > 0) {
                    s_silence_timeout_chunks =
                        (uint32_t)(sil->valueint / 20);
                    if (s_silence_timeout_chunks < 1) {
                        s_silence_timeout_chunks = 1;
                    }
                }
            }
            cJSON_Delete(input);
        }
    }

    ESP_LOGI(TAG, "ASR config: max_duration=%ums, silence_chunks=%u",
             (unsigned)s_max_duration_ms,
             (unsigned)s_silence_timeout_chunks);

    /* Register ASR callbacks if not already done */
    if (!s_asr_handlers_registered) {
        voice_ws_register_text_handler(asr_text_handler, NULL);
        voice_ws_register_state_handler(asr_state_handler, NULL);
        s_asr_handlers_registered = true;
    }

    /* Open ADC device */
    if (ensure_adc_dev() != ESP_OK) {
        snprintf(output, output_size,
                 "Error: no audio ADC device available");
        return ESP_ERR_NOT_FOUND;
    }

    /* Create completion semaphore */
    s_asr_done_sem = xSemaphoreCreateBinary();
    if (!s_asr_done_sem) {
        release_adc_dev();
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize state */
    s_asr_active = true;
    s_asr_error = false;
    s_asr_result[0] = '\0';
    s_stop_requested = false;
    s_recording = true;

    /* Start capture task */
    BaseType_t ok = xTaskCreate(asr_capture_task, "asr_capture",
                                4096, NULL, 5, &s_capture_task);
    if (ok != pdPASS) {
        asr_cleanup();
        snprintf(output, output_size,
                 "Error: failed to start capture task");
        return ESP_ERR_NO_MEM;
    }

    /* If stop was requested before the capture task started (fast tap-and-release),
     * honour it now so the capture loop exits promptly */
    if (s_stop_requested) {
        s_recording = false;
    }

    /* Wait for completion (or timeout in the task) */
    TickType_t total_wait = pdMS_TO_TICKS(s_max_duration_ms +
                                          ASR_RESULT_TIMEOUT_MS + 5000);
    if (xSemaphoreTake(s_asr_done_sem, total_wait) != pdTRUE) {
        asr_cleanup();
        snprintf(output, output_size,
                 "Error: ASR total timeout");
        return ESP_ERR_TIMEOUT;
    }

    /* Cleanup */
    asr_cleanup();

    /* Return result */
    if (s_asr_error || s_asr_result[0] == '\0') {
        strlcpy(output, s_asr_result[0] ? s_asr_result : "Error: no result",
                output_size);
        return ESP_FAIL;
    }

    strlcpy(output, s_asr_result, output_size);
    ESP_LOGI(TAG, "ASR complete: \"%s\"", output);
    return ESP_OK;
}

/* --- descriptors and registration --- */

static const claw_cap_descriptor_t s_asr_descriptors[] = {
    {
        .id = "start_asr",
        .name = "start_asr",
        .family = "voice",
        .description = "Listen to microphone and convert speech to text "
                       "via cloud ASR. Captures audio until silence or "
                       "timeout, returns transcribed text.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
                "\"max_duration_ms\":{"
                    "\"type\":\"integer\","
                    "\"description\":\"Max recording duration in ms (default 10000, max 30000)\""
                "},"
                "\"silence_timeout_ms\":{"
                    "\"type\":\"integer\","
                    "\"description\":\"Auto-stop after silence for this many ms (default 1000, 0 to disable)\""
                "}"
            "},"
            "\"required\":[]}",
        .execute = cap_asr_execute,
    },
};

static const claw_cap_group_t s_asr_group = {
    .group_id = "cap_asr",
    .descriptors = s_asr_descriptors,
    .descriptor_count = sizeof(s_asr_descriptors) / sizeof(s_asr_descriptors[0]),
};

esp_err_t cap_asr_register_group(void)
{
    if (claw_cap_group_exists(s_asr_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_asr_group);
}
