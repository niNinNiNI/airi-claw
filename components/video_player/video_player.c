/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "video_player.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app_stream_adapter.h"
#include "display_arbiter.h"
#include "esp_board_manager_includes.h"
#include "esp_cache.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

static const char *TAG = "video_player";

#define DISPLAY_BUFFER_SIZE (480 * 800 * 2)  /* RGB565, match LCD resolution */

#define MAX_PLAYLIST_ITEMS  50
#define MAX_FILENAME_LEN    512

#define SD_MOUNT_POINT  "/sdcard"

/* ---- static state ---- */
static esp_lcd_panel_handle_t s_panel;
static void *s_lcd_buffer[2];
static SemaphoreHandle_t s_vsync_sem;
static app_stream_adapter_handle_t s_stream_adapter;
static esp_codec_dev_handle_t s_audio_dev;

static char *s_playlist[MAX_PLAYLIST_ITEMS];
static int s_playlist_count;
static int s_current_index;

static TaskHandle_t s_video_task_handle;
static bool s_running;

/* ---- helpers ---- */
static bool is_video_file(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (ext == NULL) {
        return false;
    }
    return (strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".avi") == 0);
}

static void playlist_cleanup(void)
{
    for (int i = 0; i < s_playlist_count; i++) {
        free(s_playlist[i]);
        s_playlist[i] = NULL;
    }
    s_playlist_count = 0;
}

static esp_err_t scan_media_files(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dir_path);
        return ESP_FAIL;
    }

    playlist_cleanup();

    struct dirent *entry;
    char full_path[MAX_FILENAME_LEN];
    size_t dir_len = strlen(dir_path);

    ESP_LOGD(TAG, "Scanning directory: %s", dir_path);

    while ((entry = readdir(dir)) != NULL && s_playlist_count < MAX_PLAYLIST_ITEMS) {
        if (entry->d_type == DT_REG && is_video_file(entry->d_name)) {
            size_t name_len = strlen(entry->d_name);
            if (dir_len + 1 + name_len < MAX_FILENAME_LEN) {
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

                struct stat st;
                if (stat(full_path, &st) == 0 && st.st_size > 0) {
                    s_playlist[s_playlist_count] = strdup(full_path);
                    if (s_playlist[s_playlist_count] != NULL) {
                        ESP_LOGD(TAG, "  [%d] %s", s_playlist_count, entry->d_name);
                        s_playlist_count++;
                    }
                }
            }
        }
    }

    closedir(dir);

    if (s_playlist_count == 0) {
        ESP_LOGW(TAG, "No video files found in directory");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Found %d video file(s)", s_playlist_count);
    return ESP_OK;
}

static bool vsync_ready_callback(esp_lcd_panel_handle_t panel,
                                 esp_lcd_dpi_panel_event_data_t *edata,
                                 void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &task_woken);
    }
    return false;
}

static esp_err_t display_decoded_frame(uint8_t *buffer, uint32_t buffer_size,
                                       uint32_t width, uint32_t height,
                                       uint32_t buffer_index, void *user_data)
{
    if (!display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
        return ESP_OK;
    }

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, width, height, buffer);

    xSemaphoreTake(s_vsync_sem, 0);
    xSemaphoreTake(s_vsync_sem, portMAX_DELAY);

    return ESP_OK;
}

static void video_task(void *arg)
{
    uint32_t total_play_count = 0;

    ESP_LOGD(TAG, "Video task started");

    display_arbiter_acquire(DISPLAY_ARBITER_OWNER_VIDEO);

    while (s_running && display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
        const char *current_file = s_playlist[s_current_index];
        total_play_count++;

        ESP_LOGI(TAG, "=== Playing [%d/%d] #%u: %s ===",
                 s_current_index + 1, s_playlist_count,
                 total_play_count, strrchr(current_file, '/') + 1);

        esp_err_t ret = app_stream_adapter_set_file(s_stream_adapter, current_file,
                                                     s_audio_dev != NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set file: %s, moving to next", current_file);
            goto next_file;
        }

        uint32_t width, height, fps, duration;
        ret = app_stream_adapter_get_info(s_stream_adapter, &width, &height, &fps, &duration);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Media info: %"PRIu32"x%"PRIu32", %"PRIu32" fps, duration: %"PRIu32" ms",
                     width, height, fps, duration);
        }

        ret = app_stream_adapter_start(s_stream_adapter);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start playback: %d", ret);
            goto next_file;
        }

        /* Monitor playback */
        uint32_t stable_count = 0;
        uint32_t last_frames = 0;

        while (s_running && display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
            bool eos = false;
            ret = app_stream_adapter_is_eos(s_stream_adapter, &eos);

            if (ret == ESP_OK && eos) {
                ESP_LOGD(TAG, "Playback finished - end of stream reached");
                break;
            }

            app_stream_stats_t stats;
            ret = app_stream_adapter_get_stats(s_stream_adapter, &stats);

            if (ret == ESP_OK) {
                if (stats.frames_processed == last_frames) {
                    stable_count++;
                    if (stable_count >= 5) {
                        ESP_LOGD(TAG, "Playback finished (%" PRIu32 " frames) - fallback detection",
                                 stats.frames_processed);
                        break;
                    }
                } else {
                    stable_count = 0;
                    last_frames = stats.frames_processed;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }

        app_stream_adapter_stop(s_stream_adapter);

next_file:
        s_current_index = (s_current_index + 1) % s_playlist_count;
    }

    display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);
    ESP_LOGD(TAG, "Video task stopped");
    s_video_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---- public API ---- */

