/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMOTION_IDLE_SHAKE   = 0,
    EMOTION_SWAY         = 1,
    EMOTION_CALM         = 2,
    EMOTION_WAVE         = 3,
    EMOTION_TIRED        = 4,
    EMOTION_SLIGHT_SWAY  = 5,
    EMOTION_BREATHING    = 6,
    EMOTION_HAND_MOVES   = 7,
    EMOTION_NOD          = 8,
    EMOTION_RELAXED      = 9,
    EMOTION_COUNT        = 10,
} emotion_video_t;

/**
 * @brief Initialize the emotion video controller.
 * Scans the video directory and validates the mapping table.
 *
 * @param video_dir  Path to the directory containing emotion video files
 * @return ESP_OK on success
 */
esp_err_t emotion_video_controller_init(const char *video_dir);

/**
 * @brief Start playback of the default emotion (idle_shake).
 * Calls emotion_video_set(EMOTION_IDLE_SHAKE) internally.
 *
 * @return ESP_OK on success
 */
esp_err_t emotion_video_start_default(void);

/**
 * @brief Switch playback to a specific emotion video.
 *
 * @param emotion  The target emotion enum
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if the video file is missing
 */
esp_err_t emotion_video_set(emotion_video_t emotion);

/**
 * @brief Get the currently active emotion.
 *
 * @return Current emotion enum value
 */
emotion_video_t emotion_video_get(void);

/**
 * @brief Convert emotion enum to human-readable string.
 *
 * @param e  Emotion enum value
 * @return String name (e.g. "idle_shake"), or "unknown" for invalid values
 */
const char *emotion_video_to_string(emotion_video_t e);

/**
 * @brief Convert human-readable string to emotion enum.
 *
 * @param name  Emotion name (e.g. "idle_shake", "wave")
 * @return Emotion enum value, or EMOTION_COUNT if unknown
 */
emotion_video_t emotion_video_from_string(const char *name);

/**
 * @brief Register the emotion capability group for LLM tool visibility.
 * Must only be called after claw_cap_init().
 *
 * @return ESP_OK on success
 */
esp_err_t emotion_video_register_capabilities(void);

/**
 * @brief Register the Lua emotion module (set_emotion / get_emotion).
 *
 * @return ESP_OK on success
 */
esp_err_t emotion_video_register_lua(void);

#ifdef __cplusplus
}
#endif
