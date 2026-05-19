/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"
#include "emotion_video.h"

static esp_err_t emotion_get_handler(httpd_req_t *req)
{
    emotion_video_t current = emotion_video_get_current();
    const char *name = emotion_video_get_name(current);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddStringToObject(root, "emotion", name ? name : "unknown");
    cJSON_AddNumberToObject(root, "id", (int)current);
    cJSON_AddBoolToObject(root, "playing", true);

    return http_server_send_json_response(req, root);
}

static esp_err_t emotion_post_handler(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *emotion_item = cJSON_GetObjectItem(body, "emotion");
    if (!cJSON_IsString(emotion_item)) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'emotion' field");
    }

    emotion_video_t emotion = emotion_video_from_name(emotion_item->valuestring);
    if (emotion == EMOTION_VIDEO_COUNT) {
        cJSON_Delete(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown emotion name");
    }

    cJSON_Delete(body);

    err = emotion_video_set(emotion);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "emotion", emotion_video_get_name(emotion));
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    }

    return http_server_send_json_response(req, resp);
}

esp_err_t http_server_register_emotion_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/emotion", .method = HTTP_GET,  .handler = emotion_get_handler },
        { .uri = "/api/emotion", .method = HTTP_POST, .handler = emotion_post_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