esp_err_t video_player_init(void)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    ESP_LOGW(TAG, "LCD not supported, video player disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *lcd_handle = NULL;
    esp_err_t ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display handle: %s", esp_err_to_name(ret));
        return ret;
    }

    dev_display_lcd_handles_t *handles = (dev_display_lcd_handles_t *)lcd_handle;
    s_panel = handles->panel_handle;

    /* Try to get DPI frame buffers from the panel (may not work on all setups) */
    ret = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &s_lcd_buffer[0], &s_lcd_buffer[1]);
    if (ret != ESP_OK) {
        /* Fallback: allocate frame buffers in PSRAM */
        ESP_LOGW(TAG, "Cannot get DPI frame buffers (%s), allocating PSRAM buffers",
                 esp_err_to_name(ret));
        s_lcd_buffer[0] = heap_caps_aligned_alloc(64, DISPLAY_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        s_lcd_buffer[1] = heap_caps_aligned_alloc(64, DISPLAY_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_lcd_buffer[0] || !s_lcd_buffer[1]) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM frame buffers");
            return ESP_ERR_NO_MEM;
        }
    }

    /* VSync semaphore for tear-free rendering */
    s_vsync_sem = xSemaphoreCreateBinary();
    if (!s_vsync_sem) {
        return ESP_ERR_NO_MEM;
    }
    esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_refresh_done = vsync_ready_callback,
    };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &callbacks, NULL);

    ESP_LOGD(TAG, "Video player initialized");
    return ESP_OK;
#endif
}

esp_err_t video_player_start(void)
{
    /* SD card is already mounted by the board manager — just verify it's accessible */
    struct stat st;
    if (stat(SD_MOUNT_POINT, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "SD card not accessible at %s, skipping video playback", SD_MOUNT_POINT);
        return ESP_OK;  /* graceful fallback */
    }

    /* Scan for video files */
    esp_err_t ret = scan_media_files(SD_MOUNT_POINT);
    if (ret != ESP_OK || s_playlist_count == 0) {
        ESP_LOGI(TAG, "No video files, falling back to emote UI");
        return ESP_OK;  /* graceful fallback */
    }

    /* Initialize audio if available */
#if CONFIG_VIDEO_PLAYER_AUDIO_ENABLE && CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    void *audio_handle = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC, &audio_handle);
    if (ret == ESP_OK && audio_handle) {
        s_audio_dev = *(esp_codec_dev_handle_t *)audio_handle;
        if (s_audio_dev) {
            esp_codec_dev_set_out_vol(s_audio_dev, 80);
            ESP_LOGD(TAG, "Audio output initialized");
        }
    } else {
        ESP_LOGW(TAG, "No audio device, video-only playback");
        s_audio_dev = NULL;
    }
#else
    s_audio_dev = NULL;
#endif

    /* Initialize stream adapter */
    app_stream_adapter_config_t adapter_config = {
        .frame_cb = display_decoded_frame,
        .user_data = NULL,
        .decode_buffers = s_lcd_buffer,
        .buffer_count = 2,
        .buffer_size = DISPLAY_BUFFER_SIZE,
        .audio_dev = s_audio_dev,
        .jpeg_config = APP_STREAM_JPEG_CONFIG_DEFAULT_RGB565(),
    };

    ret = app_stream_adapter_init(&adapter_config, &s_stream_adapter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stream adapter init failed: %d", ret);
        return ret;
    }

    /* Start video playback task */
    s_running = true;
    s_current_index = 0;

    BaseType_t task_ret = xTaskCreate(video_task, "video_task",
                                       8 * 1024, NULL,
                                       6, &s_video_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create video task");
        s_running = false;
        app_stream_adapter_deinit(s_stream_adapter);
        s_stream_adapter = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Video player started with %d file(s)", s_playlist_count);
    return ESP_OK;
}

esp_err_t video_player_stop(void)
{
    s_running = false;

    if (s_video_task_handle) {
        /* Wait for task to exit */
        while (s_video_task_handle != NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (s_stream_adapter) {
        app_stream_adapter_deinit(s_stream_adapter);
        s_stream_adapter = NULL;
    }

    display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);

    playlist_cleanup();

    ESP_LOGD(TAG, "Video player stopped");
    return ESP_OK;
}
