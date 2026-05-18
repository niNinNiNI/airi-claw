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
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

static const char *TAG = "video_player";

#define DISPLAY_WIDTH         480
#define DISPLAY_HEIGHT        800
#define DISPLAY_BUFFER_SIZE   (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)  /* RGB565 */

#define MAX_PLAYLIST_ITEMS        50
#define MAX_CONSECUTIVE_FAILURES  3
#define MAX_FILENAME_LEN          512

#define SD_MOUNT_POINT  "/sdcard"

/* ---- on-screen touch button ---- */
#define VIDEO_BTN_WIDTH        80
#define VIDEO_BTN_HEIGHT       50
#define VIDEO_BTN_MARGIN       20
#define VIDEO_BTN_RADIUS       10
#define VIDEO_BTN_BORDER       2
#define VIDEO_BTN_BUF_SIZE     (VIDEO_BTN_WIDTH * VIDEO_BTN_HEIGHT * 2)

/* RGB565 colors */
#define VIDEO_BTN_COLOR_BG        0x18E3  /* dark grey */
#define VIDEO_BTN_COLOR_BORDER    0xFFFF  /* white */
#define VIDEO_BTN_COLOR_ICON      0xFFFF  /* white */
#define VIDEO_BTN_COLOR_PRESSED   0x7BEF  /* lighter grey when pressed */

#define TOUCH_POLL_PERIOD_MS  30

/* ---- static state ---- */
static esp_lcd_panel_handle_t s_panel;
static void *s_lcd_buffer[2];
static SemaphoreHandle_t s_vsync_sem;
static app_stream_adapter_handle_t s_stream_adapter;


static char *s_playlist[MAX_PLAYLIST_ITEMS];
static int s_playlist_count;
static int s_current_index;

static TaskHandle_t s_video_task_handle;
static bool s_running;
static int s_consecutive_failures;

