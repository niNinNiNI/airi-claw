#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*voice_ws_text_cb_t)(const char *type, const char *json,
                                   void *user_data);
typedef void (*voice_ws_binary_cb_t)(const uint8_t *data, size_t len,
                                     void *user_data);
typedef void (*voice_ws_state_cb_t)(bool connected, void *user_data);

esp_err_t voice_ws_set_server(const char *ip, uint16_t port);

esp_err_t voice_ws_connect(uint32_t timeout_ms);
bool voice_ws_is_connected(void);

esp_err_t voice_ws_send_text(const char *json_str);
esp_err_t voice_ws_send_binary(const uint8_t *data, size_t len);

esp_err_t voice_ws_register_text_handler(voice_ws_text_cb_t cb,
                                         void *user_data);
esp_err_t voice_ws_register_binary_handler(voice_ws_binary_cb_t cb,
                                           void *user_data);
esp_err_t voice_ws_register_state_handler(voice_ws_state_cb_t cb,
                                          void *user_data);

#ifdef __cplusplus
}
#endif
