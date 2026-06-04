#include "cap_voice.h"

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
#include "decoder/impl/esp_opus_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "cap_voice";

/* --- server config --- */
static char s_server_url[128];

/* --- WebSocket handlers registered flag --- */
static bool s_ws_handlers_registered;

/* --- Opus decoder --- */
static void *s_opus_dec;
static bool s_opus_ready;

/* --- TTS completion signaling --- */
static SemaphoreHandle_t s_tts_done_sem;
static char s_tts_result_text[256];
static volatile bool s_tts_error;
static volatile bool s_tts_active;

/* binary frame diagnostic counter */
static unsigned s_binary_frame_count;

/* --- audio playback --- */
static esp_codec_dev_handle_t s_codec_dev;
static volatile TaskHandle_t s_playback_task;
static volatile bool s_playback_running;

/* ring buffer for 48kHz stereo 16-bit PCM */
#define PLAYBACK_BUF_SAMPLES (48000 * 8) /* 8 seconds buffer */
static int16_t *s_playback_buf;
static volatile size_t s_playback_write_idx;
static volatile size_t s_playback_read_idx;
static volatile size_t s_playback_available;
static SemaphoreHandle_t s_playback_mutex;

/* --- Voice processing task (offloads heavy work from websocket_task) --- */

/* Queue item: raw Opus frame received from WebSocket */
typedef struct {
    uint8_t *data;
    size_t len;
} opus_frame_t;

static QueueHandle_t s_opus_frame_queue;
static volatile TaskHandle_t s_voice_process_task;

/* Large queue to absorb the TTS server's burst rate.
 * Opus frames are ~20ms audio each; at 24000 Hz mono that's ~480 bytes/frame.
 * 2048 frames = ~41 seconds of audio buffered — sized to hold an entire
 * multi-sentence TTS response delivered in a single burst over WebSocket. */
#define OPUS_FRAME_QUEUE_LEN    2048
#define VOICE_PROCESS_STACK     8192

#define VOICE_DEFAULT_VOICE_TYPE "zh_female_qingxin"
#define WS_CONNECT_TIMEOUT_MS    10000
#define TTS_TIMEOUT_MS           120000
#define OPUS_DECODE_BUF_SIZE     8192

/* --- async TTS request queue --- */
typedef struct {
    char text[2048];
    char voice_type[64];
} tts_request_t;

#define TTS_REQUEST_QUEUE_LEN 2
static QueueHandle_t s_tts_request_queue;
static TaskHandle_t s_tts_playback_task;

/* --- helpers --- */

/*
 * Clean TTS text: strip markdown formatting, code blocks, emoji, URLs
 * so the spoken output sounds natural. The input is modified in-place.
 */