/* ---- touch button state ---- */
static esp_lcd_touch_handle_t s_touch;
static TaskHandle_t s_touch_task_handle;
static uint8_t *s_btn_buffer;
static int s_btn_x, s_btn_y;
static bool s_btn_pressed;
static video_player_button_cb_t s_button_cb;
static void *s_button_cb_data;

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

    while ((entry = readdir(dir)) != NULL && s_playlist_count < MAX_PLAYLIST_ITEMS) {
        if (entry->d_type == DT_REG && is_video_file(entry->d_name)) {
            size_t name_len = strlen(entry->d_name);
            if (dir_len + 1 + name_len < MAX_FILENAME_LEN) {
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

                struct stat st;
                if (stat(full_path, &st) == 0 && st.st_size > 0) {
                    s_playlist[s_playlist_count] = strdup(full_path);
                    if (s_playlist[s_playlist_count] != NULL) {
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

/* ---- on-screen button helpers ---- */

static bool in_round_rect(int px, int py, int rx, int ry, int rw, int rh, int r)
{
    if (py >= ry + r && py < ry + rh - r) {
        return (px >= rx && px < rx + rw);
    }
    if (px >= rx + r && px < rx + rw - r) {
        return (py >= ry && py < ry + rh);
    }
    int cx = (px < rx + r) ? (rx + r) : (rx + rw - 1 - r);
    int cy = (py < ry + r) ? (ry + r) : (ry + rh - 1 - r);
    int dx = px - cx;
    int dy = py - cy;
    return (dx * dx + dy * dy) <= r * r;
}

static void draw_button_overlay(bool pressed)
{
    int w = VIDEO_BTN_WIDTH;
    int h = VIDEO_BTN_HEIGHT;
    int r = VIDEO_BTN_RADIUS;
    int border = VIDEO_BTN_BORDER;

    uint16_t bg = pressed ? VIDEO_BTN_COLOR_PRESSED : VIDEO_BTN_COLOR_BG;

    /* Fill with transparent black first (so corners outside the round rect stay transparent) */
    memset(s_btn_buffer, 0, VIDEO_BTN_BUF_SIZE);

    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            if (!in_round_rect(dx, dy, 0, 0, w, h, r)) {
                continue;
            }

            bool is_border;
            if (dx < border || dx >= w - border || dy < border || dy >= h - border) {
                is_border = true;
            } else if (r > border && (dx < r || dx >= w - r || dy < r || dy >= h - r)) {
                is_border = !in_round_rect(dx, dy, border, border,
                                           w - 2 * border, h - 2 * border, r - border);
            } else {
                is_border = false;
            }

            uint16_t *pixel = (uint16_t *)(s_btn_buffer + (dy * w + dx) * 2);
            *pixel = is_border ? VIDEO_BTN_COLOR_BORDER : bg;
        }
    }

    /* draw stop icon — a small filled square in the centre */
    int icon_sz = 14;
    int ix = (w - icon_sz) / 2;
    int iy = (h - icon_sz) / 2;
    for (int dy = 0; dy < icon_sz; dy++) {
        for (int dx = 0; dx < icon_sz; dx++) {
            uint16_t *pixel = (uint16_t *)(s_btn_buffer + ((iy + dy) * w + (ix + dx)) * 2);
            *pixel = VIDEO_BTN_COLOR_ICON;
        }
    }
}

static void touch_task(void *arg)
{
    bool was_pressed = false;

    while (s_running) {
        esp_lcd_touch_point_data_t point;
        uint8_t count = 0;

        if (s_touch->config.interrupt_callback != NULL) {
            esp_lcd_touch_get_data(s_touch, &point, &count, 1);
        } else {
            esp_lcd_touch_read_data(s_touch);
            esp_lcd_touch_get_data(s_touch, &point, &count, 1);
        }

        bool hit = false;
        if (count > 0) {
            hit = in_round_rect(point.x, point.y,
                                s_btn_x, s_btn_y,
                                VIDEO_BTN_WIDTH, VIDEO_BTN_HEIGHT,
                                VIDEO_BTN_RADIUS);
        }

        s_btn_pressed = hit;

        if (hit && !was_pressed && s_button_cb) {
            s_button_cb(s_button_cb_data);
        }

        was_pressed = hit;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_PERIOD_MS));
    }

    s_touch_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---- frame display ---- */

static esp_err_t display_decoded_frame(uint8_t *buffer, uint32_t buffer_size,
                                       uint32_t width, uint32_t height,
                                       uint32_t buffer_index, void *user_data)
{
    if (!display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
        return ESP_OK;
    }

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, width, height, buffer);

    if (s_btn_buffer) {
        draw_button_overlay(s_btn_pressed);
        esp_lcd_panel_draw_bitmap(s_panel, s_btn_x, s_btn_y,
                                  s_btn_x + VIDEO_BTN_WIDTH,
                                  s_btn_y + VIDEO_BTN_HEIGHT,
                                  s_btn_buffer);
    }

    xSemaphoreTake(s_vsync_sem, 0);
    xSemaphoreTake(s_vsync_sem, portMAX_DELAY);

    return ESP_OK;
}

static void video_task(void *arg)
{
    uint32_t total_play_count = 0;

    display_arbiter_acquire(DISPLAY_ARBITER_OWNER_VIDEO);

    while (s_running && display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
        const char *current_file = s_playlist[s_current_index];
        total_play_count++;
        bool play_ok = false;

        ESP_LOGD(TAG, "=== Playing [%d/%d] #%u: %s ===",
                 s_current_index + 1, s_playlist_count,
                 total_play_count, strrchr(current_file, '/') + 1);

        esp_err_t ret = app_stream_adapter_set_file(s_stream_adapter, current_file, false);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set file: %s, moving to next", current_file);
            goto next_file;
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
                break;
            }

            app_stream_stats_t stats;
            ret = app_stream_adapter_get_stats(s_stream_adapter, &stats);

            if (ret == ESP_OK) {
                if (stats.frames_processed == last_frames) {
                    stable_count++;
                    if (stable_count >= 5) {
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
        play_ok = true;

next_file:
        s_current_index = (s_current_index + 1) % s_playlist_count;
        if (play_ok) {
            s_consecutive_failures = 0;
        } else {
            s_consecutive_failures++;
            if (s_consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                ESP_LOGE(TAG, "%d consecutive failures, giving up", s_consecutive_failures);
                break;
            }
        }
    }

    display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);
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

    /* Set up on-screen touch button position (bottom-left corner) */
    s_btn_x = VIDEO_BTN_MARGIN;
    s_btn_y = DISPLAY_HEIGHT - VIDEO_BTN_HEIGHT - VIDEO_BTN_MARGIN;

    /* Get touch handle for button interaction */
    void *touch_handle = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle);
    if (ret == ESP_OK && touch_handle) {
        dev_lcd_touch_i2c_handles_t *th = (dev_lcd_touch_i2c_handles_t *)touch_handle;
        s_touch = th->touch_handle;
        ESP_LOGI(TAG, "Touch controller ready for on-screen button");
    } else {
        s_touch = NULL;
        ESP_LOGW(TAG, "No touch controller — on-screen button disabled");
    }

    /* Allocate button overlay buffer */
    s_btn_buffer = heap_caps_aligned_alloc(64, VIDEO_BTN_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_btn_buffer) {
        ESP_LOGW(TAG, "Failed to allocate button buffer — button disabled");
    }

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

    /* Initialize stream adapter (video-only, no audio decoding) */
    app_stream_adapter_config_t adapter_config = {
        .frame_cb = display_decoded_frame,
        .user_data = NULL,
        .decode_buffers = s_lcd_buffer,
        .buffer_count = 2,
        .buffer_size = DISPLAY_BUFFER_SIZE,
        .audio_dev = NULL,
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
    s_consecutive_failures = 0;

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

    /* Start touch polling task if touch controller is available */
    if (s_touch) {
        BaseType_t touch_ret = xTaskCreate(touch_task, "vid_touch",
                                           2 * 1024, NULL,
                                           4, &s_touch_task_handle);
        if (touch_ret != pdPASS) {
            ESP_LOGW(TAG, "Failed to create touch task — button disabled");
            s_touch_task_handle = NULL;
        }
    }

    ESP_LOGI(TAG, "Video player started with %d file(s)", s_playlist_count);
    return ESP_OK;
}

esp_err_t video_player_stop(void)
{
    s_running = false;

    /* Wait for touch task to exit */
    while (s_touch_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_video_task_handle) {
        /* Wait for video task to exit */
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

    return ESP_OK;
}

esp_err_t video_player_set_button_callback(video_player_button_cb_t cb, void *user_data)
{
    s_button_cb = cb;
    s_button_cb_data = user_data;
    return ESP_OK;
}
