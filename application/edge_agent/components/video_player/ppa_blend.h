/*
 * PPA Hardware Acceleration for Alpha Blending
 * Frame-level fade-in/fade-out transitions for video playback
 */

#ifndef PPA_BLEND_H
#define PPA_BLEND_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize PPA blend client
 *
 * @return ESP_OK on success
 */
esp_err_t ppa_blend_init(void);

/**
 * @brief Deinitialize PPA blend client
 *
 * @return ESP_OK on success
 */
esp_err_t ppa_blend_deinit(void);

/**
 * @brief Blend two RGB565 buffers with given alpha value using PPA hardware
 *
 * Performs: out = bg * (1 - alpha/255) + fg * (alpha/255)
 *
 * @param bg_buf Background buffer (fade out frame)
 * @param fg_buf Foreground buffer (fade in frame)
 * @param out_buf Output buffer for blended result
 * @param width Width in pixels
 * @param height Height in pixels
 * @param alpha Alpha value (0-255), 0 = only bg, 255 = only fg
 *
 * @return ESP_OK on success
 */
esp_err_t ppa_blend_rgb565(const uint8_t *bg_buf, const uint8_t *fg_buf,
                            uint8_t *out_buf, int width, int height, uint8_t alpha);

/**
 * @brief Get PPA cache alignment requirement
 *
 * @return Alignment in bytes
 */
size_t ppa_blend_get_alignment(void);

#ifdef __cplusplus
}
#endif

#endif // PPA_BLEND_H
