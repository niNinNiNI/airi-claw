/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "emotion_video_controller.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "video_player.h"

#include "cJSON.h"
#include "claw_cap.h"

#include "cap_lua.h"
#include "lauxlib.h"
#include "lua.h"

static const char *TAG = "emotion_video";

/* ---- emotion state ---- */
static emotion_video_t s_current_emotion = EMOTION_IDLE_SHAKE;
static bool s_initialized = false;

/* ---- mapping table ---- */
typedef struct {
    emotion_video_t emotion;
    const char *name;
    const char *file_name;
} emotion_mapping_t;

static emotion_mapping_t s_emotion_map[EMOTION_COUNT] = {
    [EMOTION_IDLE_SHAKE]  = { EMOTION_IDLE_SHAKE,  "idle_shake",  "m01_idle_shake.mp4"  },
    [EMOTION_SWAY]        = { EMOTION_SWAY,         "sway",        "m02_sway.mp4"         },
    [EMOTION_CALM]        = { EMOTION_CALM,         "calm",        "m03_calm.mp4"         },
    [EMOTION_WAVE]        = { EMOTION_WAVE,         "wave",        "m04_wave.mp4"         },
    [EMOTION_TIRED]       = { EMOTION_TIRED,        "tired",       "m05_tired.mp4"        },
    [EMOTION_SLIGHT_SWAY] = { EMOTION_SLIGHT_SWAY,  "slight_sway", "m06_slight_sway.mp4"  },
    [EMOTION_BREATHING]   = { EMOTION_BREATHING,    "breathing",   "m07_breathing.mp4"    },
    [EMOTION_HAND_MOVES]  = { EMOTION_HAND_MOVES,   "hand_moves",  "m08_hand_moves.mp4"   },
    [EMOTION_NOD]         = { EMOTION_NOD,          "nod",         "m09_nod.mp4"          },
    [EMOTION_RELAXED]     = { EMOTION_RELAXED,      "relaxed",     "m10_relaxed.mp4"      },
};

static char s_video_base_dir[256];

/* ---- helpers ---- */

static bool emotion_video_file_exists(const char *base_dir, const char *file_name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", base_dir, file_name);
    struct stat st;
    return (stat(path, &st) == 0 && st.st_size > 0);
}

static void emotion_video_build_path(const char *base_dir, const char *file_name, char *path, size_t size)
{
    snprintf(path, size, "%s/%s", base_dir, file_name);
}

/* ---- public API ---- */

