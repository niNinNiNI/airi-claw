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
#include "freertos/queue.h"
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
#define VIDEO_BTN_COLOR_BG          0x18E3  /* dark grey (idle) */
#define VIDEO_BTN_COLOR_BORDER      0xFFFF  /* white */
#define VIDEO_BTN_COLOR_ICON        0xFFFF  /* white */
#define VIDEO_BTN_COLOR_PRESSED     0x7BEF  /* lighter grey when pressed */
#define VIDEO_BTN_COLOR_LISTENING   0xC841  /* red-ish (listening) */
#define VIDEO_BTN_COLOR_SUCCESS     0x3E6B  /* green (success) */
#define VIDEO_BTN_COLOR_ERROR       0xD965  /* red (error) */

#define TOUCH_POLL_PERIOD_MS  30

/* ---- command queue types ---- */
typedef enum {
    VIDEO_CMD_SWITCH_FILE,
    VIDEO_CMD_STOP,
} video_cmd_t;

typedef struct {
    video_cmd_t cmd;
    char path[MAX_FILENAME_LEN];
    bool loop;
} video_cmd_msg_t;

#define VIDEO_CMD_QUEUE_LEN  4

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

/* ---- single-file / command-driven playback state ---- */
static QueueHandle_t s_video_cmd_queue;
static bool s_single_file_mode;
static bool s_loop_current;
static char s_current_file_path[MAX_FILENAME_LEN];
static bool s_stream_adapter_initialized;

/* ---- playlist mode state ---- */
static bool s_playlist_mode;
static bool s_playlist_loop;
static char **s_playlist_paths;
static int s_playlist_len;
static int s_playlist_idx;

/* ---- touch button state ---- */
static esp_lcd_touch_handle_t s_touch;
static TaskHandle_t s_touch_task_handle;
static uint8_t *s_btn_buffer;
static int s_btn_x, s_btn_y;
static bool s_btn_pressed;
static int s_btn_state;  /* 0=idle, 1=listening, 2=success, 3=error */
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