static void clean_tts_text(char *text)
{
    if (!text || !text[0]) return;

    char *out = text;
    char *in = text;
    bool in_code_block = false;
    bool prev_newline = false;

    while (*in) {
        /* Skip fenced code blocks: ``` ... ``` */
        if (strncmp(in, "```", 3) == 0) {
            in_code_block = !in_code_block;
            in += 3;
            while (*in == '`') in++;  /* skip trailing backticks */
            while (*in == '\r' || *in == '\n') in++;
            continue;
        }
        if (in_code_block) {
            in++;
            continue;
        }

        /* Skip inline code backticks */
        if (*in == '`') {
            in++;
            while (*in && *in != '`') in++;
            if (*in == '`') in++;
            continue;
        }

        /* Strip bold/italic markers: ** __ * _ */
        if ((strncmp(in, "**", 2) == 0) || (strncmp(in, "__", 2) == 0)) {
            in += 2;
            continue;
        }
        if (*in == '*' || *in == '_') {
            /* Only strip if not part of a URL */
            if (in == text || *(in-1) == ' ' || *(in-1) == '\n' ||
                (*(in+1) && *(in+1) != ' ' && *(in+1) != '\n')) {
                in++;
                continue;
            }
        }

        /* Strip header markers: # ## ### etc. at line start */
        if (*in == '#' && (in == text || *(in-1) == '\n')) {
            while (*in == '#') in++;
            if (*in == ' ') in++;
            continue;
        }

        /* Strip blockquote: > at line start */
        if (*in == '>' && (in == text || *(in-1) == '\n')) {
            in++;
            if (*in == ' ') in++;
            continue;
        }

        /* Strip horizontal rules: --- *** ___ */
        if ((strncmp(in, "---", 3) == 0 || strncmp(in, "***", 3) == 0 ||
             strncmp(in, "___", 3) == 0) && (in == text || *(in-1) == '\n')) {
            in += 3;
            while (*in == '-' || *in == '*' || *in == '_') in++;
            while (*in == '\r' || *in == '\n') in++;
            continue;
        }

        /* Strip image: ![alt](url) */
        if (strncmp(in, "![", 2) == 0) {
            in += 2;
            while (*in && *in != ']') in++;
            if (*in == ']') in++;
            if (*in == '(') {
                in++;
                while (*in && *in != ')') in++;
                if (*in == ')') in++;
            }
            continue;
        }

        /* Strip link: [text](url) → keep text */
        if (*in == '[') {
            char *link_text = in + 1;
            char *close_bracket = strchr(link_text, ']');
            if (close_bracket && *(close_bracket + 1) == '(') {
                char *close_paren = strchr(close_bracket + 2, ')');
                if (close_paren) {
                    /* Copy link text */
                    while (link_text < close_bracket) {
                        *out++ = *link_text++;
                    }
                    in = close_paren + 1;
                    continue;
                }
            }
        }

        /* Replace standalone URLs with <链接> */
        if (strncmp(in, "http://", 7) == 0 || strncmp(in, "https://", 8) == 0) {
            const char *link = "\347\275\221\345\235\200";  /* "网址" in UTF-8 */
            while (*link) *out++ = *link++;
            while (*in && *in != ' ' && *in != '\n' && *in != '\r') in++;
            continue;
        }

        /* Strip emoji (U+1F600-U+1F9FF and misc symbol ranges) */
        {
            unsigned char c = (unsigned char)*in;
            if (c == 0xF0) {
                /* 4-byte UTF-8 sequence (including emoji) */
                int skip = 1;
                while (skip < 4 && (unsigned char)in[skip] >= 0x80 &&
                       (unsigned char)in[skip] < 0xC0) skip++;
                if (skip == 4) { in += 4; continue; }
            }
            if (c == 0xE2) {
                /* Some emoji/dingbats in U+2600-U+27BF, U+2702-U+27B0 etc */
                if ((unsigned char)in[1] == 0x9C || (unsigned char)in[1] == 0x9D ||
                    (unsigned char)in[1] == 0x9E || (unsigned char)in[1] == 0xA4 ||
                    (unsigned char)in[1] == 0x9A) {
                    in += 3;
                    continue;
                }
            }
        }

        /* Strip HTML tags */
        if (*in == '<') {
            in++;
            while (*in && *in != '>') in++;
            if (*in == '>') in++;
            continue;
        }

        /* Normalize whitespace: collapse multiple spaces/newlines */
        if (*in == '\r') { in++; continue; }
        if (*in == '\n' || *in == ' ') {
            if (!prev_newline) {
                *out++ = (*in == '\n') ? '\n' : ' ';
                prev_newline = true;
            }
            in++;
            continue;
        }

        prev_newline = false;
        *out++ = *in++;
    }

    /* Trim trailing whitespace */
    *out = '\0';
    while (out > text && (*(out-1) == ' ' || *(out-1) == '\n')) {
        *(--out) = '\0';
    }
}

static esp_err_t ensure_audio_dev(void)
{
    void *audio_handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &audio_handle);
    if (ret != ESP_OK || !audio_handle) {
        ESP_LOGW(TAG, "No audio DAC device available");
        return ESP_ERR_NOT_FOUND;
    }

    esp_codec_dev_handle_t dev = *(esp_codec_dev_handle_t *)audio_handle;
    if (!dev) {
        ESP_LOGW(TAG, "Audio DAC handle is NULL");
        return ESP_ERR_NOT_FOUND;
    }

    /* Close first if another component (ASR, video player) left the shared
     * codec open at a different sample rate. Reopen guarantees correct
     * TTS format (48kHz stereo 16-bit). */
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        s_codec_dev = NULL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 48000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    ret = esp_codec_dev_open(dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open audio codec: %d", ret);
        return ESP_FAIL;
    }

    esp_codec_dev_set_out_vol(dev, 80);
    esp_codec_dev_set_out_mute(dev, false); /* unmute in case ASR left it muted */

    s_codec_dev = dev;
    ESP_LOGI(TAG, "Audio codec opened (48kHz stereo 16-bit)");
    return ESP_OK;
}

/*
 * Linear resample: mono src_rate -> stereo 48000 Hz.
 */