esp_err_t emotion_video_controller_init(const char *video_dir)
{
    if (!video_dir || !video_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_video_base_dir, video_dir, sizeof(s_video_base_dir));

    int found = 0;
    for (int i = 0; i < EMOTION_COUNT; i++) {
        if (emotion_video_file_exists(video_dir, s_emotion_map[i].file_name)) {
            found++;
        } else {
            ESP_LOGW(TAG, "Emotion video not found: %s/%s", video_dir, s_emotion_map[i].file_name);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Emotion controller initialized, %d/%d video files found", found, EMOTION_COUNT);
    return ESP_OK;
}

esp_err_t emotion_video_start_default(void)
{
    return emotion_video_set(EMOTION_IDLE_SHAKE);
}

esp_err_t emotion_video_set(emotion_video_t emotion)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Emotion controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (emotion >= EMOTION_COUNT) {
        ESP_LOGE(TAG, "Invalid emotion: %d", emotion);
        return ESP_ERR_INVALID_ARG;
    }

    char path[512];
    emotion_video_build_path(s_video_base_dir, s_emotion_map[emotion].file_name, path, sizeof(path));

    if (!emotion_video_file_exists(s_video_base_dir, s_emotion_map[emotion].file_name)) {
        ESP_LOGE(TAG, "Video file missing for emotion %s: %s", s_emotion_map[emotion].name, path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Single-file loop: play this emotion video continuously until another switch */
    esp_err_t ret = video_player_play_file(path, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to emotion %s: %s", s_emotion_map[emotion].name, esp_err_to_name(ret));
        return ret;
    }

    s_current_emotion = emotion;
    ESP_LOGI(TAG, "Emotion switched to: %s", s_emotion_map[emotion].name);
    return ESP_OK;
}

emotion_video_t emotion_video_get(void)
{
    return s_current_emotion;
}

const char *emotion_video_to_string(emotion_video_t e)
{
    if (e >= EMOTION_COUNT) {
        return "unknown";
    }
    return s_emotion_map[e].name;
}

emotion_video_t emotion_video_from_string(const char *name)
{
    if (!name) {
        return EMOTION_COUNT;
    }

    /* Try direct enum index */
    char *endptr = NULL;
    long idx = strtol(name, &endptr, 10);
    if (endptr && *endptr == '\0' && idx >= 0 && idx < EMOTION_COUNT) {
        return (emotion_video_t)idx;
    }

    /* Try name match */
    for (int i = 0; i < EMOTION_COUNT; i++) {
        if (strcasecmp(name, s_emotion_map[i].name) == 0) {
            return (emotion_video_t)i;
        }
    }

    return EMOTION_COUNT;
}

/* ---- LLM capability registration ---- */

static esp_err_t cap_emotion_set_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output, size_t output_size)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"invalid JSON\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *emotion_item = cJSON_GetObjectItem(root, "emotion");
    if (!cJSON_IsString(emotion_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"error\":\"field 'emotion' is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    emotion_video_t e = emotion_video_from_string(emotion_item->valuestring);
    cJSON_Delete(root);

    if (e >= EMOTION_COUNT) {
        snprintf(output, output_size, "{\"error\":\"unknown emotion: %s\"}", emotion_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = emotion_video_set(e);
    if (ret != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"failed to set emotion: %s\"}", esp_err_to_name(ret));
        return ret;
    }

    snprintf(output, output_size, "{\"emotion\":\"%s\",\"index\":%d}",
             emotion_video_to_string(e), (int)e);
    return ESP_OK;
}

static esp_err_t cap_emotion_get_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    emotion_video_t e = emotion_video_get();
    snprintf(output, output_size, "{\"emotion\":\"%s\",\"index\":%d}",
             emotion_video_to_string(e), (int)e);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_emotion_descriptors[] = {
    {
        .id = "set_emotion",
        .name = "set_emotion",
        .family = "emotion",
        .description = "Switch the character's emotional expression animation. "
                       "Available emotions: idle_shake, sway, calm, wave, tired, "
                       "slight_sway, breathing, hand_moves, nod, relaxed.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"emotion\":{\"type\":\"string\","
                "\"enum\":[\"idle_shake\",\"sway\",\"calm\",\"wave\",\"tired\","
                         "\"slight_sway\",\"breathing\",\"hand_moves\","
                         "\"nod\",\"relaxed\"],"
                "\"description\":\"The emotion animation to display\"}"
            "},"
            "\"required\":[\"emotion\"]}",
        .execute = cap_emotion_set_execute,
    },
    {
        .id = "get_emotion",
        .name = "get_emotion",
        .family = "emotion",
        .description = "Get the currently displayed emotion animation.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,  /* query-only, not shown to LLM as an action tool */
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_emotion_get_execute,
    },
};

static const claw_cap_group_t s_emotion_group = {
    .group_id = "cap_emotion",
    .descriptors = s_emotion_descriptors,
    .descriptor_count = sizeof(s_emotion_descriptors) / sizeof(s_emotion_descriptors[0]),
};

esp_err_t emotion_video_register_capabilities(void)
{
    if (claw_cap_group_exists(s_emotion_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_emotion_group);
}

/* ---- Lua module registration ---- */

static int lua_set_emotion(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    emotion_video_t e = emotion_video_from_string(name);

    if (e >= EMOTION_COUNT) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "unknown emotion: %s", name);
        return 2;
    }

    esp_err_t ret = emotion_video_set(e);
    lua_pushboolean(L, ret == ESP_OK ? 1 : 0);
    if (ret != ESP_OK) {
        lua_pushstring(L, esp_err_to_name(ret));
        return 2;
    }
    return 1;
}

static int lua_get_emotion(lua_State *L)
{
    emotion_video_t e = emotion_video_get();
    lua_pushstring(L, emotion_video_to_string(e));
    return 1;
}

static const luaL_Reg s_emotion_lib[] = {
    { "set_emotion", lua_set_emotion },
    { "get_emotion", lua_get_emotion },
    { NULL, NULL },
};

static int luaopen_emotion(lua_State *L)
{
    luaL_newlib(L, s_emotion_lib);
    return 1;
}

esp_err_t emotion_video_register_lua(void)
{
    return cap_lua_register_module("emotion", luaopen_emotion);
}
