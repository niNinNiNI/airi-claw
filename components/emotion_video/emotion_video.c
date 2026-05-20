/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "emotion_video.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "video_player.h"

static const char *TAG = "emotion_video";

#define VIDEO_BASE_DIR "/sdcard/videos"

static const char *s_emotion_files[EMOTION_VIDEO_COUNT] = {
    [EMOTION_VIDEO_M01_IDLE_SHAKE]  = VIDEO_BASE_DIR "/m01_idle_shake.mp4",
    [EMOTION_VIDEO_M02_SWAY]        = VIDEO_BASE_DIR "/m02_sway.mp4",
    [EMOTION_VIDEO_M03_CALM]        = VIDEO_BASE_DIR "/m03_calm.mp4",
    [EMOTION_VIDEO_M04_WAVE]        = VIDEO_BASE_DIR "/m04_wave.mp4",
    [EMOTION_VIDEO_M05_TIRED]       = VIDEO_BASE_DIR "/m05_tired.mp4",
    [EMOTION_VIDEO_M06_SLIGHT_SWAY] = VIDEO_BASE_DIR "/m06_slight_sway.mp4",
    [EMOTION_VIDEO_M07_BREATHING]   = VIDEO_BASE_DIR "/m07_breathing.mp4",
    [EMOTION_VIDEO_M08_HAND_MOVES]  = VIDEO_BASE_DIR "/m08_hand_moves.mp4",
    [EMOTION_VIDEO_M09_NOD]         = VIDEO_BASE_DIR "/m09_nod.mp4",
    [EMOTION_VIDEO_M10_RELAXED]     = VIDEO_BASE_DIR "/m10_relaxed.mp4",
};

static const char *s_emotion_names[EMOTION_VIDEO_COUNT] = {
    [EMOTION_VIDEO_M01_IDLE_SHAKE]  = "m01_idle_shake",
    [EMOTION_VIDEO_M02_SWAY]        = "m02_sway",
    [EMOTION_VIDEO_M03_CALM]        = "m03_calm",
    [EMOTION_VIDEO_M04_WAVE]        = "m04_wave",
    [EMOTION_VIDEO_M05_TIRED]       = "m05_tired",
    [EMOTION_VIDEO_M06_SLIGHT_SWAY] = "m06_slight_sway",
    [EMOTION_VIDEO_M07_BREATHING]   = "m07_breathing",
    [EMOTION_VIDEO_M08_HAND_MOVES]  = "m08_hand_moves",
    [EMOTION_VIDEO_M09_NOD]         = "m09_nod",
    [EMOTION_VIDEO_M10_RELAXED]     = "m10_relaxed",
};

static emotion_video_t s_current_emotion = EMOTION_VIDEO_M01_IDLE_SHAKE;
static SemaphoreHandle_t s_emotion_lock;

static void ensure_lock(void)
{
    if (!s_emotion_lock) {
        s_emotion_lock = xSemaphoreCreateMutex();
    }
}

esp_err_t emotion_video_set(emotion_video_t emotion)
{
    if (emotion >= EMOTION_VIDEO_COUNT) {
        ESP_LOGE(TAG, "Invalid emotion index: %d", emotion);
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = s_emotion_files[emotion];
    if (!path) {
        ESP_LOGE(TAG, "No file path for emotion %d", emotion);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Setting emotion: %s -> %s", s_emotion_names[emotion], path);

    esp_err_t ret = video_player_play_file(path, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play file: %s (%s)", path, esp_err_to_name(ret));
        return ret;
    }

    ensure_lock();
    if (xSemaphoreTake(s_emotion_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_current_emotion = emotion;
        xSemaphoreGive(s_emotion_lock);
    }

    return ESP_OK;
}

emotion_video_t emotion_video_get_current(void)
{
    emotion_video_t current;

    ensure_lock();
    if (xSemaphoreTake(s_emotion_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        current = s_current_emotion;
        xSemaphoreGive(s_emotion_lock);
    } else {
        current = EMOTION_VIDEO_M01_IDLE_SHAKE;
    }
    return current;
}

esp_err_t emotion_video_stop(void)
{
    ESP_LOGI(TAG, "Stopping emotion video");
    return video_player_stop();
}

const char *emotion_video_get_name(emotion_video_t emotion)
{
    if (emotion >= EMOTION_VIDEO_COUNT) {
        return NULL;
    }
    return s_emotion_names[emotion];
}

emotion_video_t emotion_video_from_name(const char *name)
{
    if (!name) {
        return EMOTION_VIDEO_COUNT;
    }

    for (int i = 0; i < EMOTION_VIDEO_COUNT; i++) {
        if (strcmp(name, s_emotion_names[i]) == 0) {
            return (emotion_video_t)i;
        }
    }

    return EMOTION_VIDEO_COUNT;
}

esp_err_t emotion_video_start_default_cycle(void)
{
    const char *playlist[] = {
        s_emotion_files[EMOTION_VIDEO_M01_IDLE_SHAKE],
        s_emotion_files[EMOTION_VIDEO_M02_SWAY],
        s_emotion_files[EMOTION_VIDEO_M03_CALM],
    };

    ESP_LOGI(TAG, "Starting default emotion cycle: m01 -> m02 -> m03");

    esp_err_t ret = video_player_play_playlist(playlist, 3, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start default cycle: %s", esp_err_to_name(ret));
        return ret;
    }

    ensure_lock();
    if (xSemaphoreTake(s_emotion_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_current_emotion = EMOTION_VIDEO_M01_IDLE_SHAKE;
        xSemaphoreGive(s_emotion_lock);
    }

    return ESP_OK;
}