static size_t resample_mono_to_stereo_48k(const int16_t *src, size_t src_samples,
                                          int src_rate,
                                          int16_t *dst, size_t dst_capacity)
{
    if (src_rate <= 0 || src_samples == 0 || !dst || dst_capacity < 2) {
        return 0;
    }

    double step = (double)src_rate / 48000.0;
    size_t dst_i = 0;
    size_t max_src_idx = src_samples - 1;

    for (double pos = 0.0; (size_t)pos < src_samples && dst_i + 1 < dst_capacity;
         pos += step) {

        size_t idx0 = (size_t)pos;
        size_t idx1 = idx0 + 1;
        if (idx1 > max_src_idx) idx1 = max_src_idx;

        double frac = pos - (double)idx0;
        int16_t s0 = src[idx0];
        int16_t s1 = src[idx1];
        int16_t val = (int16_t)((double)s0 + frac * (double)(s1 - s0));

        dst[dst_i++] = val; /* L */
        dst[dst_i++] = val; /* R */
    }

    return dst_i;
}

/* --- Opus decoder --- */

static esp_err_t ensure_opus_decoder(void)
{
    if (s_opus_ready) {
        return ESP_OK;
    }

    esp_audio_err_t ret = esp_opus_dec_register();
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to register Opus decoder: %d", ret);
        return ESP_FAIL;
    }

    esp_opus_dec_cfg_t cfg = {
        .sample_rate = 24000,
        .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
        .self_delimited = false,
    };

    ret = esp_opus_dec_open(&cfg, sizeof(cfg), &s_opus_dec);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open Opus decoder: %d", ret);
        return ESP_FAIL;
    }

    s_opus_ready = true;
    ESP_LOGI(TAG, "Opus decoder ready (24kHz mono)");
    return ESP_OK;
}

/* --- playback task (runs independently) --- */

