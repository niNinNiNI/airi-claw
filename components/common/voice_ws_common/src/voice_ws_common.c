#include "voice_ws_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "voice_ws";

#define WS_CONNECT_TIMEOUT_MS 10000
#define MAX_HANDLERS          4

/* --- server config --- */
static char s_server_url[128];

/* --- WebSocket --- */
static esp_websocket_client_handle_t s_ws_client;
static volatile bool s_ws_connected;
static SemaphoreHandle_t s_ws_connect_sem;

/* --- callback registry --- */
static struct {
    voice_ws_text_cb_t cb;
    void *user_data;
} s_text_handlers[MAX_HANDLERS];
static size_t s_text_handler_count;

static struct {
    voice_ws_binary_cb_t cb;
    void *user_data;
} s_binary_handlers[MAX_HANDLERS];
static size_t s_binary_handler_count;

static struct {
    voice_ws_state_cb_t cb;
    void *user_data;
} s_state_handlers[MAX_HANDLERS];
static size_t s_state_handler_count;

/* --- helpers --- */

static void notify_state(bool connected)
{
    for (size_t i = 0; i < s_state_handler_count; i++) {
        if (s_state_handlers[i].cb) {
            s_state_handlers[i].cb(connected, s_state_handlers[i].user_data);
        }
    }
}

/* --- WebSocket event handler --- */

static void ws_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ws_connected = true;
        xSemaphoreGive(s_ws_connect_sem);
        notify_state(true);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket disconnected");
        s_ws_connected = false;
        notify_state(false);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x02 && data->fin) {
            /* binary frame */
            if (data->data_ptr && data->data_len > 0) {
                for (size_t i = 0; i < s_binary_handler_count; i++) {
                    if (s_binary_handlers[i].cb) {
                        s_binary_handlers[i].cb(
                            (const uint8_t *)data->data_ptr,
                            (size_t)data->data_len,
                            s_binary_handlers[i].user_data);
                    }
                }
            }
        } else if (data->op_code == 0x01 && data->fin) {
            /* text frame: parse JSON type and dispatch */
            if (data->data_len > 0 && data->data_len < 1024) {
                char *msg = malloc((size_t)data->data_len + 1);
                if (!msg) break;
                memcpy(msg, data->data_ptr, (size_t)data->data_len);
                msg[data->data_len] = '\0';

                cJSON *root = cJSON_Parse(msg);
                if (!root) {
                    free(msg);
                    break;
                }

                cJSON *type = cJSON_GetObjectItem(root, "type");
                const char *type_str =
                    cJSON_IsString(type) ? type->valuestring : NULL;

                if (type_str) {
                    for (size_t i = 0; i < s_text_handler_count; i++) {
                        if (s_text_handlers[i].cb) {
                            s_text_handlers[i].cb(
                                type_str, msg,
                                s_text_handlers[i].user_data);
                        }
                    }
                }
                cJSON_Delete(root);
                free(msg);
            }
        }
        break;

    default:
        break;
    }
}

/* --- public API --- */

esp_err_t voice_ws_set_server(const char *ip, uint16_t port)
{
    if (!ip || !ip[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_server_url, sizeof(s_server_url), "ws://%s:%u", ip,
             (unsigned)port);
    ESP_LOGI(TAG, "Voice server URL: %s", s_server_url);
    return ESP_OK;
}

esp_err_t voice_ws_connect(uint32_t timeout_ms)
{
    if (s_ws_connected && s_ws_client) {
        return ESP_OK;
    }

    if (s_server_url[0] == '\0') {
        ESP_LOGE(TAG, "Server URL not configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* clean up any previous client */
    if (s_ws_client) {
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
    s_ws_connected = false;

    if (!s_ws_connect_sem) {
        s_ws_connect_sem = xSemaphoreCreateBinary();
        if (!s_ws_connect_sem) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_websocket_client_config_t cfg = {
        .uri = s_server_url,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = 5000,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
        .ping_interval_sec = 15,
        .buffer_size = 16384,
        .network_timeout_ms = 10000,
        .task_stack = 12288,
    };

    s_ws_client = esp_websocket_client_init(&cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);

    uint32_t wait_ms = timeout_ms ? timeout_ms : WS_CONNECT_TIMEOUT_MS;
    TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
    if (xSemaphoreTake(s_ws_connect_sem, wait_ticks) != pdTRUE) {
        ESP_LOGE(TAG, "WebSocket connect timeout");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "WebSocket connected to %s", s_server_url);
    return ESP_OK;
}

bool voice_ws_is_connected(void)
{
    return s_ws_connected;
}

esp_err_t voice_ws_send_text(const char *json_str)
{
    if (!s_ws_connected || !s_ws_client) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Send JSON as BINARY frame to avoid server-side UTF-8 validation
     * failures. The Java server's onWebSocketBinary handler already tries
     * to parse binary payloads as JSON first, so control messages arrive
     * via the same handleEspMessage path regardless of frame type.
     */
    int len = (int)strlen(json_str);
    int ret = esp_websocket_client_send_bin(s_ws_client, json_str,
                                            len, pdMS_TO_TICKS(3000));
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to send text (as binary): %d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t voice_ws_send_binary(const uint8_t *data, size_t len)
{
    if (!s_ws_connected || !s_ws_client) {
        return ESP_ERR_INVALID_STATE;
    }
    int ret = esp_websocket_client_send_bin(
        s_ws_client, (const char *)data, (int)len, pdMS_TO_TICKS(3000));
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to send binary (%d bytes): %d", (int)len, ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t voice_ws_register_text_handler(voice_ws_text_cb_t cb,
                                         void *user_data)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (s_text_handler_count >= MAX_HANDLERS) return ESP_ERR_NO_MEM;
    s_text_handlers[s_text_handler_count].cb = cb;
    s_text_handlers[s_text_handler_count].user_data = user_data;
    s_text_handler_count++;
    return ESP_OK;
}

esp_err_t voice_ws_register_binary_handler(voice_ws_binary_cb_t cb,
                                           void *user_data)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (s_binary_handler_count >= MAX_HANDLERS) return ESP_ERR_NO_MEM;
    s_binary_handlers[s_binary_handler_count].cb = cb;
    s_binary_handlers[s_binary_handler_count].user_data = user_data;
    s_binary_handler_count++;
    return ESP_OK;
}

esp_err_t voice_ws_register_state_handler(voice_ws_state_cb_t cb,
                                          void *user_data)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (s_state_handler_count >= MAX_HANDLERS) return ESP_ERR_NO_MEM;
    s_state_handlers[s_state_handler_count].cb = cb;
    s_state_handlers[s_state_handler_count].user_data = user_data;
    s_state_handler_count++;
    return ESP_OK;
}