static void draw_button_overlay(bool pressed, int state)
{
    int w = VIDEO_BTN_WIDTH;
    int h = VIDEO_BTN_HEIGHT;
    int r = VIDEO_BTN_RADIUS;
    int border = VIDEO_BTN_BORDER;

    uint16_t bg;
    switch (state) {
    case 1:  bg = VIDEO_BTN_COLOR_LISTENING; break;
    case 2:  bg = VIDEO_BTN_COLOR_SUCCESS;   break;
    case 3:  bg = VIDEO_BTN_COLOR_ERROR;     break;
    default: bg = pressed ? VIDEO_BTN_COLOR_PRESSED : VIDEO_BTN_COLOR_BG; break;
    }

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

    /* draw microphone icon centred in the button */
    int cx = w / 2;        /* centre x */
    int mic_top = 12;      /* top of mic head */
    int head_r = 7;        /* radius of mic head circle */
    int head_cy = mic_top + head_r;  /* centre y of mic head */
    int body_w = 5;        /* mic body width */
    int body_h = 12;       /* mic body height */
    int body_y = head_cy + head_r;  /* top of mic body */
    int base_w = 14;       /* base line width */
    int base_h = 3;        /* base line height */
    int base_y = body_y + body_h + 1;  /* top of base line */

    /* mic head — filled circle */
    for (int dy = -head_r; dy <= head_r; dy++) {
        for (int dx = -head_r; dx <= head_r; dx++) {
            if (dx * dx + dy * dy <= head_r * head_r) {
                int px = cx + dx;
                int py = head_cy + dy;
                if (px >= 0 && px < w && py >= 0 && py < h) {
                    uint16_t *pixel = (uint16_t *)(s_btn_buffer + (py * w + px) * 2);
                    *pixel = VIDEO_BTN_COLOR_ICON;
                }
            }
        }
    }

    /* mic body — filled rectangle */
    int body_x = cx - body_w / 2;
    for (int dy = 0; dy < body_h; dy++) {
        for (int dx = 0; dx < body_w; dx++) {
            int px = body_x + dx;
            int py = body_y + dy;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                uint16_t *pixel = (uint16_t *)(s_btn_buffer + (py * w + px) * 2);
                *pixel = VIDEO_BTN_COLOR_ICON;
            }
        }
    }

    /* mic base — horizontal line */
    int base_x = cx - base_w / 2;
    for (int dy = 0; dy < base_h; dy++) {
        for (int dx = 0; dx < base_w; dx++) {
            int px = base_x + dx;
            int py = base_y + dy;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                uint16_t *pixel = (uint16_t *)(s_btn_buffer + (py * w + px) * 2);
                *pixel = VIDEO_BTN_COLOR_ICON;
            }
        }
    }

    /* recording dot (listening state) — small filled circle next to the mic */
    if (state == 1) {
        int dot_r = 3;
        int dot_cx = cx + head_r + 8;
        int dot_cy = head_cy - 3;
        for (int dy = -dot_r; dy <= dot_r; dy++) {
            for (int dx = -dot_r; dx <= dot_r; dx++) {
                if (dx * dx + dy * dy <= dot_r * dot_r) {
                    int px = dot_cx + dx;
                    int py = dot_cy + dy;
                    if (px >= 0 && px < w && py >= 0 && py < h) {
                        uint16_t *pixel = (uint16_t *)(s_btn_buffer + (py * w + px) * 2);
                        *pixel = VIDEO_BTN_COLOR_ICON;
                    }
                }
            }
        }
    }

    /* checkmark (success state) */
    if (state == 2) {
        int ck_cx = cx + head_r + 8;
        int ck_cy = head_cy;
        /* Simple checkmark using a few pixels: "V" shape */
        for (int i = 0; i < 5; i++) {
            int px = ck_cx - 3 + i;
            int py = ck_cy - 2 + i;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                uint16_t *pixel = (uint16_t *)(s_btn_buffer + (py * w + px) * 2);
                *pixel = VIDEO_BTN_COLOR_ICON;
            }
            /* upper arm */
            int px2 = ck_cx + i;
            int py2 = ck_cy + 2 - i;
            if (px2 >= 0 && px2 < w && py2 >= 0 && py2 < h && i < 4) {
                uint16_t *pixel2 = (uint16_t *)(s_btn_buffer + (py2 * w + px2) * 2);
                *pixel2 = VIDEO_BTN_COLOR_ICON;
            }
        }
    }

    /* X mark (error state) */
    if (state == 3) {
        int x_cx = cx + head_r + 8;
        int x_cy = head_cy;
        int x_sz = 4;
        for (int i = -x_sz; i <= x_sz; i++) {
            /* \ diagonal */
            int px = x_cx + i;
            int py = x_cy + i;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                uint16_t *pixel = (uint16_t *)(s_btn_buffer + (py * w + px) * 2);
                *pixel = VIDEO_BTN_COLOR_ICON;
            }
            /* / diagonal */
            py = x_cy - i;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                uint16_t *pixel = (uint16_t *)(s_btn_buffer + (py * w + px) * 2);
                *pixel = VIDEO_BTN_COLOR_ICON;
            }
        }
    }
}

static void touch_task(void *arg)
{
    bool was_pressed = false;

    while (s_running) {
        esp_lcd_touch_point_data_t point;
        uint8_t count = 0;

        esp_err_t ret = esp_lcd_touch_read_data(s_touch);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch read_data failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_PERIOD_MS));
            continue;
        }

        ret = esp_lcd_touch_get_data(s_touch, &point, &count, 1);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch get_data failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_PERIOD_MS));
            continue;
        }

        bool hit = false;
        if (count > 0) {
            /* Coordinates are already mirrored by esp_lcd_touch_get_data()
             * according to the board config (mirror_x / mirror_y flags).
             * No additional mirroring needed here. */
            ESP_LOGD(TAG, "Touch at (%" PRIu16 ", %" PRIu16 ") btn=(%d,%d)",
                     point.x, point.y, s_btn_x, s_btn_y);
            hit = in_round_rect(point.x, point.y,
                                s_btn_x, s_btn_y,
                                VIDEO_BTN_WIDTH, VIDEO_BTN_HEIGHT,
                                VIDEO_BTN_RADIUS);
        }

        s_btn_pressed = hit;

        if (hit && !was_pressed && s_button_cb) {
            ESP_LOGI(TAG, "Button pressed, triggering callback");
            s_button_cb(s_button_cb_data, true);
        } else if (!hit && was_pressed && s_button_cb) {
            ESP_LOGI(TAG, "Button released, triggering callback");
            s_button_cb(s_button_cb_data, false);
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
        draw_button_overlay(s_btn_pressed, s_btn_state);
        esp_lcd_panel_draw_bitmap(s_panel, s_btn_x, s_btn_y,
                                  s_btn_x + VIDEO_BTN_WIDTH,
                                  s_btn_y + VIDEO_BTN_HEIGHT,
                                  s_btn_buffer);
    }

    xSemaphoreTake(s_vsync_sem, 0);
    xSemaphoreTake(s_vsync_sem, portMAX_DELAY);

    return ESP_OK;
}