static void playback_task(void *arg)
{
    (void)arg;
    unsigned write_fail_count = 0;

    while (1) {
        if (!s_playback_running) {
            write_fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t avail;
        xSemaphoreTake(s_playback_mutex, portMAX_DELAY);
        avail = s_playback_available;
        xSemaphoreGive(s_playback_mutex);

        if (avail == 0) {
            write_fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t chunk = avail;
        if (chunk > 960) chunk = 960; /* ~10ms @ 48kHz stereo */

        if (s_playback_read_idx + chunk > PLAYBACK_BUF_SAMPLES) {
            chunk = PLAYBACK_BUF_SAMPLES - s_playback_read_idx;
        }

        esp_err_t ret = esp_codec_dev_write(s_codec_dev,
                            (void *)(s_playback_buf + s_playback_read_idx),
                            (int)(chunk * sizeof(int16_t)));
        if (ret == ESP_OK) {
            write_fail_count = 0;
            xSemaphoreTake(s_playback_mutex, portMAX_DELAY);
            s_playback_read_idx = (s_playback_read_idx + chunk) % PLAYBACK_BUF_SAMPLES;
            s_playback_available -= chunk;
            xSemaphoreGive(s_playback_mutex);
        } else {
            write_fail_count++;
            if (write_fail_count <= 3 || write_fail_count % 100 == 0) {
                ESP_LOGE(TAG, "Audio write error: %d (count=%u)", ret, write_fail_count);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static esp_err_t start_playback_task(void)
{
    /* ensure_audio_dev() must be called by the caller (do_tts) before
     * starting the playback task, so s_codec_dev is already valid here. */
    if (!s_codec_dev) {
        ESP_LOGE(TAG, "Audio codec not initialized before playback start");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_playback_buf) {
        s_playback_buf = heap_caps_calloc(1, PLAYBACK_BUF_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
        if (!s_playback_buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_playback_mutex) {
        s_playback_mutex = xSemaphoreCreateMutex();
        if (!s_playback_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Always reset ring buffer state for each TTS playback */
    s_playback_read_idx = 0;
    s_playback_write_idx = 0;
    s_playback_available = 0;
    s_playback_running = true;

    if (s_playback_task) {
        return ESP_OK; /* reuse existing task, it watches s_playback_running */
    }

    BaseType_t ok = xTaskCreateWithCaps(playback_task, "voice_play",
                                        5120, NULL, 5,
                                        (TaskHandle_t *)&s_playback_task,
                                        MALLOC_CAP_SPIRAM);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

/*
 * Push 48kHz stereo PCM into the ring buffer.
 * Non-blocking: returns immediately, data plays asynchronously.
 */
static esp_err_t push_pcm_async(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) {
        return ESP_OK;
    }

    /* Guard against use-after-free during cleanup */
    if (!s_playback_mutex || !s_playback_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Push with backpressure: wait briefly if buffer is full instead of
     * dropping audio. Includes a total timeout to prevent stalls. */
    size_t remaining = samples;
    size_t src_off = 0;
    TickType_t start_ticks = xTaskGetTickCount();
    const TickType_t push_timeout_ticks = pdMS_TO_TICKS(5000);

    while (remaining > 0) {
        if (!s_playback_running) {
            ESP_LOGW(TAG, "push_pcm_async: playback stopped, aborting");
            return ESP_ERR_INVALID_STATE;
        }
        if ((xTaskGetTickCount() - start_ticks) >= push_timeout_ticks) {
            ESP_LOGW(TAG, "push_pcm_async: timeout waiting for buffer space");
            return ESP_ERR_TIMEOUT;
        }

        xSemaphoreTake(s_playback_mutex, portMAX_DELAY);
        size_t space = PLAYBACK_BUF_SAMPLES - s_playback_available;
        size_t chunk = remaining;
        if (chunk > space) chunk = space;
        if (chunk > PLAYBACK_BUF_SAMPLES - s_playback_write_idx) {
            chunk = PLAYBACK_BUF_SAMPLES - s_playback_write_idx;
        }
        xSemaphoreGive(s_playback_mutex);

        if (chunk == 0) {
            if (!s_playback_running) {
                return ESP_ERR_INVALID_STATE;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        xSemaphoreTake(s_playback_mutex, portMAX_DELAY);
        memcpy(s_playback_buf + s_playback_write_idx,
               pcm + src_off, chunk * sizeof(int16_t));
        s_playback_write_idx = (s_playback_write_idx + chunk) % PLAYBACK_BUF_SAMPLES;
        s_playback_available += chunk;
        xSemaphoreGive(s_playback_mutex);

        remaining -= chunk;
        src_off += chunk;
    }

    return ESP_OK;
}

/*
 * Voice processing task: receives raw Opus frames from the WebSocket handler
 * via a queue, decodes, resamples, and pushes to the playback ring buffer.
 *
 * Critical design notes:
 * - Each Opus frame decode + resample takes ~2-5ms of CPU on ESP32-P4.
 * - The TTS server may send frames back-to-back faster than we can decode,
 *   so we use a generous queue depth (2048) to absorb bursts.
 * - To avoid watchdog timeout: we process at most 10 contiguous frames,
 *   then yield for 50ms — long enough for the IDLE task to reset the WDT.
 * - Audio is pushed into a ring buffer; the playback_task drains it
 *   independently to the codec on the other core.
 */
static void voice_process_task(void *arg)
{
    (void)arg;
    opus_frame_t frame;

    /* Ensure decoder is ready */
    if (ensure_opus_decoder() != ESP_OK) {
        ESP_LOGE(TAG, "Voice process: Opus decoder init failed");
        s_voice_process_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* ================================================================
         * Batch processing: process up to 10 frames, then yield for 50ms
         * to let the IDLE task reset the watchdog timer.
         *
         * Why 10? Each decode + resample takes ~2-5ms, so 10 frames is
         * ~20-50ms of CPU time — well within the 5-second watchdog limit,
         * even with worst-case scheduling delays.
         *
         * Why 50ms? Long enough for the IDLE task on the same core to run
         * and reset the task WDT (which runs at 1 Hz in IDLE0).
         * ================================================================ */
        int frames_this_batch = 0;

        while (frames_this_batch < 10) {
            if (xQueueReceive(s_opus_frame_queue, &frame, 0) != pdTRUE) {
                /* Queue empty — nothing to do right now. Wait for data. */
                if (frames_this_batch == 0) {
                    /* Only block if we haven't done any work in this batch */
                    xQueueReceive(s_opus_frame_queue, &frame, portMAX_DELAY);
                } else {
                    /* End this batch early since queue is empty */
                    break;
                }
            }

            if (!frame.data || frame.len == 0) {
                free(frame.data);
                continue;
            }

            frames_this_batch++;

            /* Decode Opus frame */
            uint8_t *decode_buf = malloc(OPUS_DECODE_BUF_SIZE);
            if (!decode_buf) {
                ESP_LOGE(TAG, "Voice process: decode buffer alloc failed");
                free(frame.data);
                continue;
            }

            esp_audio_dec_in_raw_t raw = {
                .buffer = frame.data,
                .len = (uint32_t)frame.len,
            };
            esp_audio_dec_out_frame_t out_frame = {
                .buffer = decode_buf,
                .len = OPUS_DECODE_BUF_SIZE,
            };
            esp_audio_dec_info_t dec_info;

            esp_audio_err_t ret = esp_opus_dec_decode(s_opus_dec, &raw, &out_frame, &dec_info);
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Voice process: Opus decode failed: %d", ret);
                free(decode_buf);
                free(frame.data);
                continue;
            }

            if (out_frame.decoded_size > 0) {
                size_t pcm_samples = out_frame.decoded_size / sizeof(int16_t);

                /* Resample to 48kHz stereo and push to ring buffer */
                size_t max_dst = (size_t)((double)pcm_samples * 48000.0 / 24000.0 + 10) * 2;
                int16_t *playback_pcm = heap_caps_calloc(1, max_dst * sizeof(int16_t),
                                                         MALLOC_CAP_SPIRAM);
                if (playback_pcm) {
                    size_t out_samples = resample_mono_to_stereo_48k(
                        (const int16_t *)decode_buf, pcm_samples, 24000,
                        playback_pcm, max_dst);
                    if (out_samples > 0) {
                        push_pcm_async(playback_pcm, out_samples);
                    }
                    free(playback_pcm);
                }
            }

            free(decode_buf);
            free(frame.data);
        }

        /* Yield briefly so the IDLE task can reset the task WDT.
         * One tick (~10ms at 100 Hz) is sufficient; the WDT timeout is
         * 5 seconds, and this batch processed at most 10 frames. */
        vTaskDelay(1);
    }
}

/*
 * Release all voice resources so the shared audio codec device and DRAM
 * (task stacks, TCBs) are available for other components (video player etc.).
 *
 * If graceful is true, wait for remaining audio to play out before stopping.
 * If false, stop immediately (used on error / timeout paths).
 */
static void voice_cleanup(bool graceful)
{
    if (graceful) {
        /*
         * Graceful shutdown: let the entire pipeline drain in order.
         *
         * Pipeline: Opus queue → voice_process_task → ring buffer → playback_task → codec
         *
         * 1. Wait for all queued Opus frames to be picked up by voice_process_task.
         * 2. Wait a bit more for the last decoded frames to land in the ring buffer.
         * 3. Wait for the ring buffer to be fully drained by playback_task.
         * 4. Only then stop playback and release resources.
         *
         * playback_task must remain RUNNING through step 3, otherwise the ring
         * buffer never empties and remaining audio is silently discarded.
         */

        /* Step 1: drain Opus frame queue */
        if (s_opus_frame_queue) {
            int patience = 200; /* 200 * 50ms = 10 seconds max */
            while (uxQueueMessagesWaiting(s_opus_frame_queue) > 0 && patience-- > 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (patience <= 0) {
                ESP_LOGW(TAG, "Opus queue drain timed out, %u frames remaining",
                         (unsigned)uxQueueMessagesWaiting(s_opus_frame_queue));
            }
        }

        /* Step 2: let voice_process_task finish decoding the last frame(s).
         * One tick is enough — the task processes in batches and yields every
         * 10 frames + 1 tick, so any in-flight decode completes quickly. */
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Step 3: wait for ring buffer to empty while playback_task is still running.
         * 8-second buffer × 5 consecutive empty reads = up to ~8.5 seconds. */
        {
            int empty_count = 0;
            int patience = 200; /* 200 * 50ms = 10 seconds max */
            while (empty_count < 5 && patience-- > 0) {
                if (s_playback_mutex) {
                    xSemaphoreTake(s_playback_mutex, portMAX_DELAY);
                    size_t avail = s_playback_available;
                    xSemaphoreGive(s_playback_mutex);
                    if (avail == 0) {
                        empty_count++;
                    } else {
                        empty_count = 0;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (patience <= 0) {
                ESP_LOGW(TAG, "Ring buffer drain timed out, %u samples remaining",
                         (unsigned)s_playback_available);
            }
        }
    }

    /* Now safe to stop playback */
    s_playback_running = false;

    /* Discard any leftover Opus frames */
    if (s_opus_frame_queue) {
        opus_frame_t leftover;
        while (xQueueReceive(s_opus_frame_queue, &leftover, 0)) {
            free(leftover.data);
        }
    }

    /* Reset ring buffer indices */
    s_playback_read_idx = 0;
    s_playback_write_idx = 0;
    s_playback_available = 0;

    /* Close shared audio codec so other components can use it */
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        s_codec_dev = NULL;
    }

    s_tts_active = false;
    ESP_LOGI(TAG, "Voice playback complete");
}

static esp_err_t start_voice_process_task(void)
{
    if (!s_opus_frame_queue) {
        s_opus_frame_queue = xQueueCreate(OPUS_FRAME_QUEUE_LEN, sizeof(opus_frame_t));
        if (!s_opus_frame_queue) {
            ESP_LOGE(TAG, "Failed to create Opus frame queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_voice_process_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreateWithCaps(voice_process_task, "voice_proc",
                                        VOICE_PROCESS_STACK, NULL, 6,
                                        (TaskHandle_t *)&s_voice_process_task,
                                        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create voice process task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* --- WebSocket callbacks (registered once via voice_ws_common) --- */

static void tts_text_handler(const char *type, const char *json,
                             void *user_data)
{
    (void)user_data;
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    if (strcmp(type, "tts_end") == 0 && s_tts_active) {
        unsigned queued = s_opus_frame_queue ?
            (unsigned)uxQueueMessagesWaiting(s_opus_frame_queue) : 0;
        ESP_LOGI(TAG, "tts_end received (queue depth: %u), signaling completion", queued);
        snprintf(s_tts_result_text, sizeof(s_tts_result_text),
                 "Done: tts complete");
        s_tts_error = false;
        s_tts_active = false;
        xSemaphoreGive(s_tts_done_sem);
    } else if (strcmp(type, "error") == 0 && s_tts_active) {
        cJSON *msg_item = cJSON_GetObjectItem(root, "message");
        const char *err_str = cJSON_IsString(msg_item) ?
            msg_item->valuestring : "unknown error";
        snprintf(s_tts_result_text, sizeof(s_tts_result_text),
                 "Error: %s", err_str);
        s_tts_error = true;
        s_tts_active = false;
        xSemaphoreGive(s_tts_done_sem);
    } else if (strcmp(type, "status") == 0) {
        cJSON *msg_item = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(msg_item)) {
            ESP_LOGI(TAG, "Server status: %s", msg_item->valuestring);
        }
    } else if (strcmp(type, "pong") == 0) {
        ESP_LOGD(TAG, "Pong received");
    } else if (strcmp(type, "tts_start") == 0) {
        cJSON *sr = cJSON_GetObjectItem(root, "sample_rate");
        cJSON *ch = cJSON_GetObjectItem(root, "channels");
        int rate = (cJSON_IsNumber(sr)) ? sr->valueint : 0;
        int channels = (cJSON_IsNumber(ch)) ? ch->valueint : 0;
        if (rate != 0 && rate != 24000) {
            ESP_LOGW(TAG, "tts_start: unexpected sample_rate %d (expected 24000)", rate);
        }
        if (channels != 0 && channels != 1) {
            ESP_LOGW(TAG, "tts_start: unexpected channels %d (expected 1)", channels);
        }
        ESP_LOGI(TAG, "tts_start received: rate=%d ch=%d", rate, channels);
    }
    cJSON_Delete(root);
}

static void tts_binary_handler(const uint8_t *data, size_t len,
                               void *user_data)
{
    (void)user_data;
    if (!s_tts_active || !data || len <= 0) {
        return;
    }

    if (len < 4 || len > 65536) {
        ESP_LOGW(TAG, "Skipping binary frame: unexpected size %d", (int)len);
        return;
    }

    /* Skip Ogg/Opus container header packets */
    if (len >= 8 &&
        (memcmp(data, "OpusHead", 8) == 0 ||
         memcmp(data, "OpusTags", 8) == 0)) {
        ESP_LOGI(TAG, "Skipping Opus container header (%.8s)", (const char *)data);
        return;
    }

    s_binary_frame_count++;
    if (s_binary_frame_count <= 5 || s_binary_frame_count % 50 == 0) {
        ESP_LOGI(TAG, "Binary frame #%u: %d bytes (queue depth: %u)",
                 s_binary_frame_count, (int)len,
                 s_opus_frame_queue ? (unsigned)uxQueueMessagesWaiting(s_opus_frame_queue) : 0);
    }

    uint8_t *frame_buf = malloc(len);
    if (!frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate Opus frame buffer");
        return;
    }
    memcpy(frame_buf, data, len);

    opus_frame_t frame = {
        .data = frame_buf,
        .len = len,
    };

    if (xQueueSend(s_opus_frame_queue, &frame, pdMS_TO_TICKS(5)) != pdTRUE) {
        ESP_LOGW(TAG, "Opus frame queue full (%u/%u), dropping frame",
                 (unsigned)OPUS_FRAME_QUEUE_LEN,
                 (unsigned)uxQueueMessagesWaiting(s_opus_frame_queue));
        free(frame_buf);
    }
}

static void tts_state_handler(bool connected, void *user_data)
{
    (void)user_data;
    if (!connected && s_tts_active) {
        s_tts_error = true;
        xSemaphoreGive(s_tts_done_sem);
    }
}

/* --- WebSocket connection management (delegated to voice_ws_common) --- */

static esp_err_t ensure_ws_connected(void)
{
    /* Register TTS callbacks once per session — must happen before the
     * early-return below, otherwise the handlers are never installed when
     * the WebSocket was already connected by a prior capability (e.g. ASR). */
    if (!s_ws_handlers_registered) {
        voice_ws_register_text_handler(tts_text_handler, NULL);
        voice_ws_register_binary_handler(tts_binary_handler, NULL);
        voice_ws_register_state_handler(tts_state_handler, NULL);
        s_ws_handlers_registered = true;
    }

    if (voice_ws_is_connected()) {
        return ESP_OK;
    }

    if (s_server_url[0] == '\0') {
        ESP_LOGE(TAG, "Server URL not configured");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = voice_ws_connect(WS_CONNECT_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

/* --- TTS request --- */

static esp_err_t send_tts_request(const char *text, const char *voice_type)
{
    if (!voice_ws_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "type", "tts_request");
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "voice",
                            voice_type ? voice_type : VOICE_DEFAULT_VOICE_TYPE);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    esp_err_t err = voice_ws_send_text(json_str);
    free(json_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send TTS request: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS request sent: \"%s\"", text);
    return ESP_OK;
}

/* --- core TTS logic (shared by sync and async paths) --- */

static esp_err_t do_tts(const char *text, const char *voice_type,
                        char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!text || !text[0]) {
        snprintf(output, output_size, "Error: text is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_server_url[0] == '\0') {
        snprintf(output, output_size,
                 "Error: voice server not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (ensure_ws_connected() != ESP_OK) {
        snprintf(output, output_size,
                 "Error: WebSocket connection failed");
        return ESP_ERR_INVALID_STATE;
    }

    /* Start voice process task early so it's ready to receive Opus frames */
    if (start_voice_process_task() != ESP_OK) {
        snprintf(output, output_size,
                 "Error: failed to start voice processing task");
        return ESP_FAIL;
    }

    if (ensure_audio_dev() != ESP_OK) {
        snprintf(output, output_size,
                 "Error: no audio output device available");
        return ESP_ERR_NOT_FOUND;
    }

    char text_copy[2048];
    strlcpy(text_copy, text, sizeof(text_copy));
    clean_tts_text(text_copy);

    if (!text_copy[0]) {
        snprintf(output, output_size, "Error: no speakable text after cleaning");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "TTS speak: \"%s\"", text_copy);

    /* ensure playback task is running and active */
    esp_err_t err = start_playback_task();
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: failed to start playback (%s)", esp_err_to_name(err));
        return err;
    }

    /* create completion semaphore if first use */
    if (!s_tts_done_sem) {
        s_tts_done_sem = xSemaphoreCreateBinary();
        if (!s_tts_done_sem) {
            snprintf(output, output_size, "Error: out of memory");
            return ESP_ERR_NO_MEM;
        }
    }

    /* mark TTS as active and send request */
    s_tts_active = true;
    s_tts_error = false;
    s_tts_result_text[0] = '\0';
    s_binary_frame_count = 0;

    err = send_tts_request(text_copy, voice_type);
    if (err != ESP_OK) {
        s_tts_active = false;
        snprintf(output, output_size,
                 "Error: failed to send TTS request (%s)", esp_err_to_name(err));
        voice_cleanup(false);
        return err;
    }

    /* wait for tts_end or error */
    TickType_t wait_ticks = pdMS_TO_TICKS(TTS_TIMEOUT_MS);
    if (xSemaphoreTake(s_tts_done_sem, wait_ticks) != pdTRUE) {
        s_tts_active = false;
        snprintf(output, output_size,
                 "Error: TTS request timed out");
        voice_cleanup(false);
        return ESP_ERR_TIMEOUT;
    }

    if (s_tts_error) {
        snprintf(output, output_size, "%s", s_tts_result_text);
        voice_cleanup(false);
        return ESP_FAIL;
    }

    /* Normal completion — let remaining audio play out, then release resources */
    voice_cleanup(true);

    strlcpy(output, s_tts_result_text, output_size);
    ESP_LOGI(TAG, "TTS playback complete");
    return ESP_OK;
}

/* --- async TTS playback task --- */

static void tts_playback_task_fn(void *arg)
{
    (void)arg;
    tts_request_t req;

    while (1) {
        if (xQueueReceive(s_tts_request_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Drain any stale requests — only play the latest */
        tts_request_t next;
        while (xQueueReceive(s_tts_request_queue, &next, 0) == pdTRUE) {
            ESP_LOGI(TAG, "TTS skipping stale request: \"%s\"", req.text);
            req = next;
        }

        char output[256];
        esp_err_t err = do_tts(req.text, req.voice_type, output, sizeof(output));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Async TTS failed: %s", output);
        }
    }
}

/* --- execute callbacks --- */

static esp_err_t cap_voice_execute(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output, size_t output_size)
{
    (void)ctx;

    cJSON *input = cJSON_Parse(input_json ? input_json : "{}");
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *text_item = cJSON_GetObjectItem(input, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring ||
        !text_item->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: text is required");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *voice_item = cJSON_GetObjectItem(input, "voice");
    const char *voice_type = cJSON_IsString(voice_item) ?
                             voice_item->valuestring : NULL;

    char text_copy[2048];
    strlcpy(text_copy, text_item->valuestring, sizeof(text_copy));
    cJSON_Delete(input);

    return do_tts(text_copy, voice_type, output, output_size);
}

/*
 * Non-blocking TTS: posts the request to a playback queue and returns
 * immediately. Used by the Event Router's voice channel outbound rule
 * to avoid blocking the router task during audio playback.
 */
static esp_err_t cap_voice_send_message_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output, size_t output_size)
{
    (void)ctx;

    cJSON *input = cJSON_Parse(input_json ? input_json : "{}");
    if (!input) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    /* Accept both "text" and "message" fields for compatibility */
    cJSON *text_item = cJSON_GetObjectItem(input, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring) {
        text_item = cJSON_GetObjectItem(input, "message");
    }
    if (!cJSON_IsString(text_item) || !text_item->valuestring ||
        !text_item->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: text is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_tts_request_queue || !s_tts_playback_task) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: TTS task not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    tts_request_t req;
    strlcpy(req.text, text_item->valuestring, sizeof(req.text));

    cJSON *voice_item = cJSON_GetObjectItem(input, "voice");
    if (cJSON_IsString(voice_item) && voice_item->valuestring) {
        strlcpy(req.voice_type, voice_item->valuestring, sizeof(req.voice_type));
    } else {
        req.voice_type[0] = '\0';
    }

    cJSON_Delete(input);

    /* Non-blocking: if queue is full, drop oldest to make room for latest */
    tts_request_t drop;
    while (xQueueSend(s_tts_request_queue, &req, 0) != pdTRUE) {
        xQueueReceive(s_tts_request_queue, &drop, 0);
    }

    snprintf(output, output_size, "Done: tts queued");
    ESP_LOGI(TAG, "Async TTS queued: \"%s\"", req.text);
    return ESP_OK;
}

/* --- descriptors and registration --- */

static const claw_cap_descriptor_t s_voice_descriptors[] = {
    {
        .id = "get_tts",
        .name = "get_tts",
        .family = "voice",
        .description = "Convert text to speech via local WebSocket TTS server and play through the speaker.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
                "\"text\":{\"type\":\"string\",\"description\":\"Text to speak\"},"
                "\"voice\":{\"type\":\"string\",\"description\":\"Voice type ID\"}"
            "},"
            "\"required\":[\"text\"]}",
        .execute = cap_voice_execute,
    },
    {
        .id = "voice_send_message",
        .name = "voice_send_message",
        .family = "voice",
        .description = "Send a voice reply via TTS (non-blocking). Posts text to the TTS playback queue and returns immediately.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
                "\"text\":{\"type\":\"string\",\"description\":\"Text to speak\"},"
                "\"message\":{\"type\":\"string\",\"description\":\"Alternative text field\"},"
                "\"voice\":{\"type\":\"string\",\"description\":\"Voice type ID\"}"
            "},"
            "\"required\":[\"text\"]}",
        .execute = cap_voice_send_message_execute,
    },
};

static const claw_cap_group_t s_voice_group = {
    .group_id = "cap_voice",
    .descriptors = s_voice_descriptors,
    .descriptor_count = sizeof(s_voice_descriptors) / sizeof(s_voice_descriptors[0]),
};

esp_err_t cap_voice_register_group(void)
{
    if (claw_cap_group_exists(s_voice_group.group_id)) {
        return ESP_OK;
    }

    esp_err_t err = claw_cap_register_group(&s_voice_group);
    if (err != ESP_OK) {
        return err;
    }

    /* Initialize async TTS infrastructure */
    if (!s_tts_request_queue) {
        s_tts_request_queue = xQueueCreate(TTS_REQUEST_QUEUE_LEN,
                                           sizeof(tts_request_t));
        if (!s_tts_request_queue) {
            ESP_LOGE(TAG, "Failed to create TTS request queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_tts_playback_task) {
        BaseType_t ok = xTaskCreateWithCaps(tts_playback_task_fn, "tts_play",
                                            8192, NULL, 5, &s_tts_playback_task,
                                            MALLOC_CAP_SPIRAM);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create TTS playback task");
            vQueueDelete(s_tts_request_queue);
            s_tts_request_queue = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Async TTS task started");
    }

    return ESP_OK;
}

esp_err_t cap_voice_set_server(const char *server_ip, uint16_t server_port)
{
    if (!server_ip || !server_ip[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_server_url, sizeof(s_server_url), "ws://%s:%u", server_ip,
             (unsigned)server_port);
    ESP_LOGI(TAG, "Voice TTS server: %s", s_server_url);

    /* Also configure the shared WebSocket layer so cap_asr can use it */
    voice_ws_set_server(server_ip, server_port);
    return ESP_OK;
}
