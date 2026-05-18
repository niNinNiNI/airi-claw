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

typedef void (*video_player_button_cb_t)(void *user_data);

esp_err_t video_player_init(void);
esp_err_t video_player_start(void);
esp_err_t video_player_stop(void);

/**
 * @brief Register a callback for on-screen button press during video playback.
 * Called from the touch task context — keep it short and non-blocking.
 */
esp_err_t video_player_set_button_callback(video_player_button_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