/* ---- stream adapter lazy init ---- */

static esp_err_t ensure_stream_adapter(void)
{
    if (s_stream_adapter_initialized) {
        return ESP_OK;
    }

    app_stream_adapter_config_t adapter_config = {
        .frame_cb = display_decoded_frame,
        .user_data = NULL,
        .decode_buffers = s_lcd_buffer,
        .buffer_count = 2,
        .buffer_size = DISPLAY_BUFFER_SIZE,
        .audio_dev = NULL,
        .jpeg_config = APP_STREAM_JPEG_CONFIG_DEFAULT_RGB565(),
    };

    esp_err_t ret = app_stream_adapter_init(&adapter_config, &s_stream_adapter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stream adapter init failed: %d", ret);
        return ret;
    }

    s_stream_adapter_initialized = true;
    ESP_LOGI(TAG, "Stream adapter initialized");
    return ESP_OK;
}

/* ---- video task (command-driven state machine) ---- */

static esp_err_t play_single_file(const char *path)
{
    esp_err_t ret = app_stream_adapter_set_file(s_stream_adapter, path, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set file: %s", path);
        return ret;
    }

    ret = app_stream_adapter_start(s_stream_adapter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start playback: %d", ret);
        return ret;
    }

    return ESP_OK;
}

static void stop_current_playback(void)
{
    app_stream_adapter_stop(s_stream_adapter);
}

