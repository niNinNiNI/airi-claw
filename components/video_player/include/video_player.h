/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*video_player_button_cb_t)(void *user_data);

esp_err_t video_player_init(void);
esp_err_t video_player_start(void);
esp_err_t video_player_stop(void);

/**
 * @brief Play a specific video file, optionally looping.
 * If a video is already playing, switches to the new file.
 *
 * @param path  Full path to the video file (e.g., "/sdcard/videos/m01_idle_shake.mp4")
 * @param loop  If true, replay the same file indefinitely; if false, play once and stop.
 * @return ESP_OK on success
 */
esp_err_t video_player_play_file(const char *path, bool loop);

/**
 * @brief Switch to a new video file immediately.
 * Shortcut for video_player_play_file(path, true).
 *
 * @param path  Full path to the video file
 * @return ESP_OK on success
 */
esp_err_t video_player_switch_to(const char *path);

/**
 * @brief Play a list of video files in sequence, optionally looping the entire playlist.
 * If a video is already playing, switches to the first file in the playlist.
 *
 * @param paths  Array of full paths to video files
 * @param count  Number of paths in the array
 * @param loop   If true, replay the entire playlist indefinitely; if false, play once and stop.
 * @return ESP_OK on success
 */
esp_err_t video_player_play_playlist(const char **paths, int count, bool loop);

/**
 * @brief Get the path of the currently playing file.
 *
 * @param path      Output buffer for the path string
 * @param path_size Size of the output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if nothing is playing
 */
esp_err_t video_player_get_current(char *path, size_t path_size);

/**
 * @brief Register a callback for on-screen button press during video playback.
 * Called from the touch task context — keep it short and non-blocking.
 */
esp_err_t video_player_set_button_callback(video_player_button_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
