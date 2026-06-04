#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_voice_register_group(void);

esp_err_t cap_voice_set_server(const char *server_ip, uint16_t server_port);

#ifdef __cplusplus
}
#endif