static void video_task(void *arg)
{
    video_cmd_msg_t msg;
    bool active = false;      /* currently playing a file */
    bool loop_enabled = false;
    char active_path[MAX_FILENAME_LEN] = {0};
    int consecutive_failures = 0;

    /* Wait for first command */
    while (s_running) {
        if (xQueueReceive(s_video_cmd_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (msg.cmd == VIDEO_CMD_STOP) {
                continue;
            }
            /* SWITCH_FILE — start playing */
            strlcpy(active_path, msg.path, sizeof(active_path));
            loop_enabled = msg.loop;
            break;
        }
    }

    while (s_running) {
        /* Acquire display before playback — wait if another owner holds it */
        esp_err_t acq;
        while (s_running && (acq = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_VIDEO)) != ESP_OK) {
            if (acq == ESP_ERR_INVALID_STATE) {
                vTaskDelay(pdMS_TO_TICKS(500));
            } else {
                ESP_LOGE(TAG, "display acquire failed: %s", esp_err_to_name(acq));
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (!s_running) {
            break;
        }
        active = true;

        ESP_LOGD(TAG, "Playing: %s (loop=%d)", strrchr(active_path, '/') + 1, loop_enabled);

        esp_err_t ret = play_single_file(active_path);
        if (ret != ESP_OK) {
            consecutive_failures++;
            display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);
            active = false;
            if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                ESP_LOGE(TAG, "%d consecutive failures, giving up", consecutive_failures);
                break;
            }
            /* Wait for next command */
            while (s_running) {
                if (xQueueReceive(s_video_cmd_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (msg.cmd == VIDEO_CMD_STOP) {
                        continue;
                    }
                    strlcpy(active_path, msg.path, sizeof(active_path));
                    loop_enabled = msg.loop;
                    consecutive_failures = 0;
                    break;
                }
            }
            continue;
        }

        /* Monitor playback: check EOS and command queue */
        uint32_t stable_count = 0;
        uint32_t last_frames = 0;
        bool switched = false;

        while (s_running && display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
            /* Check for incoming commands */
            video_cmd_msg_t new_msg;
            if (xQueueReceive(s_video_cmd_queue, &new_msg, 0) == pdTRUE) {
                if (new_msg.cmd == VIDEO_CMD_STOP) {
                    stop_current_playback();
                    display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);
                    active = false;
                    s_playlist_mode = false;
                    switched = true;
                    break;
                }
                if (new_msg.cmd == VIDEO_CMD_SWITCH_FILE) {
                    stop_current_playback();
                    strlcpy(active_path, new_msg.path, sizeof(active_path));
                    loop_enabled = new_msg.loop;
                    consecutive_failures = 0;
                    switched = true;
                    break;
                }
            }

            /* Check EOS */
            bool eos = false;
            ret = app_stream_adapter_is_eos(s_stream_adapter, &eos);
            if (ret == ESP_OK && eos) {
                break;
            }

            /* Stall detection */
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

        if (switched) {
            continue;  /* already handled — new file set in active_path */
        }

        /* Check if display was taken by another owner (e.g. Lua) */
        if (!display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
            stop_current_playback();
            consecutive_failures = 0;
            active = false;

            ESP_LOGI(TAG, "Display taken by another owner, waiting to re-acquire");

            while (s_running && !display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_VIDEO)) {
                video_cmd_msg_t wait_msg;
                if (xQueueReceive(s_video_cmd_queue, &wait_msg, 0) == pdTRUE) {
                    if (wait_msg.cmd == VIDEO_CMD_STOP) {
                        s_playlist_mode = false;
                        break;
                    } else if (wait_msg.cmd == VIDEO_CMD_SWITCH_FILE) {
                        strlcpy(active_path, wait_msg.path, sizeof(active_path));
                        loop_enabled = wait_msg.loop;
                        consecutive_failures = 0;
                    }
                }
                esp_err_t acq_ret = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_VIDEO);
                if (acq_ret == ESP_OK) {
                    active = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            if (!s_running) {
                break;
            }
            if (active) {
                ESP_LOGI(TAG, "Display re-acquired, restarting playback");
                continue;
            }
            /* STOP received while waiting — wait for next command */
            while (s_running) {
                if (xQueueReceive(s_video_cmd_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (msg.cmd == VIDEO_CMD_STOP) {
                        continue;
                    }
                    strlcpy(active_path, msg.path, sizeof(active_path));
                    loop_enabled = msg.loop;
                    break;
                }
            }
            continue;
        }

        /* Playback ended (EOS or stall) */
        stop_current_playback();
        consecutive_failures = 0;

        if (s_playlist_mode) {
            /* Advance to next playlist entry */
            s_playlist_idx++;
            if (s_playlist_idx >= s_playlist_len) {
                if (s_playlist_loop) {
                    s_playlist_idx = 0;
                } else {
                    display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);
                    active = false;
                    s_playlist_mode = false;
                    /* Wait for next command */
                    while (s_running) {
                        if (xQueueReceive(s_video_cmd_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                            if (msg.cmd == VIDEO_CMD_STOP) {
                                continue;
                            }
                            strlcpy(active_path, msg.path, sizeof(active_path));
                            loop_enabled = msg.loop;
                            break;
                        }
                    }
                    continue;
                }
            }
            strlcpy(active_path, s_playlist_paths[s_playlist_idx], sizeof(active_path));
            loop_enabled = false;
            continue;
        }

        if (!loop_enabled) {
            display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);
            active = false;

            /* Wait for next command */
            while (s_running) {
                if (xQueueReceive(s_video_cmd_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (msg.cmd == VIDEO_CMD_STOP) {
                        continue;
                    }
                    strlcpy(active_path, msg.path, sizeof(active_path));
                    loop_enabled = msg.loop;
                    break;
                }
            }
        }
        /* If loop_enabled, loop back to play the same file again */
    }

    if (active) {
        stop_current_playback();
        display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);
    }

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

    /* Create command queue for video_task */
    s_video_cmd_queue = xQueueCreate(VIDEO_CMD_QUEUE_LEN, sizeof(video_cmd_msg_t));
    if (!s_video_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    s_stream_adapter_initialized = false;
    s_running = true;

    /* Pre-initialize stream adapter */
    ret = ensure_stream_adapter();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Stream adapter init deferred");
    }

    /* Start touch polling task if touch controller is available */
    if (s_touch && s_touch_task_handle == NULL) {
        BaseType_t touch_ret = xTaskCreate(touch_task, "vid_touch",
                                           2 * 1024, NULL,
                                           4, &s_touch_task_handle);
        if (touch_ret != pdPASS) {
            ESP_LOGW(TAG, "Failed to create touch task — button disabled");
            s_touch_task_handle = NULL;
        }
    }

    /* Create video task (starts waiting for commands) */
    BaseType_t task_ret = xTaskCreate(video_task, "video_task",
                                       8 * 1024, NULL,
                                       6, &s_video_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create video task");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Video player initialized");
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

    /* Send first file to video_task (playlist mode) */
    s_single_file_mode = false;
    s_loop_current = false;
    s_current_index = 0;

    video_cmd_msg_t msg = {
        .cmd = VIDEO_CMD_SWITCH_FILE,
        .loop = false,
    };
    strlcpy(msg.path, s_playlist[0], sizeof(msg.path));
    xQueueSend(s_video_cmd_queue, &msg, pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Video player started with %d file(s)", s_playlist_count);
    return ESP_OK;
}

esp_err_t video_player_stop(void)
{
    s_running = false;

    /* Send STOP command to video task */
    if (s_video_cmd_queue && s_video_task_handle) {
        video_cmd_msg_t msg = { .cmd = VIDEO_CMD_STOP };
        xQueueSend(s_video_cmd_queue, &msg, 0);
    }

    /* Wait for touch task to exit */
    while (s_touch_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Wait for video task to exit */
    while (s_video_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    display_arbiter_release(DISPLAY_ARBITER_OWNER_VIDEO);

    s_playlist_mode = false;
    if (s_playlist_paths) {
        for (int i = 0; i < s_playlist_len; i++) {
            free(s_playlist_paths[i]);
        }
        free(s_playlist_paths);
        s_playlist_paths = NULL;
    }
    s_playlist_len = 0;
    s_playlist_idx = 0;

    playlist_cleanup();

    return ESP_OK;
}

esp_err_t video_player_play_file(const char *path, bool loop)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_video_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Switch to single-file mode, clear any active playlist */
    strlcpy(s_current_file_path, path, sizeof(s_current_file_path));
    s_single_file_mode = true;
    s_loop_current = loop;
    s_playlist_mode = false;

    video_cmd_msg_t msg = {
        .cmd = VIDEO_CMD_SWITCH_FILE,
        .loop = loop,
    };
    strlcpy(msg.path, path, sizeof(msg.path));

    if (xQueueSend(s_video_cmd_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Command queue full, failed to send play_file");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Queued play_file: %s (loop=%d)", strrchr(path, '/') + 1, loop);
    return ESP_OK;
}

esp_err_t video_player_switch_to(const char *path)
{
    return video_player_play_file(path, true);
}

esp_err_t video_player_play_playlist(const char **paths, int count, bool loop)
{
    if (!paths || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_video_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify all files exist */
    for (int i = 0; i < count; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) {
            ESP_LOGE(TAG, "Playlist file not found: %s", paths[i]);
            return ESP_ERR_NOT_FOUND;
        }
    }

    /* Free any previous playlist */
    if (s_playlist_paths) {
        for (int i = 0; i < s_playlist_len; i++) {
            free(s_playlist_paths[i]);
        }
        free(s_playlist_paths);
        s_playlist_paths = NULL;
    }

    /* Copy playlist */
    s_playlist_paths = calloc(count, sizeof(char *));
    if (!s_playlist_paths) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < count; i++) {
        s_playlist_paths[i] = strdup(paths[i]);
        if (!s_playlist_paths[i]) {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++) {
                free(s_playlist_paths[j]);
            }
            free(s_playlist_paths);
            s_playlist_paths = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_playlist_len = count;
    s_playlist_idx = 0;
    s_playlist_loop = loop;
    s_playlist_mode = true;

    /* Update tracking state */
    strlcpy(s_current_file_path, paths[0], sizeof(s_current_file_path));
    s_single_file_mode = false;
    s_loop_current = false;

    /* Send first file to video_task */
    video_cmd_msg_t msg = {
        .cmd = VIDEO_CMD_SWITCH_FILE,
        .loop = false,
    };
    strlcpy(msg.path, paths[0], sizeof(msg.path));

    if (xQueueSend(s_video_cmd_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Command queue full, failed to send play_playlist");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Queued playlist: %d file(s) (loop=%d)", count, loop);
    return ESP_OK;
}

esp_err_t video_player_get_current(char *path, size_t path_size)
{
    if (!path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_current_file_path[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    strlcpy(path, s_current_file_path, path_size);
    return ESP_OK;
}

esp_err_t video_player_set_button_callback(video_player_button_cb_t cb, void *user_data)
{
    s_button_cb = cb;
    s_button_cb_data = user_data;
    return ESP_OK;
}

void video_player_set_button_state(int state)
{
    s_btn_state = state;
}
