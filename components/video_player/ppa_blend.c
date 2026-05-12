/*
 * PPA Hardware Acceleration for Alpha Blending
 * Frame-level fade-in/fade-out transitions for video playback
 */

#include "ppa_blend.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

static const char *TAG = "ppa_blend";

static ppa_client_handle_t s_blend_handle = NULL;
static size_t s_cache_align = 0;

#define PPA_BLEND_ALIGNMENT 64
#define PPA_BLEND_ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

esp_err_t ppa_blend_init(void)
{
    if (s_blend_handle != NULL) {
        return ESP_OK;
    }

    ppa_client_config_t blend_cfg = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = 0,  // Use default
    };
    esp_err_t ret = ppa_register_client(&blend_cfg, &s_blend_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA blend client: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_cache_align == 0) {
        esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_cache_align);
        if (s_cache_align == 0) {
            s_cache_align = PPA_BLEND_ALIGNMENT;
        }
    }

    ESP_LOGI(TAG, "PPA blend client initialized, cache alignment: %d bytes", (int)s_cache_align);
    return ESP_OK;
}

esp_err_t ppa_blend_deinit(void)
{
    if (s_blend_handle != NULL) {
        ppa_unregister_client(s_blend_handle);
        s_blend_handle = NULL;
    }
    return ESP_OK;
}

size_t ppa_blend_get_alignment(void)
{
    if (s_cache_align == 0) {
        return PPA_BLEND_ALIGNMENT;
    }
    return s_cache_align;
}

static void ppa_cache_sync(void *buf, size_t bytes, int flags)
{
    if (!buf || !esp_ptr_external_ram(buf)) {
        return;
    }

    size_t align = s_cache_align;
    uintptr_t addr = (uintptr_t)buf;
    uintptr_t aligned_addr = addr & ~(align - 1);
    size_t total = PPA_BLEND_ALIGN_UP(bytes + (addr - aligned_addr), align);

    esp_cache_msync((void *)aligned_addr, total, flags);
}

esp_err_t ppa_blend_rgb565(const uint8_t *bg_buf, const uint8_t *fg_buf,
                            uint8_t *out_buf, int width, int height, uint8_t alpha)
{
    if (s_blend_handle == NULL) {
        ESP_LOGE(TAG, "PPA blend client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!bg_buf || !fg_buf || !out_buf || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t frame_size = (size_t)width * height * 2;

    ppa_cache_sync((void *)bg_buf, frame_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_cache_sync((void *)fg_buf, frame_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = (uint8_t *)bg_buf,
            .pic_w = width,
            .pic_h = height,
            .block_w = width,
            .block_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
            .yuv_range = 0,
            .yuv_std = 0,
        },
        .in_fg = {
            .buffer = (uint8_t *)fg_buf,
            .pic_w = width,
            .pic_h = height,
            .block_w = width,
            .block_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
            .yuv_range = 0,
            .yuv_std = 0,
        },
        .out = {
            .buffer = out_buf,
            .buffer_size = PPA_BLEND_ALIGN_UP(frame_size, s_cache_align),
            .pic_w = width,
            .pic_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
            .yuv_range = 0,
            .yuv_std = 0,
        },
        .bg_rgb_swap = false,
        .bg_byte_swap = false,
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 255 - alpha,
        .fg_rgb_swap = false,
        .fg_byte_swap = false,
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .fg_alpha_fix_val = alpha,
        .bg_ck_en = false,
        .fg_ck_en = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_blend(s_blend_handle, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA blend failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ppa_cache_sync(out_buf, frame_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    return ESP_OK;
}
