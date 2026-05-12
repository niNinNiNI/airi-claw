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

esp_err_t video_player_init(void);
esp_err_t video_player_start(void);
esp_err_t video_player_stop(void);

#ifdef __cplusplus
}
#endif
