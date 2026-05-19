/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMOTION_VIDEO_M01_IDLE_SHAKE  = 0,
    EMOTION_VIDEO_M02_SWAY        = 1,
    EMOTION_VIDEO_M03_CALM        = 2,
    EMOTION_VIDEO_M04_WAVE        = 3,
    EMOTION_VIDEO_M05_TIRED       = 4,
    EMOTION_VIDEO_M06_SLIGHT_SWAY = 5,
    EMOTION_VIDEO_M07_BREATHING   = 6,
    EMOTION_VIDEO_M08_HAND_MOVES  = 7,
    EMOTION_VIDEO_M09_NOD         = 8,
    EMOTION_VIDEO_M10_RELAXED     = 9,
    EMOTION_VIDEO_COUNT           = 10,
} emotion_video_t;

/**
 * @brief Switch to the specified emotion video (loops indefinitely).
 *
 * @param emotion  Emotion enum value
 * @return ESP_OK on success
 */
esp_err_t emotion_video_set(emotion_video_t emotion);

/**
 * @brief Get the currently playing emotion.
 *
 * @return Current emotion enum value
 */
emotion_video_t emotion_video_get_current(void);

/**
 * @brief Stop emotion video playback and release the display.
 *
 * @return ESP_OK on success
 */
esp_err_t emotion_video_stop(void);

/**
 * @brief Get the human-readable name for an emotion.
 *
 * @param emotion  Emotion enum value
 * @return String name (e.g., "m01_idle_shake") or NULL if invalid
 */
const char *emotion_video_get_name(emotion_video_t emotion);

/**
 * @brief Look up an emotion enum by name.
 *
 * @param name  Emotion name string (e.g., "m04_wave")
 * @return Matching emotion enum, or EMOTION_VIDEO_COUNT if not found
 */
emotion_video_t emotion_video_from_name(const char *name);

#ifdef __cplusplus
}
#endif
