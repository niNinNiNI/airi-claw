/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <string.h>

#include "cJSON.h"
#include "emotion_video_controller.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "http_emotion";

/* GET /api/emotion */
static esp_err_t emotion_get_handler(httpd_req_t *req)
{
    emotion_video_t e = emotion_video_get();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }

    http_server_json_add_string(root, "emotion", emotion_video_to_string(e));
    cJSON_AddNumberToObject(root, "index", (int)e);
    return http_server_send_json_response(req, root);
}

/* POST /api/emotion  body: {"emotion": "wave"} */
static esp_err_t emotion_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *emotion_json = cJSON_GetObjectItem(body, "emotion");
    if (!cJSON_IsString(emotion_json) && !cJSON_IsNumber(emotion_json)) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                  "Field 'emotion' is required (string or index)");
    }

    emotion_video_t e;
    if (cJSON_IsString(emotion_json)) {
        e = emotion_video_from_string(emotion_json->valuestring);
    } else {
        e = (emotion_video_t)emotion_json->valueint;
    }

    if (e >= EMOTION_COUNT) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Unknown emotion: %s",
                 cJSON_IsString(emotion_json) ? emotion_json->valuestring : "(index)");
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
    }

    err = emotion_video_set(e);
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
        http_server_json_add_string(root, "emotion", emotion_video_to_string(e));
        cJSON_AddNumberToObject(root, "index", (int)e);
    }

    cJSON_Delete(body);
    if (!root) {
        return httpd_resp_send_500(req);
    }
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_emotion_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/emotion", .method = HTTP_GET,  .handler = emotion_get_handler  },
        { .uri = "/api/emotion", .method = HTTP_POST, .handler = emotion_post_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &handlers[i]),
                            TAG, "Failed to register emotion route");
    }

    ESP_LOGI(TAG, "Emotion API routes registered");
    return ESP_OK;
}
